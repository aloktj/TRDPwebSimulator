#include "telegram_model.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <system_error>

#include <tinyxml2.h>

namespace trdp {

namespace {

std::once_flag xmlBootstrapFlag;
std::string defaultXmlPath = "configs/default.xml";
bool defaultXmlLoaded = false;

std::string toUpper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

std::size_t fieldTypeSize(FieldType type) {
    switch (type) {
    case FieldType::BOOL:
    case FieldType::INT8:
    case FieldType::UINT8:
        return 1U;
    case FieldType::INT16:
    case FieldType::UINT16:
        return 2U;
    case FieldType::INT32:
    case FieldType::UINT32:
    case FieldType::FLOAT:
        return 4U;
    case FieldType::DOUBLE:
        return 8U;
    case FieldType::STRING:
    case FieldType::BYTES:
        return 0U;
    }
    return 0U;
}

FieldType parseFieldType(const std::string &rawType) {
    const auto type = toUpper(rawType);
    if (type == "BOOL" || type == "BIT" || type == "BITSET" || type == "BITSET8" || type == "BITSET16") {
        return FieldType::BOOL;
    }
    if (type == "INT8" || type == "SINT8" || type == "I8") {
        return FieldType::INT8;
    }
    if (type == "UINT8" || type == "U8" || type == "BYTE" || type == "CHAR8" || type == "CHAR") {
        return FieldType::UINT8;
    }
    if (type == "INT16" || type == "SINT16" || type == "I16") {
        return FieldType::INT16;
    }
    if (type == "UINT16" || type == "U16") {
        return FieldType::UINT16;
    }
    if (type == "INT32" || type == "SINT32" || type == "I32") {
        return FieldType::INT32;
    }
    if (type == "UINT32" || type == "U32") {
        return FieldType::UINT32;
    }
    if (type == "FLOAT" || type == "FLOAT32" || type == "REAL32") {
        return FieldType::FLOAT;
    }
    if (type == "DOUBLE" || type == "FLOAT64" || type == "REAL64") {
        return FieldType::DOUBLE;
    }
    if (type == "STRING" || type == "STRING8" || type == "STR") {
        return FieldType::STRING;
    }
    return FieldType::BYTES;
}

std::size_t parseSizeAttribute(const tinyxml2::XMLElement &element, const char *name, std::size_t fallback = 0U) {
    if (const char *value = element.Attribute(name)) {
        char *endPtr = nullptr;
        const auto parsed = std::strtoull(value, &endPtr, 10);
        if (endPtr != value) {
            return static_cast<std::size_t>(parsed);
        }
    }
    return fallback;
}

Direction parseDirection(const tinyxml2::XMLElement &element) {
    if (const char *dirAttr = element.Attribute("dir")) {
        const auto upper = toUpper(dirAttr);
        if (upper == "RX" || upper == "SUB" || upper == "IN" || upper == "INPUT") {
            return Direction::Rx;
        }
    }
    if (const char *dirAttr = element.Attribute("direction")) {
        const auto upper = toUpper(dirAttr);
        if (upper == "RX" || upper == "SUB" || upper == "IN" || upper == "INPUT") {
            return Direction::Rx;
        }
    }
    return Direction::Tx;
}

TelegramType parseTelegramType(const tinyxml2::XMLElement &element) {
    const std::string name = toUpper(element.Name() ? element.Name() : "");
    if (name.find("PD") != std::string::npos) {
        return TelegramType::PD;
    }
    if (name.find("MD") != std::string::npos) {
        return TelegramType::MD;
    }
    if (const char *typeAttr = element.Attribute("type")) {
        const auto upper = toUpper(typeAttr);
        if (upper == "MD") {
            return TelegramType::MD;
        }
    }
    return TelegramType::PD;
}

std::optional<std::uint32_t> parseComId(const tinyxml2::XMLElement &element) {
    const char *attrs[] = {"comid", "comId", "ComId", "id"};
    for (const auto *attr : attrs) {
        if (const char *value = element.Attribute(attr)) {
            char *endPtr = nullptr;
            const auto parsed = std::strtoul(value, &endPtr, 10);
            if (endPtr != value) {
                return static_cast<std::uint32_t>(parsed);
            }
        }
    }
    for (auto child = element.FirstChildElement(); child != nullptr; child = child->NextSiblingElement()) {
        if (std::string(child->Name()) == "comId" || std::string(child->Name()) == "ComId") {
            const char *text = child->GetText();
            if (text != nullptr) {
                char *endPtr = nullptr;
                const auto parsed = std::strtoul(text, &endPtr, 10);
                if (endPtr != text) {
                    return static_cast<std::uint32_t>(parsed);
                }
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> parseDatasetRef(const tinyxml2::XMLElement &element) {
    const char *attrs[] = {"dataset", "datasetName", "dsName", "datasetRef"};
    for (const auto *attr : attrs) {
        if (const char *value = element.Attribute(attr)) {
            return std::string(value);
        }
    }
    for (auto child = element.FirstChildElement(); child != nullptr; child = child->NextSiblingElement()) {
        const std::string tagName = child->Name();
        if (tagName == "dataset" || tagName == "Dataset" || tagName == "dataSet") {
            if (const char *text = child->GetText()) {
                return std::string(text);
            }
        }
    }
    return std::nullopt;
}

bool elementMatches(const tinyxml2::XMLElement &element, const std::vector<std::string> &names) {
    const auto nameUpper = toUpper(element.Name() ? element.Name() : "");
    return std::any_of(names.begin(), names.end(), [&nameUpper](const std::string &candidate) {
        return nameUpper == toUpper(candidate);
    });
}

void collectElements(const tinyxml2::XMLElement &root, const std::vector<std::string> &names,
                     std::vector<const tinyxml2::XMLElement *> &out) {
    if (elementMatches(root, names)) {
        out.push_back(&root);
    }
    for (auto child = root.FirstChildElement(); child != nullptr; child = child->NextSiblingElement()) {
        collectElements(*child, names, out);
    }
}
} // namespace

const FieldDef *DatasetDef::findField(const std::string &fieldName) const {
    const auto it = std::find_if(fields.begin(), fields.end(), [&fieldName](const FieldDef &field) {
        return field.name == fieldName;
    });
    if (it == fields.end()) {
        return nullptr;
    }
    return &(*it);
}

std::size_t DatasetDef::computeSize() const {
    if (size > 0) {
        return size;
    }

    std::size_t maxOffset = 0;
    for (const auto &field : fields) {
        const auto typeSize = fieldTypeSize(field.type);
        const auto baseSize = typeSize == 0 ? std::max<std::size_t>(1, field.size) : typeSize;
        const std::size_t effectiveSize = baseSize * std::max<std::size_t>(1, field.arrayLength);
        maxOffset = std::max(maxOffset, field.offset + effectiveSize);
    }
    return maxOffset;
}

TelegramRuntime::TelegramRuntime(const DatasetDef &dataset)
    : datasetDef(dataset), buffer(calculateInitialBufferSize()) {
    for (const auto &field : datasetDef.fields) {
        fieldValues.emplace(field.name, FieldValue{});
    }
}

std::optional<FieldValue> TelegramRuntime::getFieldValue(const std::string &fieldName) const {
    std::shared_lock lock(mtx);
    const auto it = fieldValues.find(fieldName);
    if (it == fieldValues.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::map<std::string, FieldValue> TelegramRuntime::snapshotFields() const {
    std::shared_lock lock(mtx);
    return fieldValues;
}

bool TelegramRuntime::setFieldValue(const std::string &fieldName, const FieldValue &value) {
    std::unique_lock lock(mtx);
    const auto it = fieldValues.find(fieldName);
    if (it == fieldValues.end()) {
        return false;
    }
    it->second = value;
    return true;
}

std::vector<std::uint8_t> TelegramRuntime::getBufferCopy() const {
    std::shared_lock lock(mtx);
    return buffer;
}

void TelegramRuntime::overwriteBuffer(const std::vector<std::uint8_t> &data) {
    std::unique_lock lock(mtx);
    buffer = data;
}

void TelegramRuntime::updateBuffer(const std::function<void(std::vector<std::uint8_t> &)> &mutator) {
    std::unique_lock lock(mtx);
    mutator(buffer);
}

std::size_t TelegramRuntime::bufferSize() const noexcept { return buffer.size(); }

std::size_t TelegramRuntime::calculateInitialBufferSize() const { return datasetDef.computeSize(); }

TelegramRegistry &TelegramRegistry::instance() {
    static TelegramRegistry registry;
    return registry;
}

void TelegramRegistry::registerDataset(const DatasetDef &dataset) {
    std::unique_lock lock(mtx);
    datasets[dataset.name] = dataset;
}

void TelegramRegistry::registerTelegram(const TelegramDef &telegram) {
    std::unique_lock lock(mtx);
    const auto datasetIt = datasets.find(telegram.datasetName);
    if (datasetIt == datasets.end()) {
        throw std::invalid_argument("Dataset not registered for telegram: " + telegram.datasetName);
    }
    telegrams[telegram.comId] = telegram;
}

void TelegramRegistry::clear() {
    std::unique_lock lock(mtx);
    datasets.clear();
    telegrams.clear();
    runtimes.clear();
}

std::vector<DatasetDef> TelegramRegistry::listDatasets() const {
    std::shared_lock lock(mtx);
    std::vector<DatasetDef> result;
    result.reserve(datasets.size());
    for (const auto &entry : datasets) {
        result.push_back(entry.second);
    }
    return result;
}

std::optional<DatasetDef> TelegramRegistry::getDatasetCopy(const std::string &name) const {
    std::shared_lock lock(mtx);
    const auto it = datasets.find(name);
    if (it == datasets.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<TelegramDef> TelegramRegistry::getTelegramCopy(std::uint32_t comId) const {
    std::shared_lock lock(mtx);
    const auto it = telegrams.find(comId);
    if (it == telegrams.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<TelegramDef> TelegramRegistry::listTelegrams() const {
    std::shared_lock lock(mtx);
    std::vector<TelegramDef> result;
    result.reserve(telegrams.size());
    for (const auto &entry : telegrams) {
        result.push_back(entry.second);
    }
    return result;
}

std::shared_ptr<TelegramRuntime> TelegramRegistry::getOrCreateRuntime(std::uint32_t comId) {
    std::unique_lock lock(mtx);
    const auto runtimeIt = runtimes.find(comId);
    if (runtimeIt != runtimes.end()) {
        return runtimeIt->second;
    }

    const auto telegramIt = telegrams.find(comId);
    if (telegramIt == telegrams.end()) {
        return nullptr;
    }

    const auto datasetIt = datasets.find(telegramIt->second.datasetName);
    if (datasetIt == datasets.end()) {
        return nullptr;
    }

    auto runtime = std::make_shared<TelegramRuntime>(datasetIt->second);
    runtimes.emplace(comId, runtime);
    return runtime;
}

bool loadFromTauXml(const std::string &xmlPath) {
    defaultXmlLoaded = false;

    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(xmlPath.c_str()) != tinyxml2::XML_SUCCESS) {
        std::cerr << "Failed to load TRDP XML: " << xmlPath << " (" << doc.ErrorStr() << ")\n";
        return false;
    }

    const auto *root = doc.RootElement();
    if (root == nullptr) {
        std::cerr << "Invalid TRDP XML: missing root element\n";
        return false;
    }

    TelegramRegistry::instance().clear();

    std::vector<const tinyxml2::XMLElement *> datasetNodes;
    collectElements(*root, {"dataset", "DataSet", "Dataset"}, datasetNodes);

    for (const auto *dsNode : datasetNodes) {
        DatasetDef dataset;
        if (const char *name = dsNode->Attribute("name")) {
            dataset.name = name;
        }
        if (dataset.name.empty()) {
            if (const char *id = dsNode->Attribute("id")) {
                dataset.name = id;
            }
        }
        dataset.size = parseSizeAttribute(*dsNode, "size", 0U);

        for (auto fieldNode = dsNode->FirstChildElement(); fieldNode != nullptr; fieldNode = fieldNode->NextSiblingElement()) {
            if (!fieldNode->Attribute("name")) {
                continue;
            }

            FieldDef field;
            field.name = fieldNode->Attribute("name");
            if (const char *type = fieldNode->Attribute("type")) {
                field.type = parseFieldType(type);
            }
            field.offset = parseSizeAttribute(*fieldNode, "offset", field.offset);
            field.bitOffset = parseSizeAttribute(*fieldNode, "bitoffs", field.bitOffset);
            field.bitOffset = parseSizeAttribute(*fieldNode, "bitOffset", field.bitOffset);
            field.size = parseSizeAttribute(*fieldNode, "size", field.size);
            field.arrayLength = parseSizeAttribute(*fieldNode, "array", field.arrayLength);
            field.arrayLength = parseSizeAttribute(*fieldNode, "arraySize", field.arrayLength);
            if (field.arrayLength == 0) {
                field.arrayLength = 1;
            }

            dataset.fields.push_back(field);
        }

        if (!dataset.name.empty()) {
            TelegramRegistry::instance().registerDataset(dataset);
        }
    }

    std::vector<const tinyxml2::XMLElement *> telegramNodes;
    collectElements(*root, {"pd", "PD", "md", "MD", "telegram", "Telegram", "comid", "ComId"}, telegramNodes);
    for (const auto *tgNode : telegramNodes) {
        const auto comId = parseComId(*tgNode);
        if (!comId.has_value()) {
            continue;
        }

        const auto datasetRef = parseDatasetRef(*tgNode);
        if (!datasetRef.has_value()) {
            continue;
        }

        TelegramDef telegram;
        telegram.comId = comId.value();
        telegram.datasetName = datasetRef.value();
        telegram.direction = parseDirection(*tgNode);
        telegram.type = parseTelegramType(*tgNode);
        if (const char *nameAttr = tgNode->Attribute("name")) {
            telegram.name = nameAttr;
        } else if (const char *nameAttrAlt = tgNode->Attribute("comment")) {
            telegram.name = nameAttrAlt;
        } else {
            telegram.name = "ComId" + std::to_string(telegram.comId);
        }

        try {
            TelegramRegistry::instance().registerTelegram(telegram);
        } catch (const std::exception &ex) {
            std::cerr << "Skipping telegram with ComId " << telegram.comId << ": " << ex.what() << "\n";
        }
    }

    defaultXmlLoaded = true;
    return true;
}

std::optional<std::filesystem::path> executableDir() {
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return std::nullopt;
    }
    return exe.parent_path();
}

std::optional<std::filesystem::path> resolveXmlPath(const std::string &rawPath) {
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    const fs::path requested(rawPath);

    if (requested.is_absolute()) {
        candidates.push_back(requested);
    } else {
        candidates.push_back(fs::current_path() / requested);
        const auto cwdParent = fs::current_path().parent_path();
        if (!cwdParent.empty()) {
            candidates.push_back(cwdParent / requested);
        }
        if (const auto exeDir = executableDir()) {
            candidates.push_back(*exeDir / requested);
            const auto exeParent = exeDir->parent_path();
            if (!exeParent.empty()) {
                candidates.push_back(exeParent / requested);
            }
        }
    }

    for (const auto &candidate : candidates) {
        std::error_code ec;
        if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec)) {
            std::error_code canonicalEc;
            const auto canonical = fs::canonical(candidate, canonicalEc);
            return canonicalEc ? candidate : canonical;
        }
    }
    return std::nullopt;
}

bool loadDefaultXmlInternal() {
    const char *envPath = std::getenv("TRDP_XML_PATH");
    std::string path = envPath != nullptr ? std::string(envPath) : defaultXmlPath;

    const auto resolved = resolveXmlPath(path);
    if (!resolved) {
        std::cerr << "TRDP XML not found: " << path
                  << " (checked current directory, parent, and executable locations)\n";
        return false;
    }

    defaultXmlLoaded = loadFromTauXml(resolved->string());
    return defaultXmlLoaded;
}
} // namespace

void setDefaultXmlConfig(const std::string &xmlPath) { defaultXmlPath = xmlPath; }

bool ensureRegistryInitialized() {
    std::call_once(xmlBootstrapFlag, []() { defaultXmlLoaded = loadDefaultXmlInternal(); });
    return defaultXmlLoaded;
}

} // namespace trdp

