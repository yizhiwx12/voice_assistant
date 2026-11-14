#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_attr.h"

// 由 DMA 缓冲身份推进此时钟，而非经过时间或 PCM 能量。
// 驱动使用短任务/ISR 临界区串行修改，区外仅读取原子发布结果。
class PlaybackProgressClock {
public:
    struct Snapshot {
        uint32_t serial = 0;
        uint64_t completed_samples = 0;
        bool available = false;
        bool coherent = false;
    };

    void reset()
    {
        slots_ = {};
        publish(0, 0, false);
    }
    void bind(uint32_t serial)
    {
        // 只有前一个 DMA 环排空后才允许绑定。
        slots_ = {};
        publish(serial, 0, serial != 0);
    }
    void IRAM_ATTR invalidate() { publish(serial_, samples_, false); }

    void beforeWrite(const void *buffer, uint32_t serial, uint64_t end_sample, bool content)
    {
        auto *slot = find(buffer);
        if (!slot || slot->writing) { invalidate(); return; }
        slot->writing = true;
        slot->interrupted = false;
        slot->pending_serial = serial;
        slot->pending_end = end_sample;
        slot->pending_content = content;
    }
    void afterWrite(const void *buffer)
    {
        auto *slot = find(buffer);
        if (!slot || !slot->writing) { invalidate(); return; }
        if (!slot->interrupted && slot->pending_content) {
            slot->serial = slot->pending_serial;
            slot->end_sample = slot->pending_end;
        }
        if (!slot->interrupted && slot->pending_content) slot->content = true;
        slot->writing = false;
    }
    void IRAM_ATTR onSent(const void *buffer, bool amplifier_enabled)
    {
        auto *slot = find(buffer);
        if (!slot) { publish(serial_, samples_, false); return; }
        if (slot->writing) {
            // DMA 在 memcpy 期间完成，无法证明内容边界；
            // 冻结本回答游标，不做估算。
            slot->interrupted = true;
            publish(serial_, samples_, false);
        } else if (slot->content && slot->serial == serial_) {
            if (!amplifier_enabled || slot->end_sample < samples_)
                publish(serial_, samples_, false);
            else if (available_) publish(serial_, slot->end_sample, true);
        }
        slot->content = false;
    }
    Snapshot snapshot() const
    {
        Snapshot result{};
        const uint32_t before = sequence_.load(std::memory_order_acquire);
        result.serial = published_serial_.load(std::memory_order_relaxed);
        result.completed_samples = static_cast<uint64_t>(high_.load(std::memory_order_relaxed)) << 32 |
                                   low_.load(std::memory_order_relaxed);
        result.available = published_available_.load(std::memory_order_relaxed);
        result.coherent = !(before & 1) && before == sequence_.load(std::memory_order_acquire);
        if (!result.coherent) result.available = false;
        return result;
    }

private:
    struct Slot {
        const void *buffer = nullptr;
        uint64_t end_sample = 0, pending_end = 0;
        uint32_t serial = 0, pending_serial = 0;
        bool content = false, writing = false, interrupted = false, pending_content = false;
    };
    Slot *IRAM_ATTR find(const void *buffer)
    {
        if (!buffer) return nullptr;
        for (auto &slot : slots_) if (slot.buffer == buffer) return &slot;
        for (auto &slot : slots_) {
            if (!slot.buffer) { slot.buffer = buffer; return &slot; }
        }
        return nullptr;
    }
    void IRAM_ATTR publish(uint32_t serial, uint64_t samples, bool available)
    {
        serial_ = serial;
        samples_ = samples;
        available_ = available;
        sequence_.fetch_add(1, std::memory_order_acq_rel);
        published_serial_.store(serial, std::memory_order_relaxed);
        low_.store(static_cast<uint32_t>(samples), std::memory_order_relaxed);
        high_.store(static_cast<uint32_t>(samples >> 32), std::memory_order_relaxed);
        published_available_.store(available, std::memory_order_relaxed);
        sequence_.fetch_add(1, std::memory_order_release);
    }
    std::array<Slot, 4> slots_{};
    uint32_t serial_ = 0;
    uint64_t samples_ = 0;
    bool available_ = false;
    std::atomic<uint32_t> sequence_{0}, published_serial_{0}, low_{0}, high_{0};
    std::atomic<bool> published_available_{false};
};
