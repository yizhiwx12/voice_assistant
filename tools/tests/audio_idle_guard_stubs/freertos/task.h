#pragma once
#include "FreeRTOS.h"
TickType_t xTaskGetTickCount();
unsigned xPortGetCoreID();
void vTaskDelay(TickType_t);
