#pragma once

#include "esp_err.h"

namespace voiceaudio {

// 仅在启动阶段对生产 FD/AEC 前端执行合成测试，须早于麦克风、扬声器或网络初始化。
// 每个用例独占一个新 AFE；成功仅表示有限探测流程完成，不表示 DSP 结果通过。
// 停止超时会保留工作任务持有的全部存储并返回错误。
// 出现任何错误后，调用方都不得继续正常音频启动。
esp_err_t runAfeSignalProbe();

}  // 命名空间 voiceaudio
