#pragma once
inline unsigned mock_delay_ticks = 0;
inline void vTaskDelay(unsigned ticks) { mock_delay_ticks += ticks; }
