#include "platform/firmware/secret_store.h"

#include "platform/firmware/nvs_blob_store.h"

namespace homedeck {

namespace {
constexpr char kTag[] = "secret_store";
}  // namespace

bool FirmwareSecretStore::Set(const std::string& ns, const std::string& key, const std::string& value) {
    return NvsSetBlob(kPartitionName, kTag, ns, key, value);
}

std::optional<std::string> FirmwareSecretStore::Get(const std::string& ns, const std::string& key) {
    return NvsGetBlob(kPartitionName, kTag, ns, key);
}

bool FirmwareSecretStore::Erase(const std::string& ns, const std::string& key) {
    return NvsEraseBlob(kPartitionName, ns, key);
}

}  // namespace homedeck
