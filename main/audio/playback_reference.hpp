#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_attr.h"

// 捕获 MAX98357A 已完成的 TX DMA 块，
// 不在 I2S 中断中分配、打印、滤波或访问 PSRAM。
class PlaybackReference {
public:
    static constexpr size_t kSamplesPerBlock = 240;

    enum class IntervalKind : uint8_t {
        Unknown = 0,
        Content,
        DigitalSilence,
        AmplifierMuted,
    };

    struct Block {
        uint32_t dma_generation = 0;
        uint32_t playback_serial = 0;
        uint32_t history_sequence = 0;  // 由 pump() 分配，绝不由 ISR 分配。
        uint64_t first_sample_24k = 0;
        int64_t observed_end_us = 0;
        uint16_t sample_count = 0;
        IntervalKind interval_kind = IntervalKind::Unknown;
        bool amplifier_enabled = false;
        bool known_silence = false;
        bool stereo_match = false;
        bool gap_before = false;
        bool amplifier_boundary = false;  // SD 可能在 DMA 块中途变化。
        std::array<int16_t, kSamplesPerBlock> mono{};
    };

    struct Metrics {
        bool enabled = false;
        bool history_available = false;
        uint32_t dma_generation = 0;
        uint32_t playback_serial = 0;
        uint32_t submitted_frames = 0;
        uint32_t partial_write_bytes = 0;
        uint32_t completed_blocks = 0;
        uint32_t completed_frames = 0;
        uint32_t completed_sample_24k_low = 0;
        uint32_t last_observed_end_us_low = 0;
        uint32_t amplifier_muted_blocks = 0;
        uint32_t amplifier_boundary_blocks = 0;
        uint32_t known_silence_blocks = 0;
        uint32_t non_silence_blocks = 0;
        uint32_t stereo_mismatch_samples = 0;
        uint32_t invalid_blocks = 0;
        uint32_t queue_dropped_blocks = 0;
        uint32_t history_blocks = 0;
        uint32_t latest_history_sequence = 0;
        uint32_t history_overwrites = 0;
        uint32_t history_unavailable_blocks = 0;
        uint32_t queue_blocks = 0;
        bool discontinuity = false;
        bool binding_fault = false;
    };
    struct Tag {
        uint32_t dma_generation = 0;
        uint32_t playback_serial = 0;
    };

    PlaybackReference() = default;
    PlaybackReference(const PlaybackReference &) = delete;
    PlaybackReference &operator=(const PlaybackReference &) = delete;

    // 观测器分配失败仅使参考采集降级；
    // 调用方仍可继续正常半双工播放。
    esp_err_t start();
    void stop();
    void beginGeneration();
    // 提交音频前绑定。新建空代次和成功排空的环可在
    // 下一次 PCM 写入前反复改变序号。
    bool bindPlaybackSerial(uint32_t serial);
    void recordSubmittedBytes(size_t bytes, size_t requested_bytes, bool successful = true);
    void markDrained();
    void markAmplifierTransition();
    void setAmplifierTransitionActive(bool active);
    bool enabled() const { return queue_ != nullptr; }

    // 仅由 I2S on_sent ISR 在驱动自动清零 DMA 前调用。
    bool onDmaSent(const void *stereo_pcm, size_t bytes, bool amplifier_enabled);

    // 任务上下文交接：把有界 ISR 事件移入 PSRAM 历史。
    void pump();
    Metrics metrics() const;
    // 供采集任务安全访问的无锁子集；metrics() 还包含
    // 仅由播放持有者访问的历史计数，不能并发读取。
    Tag tag() const
    {
        return {generation_.load(std::memory_order_acquire),
                playback_serial_.load(std::memory_order_acquire)};
    }
    // 仅在任务上下文调用。驱动持有者将其与 pump/生命周期操作串行化。
    size_t copyHistory(Block *destination, size_t capacity, uint32_t generation = 0) const;
    // 返回 cursor 之后的下一个历史块。有界历史已丢弃
    // 中间一个或多个块时，设置 overwritten。
    bool copyHistoryAfter(uint32_t cursor, Block *destination, bool *overwritten = nullptr) const;

private:
    static constexpr uint32_t kQueueBlocks = 8;
    static constexpr uint32_t kHistoryBlocks = 30;

    struct Counters {
        std::atomic<uint32_t> submitted_frames{0};
        std::atomic<uint32_t> partial_write_bytes{0};
        std::atomic<uint32_t> completed_blocks{0};
        std::atomic<uint32_t> completed_frames{0};
        std::atomic<uint32_t> amplifier_muted_blocks{0};
        std::atomic<uint32_t> amplifier_boundary_blocks{0};
        std::atomic<uint32_t> known_silence_blocks{0};
        std::atomic<uint32_t> non_silence_blocks{0};
        std::atomic<uint32_t> stereo_mismatch_samples{0};
        std::atomic<uint32_t> invalid_blocks{0};
        std::atomic<uint32_t> queue_dropped_blocks{0};
    };

    bool pop(Block *block);

    Block *queue_ = nullptr;       // 内部 RAM，由 ISR 写入。
    Block *history_ = nullptr;     // PSRAM，由任务上下文中的 pump() 写入。
    std::atomic<uint32_t> queue_write_{0};
    std::atomic<uint32_t> queue_read_{0};
    std::atomic<uint32_t> generation_{0};
    std::atomic<uint32_t> playback_serial_{0};
    std::atomic<uint32_t> completed_sample_24k_low_{0};
    std::atomic<uint32_t> last_observed_end_us_low_{0};
    uint64_t sample_cursor_ = 0;  // 仅 ISR 写入，TX 删除后重置。
    std::atomic<uint32_t> amplifier_boundary_{0};
    std::atomic<uint32_t> amplifier_transition_active_{0};
    std::atomic<uint32_t> pending_gap_{0};
    std::atomic<uint32_t> discontinuity_{0};
    // 即使不可能有已提交 PCM 发声，历史缺口仍保留用于诊断。
    // 此处只锁存会破坏安全序号绑定的故障。
    std::atomic<uint32_t> binding_fault_{0};
    Counters counters_{};
    uint32_t history_write_ = 0;   // 仅任务上下文写入。
    uint32_t history_count_ = 0;
    uint32_t history_sequence_ = 0;
    uint32_t history_overwrites_ = 0;
    uint32_t history_unavailable_ = 0;
    bool rebind_allowed_ = false;  // 串行化驱动持有者访问，绝不在 ISR 中访问。
    bool write_failed_ = false;
};
