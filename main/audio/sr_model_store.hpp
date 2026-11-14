#pragma once

#include "esp_err.h"
#include "model_path.h"

// 持有模型映射。调用 unload() 前，所有 AFE 使用者都必须先销毁。
class SrModelStore {
public:
    SrModelStore() = default;
    ~SrModelStore() { unload(); }
    SrModelStore(const SrModelStore &) = delete;
    SrModelStore &operator=(const SrModelStore &) = delete;

    esp_err_t load();
    void unload();
    srmodel_list_t *models() const { return models_; }
    const char *vadModelName() const { return vad_model_name_; }

private:
    srmodel_list_t *models_ = nullptr;
    const char *vad_model_name_ = nullptr;
};
