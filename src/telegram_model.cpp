#include "telegram_model.h"

#include <algorithm>
#include <stdexcept>

namespace trdp {

namespace {
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
}

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
    std::size_t maxOffset = 0;
    for (const auto &field : fields) {
        const auto typeSize = fieldTypeSize(field.type);
        const std::size_t effectiveSize = typeSize == 0 ? field.size : typeSize;
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

} // namespace trdp

