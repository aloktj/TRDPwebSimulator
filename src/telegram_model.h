#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <variant>
#include <vector>

namespace trdp {

enum class FieldType {
    BOOL,
    INT8,
    UINT8,
    INT16,
    UINT16,
    INT32,
    UINT32,
    FLOAT,
    DOUBLE,
    STRING,
    BYTES,
};

struct FieldDef {
    std::string name;
    FieldType type{FieldType::BYTES};
    std::size_t offset{0};
    std::size_t size{0};
    std::size_t bitOffset{0};
    std::size_t arrayLength{1};
};

struct DatasetDef {
    std::string name;
    std::size_t size{0};
    std::vector<FieldDef> fields;

    [[nodiscard]] const FieldDef *findField(const std::string &fieldName) const;
    [[nodiscard]] std::size_t computeSize() const;
};

enum class Direction { Tx, Rx };

enum class TelegramType { PD, MD };

using FieldValue = std::variant<
    std::monostate,
    bool,
    std::int8_t,
    std::uint8_t,
    std::int16_t,
    std::uint16_t,
    std::int32_t,
    std::uint32_t,
    float,
    double,
    std::string,
    std::vector<std::uint8_t>>;

struct TelegramDef {
    std::uint32_t comId{0};
    std::string name;
    Direction direction{Direction::Tx};
    TelegramType type{TelegramType::PD};
    std::string datasetName;
};

class TelegramRuntime {
  public:
    explicit TelegramRuntime(const DatasetDef &dataset);

    [[nodiscard]] std::optional<FieldValue> getFieldValue(const std::string &fieldName) const;
    [[nodiscard]] std::map<std::string, FieldValue> snapshotFields() const;
    bool setFieldValue(const std::string &fieldName, const FieldValue &value);

    [[nodiscard]] std::vector<std::uint8_t> getBufferCopy() const;
    void overwriteBuffer(const std::vector<std::uint8_t> &data);
    void updateBuffer(const std::function<void(std::vector<std::uint8_t> &)> &mutator);

    [[nodiscard]] std::size_t bufferSize() const noexcept;
    [[nodiscard]] const DatasetDef &dataset() const noexcept { return datasetDef; }

  private:
    DatasetDef datasetDef;
    mutable std::shared_mutex mtx;
    std::vector<std::uint8_t> buffer;
    std::map<std::string, FieldValue> fieldValues;

    std::size_t calculateInitialBufferSize() const;
};

class TelegramRegistry {
  public:
    static TelegramRegistry &instance();

    void registerDataset(const DatasetDef &dataset);
    void registerTelegram(const TelegramDef &telegram);
    void clear();

    [[nodiscard]] std::optional<DatasetDef> getDatasetCopy(const std::string &name) const;
    [[nodiscard]] std::optional<TelegramDef> getTelegramCopy(std::uint32_t comId) const;
    [[nodiscard]] std::vector<TelegramDef> listTelegrams() const;

    std::shared_ptr<TelegramRuntime> getOrCreateRuntime(std::uint32_t comId);

  private:
    TelegramRegistry() = default;

    mutable std::shared_mutex mtx;
    std::map<std::string, DatasetDef> datasets;
    std::map<std::uint32_t, TelegramDef> telegrams;
    std::map<std::uint32_t, std::shared_ptr<TelegramRuntime>> runtimes;
};

bool loadFromTauXml(const std::string &xmlPath);
void setDefaultXmlConfig(const std::string &xmlPath);
bool ensureRegistryInitialized();

} // namespace trdp

