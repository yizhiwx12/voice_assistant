#include "sr_model_store.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "esp_log.h"
#include "esp_partition.h"
#include "psa/crypto.h"
#include "sr_model_manifest.hpp"

namespace {
constexpr const char *kTag = "sr_model";

esp_err_t verifyImage(const esp_partition_t *partition) {
    using namespace voiceaudio::srmanifest;
    if (partition->size < kImageBytes || kImageBytes == 0) {
        ESP_LOGE(kTag, "Model image does not fit: partition=%lu expected=%lu",
                 static_cast<unsigned long>(partition->size),
                 static_cast<unsigned long>(kImageBytes));
        return ESP_ERR_INVALID_SIZE;
    }

    // ESP-SR 解析器接触任何 Flash 指针或长度前，先验证确切的构建产物。
    // 缺失、陈旧或损坏的模型都不得解析。
    std::array<uint8_t, 1024> block{};
    std::array<uint8_t, 32> digest{};
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    psa_status_t result = psa_crypto_init();
    if (result == PSA_SUCCESS) {
        result = psa_hash_setup(&hash, PSA_ALG_SHA_256);
    }
    esp_err_t error = ESP_OK;
    for (size_t offset = 0; result == 0 && offset < kImageBytes;) {
        const size_t bytes = std::min(block.size(), kImageBytes - offset);
        error = esp_partition_read(partition, offset, block.data(), bytes);
        if (error != ESP_OK) {
            break;
        }
        result = psa_hash_update(&hash, block.data(), bytes);
        offset += bytes;
    }
    if (error == ESP_OK && result == 0) {
        size_t digest_bytes = 0;
        result = psa_hash_finish(&hash, digest.data(), digest.size(), &digest_bytes);
        if (result == PSA_SUCCESS && digest_bytes != digest.size()) {
            result = PSA_ERROR_GENERIC_ERROR;
        }
    }
    psa_hash_abort(&hash);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "Cannot read model partition: %s", esp_err_to_name(error));
        return error;
    }
    if (result != 0) {
        ESP_LOGE(kTag, "Model SHA256 calculation failed: %d", result);
        return ESP_FAIL;
    }

    char actual_hash[65]{};
    constexpr char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < digest.size(); ++i) {
        actual_hash[2 * i] = kHex[digest[i] >> 4];
        actual_hash[2 * i + 1] = kHex[digest[i] & 15];
    }
    if (std::strcmp(actual_hash, kSha256) != 0) {
        ESP_LOGE(kTag, "Model checksum mismatch: expected=%s actual=%s; flash the matching partition table, application and srmodels.bin",
                 kSha256, actual_hash);
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(kTag, "Verified ESP-SR=%s model=%s bytes=%lu offset=0x%lx sha256=%s",
             kComponentVersion, kModelName, static_cast<unsigned long>(kImageBytes),
             static_cast<unsigned long>(partition->address), actual_hash);
    return ESP_OK;
}
}  // 命名空间

esp_err_t SrModelStore::load() {
    if (models_ != nullptr) {
        return ESP_OK;
    }
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
    if (partition == nullptr) {
        ESP_LOGE(kTag, "Missing model partition; flash the complete P4 image set");
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t verified = verifyImage(partition);
    if (verified != ESP_OK) {
        return verified;
    }
    models_ = esp_srmodel_init("model");
    if (models_ == nullptr) {
        ESP_LOGE(kTag, "ESP-SR could not map the verified model image");
        return ESP_FAIL;
    }
    for (int i = 0; i < models_->num; ++i) {
        if (models_->model_name[i] != nullptr &&
            std::strcmp(models_->model_name[i], voiceaudio::srmanifest::kModelName) == 0) {
            vad_model_name_ = models_->model_name[i];
            break;
        }
    }
    if (vad_model_name_ == nullptr) {
        ESP_LOGE(kTag, "Verified image has no expected model: %s",
                 voiceaudio::srmanifest::kModelName);
        unload();
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(kTag, "Loaded model=%s count=%d", vad_model_name_, models_->num);
    return ESP_OK;
}

void SrModelStore::unload() {
    vad_model_name_ = nullptr;
    if (models_ != nullptr) {
        esp_srmodel_deinit(models_);
        models_ = nullptr;
    }
}
