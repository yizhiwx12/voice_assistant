#pragma once

// 控制器测试替换外设，保留生产协议/状态代码。
#include <functional>
#include "esp_err.h"
#include "audio/opus_codec.hpp"
#include "network/voice_protocol.hpp"
#include "audio/source_signal_history.hpp"

class AudioService {
public:
    struct Metrics { uint32_t rms = 0, peak = 0, clipped_samples = 0, playback_queue_frames = 0; };
    struct FrontendMetrics {
        bool enabled = false, ready = false, speech = false;
        bool aec_requested = false, aec_active = false;
        esp_err_t last_error = ESP_OK;
        uint64_t feed_samples = 0, output_samples = 0;
        uint32_t output_frames = 0, queue_frames = 0, queue_peak = 0;
        uint32_t microphone_rms = 0, microphone_peak = 0;
        int32_t microphone_mean = 0;
        uint32_t microphone_ac_rms = 0;
        uint32_t reference_rms = 0, reference_peak = 0;
        int32_t reference_mean = 0;
        uint32_t reference_ac_rms = 0;
        uint32_t output_rms = 0, output_peak = 0;
        uint32_t feed_max_us = 0, fetch_max_us = 0, feed_stack_free = 0, fetch_stack_free = 0;
        uint32_t feed_chunk_samples = 0, fetch_chunk_samples = 0, output_blocks = 0;
        uint32_t vad_transitions = 0, feed_total_us = 0;
        bool vad_thresholds_valid = false, vad_input_energy_valid = false;
        int32_t vad_energy_threshold_cdbfs = 0, vad_input_energy_cdbfs = 0, vad_input_energy_peak_cdbfs = 0;
        int32_t vad_probability_threshold_milli = -1;
        uint32_t vad_input_energy_frames = 0, vad_input_energy_invalid_frames = 0;
    };
    FrontendMetrics frontend_metrics{};
    using FrontendSourceWindow = voiceaudio::SourceSignalHistory::Window;
    FrontendSourceWindow source_window{};
    mutable uint64_t source_requested_first = 0, source_requested_end = 0;
    FrontendSourceWindow frontendSourceWindow(uint64_t first, uint64_t end) const {
        source_requested_first = first;
        source_requested_end = end;
        auto result = source_window;
        result.requested_first = first;
        result.requested_end = end;
        return result;
    }
    bool playing = false;
    unsigned stop_count = 0;
    unsigned begin_count = 0, feed_count = 0;
    esp_err_t stop_result = ESP_OK;
    esp_err_t begin_result = ESP_OK;
    std::function<void()> stop_hook;
    using FrontendOutputCallback = std::function<void(const int16_t *, size_t, uint64_t, bool)>;
    FrontendOutputCallback frontend_output;
    esp_err_t setFrontendOutputCallback(FrontendOutputCallback output) {
        frontend_output = std::move(output); return ESP_OK;
    }
    esp_err_t start(std::function<void(const int16_t *, size_t)>,
                    std::function<void(const voice::Identity &)>,
                    std::function<void(esp_err_t)>, uint8_t, uint8_t) { return ESP_OK; }
    esp_err_t beginPlayback(const voice::Identity &, uint32_t, bool, uint32_t) { ++begin_count; return begin_result; }
    esp_err_t feedPlayback(const voice::Identity &, uint32_t, uint32_t, uint64_t,
                          const uint8_t *, size_t) { ++feed_count; return ESP_OK; }
    esp_err_t endPlayback(const voice::Identity &, uint32_t, uint64_t) { return ESP_OK; }
    esp_err_t stopPlayback() { playing = false; ++stop_count; if (stop_hook) stop_hook(); return stop_result; }
    bool isPlaying() const { return playing; }
    struct PlaybackProgress {
        voice::Identity identity{};
        uint32_t stream_id = 0;
        uint64_t completed_samples = 0;
        bool available = false, coherent = false, playing = false;
    };
    PlaybackProgress playback_progress{};
    PlaybackProgress playbackProgress() const { return playback_progress; }
    Metrics metrics() const { return {}; }
    FrontendMetrics frontendMetrics() const { return frontend_metrics; }
    void logPlaybackReference() {}
};
