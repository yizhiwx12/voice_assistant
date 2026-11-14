#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
// 仅针对 i2s_channel_write() 调用，紧邻其 DMA memcpy 前后执行。
// 生成的 SDK 源文件保存在构建目录。
void voice_i2s_write_observer(void *channel, const void *dma_buffer,
                              size_t dma_offset, size_t source_offset,
                              size_t bytes, int completed);
#ifdef __cplusplus
}
#endif
