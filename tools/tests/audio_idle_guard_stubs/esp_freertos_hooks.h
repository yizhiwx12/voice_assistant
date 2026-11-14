#pragma once
#include "esp_err.h"
using esp_freertos_idle_cb_t = bool (*)();
esp_err_t esp_register_freertos_idle_hook_for_cpu(esp_freertos_idle_cb_t, unsigned);
