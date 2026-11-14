#include "conversation_controller.hpp"
#include "app/memory_diagnostics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"

namespace {
constexpr char kTag[] = "conversation";
constexpr std::array<int64_t, 5> kReconnectDelayUs = {1000000, 2000000, 4000000, 8000000, 15000000};
constexpr int64_t kStableConnectionUs = 30000000;
bool retryableFailure(const char *reason, esp_err_t result)
{
    return result != ESP_ERR_NO_MEM && std::strcmp(reason, "audio_device") &&
        std::strcmp(reason, "playback_stop") && std::strcmp(reason, "barge_in_stop") &&
        std::strcmp(reason, "playback_begin") && std::strcmp(reason, "session_no_memory") &&
        std::strcmp(reason, "session_format") && std::strcmp(reason, "downlink_format") &&
        std::strcmp(reason, "capture_frame_size") && std::strcmp(reason, "server_protocol_failed");
}
#ifdef CONFIG_VOICE_AUTO_LISTEN_ENABLED
constexpr uint32_t kAutoPrerollFrames = (CONFIG_VOICE_AUTO_PREROLL_MS + 19) / 20;
constexpr uint32_t kAutoSilenceFrames = (CONFIG_VOICE_AUTO_SILENCE_MS + 19) / 20;
constexpr uint32_t kAutoConfirmFrames = (CONFIG_VOICE_AUTO_START_CONFIRM_MS + 19) / 20;
constexpr int64_t kAutoHandshakeUs = CONFIG_VOICE_AUTO_HANDSHAKE_MS * 1000LL;
constexpr uint32_t kAutoBufferFrames = kAutoPrerollFrames +
    (CONFIG_VOICE_AUTO_HANDSHAKE_MS + 19) / 20 + 4 +
#if defined(CONFIG_VOICE_BARGE_IN_ENABLED) && CONFIG_VOICE_BARGE_IN_ENABLED
    std::max(kAutoConfirmFrames, uint32_t{(CONFIG_VOICE_BARGE_IN_CONFIRM_MS + 19) / 20})
#else
    kAutoConfirmFrames
#endif
    ;
#else
constexpr int64_t kAutoHandshakeUs = 2000000;
#endif
#if defined(CONFIG_VOICE_BARGE_IN_OBSERVATION_ENABLED) && CONFIG_VOICE_BARGE_IN_OBSERVATION_ENABLED
constexpr uint32_t kBargeInConfirmFrames = (CONFIG_VOICE_BARGE_IN_CONFIRM_MS + 19) / 20;
constexpr uint32_t kBargeInHoldFrames = (CONFIG_VOICE_AUTO_HANDSHAKE_MS + 19) / 20;
constexpr uint32_t kBargeInBufferFrames = kAutoPrerollFrames + kBargeInConfirmFrames + kBargeInHoldFrames + 4;
#endif
const char *bargeInPhase(voiceaudio::BargeInObserver::Phase phase)
{
    using Phase = voiceaudio::BargeInObserver::Phase;
    switch (phase) {
    case Phase::Disabled: return "disabled";
    case Phase::Armed: return "armed";
    case Phase::Candidate: return "candidate";
    case Phase::Confirmed: return "confirmed";
    case Phase::Cooldown: return "cooldown";
    case Phase::Fault: return "fault";
    }
    return "unknown";
}
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json object() { return Json(cJSON_CreateObject(), cJSON_Delete); }
const char *text(cJSON *event, const char *field)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(event, field);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}
bool number(cJSON *event, const char *field, uint64_t &out, uint64_t limit = UINT32_MAX)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(event, field);
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < 0 || value->valuedouble > static_cast<double>(limit) ||
        std::floor(value->valuedouble) != value->valuedouble) return false;
    out = static_cast<uint64_t>(value->valuedouble);
    return true;
}

bool validUtf8(const char *value, size_t size, bool selection = true)
{
    for (size_t i = 0; i < size;) {
        const uint8_t lead = static_cast<uint8_t>(value[i]);
        size_t width = 0;
        uint32_t codepoint = 0;
        if (lead <= 0x7f) {
            width = 1;
            codepoint = lead;
        } else if (lead >= 0xc2 && lead <= 0xdf) {
            width = 2;
            codepoint = lead & 0x1f;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            width = 3;
            codepoint = lead & 0x0f;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            width = 4;
            codepoint = lead & 0x07;
        } else {
            return false;
        }
        if (i + width > size) return false;
        for (size_t j = 1; j < width; ++j) {
            const uint8_t continuation = static_cast<uint8_t>(value[i + j]);
            if ((continuation & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (continuation & 0x3f);
        }
        if ((width == 2 && codepoint < 0x80) || (width == 3 && codepoint < 0x800) ||
            (width == 4 && codepoint < 0x10000) || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            (codepoint < 0x20 && (selection ||
             (codepoint != '\n' && codepoint != '\r' && codepoint != '\t'))) ||
            (codepoint >= 0x7f && codepoint <= 0x9f) ||
            (selection && (codepoint == '"' || codepoint == '\\' ||
             codepoint == 0x2028 || codepoint == 0x2029))) return false;
        i += width;
    }
    return true;
}

bool selectionText(cJSON *object, const char *field, char *destination, size_t capacity)
{
    destination[0] = '\0';
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, field);
    if (!cJSON_IsString(value) || !value->valuestring) return false;
    const size_t size = std::strlen(value->valuestring);
    if (!size || size >= capacity || !validUtf8(value->valuestring, size)) return false;
    std::memcpy(destination, value->valuestring, size + 1);
    return true;
}

VoiceSelectionFlag selectionBool(cJSON *object, const char *field, bool &rejected)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, field);
    if (value == nullptr) return VoiceSelectionFlag::Unknown;
    if (cJSON_IsTrue(value)) return VoiceSelectionFlag::Yes;
    if (cJSON_IsFalse(value)) return VoiceSelectionFlag::No;
    rejected = true;
    return VoiceSelectionFlag::Unknown;
}

void parseProviderSelection(cJSON *selection, const char *field,
                            VoiceProviderSelection &provider, bool &rejected)
{
    cJSON *object = cJSON_GetObjectItemCaseSensitive(selection, field);
    if (!cJSON_IsObject(object)) {
        if (object != nullptr) rejected = true;
        return;
    }
    const bool id_ok = selectionText(object, "id", provider.id.data(), provider.id.size());
    const bool adapter_ok = selectionText(object, "adapter", provider.adapter.data(), provider.adapter.size());
    const bool model_ok = selectionText(object, "model", provider.model.data(), provider.model.size());
    const bool voice_ok = selectionText(object, "voice", provider.voice.data(), provider.voice.size());
    const bool resource_ok = selectionText(object, "resource_id", provider.resource_id.data(), provider.resource_id.size());
    const bool disabled = !std::strcmp(field, "tts") && !std::strcmp(provider.id.data(), "none");
    const bool null_adapter = disabled && cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(object, "adapter"));
    if ((cJSON_GetObjectItemCaseSensitive(object, "id") != nullptr && !id_ok) ||
        (cJSON_GetObjectItemCaseSensitive(object, "adapter") != nullptr && !adapter_ok && !null_adapter) ||
        (cJSON_GetObjectItemCaseSensitive(object, "model") != nullptr && !model_ok) ||
        (cJSON_GetObjectItemCaseSensitive(object, "voice") != nullptr && !voice_ok) ||
        (cJSON_GetObjectItemCaseSensitive(object, "resource_id") != nullptr && !resource_ok)) {
        rejected = true;
    }
    cJSON *capabilities = cJSON_GetObjectItemCaseSensitive(object, "capabilities");
    if (capabilities != nullptr && !cJSON_IsObject(capabilities)) {
        rejected = true;
    } else if (capabilities != nullptr) {
        provider.capabilities.streaming_input = selectionBool(capabilities, "streaming_input", rejected);
        provider.capabilities.streaming_output = selectionBool(capabilities, "streaming_output", rejected);
        provider.capabilities.partial_results = selectionBool(capabilities, "partial_results", rejected);
    }
}

bool hasEscapedNul(const uint8_t *data, size_t bytes)
{
    // cJSON 使用 C 字符串存储文本，遇到 NUL 会丢失解码后的长度。
    // 跳过已转义的反斜杠，避免把字面量 "\\u0000" 误判为内嵌 NUL。
    for (size_t i = 0; i + 1 < bytes; ++i) {
        if (data[i] != '\\') continue;
        if (i + 5 < bytes && data[i + 1] == 'u' && data[i + 2] == '0' &&
            data[i + 3] == '0' && data[i + 4] == '0' && data[i + 5] == '0') return true;
        ++i;
    }
    return false;
}

VoiceSessionSelection parseSessionSelection(cJSON *event, uint32_t epoch, bool escaped_nul)
{
    VoiceSessionSelection result;
    result.epoch = epoch;
    if (escaped_nul) {
        result.rejected_fields = true;
        return result;
    }
    cJSON *selection = cJSON_GetObjectItemCaseSensitive(event, "selection");
    if (!cJSON_IsObject(selection)) {
        if (selection != nullptr) result.rejected_fields = true;
        return result;
    }
    result.metadata_present = true;
    const bool profile_ok = selectionText(selection, "profile", result.profile.data(), result.profile.size());
    const bool mode_ok = selectionText(selection, "mode", result.mode.data(), result.mode.size());
    if ((cJSON_GetObjectItemCaseSensitive(selection, "profile") != nullptr && !profile_ok) ||
        (cJSON_GetObjectItemCaseSensitive(selection, "mode") != nullptr && !mode_ok)) {
        result.rejected_fields = true;
    }
    result.require_streaming_asr = selectionBool(selection, "require_streaming_asr", result.rejected_fields);
    parseProviderSelection(selection, "asr", result.asr, result.rejected_fields);
    parseProviderSelection(selection, "llm", result.llm, result.rejected_fields);
    parseProviderSelection(selection, "tts", result.tts, result.rejected_fields);
    return result;
}

const char *selectionFlag(VoiceSelectionFlag value)
{
    switch (value) {
    case VoiceSelectionFlag::Yes: return "yes";
    case VoiceSelectionFlag::No: return "no";
    case VoiceSelectionFlag::Unknown: return "unknown";
    }
    return "unknown";
}

void logProviderSelection(uint32_t epoch, const char *name, const VoiceProviderSelection &provider)
{
    const auto provided = [](const auto &value) { return value[0] ? value.data() : "not_provided"; };
    ESP_LOGI(kTag, "session_selection_%s epoch=%u id=\"%s\" adapter=\"%s\" model=\"%s\" voice=\"%s\" resource_id=\"%s\" streaming_input=%s streaming_output=%s partial_results=%s",
             name, static_cast<unsigned>(epoch), provided(provider.id), provided(provider.adapter), provided(provider.model),
             provided(provider.voice), provided(provider.resource_id),
             selectionFlag(provider.capabilities.streaming_input),
             selectionFlag(provider.capabilities.streaming_output),
             selectionFlag(provider.capabilities.partial_results));
}
bool getIdentity(cJSON *event, voice::Identity &id)
{
    uint64_t epoch, turn, generation;
    if (!number(event, "session_epoch", epoch) || !number(event, "turn_id", turn) ||
        !number(event, "generation", generation) || !epoch) return false;
    id = {static_cast<uint32_t>(epoch), static_cast<uint32_t>(turn), static_cast<uint32_t>(generation)};
    return true;
}
void addIdentity(cJSON *event, const voice::Identity &id)
{
    cJSON_AddNumberToObject(event, "session_epoch", id.epoch);
    cJSON_AddNumberToObject(event, "turn_id", id.turn);
    cJSON_AddNumberToObject(event, "generation", id.generation);
}
bool sendJson(VoiceClient &client, cJSON *event)
{
    if (!event) return false;
    char *wire = cJSON_PrintUnformatted(event);
    if (!wire) return false;
    const esp_err_t err = client.sendText(wire, std::strlen(wire), 500);
    cJSON_free(wire);
    return err == ESP_OK;
}
std::string serverUri()
{
    if (CONFIG_VOICE_WS_SERVER_URL[0]) return CONFIG_VOICE_WS_SERVER_URL;
    std::string uri = CONFIG_VOICE_AUDIO_SERVER_URL;
    if (uri.compare(0, 8, "https://") == 0) uri.replace(0, 8, "wss://");
    else if (uri.compare(0, 7, "http://") == 0) uri.replace(0, 7, "ws://");
    while (!uri.empty() && uri.back() == '/') uri.pop_back();
    return uri + "/ws/voice";
}
}  // 匿名命名空间结束

esp_err_t ConversationController::start(VoiceAssistantStatus &status, WifiStation &wifi)
{
    status_ = &status;
    wifi_ = &wifi;
    status.enableRealtime();
    status.setPipelineStage("正在连接语音服务");
#if defined(CONFIG_VOICE_DEVICE_AUTOSTART) && CONFIG_VOICE_DEVICE_AUTOSTART
#if defined(CONFIG_VOICE_BARGE_IN_ENABLED) && CONFIG_VOICE_BARGE_IN_ENABLED
    requested_mode_ = InteractionMode::BargeIn;
#elif defined(CONFIG_VOICE_AUTO_LISTEN_ENABLED) && CONFIG_VOICE_AUTO_LISTEN_ENABLED
    requested_mode_ = InteractionMode::Automatic;
#endif
#endif
    restore_mode_pending_ = true;
    uri_ = serverUri();
#ifdef CONFIG_VOICE_CODEC_PCM
    opus_ = false;
#endif
    // 为 Start/End 保留两个队列位置，不挤掉任何音频帧。
    commands_ = xQueueCreate(12, sizeof(Command));
    uplink_ = xQueueCreate(27, sizeof(Input));
    if (!commands_ || !uplink_) return ESP_ERR_NO_MEM;
#ifdef CONFIG_VOICE_AUTO_LISTEN_ENABLED
    automatic_storage_ = static_cast<voiceaudio::AutomaticUtterance::Frame *>(heap_caps_calloc(
        kAutoBufferFrames, sizeof(voiceaudio::AutomaticUtterance::Frame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (automatic_storage_) {
        for (size_t i = 0; i < kAutoBufferFrames; ++i)
            new (automatic_storage_ + i) voiceaudio::AutomaticUtterance::Frame;
        automatic_.configure(automatic_storage_, kAutoBufferFrames, kAutoPrerollFrames, kAutoSilenceFrames,
                             kAutoConfirmFrames, CONFIG_VOICE_AUTO_START_MIN_AC_RMS);
        ESP_LOGI(kTag, "Automatic onset: confirm=%u ms min_ac_rms=%u energy_half=1 tail_active=2/3 preroll=%u ms; confirmation stays local",
                 static_cast<unsigned>(kAutoConfirmFrames * 20),
                 static_cast<unsigned>(CONFIG_VOICE_AUTO_START_MIN_AC_RMS),
                 static_cast<unsigned>(kAutoPrerollFrames * 20));
    } else ESP_LOGE(kTag, "Automatic input unavailable: PSRAM buffer allocation failed; restart required");
#endif
#if defined(CONFIG_VOICE_BARGE_IN_OBSERVATION_ENABLED) && CONFIG_VOICE_BARGE_IN_OBSERVATION_ENABLED
    barge_in_storage_ = static_cast<voiceaudio::BargeInObserver::Frame *>(heap_caps_calloc(
        kBargeInBufferFrames, sizeof(voiceaudio::BargeInObserver::Frame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (barge_in_storage_) {
        for (size_t i = 0; i < kBargeInBufferFrames; ++i)
            new (barge_in_storage_ + i) voiceaudio::BargeInObserver::Frame;
        barge_in_.configure(barge_in_storage_, kBargeInBufferFrames, kAutoPrerollFrames,
                            kBargeInConfirmFrames, kBargeInHoldFrames, CONFIG_VOICE_BARGE_IN_MIN_AC_RMS);
        ESP_LOGI(kTag, "Candidate observation buffer ready: cache=%u bytes confirm=%u ms min_ac_rms=%u",
                 static_cast<unsigned>(kBargeInBufferFrames * sizeof(voiceaudio::BargeInObserver::Frame)),
                 static_cast<unsigned>(kBargeInConfirmFrames * 20),
                 static_cast<unsigned>(CONFIG_VOICE_BARGE_IN_MIN_AC_RMS));
    } else ESP_LOGW(kTag, "D1 observation unavailable: PSRAM allocation failed; existing input paths retained");
#endif
    const esp_err_t callback_err = audio_.setFrontendOutputCallback(
        [this](const int16_t *pcm, size_t count, uint64_t offset, bool speech) {
            onFrontend(pcm, count, offset, speech);
        });
    if (callback_err != ESP_OK) return callback_err;
    const esp_err_t err = audio_.start(
        [this](const int16_t *pcm, size_t count) { onCapture(pcm, count); },
        [this](const voice::Identity &id) { onPlayed(id); },
        [this](esp_err_t result) { fail("音频设备异常，请检查连接", "audio_device", result); },
        CONFIG_VOICE_MIC_GAIN, CONFIG_VOICE_PLAYBACK_VOLUME);
    if (err != ESP_OK) return err;
    if (xTaskCreate(controlEntry, "voice_control", 6144, this, 5, nullptr) != pdPASS ||
        xTaskCreateWithCaps(uplinkEntry, "voice_uplink", 49152, this, 5, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(kTag, "Voice control ready: codec=%s requested_mode=%u; local controls and optional UART follow sdkconfig",
             opus_ ? "opus" : "pcm_s16le", static_cast<unsigned>(requested_mode_));
    return ESP_OK;
}

void ConversationController::command(char value)
{
    Command command{};
    switch (value) {
    case 'r': command.kind = CommandKind::Start; break;
    case 's': command.kind = CommandKind::End; break;
    case 'c': command.kind = CommandKind::Cancel; break;
    case 'n': command.kind = CommandKind::Reconnect; break;
    case 'a': command.kind = CommandKind::Auto; break;
    case 'm': command.kind = CommandKind::Manual; break;
    case 'b': command.kind = CommandKind::Observe; break;
    case 'i': command.kind = CommandKind::BargeIn; break;
    case '?': {
        const auto metrics = audio_.metrics();
        ESP_LOGI(kTag, "heap=%u internal=%u psram=%u capture_rms=%u peak=%u clipped=%u playback_frames=%u",
                 static_cast<unsigned>(esp_get_free_heap_size()),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(metrics.rms), static_cast<unsigned>(metrics.peak),
                 static_cast<unsigned>(metrics.clipped_samples), static_cast<unsigned>(metrics.playback_queue_frames));
        logUplinkMetrics("manual");
        client_.logDispatchDiagnostics();
        logFrontendMetrics();
        logBargeInObservation(true);
        audio_.logPlaybackReference();
        logSessionSelection();
        voice::memory::log("manual");
        return;
    }
    default: return;
    }
    if (xQueueSend(commands_, &command, 0) != pdTRUE) fail("控制队列已满，请重新连接", "command_queue_full");
}

void ConversationController::localAction()
{
    const Command command{CommandKind::LocalAction};
    ESP_LOGI(kTag, "button local_action enqueue commands=%p", commands_);
    if (commands_ && xQueueSend(commands_, &command, 0) != pdTRUE)
        fail("控制暂时繁忙，请稍后重试", "command_queue_full");
}

void ConversationController::retryConnection()
{
    const Command command{CommandKind::Reconnect};
    ESP_LOGI(kTag, "button reconnect enqueue commands=%p", commands_);
    if (commands_ && xQueueSend(commands_, &command, 0) != pdTRUE)
        fail("控制暂时繁忙，请稍后重试", "command_queue_full");
}

void ConversationController::scheduleRecovery()
{
    bool retryable;
    {
        std::lock_guard<std::mutex> lock(failure_mutex_);
        retryable = failure_retryable_;
    }
    ready_since_us_ = 0;
    if (!retryable || reconnect_retries_ >= kReconnectDelayUs.size()) {
        connect_since_ = -1;
        status_->setSession(VoiceSessionState::Failed);
        status_->setPipelineStage(retryable ? "多次重连失败，请尝试重启" : "设备或配置异常，请尝试重启");
        return;
    }
    next_connect_us_ = esp_timer_get_time() + kReconnectDelayUs[reconnect_retries_++];
    connect_since_ = 0;
    restore_mode_pending_ = true;
    status_->setSession(VoiceSessionState::Connecting);
    char message[96];
    std::snprintf(message, sizeof(message), "连接中断，正在自动重连（%u/%u）",
                  static_cast<unsigned>(reconnect_retries_), static_cast<unsigned>(kReconnectDelayUs.size()));
    status_->setPipelineStage(message);
    ESP_LOGW(kTag, "recovery scheduled retry=%u limit=%u wait_ms=%u",
             static_cast<unsigned>(reconnect_retries_), static_cast<unsigned>(kReconnectDelayUs.size()),
             static_cast<unsigned>(kReconnectDelayUs[reconnect_retries_ - 1] / 1000));
}

void ConversationController::restoreInteractionMode()
{
    if (!restore_mode_pending_ || failure_latched_.load() || resetting_.load() ||
        status_->session() != VoiceSessionState::Ready || cancel_pending_.load() ||
        response_cancel_pending_.load() || status_->capture() != VoiceCaptureState::Idle) return;
    const auto metrics = audio_.frontendMetrics();
    if ((requested_mode_ == InteractionMode::Automatic || requested_mode_ == InteractionMode::BargeIn) &&
        (!automatic_storage_ || !metrics.enabled || metrics.last_error != ESP_OK)) {
        restore_mode_pending_ = false;
        fail("本地语音检测不可用，请尝试重启", "audio_device",
             metrics.last_error != ESP_OK ? metrics.last_error : ESP_ERR_INVALID_STATE);
        return;
    }
    if (requested_mode_ != InteractionMode::Manual && metrics.enabled &&
        (!metrics.ready && metrics.last_error == ESP_OK)) return;
    restore_mode_pending_ = false;
    if (requested_mode_ == InteractionMode::Manual) return;
    if (requested_mode_ == InteractionMode::Observe) {
        setBargeInObservation(true);
        return;
    }
    setAutomatic(true);
    if (requested_mode_ == InteractionMode::BargeIn) {
        if (metrics.ready && metrics.aec_active && metrics.last_error == ESP_OK && barge_in_storage_) {
            setBargeIn(true);
        } else {
            status_->setBargeIn(VoiceBargeInState::Unavailable);
            status_->setBargeInEnabled(false);
            status_->setPipelineStage(automatic_enabled_.load() ?
                "自动监听已开启，插话暂不可用" : "本地语音检测异常，请尝试重启");
        }
    }
}

void ConversationController::controlEntry(void *arg) { static_cast<ConversationController *>(arg)->controlLoop(); }
void ConversationController::uplinkEntry(void *arg) { static_cast<ConversationController *>(arg)->uplinkLoop(); }

void ConversationController::controlLoop()
{
    bool opened = false;
    for (;;) {
        updateFrontendStatus();
        const uint8_t metrics_pending = metrics_pending_.exchange(0);
        if (metrics_pending) logUplinkMetrics(metrics_pending == 2 ? "end" : "periodic");
        if (failed_.exchange(false)) {
            resetConnection();
            opened = false;
            start_after_ready_ = false;
            idle_closed_ = false;
            scheduleRecovery();
        }
        if (idle_close_pending_.exchange(false)) {
            resetConnection();
            opened = false;
            if (failure_latched_.load()) { scheduleRecovery(); continue; }
            // RX 任务报告空闲时，r 请求可能已到达。
            // 清理期间保留该请求，但绝不保留上一轮音频。
            connect_since_ = 0;
            next_connect_us_ = start_after_ready_ ? 0 : esp_timer_get_time() + 1000000;
            restore_mode_pending_ = true;
            idle_closed_ = false;
            status_->setSession(VoiceSessionState::Offline);
            status_->setPipelineStage("待机会话已结束，正在重新连接");
        }
        if (!opened && connect_since_ >= 0 && wifi_->state() == WifiStation::State::Failed) {
            connect_since_ = -1;
            status_->setSession(VoiceSessionState::Failed);
            status_->setPipelineStage("WiFi 连接失败，请检查网络或尝试重启");
        }
        if (!opened && connect_since_ >= 0 && wifi_->state() == WifiStation::State::NotConfigured) {
            connect_since_ = -1;
            status_->setSession(VoiceSessionState::Failed);
            status_->setPipelineStage("尚未配置 WiFi，请先配置网络");
        }
        if (!opened && connect_since_ >= 0 && esp_timer_get_time() >= next_connect_us_ &&
            wifi_->state() == WifiStation::State::Connected) {
            bool hard_failure = false;
            {
                std::lock_guard<std::mutex> lock(failure_mutex_);
                // 安排恢复后仍可能出现本地故障。
                // 不能为了打开新连接而清除这个故障分类。
                hard_failure = failure_latched_.load() && !failure_retryable_;
                if (!hard_failure) {
                    failed_.store(false);
                    failure_latched_.store(false);
                    first_failure_stage_ = nullptr;
                    secondary_failures_ = 0;
                    failure_retryable_ = true;
                }
            }
            if (hard_failure) { scheduleRecovery(); continue; }
            connection_attempt_.fetch_add(1);
            idle_closing_.store(false);
            idle_closed_ = false;
            // 到达本次连接尝试前，resetConnection() 已隔离旧发送者。
            // 队列中旧输入的 epoch 检查不会通过。
            resetUplinkMetrics(0, 0);
            status_->setSession(VoiceSessionState::Connecting);
            if (!reconnect_retries_) status_->setPipelineStage("正在连接语音服务");
            connect_since_ = esp_timer_get_time();
            const esp_err_t err = client_.start(uri_.c_str(),
                [this](VoiceClient::Event e, const uint8_t *data, size_t size) { onNetwork(e, data, size); });
            opened = err == ESP_OK;
            if (!opened) fail("语音服务连接失败，正在尝试恢复", "connect_start", err);
        }
        if (opened && wifi_->state() != WifiStation::State::Connected)
            fail("WiFi 已断开，正在尝试恢复", "wifi_disconnected");
        if (opened && !epoch_.load() && esp_timer_get_time() - connect_since_ > 20000000)
            fail("语音服务连接超时，正在尝试恢复", "session_ready_timeout", ESP_ERR_TIMEOUT);
        if (status_->capture() == VoiceCaptureState::Starting && !barge_in_handoff_.load() &&
            esp_timer_get_time() - speech_wait_since_ > 30000000)
            fail("识别服务准备超时，正在尝试恢复", "speech_start_timeout", ESP_ERR_TIMEOUT);
        if (start_after_ready_ && !cancel_pending_.load() && !response_cancel_pending_.load() &&
            !idle_closing_.load() && status_->session() == VoiceSessionState::Ready) {
            start_after_ready_ = false;
            startSpeech();  // 开始新一轮，不重放之前的音频。
        }
        if (status_->session() == VoiceSessionState::Ready && !failure_latched_.load()) {
            if (!ready_since_us_) ready_since_us_ = esp_timer_get_time();
            else if (reconnect_retries_ && esp_timer_get_time() - ready_since_us_ >= kStableConnectionUs)
                reconnect_retries_ = 0;
        }
        Command command{};
        const bool have_command = xQueueReceive(commands_, &command,
            pdMS_TO_TICKS(automatic_enabled_.load() || automatic_turn_.load() ? 10 : 20)) == pdTRUE;
        if (have_command) switch (command.kind) {
        case CommandKind::Start:
            requested_mode_ = InteractionMode::Manual;
            restore_mode_pending_ = false;
            setBargeInObservation(false);
            setAutomatic(false);
            if (automatic_turn_.load() && status_->capture() != VoiceCaptureState::Idle) cancelSpeech();
            if (cancel_pending_.load() || response_cancel_pending_.load()) {
                start_after_ready_ = true;
                break;
            }
            if (idle_closed_ || idle_closing_.load()) {
                idle_closed_ = false;
                start_after_ready_ = true;
                if (!idle_close_pending_.load()) connect_since_ = 0;
                status_->setPipelineStage("正在重新连接，连接后开始新录音");
            } else startSpeech();
            break;
        case CommandKind::End: endSpeech(); break;
        case CommandKind::Cancel: {
            requested_mode_ = InteractionMode::Manual;
            restore_mode_pending_ = false;
            setBargeInObservation(false);
            const bool listening = automatic_enabled_.load();
            setAutomatic(false);
            if (status_->capture() != VoiceCaptureState::Idle) cancelSpeech();
            else if (!listening || status_->playback() != VoicePlaybackState::Idle ||
                     status_->response() == VoiceResponseState::Waiting ||
                     status_->response() == VoiceResponseState::Generating) interrupt();
            break;
        }
        case CommandKind::Auto:
            requested_mode_ = InteractionMode::Automatic;
            restore_mode_pending_ = true;
            setBargeInObservation(false);
            setAutomatic(true);
            restore_mode_pending_ = !automatic_enabled_.load();
            break;
        case CommandKind::BargeIn:
            requested_mode_ = barge_in_interrupt_enabled_.load() ? InteractionMode::Automatic : InteractionMode::BargeIn;
            restore_mode_pending_ = true;
            setBargeIn(!barge_in_interrupt_enabled_.load());
            restore_mode_pending_ = requested_mode_ == InteractionMode::BargeIn && !barge_in_interrupt_enabled_.load();
            break;
        case CommandKind::Observe: {
            bool enabled;
            {
                std::lock_guard<std::mutex> capture(capture_mutex_);
                enabled = barge_in_enabled_;
            }
            setBargeInObservation(!enabled);
            requested_mode_ = enabled ? (automatic_enabled_.load() ? InteractionMode::Automatic : InteractionMode::Manual) : InteractionMode::Observe;
            restore_mode_pending_ = !enabled;
            break;
        }
        case CommandKind::Manual:
            requested_mode_ = InteractionMode::Manual;
            restore_mode_pending_ = false;
            setBargeInObservation(false);
            setAutomatic(false);
            if (automatic_turn_.load() && status_->capture() != VoiceCaptureState::Idle) cancelSpeech();
            break;
        case CommandKind::Reconnect:
            {
                std::lock_guard<std::mutex> lock(failure_mutex_);
                if (failure_latched_.load() && !failure_retryable_) {
                    status_->setSession(VoiceSessionState::Failed);
                    status_->setPipelineStage("设备或配置异常，请尝试重启");
                    break;
                }
            }
            resetConnection();
            opened = false;
            start_after_ready_ = false;
            idle_closed_ = false;
            {
                std::lock_guard<std::mutex> lock(failure_mutex_);
                if (failure_latched_.load() && !failure_retryable_) {
                    connect_since_ = -1;
                    status_->setSession(VoiceSessionState::Failed);
                    status_->setPipelineStage("设备或配置异常，请尝试重启");
                    break;
                }
            }
            connect_since_ = 0;
            next_connect_us_ = 0;
            reconnect_retries_ = 0;
            ready_since_us_ = 0;
            restore_mode_pending_ = true;
            wifi_->retry();
            status_->setSession(VoiceSessionState::Offline);
            status_->setPipelineStage("正在重新连接");
            break;
        case CommandKind::LocalAction: {
            ESP_LOGI(kTag, "button local_action consumed session=%u failure=%u cancel=%u capture=%u playback=%u response=%u auto=%u mode=%u",
                     static_cast<unsigned>(status_->session()), static_cast<unsigned>(failure_latched_.load()),
                     static_cast<unsigned>(cancel_pending_.load()), static_cast<unsigned>(status_->capture()),
                     static_cast<unsigned>(status_->playback()), static_cast<unsigned>(status_->response()),
                     static_cast<unsigned>(automatic_enabled_.load()), static_cast<unsigned>(requested_mode_));
            if (failure_latched_.load() || status_->session() != VoiceSessionState::Ready) {
                break;
            }
            if (cancel_pending_.load() || response_cancel_pending_.load()) break;
            const bool busy = status_->capture() != VoiceCaptureState::Idle ||
                status_->playback() != VoicePlaybackState::Idle ||
                status_->response() == VoiceResponseState::Waiting ||
                status_->response() == VoiceResponseState::Generating;
            if (busy) {
                setBargeInObservation(false);
                if (status_->capture() != VoiceCaptureState::Idle || cancel_pending_.load()) cancelSpeech();
                else interrupt();
                restore_mode_pending_ = true;
                status_->setPipelineStage("本轮已取消，继续等待讲话");
            } else if (automatic_enabled_.load() || restore_mode_pending_) {
                requested_mode_ = InteractionMode::Manual;
                restore_mode_pending_ = false;
                setBargeInObservation(false);
                setAutomatic(false);
                status_->setPipelineStage("已暂停监听，按键1继续");
            } else {
#if defined(CONFIG_VOICE_BARGE_IN_ENABLED) && CONFIG_VOICE_BARGE_IN_ENABLED
                requested_mode_ = InteractionMode::BargeIn;
#else
                requested_mode_ = InteractionMode::Automatic;
#endif
                restore_mode_pending_ = true;
            }
            break;
        }
        case CommandKind::Played: {
            bool valid;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                valid = accepts(command.identity);
                if (valid) {
                    reply_revision_.fetch_add(1, std::memory_order_release);
                    clearSubtitleTimeline();
                    playback_done_ = true;
                    status_->setPlayback(VoicePlaybackState::Idle);
                    if (server_done_) status_->setResponse(VoiceResponseState::Complete);
                }
            }
            if (valid && !sendControl("playback.done", command.identity)) fail("播放确认发送失败", "playback_ack_send");
            break;
        }
        }

        // 先处理采样得到的本地命令，再推进自动 VAD 或创建新回合。
        // 因此，同一控制循环中即使有候选语音刚刚就绪，
        // 用户主动暂停或取消的命令也会优先生效。
        restoreInteractionMode();
        updateBargeInHandoff();
        updateBargeInObservation();
        updateAutomatic();
        updateSubtitleProgress();
    }
}

void ConversationController::updateFrontendStatus()
{
    const auto metrics = audio_.frontendMetrics();
    VoiceFrontendState value = VoiceFrontendState::Disabled;
    if (metrics.enabled) {
        value = metrics.last_error != ESP_OK ? VoiceFrontendState::Failed :
            !metrics.ready ? VoiceFrontendState::Starting :
            metrics.speech ? VoiceFrontendState::Speech : VoiceFrontendState::Silence;
    }
    status_->setFrontend(value);
}

void ConversationController::logFrontendMetrics()
{
    voiceaudio::AutomaticUtterance::StartMetrics onset;
    {
        std::lock_guard<std::mutex> capture(capture_mutex_);
        onset = automatic_.startMetrics();
    }
    const auto metrics = audio_.frontendMetrics();
    ESP_LOGI(kTag, "local_frontend mode=%s enabled=%u ready=%u speech=%u aec_requested=%u aec_active=%u "
             "error=%d feed_samples=%llu output_samples=%llu output_frames=%u feed_chunk=%u fetch_chunk=%u "
             "output_blocks=%u mic_rms=%u mic_peak=%u ref_rms=%u ref_peak=%u "
             "mic_mean=%d mic_ac_rms=%u ref_mean=%d ref_ac_rms=%u "
             "output_rms=%u output_peak=%u input_output_aligned=0 vad_transitions=%u queue=%u queue_peak=%u "
             "feed_total_us=%u feed_max_us=%u fetch_max_us=%u feed_stack_free=%u fetch_stack_free=%u internal=%u psram=%u",
             automatic_enabled_.load() || automatic_turn_.load() ? "auto_afe" : "manual_raw",
             static_cast<unsigned>(metrics.enabled), static_cast<unsigned>(metrics.ready),
             static_cast<unsigned>(metrics.speech), static_cast<unsigned>(metrics.aec_requested),
             static_cast<unsigned>(metrics.aec_active), static_cast<int>(metrics.last_error),
             static_cast<unsigned long long>(metrics.feed_samples),
             static_cast<unsigned long long>(metrics.output_samples),
             static_cast<unsigned>(metrics.output_frames), static_cast<unsigned>(metrics.feed_chunk_samples),
             static_cast<unsigned>(metrics.fetch_chunk_samples), static_cast<unsigned>(metrics.output_blocks),
             static_cast<unsigned>(metrics.microphone_rms), static_cast<unsigned>(metrics.microphone_peak),
             static_cast<unsigned>(metrics.reference_rms), static_cast<unsigned>(metrics.reference_peak),
             static_cast<int>(metrics.microphone_mean), static_cast<unsigned>(metrics.microphone_ac_rms),
             static_cast<int>(metrics.reference_mean), static_cast<unsigned>(metrics.reference_ac_rms),
             static_cast<unsigned>(metrics.output_rms), static_cast<unsigned>(metrics.output_peak),
             static_cast<unsigned>(metrics.vad_transitions), static_cast<unsigned>(metrics.queue_frames),
             static_cast<unsigned>(metrics.queue_peak), static_cast<unsigned>(metrics.feed_total_us),
             static_cast<unsigned>(metrics.feed_max_us),
             static_cast<unsigned>(metrics.fetch_max_us), static_cast<unsigned>(metrics.feed_stack_free),
             static_cast<unsigned>(metrics.fetch_stack_free),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    ESP_LOGI(kTag, "vad_input thresholds_valid=%u energy_threshold_cdbfs=%d probability_milli=%d "
             "valid=%u current_cdbfs=%d peak_cdbfs=%d frames=%u invalid_frames=%u "
             "window_ms=128 source=fetch_data_volume peak_scope=frontend_start",
             static_cast<unsigned>(metrics.vad_thresholds_valid),
             static_cast<int>(metrics.vad_energy_threshold_cdbfs),
             static_cast<int>(metrics.vad_probability_threshold_milli),
             static_cast<unsigned>(metrics.vad_input_energy_valid),
             static_cast<int>(metrics.vad_input_energy_cdbfs),
             static_cast<int>(metrics.vad_input_energy_peak_cdbfs),
             static_cast<unsigned>(metrics.vad_input_energy_frames),
             static_cast<unsigned>(metrics.vad_input_energy_invalid_frames));
    ESP_LOGI(kTag, "auto_start confirm_ms=%u min_ac_rms=%u candidates=%u confirmations=%u rejected=%u "
             "energy_rejections=%u pending=%u window_frames=%u energy_active_frames=%u energy_tail_active_frames=%u "
             "frame_ac_rms=%u window_ac_rms=%u candidate_sample=%llu confirmed_sample=%llu scope=controller_start",
             static_cast<unsigned>(onset.confirm_frames * 20), static_cast<unsigned>(onset.min_ac_rms),
             static_cast<unsigned>(onset.candidates), static_cast<unsigned>(onset.confirmations),
             static_cast<unsigned>(onset.rejected), static_cast<unsigned>(onset.energy_rejections),
             static_cast<unsigned>(onset.pending), static_cast<unsigned>(onset.window_frames),
             static_cast<unsigned>(onset.energy_active_frames), static_cast<unsigned>(onset.energy_tail_active_frames),
             static_cast<unsigned>(onset.frame_ac_rms), static_cast<unsigned>(onset.window_ac_rms),
             static_cast<unsigned long long>(onset.candidate_offset),
             static_cast<unsigned long long>(onset.confirmed_offset));
}

void ConversationController::observeUplinkQueue()
{
    const uint32_t depth = uplink_ ? static_cast<uint32_t>(uxQueueMessagesWaiting(uplink_)) : 0;
    uint32_t peak = uplink_queue_peak_.load(std::memory_order_relaxed);
    while (depth > peak && !uplink_queue_peak_.compare_exchange_weak(peak, depth, std::memory_order_relaxed)) {}
}

void ConversationController::resetUplinkMetrics(uint32_t epoch, uint32_t turn)
{
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    uplink_metrics_ = {};
    uplink_metrics_.epoch = epoch;
    uplink_metrics_.turn = turn;
    uplink_queue_peak_.store(0);
    metrics_pending_.store(0);
}

bool ConversationController::recordUplinkMetrics(const Input &input, uint32_t packets, uint64_t send_us,
                                                 uint32_t send_max_us, esp_err_t process_result,
                                                 esp_err_t send_result, uint32_t encode_us, uint32_t frame_us)
{
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    auto &metrics = uplink_metrics_;
    // 旧 End 的网络写入尚未返回时，新的 speech.start 可能已重置统计。
    // 必须在实际发布统计的位置再次核对身份，
    // 不能只依赖上行循环较早执行的 epoch 检查。
    if (metrics.epoch != input.epoch || metrics.turn != input.turn) return false;
    bool summary = input.kind == InputKind::End;
    metrics.packets += packets;
    metrics.send_us += send_us;
    metrics.send_max_us = std::max(metrics.send_max_us, send_max_us);
    metrics.process_result = process_result;
    metrics.send_result = send_result;
    if (input.kind == InputKind::Audio) {
        ++metrics.frames;
        metrics.encode_us += encode_us;
        metrics.frame_us += frame_us;
        metrics.encode_max_us = std::max(metrics.encode_max_us, encode_us);
        metrics.frame_max_us = std::max(metrics.frame_max_us, frame_us);
        summary = metrics.frames % 100 == 0;
    }
    if (summary || process_result != ESP_OK || input.kind == InputKind::Start)
        metrics.stack_free_min = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    // 发布与重置使用同一把锁，避免其他任务重置下一轮统计后，
    // 旧 End 又发布一份过期摘要。
    if (summary) metrics_pending_.store(input.kind == InputKind::End ? 2 : 1);
    return true;
}

void ConversationController::logUplinkMetrics(const char *trigger)
{
    UplinkMetrics metrics;
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics = uplink_metrics_;
    }
    const auto transport = client_.diagnostics();
    const uint32_t depth = uplink_ ? static_cast<uint32_t>(uxQueueMessagesWaiting(uplink_)) : 0;
    ESP_LOGI(kTag, "uplink trigger=%s attempt=%u epoch=%u turn=%u stream=%u active_epoch=%u active_turn=%u metrics_current=%u frames=%u packets=%u queue=%u queue_peak=%u encode_avg_us=%llu encode_max_us=%u send_avg_us=%llu send_max_us=%u frame_avg_us=%llu frame_max_us=%u process_result=%d send_result=%d stack_free_min=%u heap=%u heap_min=%u internal=%u psram=%u transport=%s close_code=%d",
             trigger, static_cast<unsigned>(connection_attempt_.load()),
             static_cast<unsigned>(metrics.epoch), static_cast<unsigned>(metrics.turn), static_cast<unsigned>(metrics.turn),
             static_cast<unsigned>(epoch_.load()), static_cast<unsigned>(turn_.load()),
             static_cast<unsigned>(metrics.epoch == epoch_.load() && metrics.turn == turn_.load()),
             static_cast<unsigned>(metrics.frames), static_cast<unsigned>(metrics.packets), static_cast<unsigned>(depth),
             static_cast<unsigned>(uplink_queue_peak_.load()),
             static_cast<unsigned long long>(metrics.frames ? metrics.encode_us / metrics.frames : 0),
             static_cast<unsigned>(metrics.encode_max_us),
             static_cast<unsigned long long>(metrics.packets ? metrics.send_us / metrics.packets : 0),
             static_cast<unsigned>(metrics.send_max_us),
             static_cast<unsigned long long>(metrics.frames ? metrics.frame_us / metrics.frames : 0),
             static_cast<unsigned>(metrics.frame_max_us), static_cast<int>(metrics.process_result),
             static_cast<int>(metrics.send_result), static_cast<unsigned>(metrics.stack_free_min),
             static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(esp_get_minimum_free_heap_size()),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), transport.reason, transport.close_code);
}

void ConversationController::logSessionSelection()
{
    const VoiceSessionSelection selection = status_->selection();
    const char *connection = "offline";
    switch (status_->session()) {
    case VoiceSessionState::Offline: break;
    case VoiceSessionState::Connecting: connection = "connecting"; break;
    case VoiceSessionState::Ready: connection = "ready"; break;
    case VoiceSessionState::Failed: connection = "failed"; break;
    }
    const char *interaction = status_->bargeInEnabled() ? "d2" :
        status_->bargeIn() != VoiceBargeInState::Disabled ? "d1" :
        status_->listening() ? "auto" : "manual";
    ESP_LOGI(kTag, "session_selection epoch=%u metadata_present=%u rejected_fields=%u profile=\"%s\" service_mode=\"%s\" require_streaming_asr=%s interaction=%s connection=%s",
             static_cast<unsigned>(selection.epoch), static_cast<unsigned>(selection.metadata_present),
             static_cast<unsigned>(selection.rejected_fields), selection.profile[0] ? selection.profile.data() : "not_provided",
             selection.mode[0] ? selection.mode.data() : "not_provided",
             selectionFlag(selection.require_streaming_asr), interaction, connection);
    logProviderSelection(selection.epoch, "asr", selection.asr);
    logProviderSelection(selection.epoch, "llm", selection.llm);
    logProviderSelection(selection.epoch, "tts", selection.tts);
}

void ConversationController::fail(const char *stage, const char *reason, esp_err_t result)
{
    // 该互斥锁独立于状态锁和采集锁，因为持有这些锁时也可能调用 fail()。
    // 先保存界面故障状态，再发布清理请求。
    std::lock_guard<std::mutex> lock(failure_mutex_);
    if (resetting_.load() && !failure_latched_.load()) {
        ESP_LOGW(kTag, "cleanup_failure reason=%s result=%d attempt=%u",
                 reason, static_cast<int>(result), static_cast<unsigned>(connection_attempt_.load()));
        return;
    }
    if (failure_latched_.exchange(true)) {
        // 保留最初的诊断原因；但后续硬件故障
        // 仍必须阻止已损坏会话继续自动恢复。
        if (!retryableFailure(reason, result) && failure_retryable_) {
            failure_retryable_ = false;
            // 即使先前传输故障已处理、controlLoop() 正在等待延迟重试，
            // 也要唤醒清理流程。
            failed_.store(true);
        }
        if (++secondary_failures_ <= 2) {
            ESP_LOGW(kTag, "secondary_failure reason=%s result=%d attempt=%u",
                     reason, static_cast<int>(result), static_cast<unsigned>(connection_attempt_.load()));
        }
        return;
    }
    first_failure_stage_ = stage;
    // 重建会话可恢复传输或服务商错误；
    // 本地设备故障及不合法的协商格式需要人工处理。
    failure_retryable_ = retryableFailure(reason, result);
    idle_close_pending_.store(false);  // 真实故障优先处理。
    idle_closing_.store(false);
    accepting_capture_.store(false);
    ESP_LOGE(kTag, "first_failure reason=%s at_us=%lld result=%d attempt=%u epoch=%u turn=%u stream=%u",
             reason, esp_timer_get_time(), static_cast<int>(result), static_cast<unsigned>(connection_attempt_.load()),
             static_cast<unsigned>(epoch_.load()), static_cast<unsigned>(turn_.load()), static_cast<unsigned>(turn_.load()));
    logUplinkMetrics("failure");
    voice::memory::log("conversation_failure");
    if (status_) {
        status_->clearSelection();
        status_->setPipelineStage(stage);
    }
    failed_.store(true);
}

bool ConversationController::accepts(const voice::Identity &id)
{
    return id.epoch == epoch_.load() && id.turn == turn_.load() && id.turn > sealed_turn_ &&
           id.generation != 0 && id == reply_;
}

void ConversationController::resetConnection()
{
    resetting_.store(true);
    setBargeInObservation(false);
    {
        std::lock_guard<std::mutex> capture(capture_mutex_);
        accepting_capture_.store(false);
        automatic_.disable();
        automatic_enabled_.store(false);
        automatic_turn_.store(false);
        automatic_buffer_failed_.store(false);
        cancel_pending_.store(false);
        response_cancel_pending_.store(false);
        barge_in_input_turn_.store(0);
        barge_in_first_uplink_us_ = 0;
        status_->setListening(false);
        // Start 也在同一互斥锁下启用采集；释放隔离锁前先使旧 epoch 失效，
        // 防止队列中的旧 Start 再次打开采集。
        std::lock_guard<std::mutex> lock(state_mutex_);
        reply_revision_.fetch_add(1, std::memory_order_release);
        clearSubtitleTimeline();
        sealed_turn_ = turn_.load();
        epoch_.store(0);
        status_->clearSelection();
        reply_ = {};
        response_cancel_target_ = {};
    }
    const esp_err_t stopped = audio_.stopPlayback();
    if (stopped != ESP_OK) {
        std::lock_guard<std::mutex> failure(failure_mutex_);
        failure_retryable_ = false;
        failure_latched_.store(true);
        if (!first_failure_stage_) first_failure_stage_ = "扬声器停止失败，请尝试重启";
        ESP_LOGE(kTag, "cleanup_failure reason=playback_stop result=%d", static_cast<int>(stopped));
    }
    {
        // 替换连接前先等旧发送结束，
        // 不能把过期 epoch 的数据发到新连接。
        std::lock_guard<std::mutex> send(uplink_operation_);
        client_.stop();
    }
    {
        // stop() 会等待 RX 分发任务结束。
        // 提前隔离时已在处理的 session.ready 也不能恢复旧 epoch。
        std::lock_guard<std::mutex> lock(state_mutex_);
        reply_revision_.fetch_add(1, std::memory_order_release);
        clearSubtitleTimeline();
        epoch_.store(0);
        turn_.store(0);
        sealed_turn_ = 0;
        reply_ = {};
        server_done_ = playback_done_ = false;
    }
    // 编码器归上行任务独占。队列任务携带旧 epoch，会被忽略；
    // 从另一个任务重置队列会与工作任务发生竞态。
    status_->setCapture(VoiceCaptureState::Idle);
    status_->setPlayback(VoicePlaybackState::Idle);
    status_->setResponse(VoiceResponseState::Idle);
    status_->clearSelection();
    {
        // RX 已退出，上行隔离已完成。
        // 等待在途处理器最后更新状态后，再恢复首个故障信息。
        std::lock_guard<std::mutex> lock(failure_mutex_);
        if (first_failure_stage_) status_->setPipelineStage(first_failure_stage_);
    }
    resetting_.store(false);
}

bool ConversationController::sendControl(const char *type, const voice::Identity &id)
{
    if (!id.epoch || id.epoch != epoch_.load()) return false;
    auto json = object();
    if (!json) return false;
    cJSON_AddStringToObject(json.get(), "type", type);
    addIdentity(json.get(), id);
    return sendJson(client_, json.get());
}

void ConversationController::interrupt()
{
    if (failure_latched_.load()) return;
    voice::Identity old;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        old = reply_;
        // 操作扬声器或发送网络请求前先封闭当前回合。
        reply_revision_.fetch_add(1, std::memory_order_release);
        clearSubtitleTimeline();
        sealed_turn_ = std::max(sealed_turn_, turn_.load());
        reply_ = {};
        server_done_ = false;
        playback_done_ = false;
    }
    const int64_t started = esp_timer_get_time();
    const esp_err_t err = audio_.stopPlayback();
    const int64_t fence_us = esp_timer_get_time() - started;
    status_->setPlayback(VoicePlaybackState::Idle);
    // 等待 response.started 时发生取消也要封闭本轮，
    // 即使此时尚不知道回答的 generation。
    status_->setResponse(VoiceResponseState::Cancelled);
    if (old.generation) {
        if (!sendControl("response.cancel", old)) fail("打断发送失败，请重新连接", "cancel_send");
    }
    if (err != ESP_OK) fail("扬声器停止失败，请检查连接", "playback_stop", err);
    ESP_LOGI(kTag, "Local playback fence and DMA reset: %lld us", fence_us);
}

void ConversationController::startSpeech(bool automatic)
{
    if (failure_latched_.load() || idle_closing_.load() || cancel_pending_.load() ||
        response_cancel_pending_.load()) return;
    if (status_->session() != VoiceSessionState::Ready || !epoch_.load()) return;
    if (status_->capture() != VoiceCaptureState::Idle) return;
    if (automatic) {
        std::lock_guard<std::mutex> capture(capture_mutex_);
        const auto phase = automatic_.phase();
        if (!automatic_enabled_.load() || automatic_buffer_failed_.load() ||
            (phase != voiceaudio::AutomaticUtterance::Phase::Opening &&
             phase != voiceaudio::AutomaticUtterance::Phase::Draining)) return;
    }
    if (!automatic) interrupt();
    if (failure_latched_.load()) return;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (turn_.load() == UINT32_MAX) { fail("回合已达上限，请重新连接", "turn_limit"); return; }
        reply_revision_.fetch_add(1, std::memory_order_release);
        clearSubtitleTimeline();
        turn_.fetch_add(1);
        resetUplinkMetrics(epoch_.load(), turn_.load());
        reply_ = {};
    }
    status_->setConversation("");
    status_->setSamples(0);
    status_->setCapture(VoiceCaptureState::Starting);
    speech_start_queued_.store(false);
    status_->setResponse(VoiceResponseState::Idle);
    automatic_turn_.store(automatic);
    if (barge_in_handoff_.load()) {
        barge_in_input_turn_.store(turn_.load());
        ESP_LOGI(kTag, "barge_in_handoff event=speech_start at_us=%lld epoch=%u turn=%u",
                 esp_timer_get_time(), static_cast<unsigned>(epoch_.load()), static_cast<unsigned>(turn_.load()));
    }
    status_->setPipelineStage(automatic ? "检测到讲话，正在准备识别" : "正在准备，显示录音后再说话");
    speech_wait_since_ = esp_timer_get_time();
    if (!sendControl("speech.start", {epoch_.load(), turn_.load(), 0})) fail("录音请求发送失败", "speech_start_send");
}

void ConversationController::endSpeech()
{
    if (failure_latched_.load()) return;
    std::lock_guard<std::mutex> capture(capture_mutex_);
    if (automatic_turn_.load()) {
        if (status_->capture() == VoiceCaptureState::Starting || accepting_capture_.load()) {
            automatic_.finishAfter(raw_samples_);
            if (!automatic_tail_since_) automatic_tail_since_ = esp_timer_get_time();
            status_->setPipelineStage("正在排空语音尾部");
        }
        return;
    }
    if (status_->capture() == VoiceCaptureState::Starting) {
        status_->setPipelineStage("正在准备识别，请稍候");
        return;
    }
    if (!accepting_capture_.exchange(false)) return;
    status_->setCapture(VoiceCaptureState::Finishing);
    const Input input{InputKind::End, epoch_.load(), turn_.load(), {}};
    if (xQueueSend(uplink_, &input, 0) != pdTRUE) fail("音频队列已满，请重新连接", "uplink_end_queue_full");
}

void ConversationController::onCapture(const int16_t *samples, size_t count)
{
    std::lock_guard<std::mutex> capture(capture_mutex_);
    raw_samples_ += count;
    if (automatic_turn_.load()) return;
    if (!accepting_capture_.load()) return;
    if (count != 320 || uxQueueSpacesAvailable(uplink_) <= 2) {
        accepting_capture_.store(false);
        observeUplinkQueue();
        fail("上行积压，请重新连接", count != 320 ? "capture_frame_size" : "uplink_backlog", ESP_ERR_INVALID_SIZE);
        return;
    }
    Input input{InputKind::Audio, epoch_.load(), turn_.load(), {}};
    std::copy_n(samples, count, input.pcm.begin());
    if (xQueueSend(uplink_, &input, 0) != pdTRUE) fail("上行队列已满，请重新连接", "uplink_queue_full");
    observeUplinkQueue();
    if (++captured_frames_ >= 1500) {
        accepting_capture_.store(false);
        status_->setCapture(VoiceCaptureState::Finishing);
        const Input end{InputKind::End, epoch_.load(), turn_.load(), {}};
        if (xQueueSend(uplink_, &end, 0) != pdTRUE) fail("音频结束队列已满", "uplink_end_queue_full");
    }
}

void ConversationController::setAutomatic(bool enabled)
{
    if (!enabled) {
        std::lock_guard<std::mutex> capture(capture_mutex_);
        automatic_enabled_.store(false);
        status_->setListening(false);
        if (!automatic_turn_.load()) automatic_.disable();
        return;
    }
    const auto metrics = audio_.frontendMetrics();
    if (!automatic_storage_ || !metrics.enabled || !metrics.ready || metrics.last_error != ESP_OK) {
        status_->setPipelineStage("本地语音检测未就绪，请稍候或尝试重启");
        return;
    }
    if (failure_latched_.load() || status_->session() != VoiceSessionState::Ready || cancel_pending_.load()) return;
    if (status_->capture() != VoiceCaptureState::Idle) return;
    std::lock_guard<std::mutex> capture(capture_mutex_);
    automatic_enabled_.store(true);
    status_->setListening(true);
    automatic_.disable(); // updateAutomatic 只在回答和播放结束后重新启用监听。
    status_->setPipelineStage("自动监听已开启，先安静片刻再说话");
}

void ConversationController::cancelSpeech()
{
    const voice::Identity id{epoch_.load(), turn_.load(), 0};
    if (!id.epoch || cancel_pending_.load()) return;
    {
        std::lock_guard<std::mutex> capture(capture_mutex_);
        accepting_capture_.store(false);
        automatic_.disable();
        automatic_turn_.store(false);
        std::lock_guard<std::mutex> state(state_mutex_);
        reply_revision_.fetch_add(1, std::memory_order_release);
        clearSubtitleTimeline();
        sealed_turn_ = std::max(sealed_turn_, id.turn);
        reply_ = {};
        cancel_pending_.store(true);
        status_->setCapture(VoiceCaptureState::Idle);
        status_->setResponse(VoiceResponseState::Cancelled);
        status_->setPipelineStage("正在取消本轮语音");
    }
    audio_.stopPlayback();
    status_->setPlayback(VoicePlaybackState::Idle);
    // 取消前先隔离写入者：本地封闭回合后，已排队的 PCM/End 不能再提交本轮。
    // 服务端也会取消仍在进行的 ASR 握手。
    std::lock_guard<std::mutex> send(uplink_operation_);
    if (!sendControl("speech.cancel", id)) fail("取消录音发送失败", "speech_cancel_send");
    speech_wait_since_ = esp_timer_get_time();
}

void ConversationController::onFrontend(const int16_t *samples, size_t count, uint64_t offset, bool speech)
{
    std::lock_guard<std::mutex> capture(capture_mutex_);
    if (failure_latched_.load() || resetting_.load()) return;
    // 回调只负责缓存；D2 交接、停止和网络操作归控制任务处理。
    // 普通回答状态锁不会阻塞这些音频帧。
    if (barge_in_enabled_) {
        const auto before = barge_in_.snapshot();
        if (before.phase != voiceaudio::BargeInObserver::Phase::Disabled &&
            before.phase != voiceaudio::BargeInObserver::Phase::Fault) {
            const auto metrics = audio_.frontendMetrics();
            const bool current = barge_in_reply_revision_ == reply_revision_.load(std::memory_order_acquire);
            if (!metrics.ready || !metrics.aec_active || metrics.last_error != ESP_OK) {
                barge_in_.fault();
                barge_in_reason_ = "aec_unavailable";
            } else if (!current || !audio_.isPlaying() || status_->capture() != VoiceCaptureState::Idle) {
                barge_in_.disable();
                barge_in_reason_ = "playback_boundary";
                ++barge_in_boundary_resets_;
            } else {
                barge_in_.append(samples, count, offset, speech);
                const auto after = barge_in_.snapshot();
                if (after.next_offset != before.next_offset) {
                    if (!barge_in_signal_.output_frames) barge_in_signal_.reply = barge_in_reply_;
                    ++barge_in_signal_.output_frames;
                    if (speech) ++barge_in_signal_.speech_frames;
                    barge_in_signal_.output_ac_peak = std::max(barge_in_signal_.output_ac_peak, after.frame_ac_rms);
                    barge_in_signal_.microphone_ac_peak = std::max(barge_in_signal_.microphone_ac_peak,
                                                                 metrics.microphone_ac_rms);
                    barge_in_signal_.microphone_peak = std::max(barge_in_signal_.microphone_peak,
                                                              metrics.microphone_peak);
                    const uint32_t vad_result = metrics.vad_input_energy_frames + metrics.vad_input_energy_invalid_frames;
                    if (vad_result != barge_in_signal_.last_vad_result) {
                        barge_in_signal_.last_vad_result = vad_result;
                        if (metrics.vad_input_energy_valid) {
                            if (!barge_in_signal_.vad_windows ||
                                metrics.vad_input_energy_cdbfs > barge_in_signal_.vad_energy_peak_cdbfs)
                                barge_in_signal_.vad_energy_peak_cdbfs = metrics.vad_input_energy_cdbfs;
                            ++barge_in_signal_.vad_windows;
                            if (metrics.vad_thresholds_valid &&
                                metrics.vad_input_energy_cdbfs >= metrics.vad_energy_threshold_cdbfs)
                                ++barge_in_signal_.vad_above_floor;
                        }
                    }
                }
                if (after.next_offset != before.next_offset) barge_in_last_frame_us_ = esp_timer_get_time();
                const auto energySnapshot = [&](int64_t at_us, uint64_t output_sample) {
                    BargeInEnergySnapshot value;
                    value.valid = true;
                    value.at_us = at_us;
                    value.output_sample = output_sample;
                    value.speech_frames = after.speech_frames;
                    value.reply = barge_in_reply_;
                    value.aec_active = metrics.aec_active;
                    value.microphone_rms = metrics.microphone_rms;
                    value.microphone_peak = metrics.microphone_peak;
                    value.microphone_mean = metrics.microphone_mean;
                    value.microphone_ac_rms = metrics.microphone_ac_rms;
                    value.reference_rms = metrics.reference_rms;
                    value.reference_peak = metrics.reference_peak;
                    value.reference_mean = metrics.reference_mean;
                    value.reference_ac_rms = metrics.reference_ac_rms;
                    value.output_rms = metrics.output_rms;
                    value.output_peak = metrics.output_peak;
                    value.frame_sample = after.last_frame_offset;
                    value.frame_mean = after.frame_mean;
                    value.frame_rms = after.frame_rms;
                    value.frame_ac_rms = after.frame_ac_rms;
                    value.frame_peak = after.frame_peak;
                    value.window_ac_rms = after.window_ac_rms;
                    value.window_frames = after.window_frames;
                    value.min_ac_rms = after.min_ac_rms;
                    value.energy_active_frames = after.energy_active_frames;
                    value.energy_tail_active_frames = after.energy_tail_active_frames;
                    value.frame_vad = after.frame_vad;
                    // 这里只按序号记录输入历史，不代表已校准的波形对齐。
                    // 包含候选语音之前的 3 个 AFE 块，便于观察延迟输入活动；
                    // 这些统计绝不能用作候选判定门槛。
                    constexpr uint64_t source_guard = 3 * 512;
                    const uint64_t first = after.candidate_offset > source_guard
                        ? after.candidate_offset - source_guard : 0;
                    value.source_window = audio_.frontendSourceWindow(first, after.next_offset);
                    return value;
                };
                if (after.candidates != before.candidates) {
                    barge_in_candidate_energy_ = energySnapshot(esp_timer_get_time(), after.candidate_offset);
                    barge_in_confirmed_energy_ = {};
                }
                if (after.confirmations != before.confirmations) {
                    barge_in_confirmed_us_ = esp_timer_get_time();
                    barge_in_confirmed_energy_ = energySnapshot(barge_in_confirmed_us_, after.confirmed_offset);
                }
                if (after.energy_rejections != before.energy_rejections)
                    barge_in_rejected_energy_ = energySnapshot(esp_timer_get_time(), after.next_offset);
                if (after.phase == voiceaudio::BargeInObserver::Phase::Fault)
                    barge_in_reason_ = after.overflows != before.overflows ? "cache_overflow" : "sample_discontinuity";
                // 复制期间可能并发执行了封闭或启动操作。
                // 丢弃旧范围，不重放音频，也不使用旧范围的结果。
                if (after.phase != voiceaudio::BargeInObserver::Phase::Fault &&
                    barge_in_reply_revision_ != reply_revision_.load(std::memory_order_acquire)) {
                    barge_in_.disable();
                    barge_in_reason_ = "playback_boundary";
                    ++barge_in_boundary_resets_;
                }
            }
            const auto after = barge_in_.snapshot();
            if (after.phase != before.phase || after.candidates != before.candidates ||
                after.confirmations != before.confirmations || after.rejected != before.rejected ||
                after.energy_rejections != before.energy_rejections ||
                after.timeouts != before.timeouts) barge_in_log_pending_ = true;
        }
    }
    // 收到 playback.done 时，AFE 可能仍留有扬声器播放期间的输出。
    // 只有恢复后采集的音频才能进入新一轮预录缓存和 VAD 判定。
    if (offset < automatic_resume_sample_) return;
    const auto result = automatic_.append(samples, count, offset, speech);
    if (barge_in_handoff_.load() && count == 320 && samples)
        barge_in_last_frame_us_ = esp_timer_get_time();
    if (result == voiceaudio::AutomaticUtterance::Result::Triggered)
        automatic_trigger_us_ = esp_timer_get_time();
    if (result == voiceaudio::AutomaticUtterance::Result::Overflow ||
        result == voiceaudio::AutomaticUtterance::Result::Discontinuity) {
        automatic_buffer_failed_.store(true);
        automatic_.disable();
    }
}

void ConversationController::setBargeIn(bool enabled)
{
    if (!enabled) { setBargeInObservation(false); return; }
#if defined(CONFIG_VOICE_BARGE_IN_ENABLED) && CONFIG_VOICE_BARGE_IN_ENABLED
    // i 显式启用空闲监听与播放插话；
    // a 仍为半双工，b 仍只做观察。
    setBargeInObservation(true);
    {
        std::lock_guard<std::mutex> capture(capture_mutex_);
        if (!barge_in_enabled_ || !automatic_storage_) return;
        automatic_.disable();
        automatic_buffer_failed_.store(false);
        automatic_enabled_.store(true);
        status_->setListening(true);
        barge_in_interrupt_enabled_.store(true);
        barge_in_.setHandoffEnabled(true);
        status_->setBargeInEnabled(true);
        barge_in_reason_ = "interruption_armed";
    }
    status_->setPipelineStage("自动插话已开启，播报中可讲话");
#else
    status_->setPipelineStage("当前固件未启用自动插话");
#endif
}

void ConversationController::beginBargeInHandoff()
{
    voice::Identity old;
    voiceaudio::BargeInObserver::Snapshot candidate;
    int64_t confirmed_us;
    {
        std::lock_guard<std::mutex> capture(capture_mutex_);
        if (!barge_in_interrupt_enabled_.load() || barge_in_handoff_.load() ||
            response_cancel_pending_.load() || cancel_pending_.load() ||
            failure_latched_.load() || resetting_.load() || idle_closing_.load()) return;
        candidate = barge_in_.snapshot();
        if (candidate.phase != voiceaudio::BargeInObserver::Phase::Confirmed) return;
        const auto metrics = audio_.frontendMetrics();
        if (!metrics.ready || !metrics.aec_active || metrics.last_error != ESP_OK) return;
        // 普通状态锁竞争只推迟操作；不能仅因 RX 更新回答，
        // 就丢弃已经连续确认的语音。
        std::unique_lock<std::mutex> state(state_mutex_, std::try_to_lock);
        if (!state.owns_lock()) return;
        if (barge_in_reply_revision_ != reply_revision_.load(std::memory_order_acquire) ||
            !accepts(barge_in_reply_) || !audio_.isPlaying() || audio_failed_ || playback_done_ ||
            status_->capture() != VoiceCaptureState::Idle) return;
        if (!barge_in_.handoff(automatic_)) {
            barge_in_.fault();
            barge_in_reason_ = "handoff_cache";
            barge_in_log_pending_ = true;
            return;
        }
        automatic_resume_sample_ = automatic_.nextOffset();
        automatic_buffer_failed_.store(false);
        automatic_tail_since_ = 0;
        automatic_trigger_us_ = barge_in_confirmed_us_;
        confirmed_us = automatic_trigger_us_;
        barge_in_first_uplink_us_ = 0;
        barge_in_first_uplink_logged_ = false;
        barge_in_input_turn_.store(0);
        barge_in_handoff_.store(true);
        automatic_turn_.store(true);  // 原始采集不能进入此交接路径。
        accepting_capture_.store(false);
        old = barge_in_reply_;
        // 本地停播前先封闭旧回答，即使取消发送或 ACK 失败也如此。
        reply_revision_.fetch_add(1, std::memory_order_release);
        clearSubtitleTimeline();
        sealed_turn_ = std::max(sealed_turn_, old.turn);
        reply_ = {};
        server_done_ = playback_done_ = false;
        response_cancel_target_ = old;
        response_cancel_since_us_ = confirmed_us;
        response_cancel_pending_.store(true);
        ++barge_in_interruptions_;
        barge_in_reason_ = "handoff";
        barge_in_log_pending_ = true;
        status_->setCapture(VoiceCaptureState::Starting);
        status_->setBargeIn(VoiceBargeInState::Cancelling);
    }
    const int64_t stopped_at = esp_timer_get_time();
    const esp_err_t err = audio_.stopPlayback();
    const int64_t fenced_at = esp_timer_get_time();
    status_->setPlayback(VoicePlaybackState::Idle);
    status_->setResponse(VoiceResponseState::Cancelled);
    status_->setPipelineStage("已停止播报，正在取消旧回复并保留讲话");
    ESP_LOGI(kTag, "barge_in_handoff event=confirmed at_us=%lld epoch=%u turn=%u generation=%u candidate_sample=%llu confirmed_sample=%llu buffered=%u",
             confirmed_us, static_cast<unsigned>(old.epoch), static_cast<unsigned>(old.turn),
             static_cast<unsigned>(old.generation), static_cast<unsigned long long>(candidate.candidate_offset),
             static_cast<unsigned long long>(candidate.confirmed_offset), static_cast<unsigned>(candidate.buffered_frames));
    ESP_LOGI(kTag, "barge_in_handoff event=local_fence at_us=%lld epoch=%u turn=%u fence_us=%lld since_confirm_us=%lld",
             fenced_at, static_cast<unsigned>(old.epoch), static_cast<unsigned>(old.turn),
             fenced_at - stopped_at, fenced_at - confirmed_us);
    if (err != ESP_OK) { fail("插话停止播放失败，请尝试重启", "barge_in_stop", err); return; }
    if (failure_latched_.load()) return;
    if (!sendControl("response.cancel", old)) fail("插话取消发送失败，正在尝试恢复", "barge_in_cancel_send");
}

void ConversationController::updateBargeInHandoff()
{
    const int64_t now = esp_timer_get_time();
    if (failure_latched_.load() || resetting_.load()) return;
    // 显式命令即使已丢弃 PCM，仍须等待旧取消流程结束。
    // 不匹配或重复的 ACK 不能解除等待。
    if (response_cancel_pending_.load() && now - response_cancel_since_us_ > kAutoHandshakeUs) {
        fail("插话取消确认超时，正在尝试恢复", "barge_in_cancel_timeout", ESP_ERR_TIMEOUT);
        return;
    }
    if (barge_in_handoff_.load()) {
        const auto metrics = audio_.frontendMetrics();
        bool broken, stalled, expired;
        {
            std::lock_guard<std::mutex> capture(capture_mutex_);
            broken = automatic_buffer_failed_.load() || !metrics.ready || !metrics.aec_active ||
                metrics.last_error != ESP_OK || automatic_.phase() == voiceaudio::AutomaticUtterance::Phase::Disabled;
            stalled = automatic_.phase() != voiceaudio::AutomaticUtterance::Phase::Draining &&
                now - barge_in_last_frame_us_ > 500000;
            expired = now - automatic_trigger_us_ > kAutoHandshakeUs;
        }
        if (broken || stalled || expired) {
            fail("插话语音缓存或前端异常，正在尝试恢复",
                 broken ? "barge_in_cache" : stalled ? "barge_in_frontend_stalled" : "barge_in_handshake_timeout");
            return;
        }
        if (!response_cancel_pending_.load() && !barge_in_input_turn_.load()) {
            // OnControl 只确认应答。
            // 本地隔离完成且收到对应旧回答的 ACK 后，由本任务发起新的云端请求。
            status_->setCapture(VoiceCaptureState::Idle);
            status_->setBargeIn(VoiceBargeInState::Starting);
            startSpeech(true);
        }
    }
    int64_t first_uplink = 0;
    {
        std::lock_guard<std::mutex> capture(capture_mutex_);
        if (barge_in_first_uplink_us_ && !barge_in_first_uplink_logged_) {
            first_uplink = barge_in_first_uplink_us_;
            barge_in_first_uplink_logged_ = true;
        }
    }
    if (first_uplink)
        ESP_LOGI(kTag, "barge_in_handoff event=first_uplink at_us=%lld epoch=%u turn=%u",
                 first_uplink, static_cast<unsigned>(epoch_.load()), static_cast<unsigned>(barge_in_input_turn_.load()));
}

void ConversationController::setBargeInObservation(bool enabled)
{
    const auto metrics = audio_.frontendMetrics();
    std::lock_guard<std::mutex> capture(capture_mutex_);
    barge_in_interrupt_enabled_.store(false);
    status_->setBargeInEnabled(false);
    barge_in_.setHandoffEnabled(false);
    if (barge_in_handoff_.exchange(false) && !barge_in_input_turn_.load()) {
        // 显式模式切换或取消命令会丢弃待处理的近端 PCM。
        // 但仍保留旧回答 ACK 屏障，直到应答到达或超时。
        automatic_.disable();
        automatic_turn_.store(false);
        accepting_capture_.store(false);
        automatic_buffer_failed_.store(false);
        status_->setCapture(VoiceCaptureState::Idle);
    }
    barge_in_.disable();
    barge_in_enabled_ = false;
    barge_in_reply_ = {};
    barge_in_last_frame_us_ = 0;
    barge_in_log_pending_ = true;
    barge_in_reason_ = "off";
    if (!enabled) {
        status_->setBargeIn(VoiceBargeInState::Disabled);
        return;
    }
    if (!barge_in_storage_ || !metrics.enabled || !metrics.ready || !metrics.aec_requested ||
        !metrics.aec_active || metrics.last_error != ESP_OK || failure_latched_.load() ||
        resetting_.load() || cancel_pending_.load() || response_cancel_pending_.load() || idle_closing_.load() ||
        status_->session() != VoiceSessionState::Ready || !epoch_.load() ||
        status_->capture() != VoiceCaptureState::Idle) {
        barge_in_reason_ = !barge_in_storage_ ? "not_compiled_or_no_psram" : "not_ready";
        status_->setBargeIn(VoiceBargeInState::Unavailable);
        status_->setPipelineStage("插话观察不可用，需专用固件与就绪AEC");
        return;
    }
    barge_in_enabled_ = true;
    barge_in_reason_ = "armed";
    status_->setBargeIn(VoiceBargeInState::Armed);
    status_->setPipelineStage("插话观察已开启，播报中检测但不打断");
}

void ConversationController::updateBargeInObservation()
{
    using Phase = voiceaudio::BargeInObserver::Phase;
    const int64_t now = esp_timer_get_time();
    bool log = false;
    bool handoff = false;
    {
        std::lock_guard<std::mutex> capture(capture_mutex_);
        if (barge_in_enabled_) {
            const auto metrics = audio_.frontendMetrics();
            auto phase = barge_in_.snapshot().phase;
            if (failure_latched_.load() || resetting_.load() || cancel_pending_.load() ||
                idle_closing_.load() || status_->session() != VoiceSessionState::Ready ||
                !metrics.ready || !metrics.aec_active || metrics.last_error != ESP_OK ||
                phase == Phase::Fault) {
                barge_in_enabled_ = false;
                barge_in_interrupt_enabled_.store(false);
                status_->setBargeInEnabled(false);
                if (phase != Phase::Fault) {
                    barge_in_.fault();
                    // 健康前端也可能因待处理的语音取消或会话切换而被停用。
                    // 现场日志要区分这一原因
                    // 与 DSP 或参考音频故障。
                    barge_in_reason_ = !metrics.ready || !metrics.aec_active || metrics.last_error != ESP_OK
                        ? "aec_unavailable" : failure_latched_.load() ? "session_failure"
                        : resetting_.load() ? "session_reset" : cancel_pending_.load() ? "speech_cancel_pending"
                        : idle_closing_.load() ? "idle_closing" : "session_not_ready";
                }
                status_->setBargeIn(VoiceBargeInState::Unavailable);
                barge_in_log_pending_ = true;
            } else if (barge_in_handoff_.load()) {
                status_->setBargeIn(response_cancel_pending_.load() ?
                    VoiceBargeInState::Cancelling : VoiceBargeInState::Starting);
            } else {
                if (phase != Phase::Disabled &&
                    (barge_in_reply_revision_ != reply_revision_.load(std::memory_order_acquire) ||
                     !audio_.isPlaying() || status_->capture() != VoiceCaptureState::Idle)) {
                    barge_in_.disable();
                    phase = Phase::Disabled;
                    ++barge_in_boundary_resets_;
                    barge_in_reason_ = "playback_boundary";
                    barge_in_log_pending_ = true;
                }
                // 即使 RX 持有 state_mutex_，也要检查真实输出是否断供。
                if (phase != Phase::Disabled && now - barge_in_last_frame_us_ > 500000) {
                    barge_in_.fault();
                    barge_in_enabled_ = false;
                    barge_in_interrupt_enabled_.store(false);
                    status_->setBargeInEnabled(false);
                    barge_in_reason_ = "frontend_stalled";
                    barge_in_log_pending_ = true;
                    status_->setBargeIn(VoiceBargeInState::Unavailable);
                } else if (phase == Phase::Disabled) {
                    status_->setBargeIn(VoiceBargeInState::Armed);
                    // 只有激活阶段需要一致的回答身份。普通数据包或文本更新
                    // 最多让激活推迟一个节拍；
                    // 已经激活的范围无需此锁就能逐帧消费。
                    std::unique_lock<std::mutex> state(state_mutex_, std::try_to_lock);
                    if (state.owns_lock() && accepts(reply_) && !audio_failed_ && !playback_done_ &&
                        status_->response() != VoiceResponseState::Failed &&
                        audio_.isPlaying() && status_->capture() == VoiceCaptureState::Idle) {
                        barge_in_.enable(raw_samples_);
                        barge_in_reply_ = reply_;
                        barge_in_reply_revision_ = reply_revision_.load(std::memory_order_acquire);
                        barge_in_candidate_energy_ = {};
                        barge_in_confirmed_energy_ = {};
                        barge_in_rejected_energy_ = {};
                        barge_in_signal_ = {};
                        barge_in_last_frame_us_ = now;
                        barge_in_reason_ = "observing";
                        barge_in_log_pending_ = true;
                    }
                } else {
                    status_->setBargeIn(phase == Phase::Candidate ? VoiceBargeInState::Candidate :
                        phase == Phase::Confirmed ? VoiceBargeInState::Confirmed :
                        phase == Phase::Cooldown ? VoiceBargeInState::Cooldown : VoiceBargeInState::Armed);
                    handoff = barge_in_interrupt_enabled_.load() && phase == Phase::Confirmed;
                }
            }
        }
        // 自动串口报告需要限频，打印时不得持有采集隔离锁。
        // 累计计数仍会保留短暂状态变化。
        log = barge_in_log_pending_ && now - barge_in_last_log_us_ >= 500000;
    }
    if (handoff) beginBargeInHandoff();
    if (log) logBargeInObservation();
}

void ConversationController::logBargeInObservation(bool replay_events)
{
    voiceaudio::BargeInObserver::Snapshot snapshot;
    voice::Identity id;
    bool enabled;
    const char *reason;
    uint32_t revision, boundary_resets, interruptions;
    bool interrupting, uploaded;
    BargeInEnergySnapshot candidate_energy, confirmed_energy, rejected_energy;
    BargeInSignalStats signal;
    {
        std::lock_guard<std::mutex> capture(capture_mutex_);
        snapshot = barge_in_.snapshot();
        id = barge_in_reply_;
        enabled = barge_in_enabled_;
        reason = barge_in_reason_;
        revision = barge_in_reply_revision_;
        boundary_resets = barge_in_boundary_resets_;
        interrupting = barge_in_interrupt_enabled_.load();
        interruptions = barge_in_interruptions_;
        uploaded = barge_in_first_uplink_us_ != 0;
        signal = barge_in_signal_;
        // 在采集隔离锁内选取并标记事件，串口输出放在锁外。
        // 显式统计可重放最近证据；自动阶段和冷却日志
        // 只打印新观察到的事件身份。
        const auto selectEnergy = [replay_events](const BargeInEnergySnapshot &energy,
                                                 BargeInEnergyLogMarker &logged,
                                                 BargeInEnergySnapshot &selected) {
            if (!energy.valid || (!replay_events && logged.valid &&
                logged.at_us == energy.at_us && logged.reply == energy.reply)) return;
            selected = energy;
            logged = {true, energy.at_us, energy.reply};
        };
        selectEnergy(barge_in_candidate_energy_, barge_in_candidate_logged_, candidate_energy);
        selectEnergy(barge_in_confirmed_energy_, barge_in_confirmed_logged_, confirmed_energy);
        selectEnergy(barge_in_rejected_energy_, barge_in_rejected_logged_, rejected_energy);
        barge_in_last_log_us_ = esp_timer_get_time();
        barge_in_log_pending_ = false;
    }
    if (replay_events && signal.output_frames) {
        ESP_LOGI(kTag, "barge_in_signal epoch=%u turn=%u generation=%u output_frames=%u speech_frames=%u "
                 "output_ac_peak=%u mic_ac_snapshot_peak=%u mic_snapshot_peak=%u "
                 "vad_windows=%u vad_above_floor=%u vad_peak_cdbfs=%d "
                 "vad_source=fetch_data_volume cross_stage_sample_aligned=0 scope=reply_observation",
                 static_cast<unsigned>(signal.reply.epoch), static_cast<unsigned>(signal.reply.turn),
                 static_cast<unsigned>(signal.reply.generation),
                 static_cast<unsigned>(signal.output_frames), static_cast<unsigned>(signal.speech_frames),
                 static_cast<unsigned>(signal.output_ac_peak), static_cast<unsigned>(signal.microphone_ac_peak),
                 static_cast<unsigned>(signal.microphone_peak), static_cast<unsigned>(signal.vad_windows),
                 static_cast<unsigned>(signal.vad_above_floor), static_cast<int>(signal.vad_energy_peak_cdbfs));
    }
    ESP_LOGI(kTag, "barge_in_observation enabled=%u phase=%s reason=%s epoch=%u turn=%u generation=%u "
             "candidates=%u confirmations=%u rejected=%u energy_rejections=%u min_ac_rms=%u timeouts=%u overflows=%u discontinuities=%u "
             "buffered=%u peak=%u candidate_sample=%llu confirmed_sample=%llu oldest_sample=%llu next_sample=%llu "
             "reply_revision=%u boundary_resets=%u upload=%u interrupt=%u mode=%s handoffs=%u",
             static_cast<unsigned>(enabled), bargeInPhase(snapshot.phase), reason,
             static_cast<unsigned>(id.epoch), static_cast<unsigned>(id.turn), static_cast<unsigned>(id.generation),
             static_cast<unsigned>(snapshot.candidates), static_cast<unsigned>(snapshot.confirmations),
             static_cast<unsigned>(snapshot.rejected), static_cast<unsigned>(snapshot.energy_rejections),
             static_cast<unsigned>(snapshot.min_ac_rms), static_cast<unsigned>(snapshot.timeouts),
             static_cast<unsigned>(snapshot.overflows), static_cast<unsigned>(snapshot.discontinuities),
             static_cast<unsigned>(snapshot.buffered_frames), static_cast<unsigned>(snapshot.buffered_peak),
             static_cast<unsigned long long>(snapshot.candidate_offset),
             static_cast<unsigned long long>(snapshot.confirmed_offset),
             static_cast<unsigned long long>(snapshot.oldest_offset), static_cast<unsigned long long>(snapshot.next_offset),
             static_cast<unsigned>(revision), static_cast<unsigned>(boundary_resets),
             static_cast<unsigned>(uploaded), static_cast<unsigned>(interrupting),
             interrupting ? "interrupt" : "observe", static_cast<unsigned>(interruptions));
    const auto logEnergy = [](const char *event, const BargeInEnergySnapshot &energy) {
        if (!energy.valid) return;
        ESP_LOGI(kTag, "barge_in_energy event=%s at_us=%lld epoch=%u turn=%u generation=%u "
                 "output_sample=%llu speech_frames=%u aec_active=%u "
                 "mic_rms=%u mic_peak=%u ref_rms=%u ref_peak=%u out_rms=%u out_peak=%u "
                 "mic_mean=%d mic_ac_rms=%u ref_mean=%d ref_ac_rms=%u "
                 "frame_sample=%llu frame_mean=%d frame_rms=%u frame_ac_rms=%u frame_peak=%u "
                 "window_ac_rms=%u window_frames=%u min_ac_rms=%u energy_active_frames=%u "
                 "energy_tail_active_frames=%u frame_vad=%u frame_stats_aligned=1 "
                 "mic_ref_output_sample_aligned=0",
                 event, static_cast<long long>(energy.at_us),
                 static_cast<unsigned>(energy.reply.epoch), static_cast<unsigned>(energy.reply.turn),
                 static_cast<unsigned>(energy.reply.generation),
                 static_cast<unsigned long long>(energy.output_sample),
                 static_cast<unsigned>(energy.speech_frames), static_cast<unsigned>(energy.aec_active),
                 static_cast<unsigned>(energy.microphone_rms), static_cast<unsigned>(energy.microphone_peak),
                 static_cast<unsigned>(energy.reference_rms), static_cast<unsigned>(energy.reference_peak),
                 static_cast<unsigned>(energy.output_rms), static_cast<unsigned>(energy.output_peak),
                 static_cast<int>(energy.microphone_mean), static_cast<unsigned>(energy.microphone_ac_rms),
                 static_cast<int>(energy.reference_mean), static_cast<unsigned>(energy.reference_ac_rms),
                 static_cast<unsigned long long>(energy.frame_sample), static_cast<int>(energy.frame_mean),
                 static_cast<unsigned>(energy.frame_rms), static_cast<unsigned>(energy.frame_ac_rms),
                 static_cast<unsigned>(energy.frame_peak), static_cast<unsigned>(energy.window_ac_rms),
                 static_cast<unsigned>(energy.window_frames), static_cast<unsigned>(energy.min_ac_rms),
                 static_cast<unsigned>(energy.energy_active_frames),
                 static_cast<unsigned>(energy.energy_tail_active_frames),
                 static_cast<unsigned>(energy.frame_vad));
        const auto &source = energy.source_window;
        ESP_LOGI(kTag, "barge_in_source event=%s at_us=%lld epoch=%u turn=%u generation=%u "
                 "valid=%u requested_first=%llu requested_end=%llu covered_first=%llu covered_end=%llu "
                 "available_first=%llu available_end=%llu frames=%u samples=%u "
                 "raw_mean=%d raw_rms=%u raw_ac=%u raw_peak=%u raw_frame_ac_max=%u "
                 "dc_mean=%d dc_rms=%u dc_ac=%u dc_peak=%u dc_frame_ac_max=%u "
                 "ref_mean=%d ref_rms=%u ref_ac=%u ref_peak=%u ref_frame_ac_max=%u "
                 "guard_samples=1536 ordinal_mapping=%u lag_calibrated=%u waveform_aligned=0 diagnostic_only=1",
                 event, static_cast<long long>(energy.at_us),
                 static_cast<unsigned>(energy.reply.epoch), static_cast<unsigned>(energy.reply.turn),
                 static_cast<unsigned>(energy.reply.generation), static_cast<unsigned>(source.valid),
                 static_cast<unsigned long long>(source.requested_first), static_cast<unsigned long long>(source.requested_end),
                 static_cast<unsigned long long>(source.covered_first), static_cast<unsigned long long>(source.covered_end),
                 static_cast<unsigned long long>(source.available_first), static_cast<unsigned long long>(source.available_end),
                 static_cast<unsigned>(source.frames), static_cast<unsigned>(source.samples),
                 static_cast<int>(source.raw.mean), static_cast<unsigned>(source.raw.rms),
                 static_cast<unsigned>(source.raw.ac_rms), static_cast<unsigned>(source.raw.peak),
                 static_cast<unsigned>(source.raw.max_frame_ac_rms), static_cast<int>(source.dc.mean),
                 static_cast<unsigned>(source.dc.rms), static_cast<unsigned>(source.dc.ac_rms),
                 static_cast<unsigned>(source.dc.peak), static_cast<unsigned>(source.dc.max_frame_ac_rms),
                 static_cast<int>(source.reference.mean), static_cast<unsigned>(source.reference.rms),
                 static_cast<unsigned>(source.reference.ac_rms), static_cast<unsigned>(source.reference.peak),
                 static_cast<unsigned>(source.reference.max_frame_ac_rms), static_cast<unsigned>(source.ordinal_mapping),
                 static_cast<unsigned>(source.lag_calibrated));
    };
    logEnergy("candidate", candidate_energy);
    logEnergy("confirmed", confirmed_energy);
    logEnergy("rejected_low_ac", rejected_energy);
}

void ConversationController::finishCaptureLocked()
{
    if (!accepting_capture_.exchange(false)) return;
    status_->setCapture(VoiceCaptureState::Finishing);
    const Input end{InputKind::End, epoch_.load(), turn_.load(), {}};
    if (xQueueSend(uplink_, &end, 0) != pdTRUE) fail("音频结束队列已满", "uplink_end_queue_full");
    automatic_.disable();
    automatic_tail_since_ = 0;
}

void ConversationController::updateAutomatic()
{
    if (cancel_pending_.load() && esp_timer_get_time() - speech_wait_since_ > 7000000) {
        fail("取消识别超时，正在尝试恢复", "speech_cancel_timeout", ESP_ERR_TIMEOUT);
        return;
    }
    if (!automatic_enabled_.load() && !automatic_turn_.load()) return;
    if (failure_latched_.load() || resetting_.load() || cancel_pending_.load()) return;
    const auto metrics = audio_.frontendMetrics();
    const bool broken = automatic_buffer_failed_.exchange(false) ||
        !metrics.ready || metrics.last_error != ESP_OK ||
        (barge_in_input_turn_.load() == turn_.load() && barge_in_input_turn_.load() != 0 &&
         automatic_turn_.load() && !metrics.aec_active);
    if (broken) {
        setAutomatic(false);
        if (automatic_turn_.load() && status_->capture() != VoiceCaptureState::Idle) cancelSpeech();
        status_->setPipelineStage("本地语音检测异常，请尝试重启");
        return;
    }
    bool trigger = false;
    bool expired = false;
    {
        std::lock_guard<std::mutex> capture(capture_mutex_);
        const bool busy = status_->capture() != VoiceCaptureState::Idle ||
            status_->playback() != VoicePlaybackState::Idle ||
            status_->response() == VoiceResponseState::Waiting ||
            status_->response() == VoiceResponseState::Generating;
        if (!busy && !response_cancel_pending_.load() && automatic_enabled_.load() && status_->session() == VoiceSessionState::Ready) {
            automatic_turn_.store(false);
            if (automatic_.phase() == voiceaudio::AutomaticUtterance::Phase::Disabled) {
                automatic_.listen();
                automatic_resume_sample_ = raw_samples_;
                automatic_tail_since_ = 0;
            }
            trigger = automatic_.phase() == voiceaudio::AutomaticUtterance::Phase::Opening ||
                automatic_.phase() == voiceaudio::AutomaticUtterance::Phase::Draining;
        }
        const int64_t now = esp_timer_get_time();
        if (automatic_turn_.load() && status_->capture() == VoiceCaptureState::Starting &&
            now - automatic_trigger_us_ > kAutoHandshakeUs) expired = true;
        if (automatic_tail_since_ && now - automatic_tail_since_ > 2000000 &&
            automatic_.phase() != voiceaudio::AutomaticUtterance::Phase::Draining) expired = true;
        if (automatic_turn_.load() && accepting_capture_.load() && now >= automatic_next_send_us_ &&
            uxQueueSpacesAvailable(uplink_) > 2) {
            voiceaudio::AutomaticUtterance::Frame frame;
            if (automatic_.pop(frame)) {
                Input input{InputKind::Audio, epoch_.load(), turn_.load(), frame.pcm};
                if (xQueueSend(uplink_, &input, 0) != pdTRUE) expired = true;
                ++captured_frames_;
                // 握手和预录缓存按真实时间发送，
                // 不能突发灌入服务端容量有限的 ASR 队列。
                automatic_next_send_us_ = std::max(automatic_next_send_us_ + 20000, now - 20000);
                observeUplinkQueue();
            }
            if (!automatic_.size() && automatic_.phase() == voiceaudio::AutomaticUtterance::Phase::Draining)
                finishCaptureLocked();
        }
    }
    if (expired) {
        setAutomatic(false);
        cancelSpeech();
        status_->setPipelineStage("本轮准备超时，已取消；按键1恢复监听");
    } else if (trigger) startSpeech(true);
}

void ConversationController::onPlayed(const voice::Identity &id)
{
    const Command command{CommandKind::Played, id};
    if (xQueueSend(commands_, &command, 0) != pdTRUE) fail("播放确认队列已满", "playback_ack_queue_full");
}

void ConversationController::uplinkLoop()
{
    voiceaudio::OpusEncoder encoder;
    uint32_t current_epoch = 0, current_turn = 0, sequence = 0;
    uint64_t samples = 0;
    uint64_t send_elapsed_us = 0;
    uint32_t send_max_us = 0, packet_count = 0;
    esp_err_t last_send_result = ESP_OK;
    std::array<uint8_t, voice::kHeaderBytes + voice::kMaxPayloadBytes> packet{};
    auto send = [&](const uint8_t *data, size_t bytes) -> esp_err_t {
        if (!current_epoch || current_epoch != epoch_.load() || current_turn != turn_.load())
            return ESP_ERR_INVALID_STATE;
        {
            std::lock_guard<std::mutex> state(state_mutex_);
            if (current_turn <= sealed_turn_) return ESP_ERR_INVALID_STATE;
        }
        const voice::PacketHeader header{1, {current_epoch, 0, 0}, current_turn, sequence,
                                          static_cast<uint64_t>(sequence) * 320};
        if (!voice::packHeader(header, packet.data(), packet.size()) || bytes > voice::kMaxPayloadBytes)
            return ESP_ERR_INVALID_SIZE;
        std::copy_n(data, bytes, packet.data() + voice::kHeaderBytes);
        const int64_t send_started = esp_timer_get_time();
        const esp_err_t err = client_.sendBinary(packet.data(), voice::kHeaderBytes + bytes,
                                                CONFIG_VOICE_UPLINK_SEND_TIMEOUT_MS);
        const auto elapsed = static_cast<uint32_t>(esp_timer_get_time() - send_started);
        send_elapsed_us += elapsed;
        send_max_us = std::max(send_max_us, elapsed);
        last_send_result = err;
        ++packet_count;
        if (err == ESP_OK) ++sequence;
        return err;
    };
    for (;;) {
        Input input{};
        if (xQueueReceive(uplink_, &input, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        std::lock_guard<std::mutex> operation(uplink_operation_);
        if (input.epoch != epoch_.load() || input.turn != turn_.load() || !input.epoch) continue;
        if (failure_latched_.load()) continue;
        {
            std::lock_guard<std::mutex> state(state_mutex_);
            if (input.turn <= sealed_turn_) continue;
        }
        esp_err_t err = ESP_OK;
        if (input.kind == InputKind::Start && input.turn == barge_in_input_turn_.load()) {
            bool expired;
            {
                std::lock_guard<std::mutex> capture(capture_mutex_);
                expired = esp_timer_get_time() - automatic_trigger_us_ > kAutoHandshakeUs;
            }
            if (expired) {
                fail("插话识别准备超时，正在尝试恢复", "barge_in_handshake_timeout", ESP_ERR_TIMEOUT);
                continue;
            }
        }
        const int64_t frame_started = esp_timer_get_time();
        uint32_t encode_elapsed_us = 0;
        send_elapsed_us = 0;
        send_max_us = packet_count = 0;
        last_send_result = ESP_OK;
        if (input.kind == InputKind::Start) {
            encoder.close();
            current_epoch = input.epoch;
            current_turn = input.turn;
            sequence = 0;
            samples = 0;
            resetUplinkMetrics(current_epoch, current_turn);
            if (opus_) err = encoder.open();
            auto json = object();
            if (!json) err = ESP_ERR_NO_MEM;
            if (err == ESP_OK) {
                cJSON_AddStringToObject(json.get(), "type", "audio.stream.start");
                cJSON_AddNumberToObject(json.get(), "session_epoch", current_epoch);
                cJSON_AddNumberToObject(json.get(), "stream_id", current_turn);
                cJSON_AddStringToObject(json.get(), "codec", opus_ ? "opus" : "pcm_s16le");
                cJSON_AddNumberToObject(json.get(), "sample_rate_hz", 16000);
                cJSON_AddNumberToObject(json.get(), "channels", 1);
                cJSON_AddNumberToObject(json.get(), "frame_duration_ms", 20);
                cJSON_AddNumberToObject(json.get(), "pre_skip_samples", opus_ ? encoder.preSkipSamples() : 0);
                if (!sendJson(client_, json.get())) err = ESP_FAIL;
            }
            if (err == ESP_OK) {
                std::lock_guard<std::mutex> capture(capture_mutex_);
                std::lock_guard<std::mutex> state(state_mutex_);
                if (current_epoch == epoch_.load() && current_turn == turn_.load() &&
                    current_turn > sealed_turn_ && !cancel_pending_.load() && !failure_latched_.load()) {
                    if (current_turn == barge_in_input_turn_.load() &&
                        esp_timer_get_time() - automatic_trigger_us_ > kAutoHandshakeUs) {
                        err = ESP_ERR_TIMEOUT;
                    } else {
                        accepting_capture_.store(true);
                        captured_frames_ = 0;
                        if (automatic_turn_.load()) {
                            automatic_.started();
                            automatic_next_send_us_ = esp_timer_get_time();
                        }
                        if (current_turn == barge_in_input_turn_.load()) barge_in_handoff_.store(false);
                        status_->setCapture(VoiceCaptureState::Recording);
                        status_->setPipelineStage(automatic_turn_.load() ? "正在讲话，说完后自动提交" : "正在录音");
                    }
                }
            }
        } else if (input.kind == InputKind::Audio && current_epoch == input.epoch && current_turn == input.turn) {
            const int64_t encode_started = esp_timer_get_time();
            err = opus_ ? encoder.feed(input.pcm.data(), input.pcm.size(), send) :
                          send(reinterpret_cast<const uint8_t *>(input.pcm.data()), 640);
            const auto process_us = static_cast<uint64_t>(esp_timer_get_time() - encode_started);
            encode_elapsed_us = opus_ && process_us > send_elapsed_us ? static_cast<uint32_t>(process_us - send_elapsed_us) : 0;
            samples += input.pcm.size();
            status_->setSamples(static_cast<uint32_t>(samples));
        } else if (input.kind == InputKind::End && current_epoch == input.epoch && current_turn == input.turn) {
            if (opus_) err = encoder.finish(send);
            bool sealed;
            {
                std::lock_guard<std::mutex> state(state_mutex_);
                sealed = current_turn <= sealed_turn_;
            }
            if (sealed) { encoder.close(); current_epoch = 0; continue; }
            if (err == ESP_OK) {
                auto json = object();
                if (!json) err = ESP_ERR_NO_MEM;
                else {
                    cJSON_AddStringToObject(json.get(), "type", "audio.stream.end");
                    cJSON_AddNumberToObject(json.get(), "session_epoch", current_epoch);
                    cJSON_AddNumberToObject(json.get(), "stream_id", current_turn);
                    cJSON_AddNumberToObject(json.get(), "valid_end_sample", samples + (opus_ ? encoder.preSkipSamples() : 0));
                    if (!sendJson(client_, json.get())) err = ESP_FAIL;
                    {
                        std::lock_guard<std::mutex> state(state_mutex_);
                        sealed = current_turn <= sealed_turn_;
                    }
                    if (!sealed && err == ESP_OK &&
                        !sendControl("speech.end", {current_epoch, current_turn, 0})) err = ESP_FAIL;
                }
            }
            encoder.close();
            current_epoch = 0;
        }
        observeUplinkQueue();
        const auto elapsed = static_cast<uint32_t>(esp_timer_get_time() - frame_started);
        // 115200 波特率下串口输出可能耗时数十毫秒。
        // 让控制任务在编码和网络发送隔离锁之外打印摘要。
        recordUplinkMetrics(input, packet_count, send_elapsed_us, send_max_us,
                            err, last_send_result, encode_elapsed_us, elapsed);
        if (input.kind == InputKind::Audio && packet_count && err == ESP_OK &&
            input.turn == barge_in_input_turn_.load()) {
            std::lock_guard<std::mutex> capture(capture_mutex_);
            if (!barge_in_first_uplink_us_) barge_in_first_uplink_us_ = esp_timer_get_time();
        }
        if (err != ESP_OK) {
            {
                std::lock_guard<std::mutex> state(state_mutex_);
                if (input.turn <= sealed_turn_) continue;
            }
            const char *reason = last_send_result != ESP_OK ? "uplink_send" :
                input.kind == InputKind::Start ? "uplink_start" :
                input.kind == InputKind::End ? "uplink_end" : "uplink_encode";
            fail("上行语音发送失败，正在尝试恢复", reason, err);
        }
    }
}

void ConversationController::onNetwork(VoiceClient::Event event, const uint8_t *data, size_t size)
{
    if (idle_closing_.load()) return;  // session.closed 后的预期关闭与清理。
    if ((failure_latched_.load() || resetting_.load()) &&
        event != VoiceClient::Event::Disconnected && event != VoiceClient::Event::Failure)
        return;
    switch (event) {
    case VoiceClient::Event::Connected: {
        auto json = object();
        if (!json) { fail("会话内存不足", "session_no_memory", ESP_ERR_NO_MEM); return; }
        cJSON_AddStringToObject(json.get(), "type", "session.start");
        cJSON_AddNumberToObject(json.get(), "version", 1);
        cJSON_AddStringToObject(json.get(), "profile", CONFIG_VOICE_SERVER_PROFILE);
        cJSON_AddStringToObject(json.get(), "uplink_codec", opus_ ? "opus" : "pcm_s16le");
        cJSON_AddStringToObject(json.get(), "downlink_codec", opus_ ? "opus" : "pcm_s16le");
        if (!sendJson(client_, json.get())) fail("会话建立失败", "session_start_send");
        break;
    }
    case VoiceClient::Event::Text: onControl(data, size); break;
    case VoiceClient::Event::Binary: onBinary(data, size); break;
    case VoiceClient::Event::Disconnected:
        fail("语音连接已断开，正在尝试恢复", "network_disconnected", ESP_ERR_INVALID_STATE); break;
    case VoiceClient::Event::Failure:
        fail("语音连接已断开，正在尝试恢复", "network_failure"); break;
    }
}

void ConversationController::onBinary(const uint8_t *data, size_t bytes)
{
    voice::PacketHeader header{};
    if (!voice::unpackHeader(data, bytes, header) || header.kind != 2) {
        fail("下行音频协议错误", "downlink_header"); return;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!accepts(header.identity) || audio_failed_) return;  // 在解码或入队之前完成检查。
    }
    const esp_err_t err = audio_.feedPlayback(header.identity, header.stream_id,
        header.sequence, header.sample_offset, data + voice::kHeaderBytes, bytes - voice::kHeaderBytes);
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!accepts(header.identity)) return;
    if (err == ESP_OK) status_->setPlayback(VoicePlaybackState::Playing);
    else fail("下行音频无效或播放队列停滞", "downlink_audio", err);
}

void ConversationController::SubtitleTextDeleter::operator()(char *text) const
{
    heap_caps_free(text);
}

bool ConversationController::prepareSubtitleText(const voice::Identity &identity)
{
    if (!subtitle_text_) {
        subtitle_text_.reset(static_cast<char *>(heap_caps_calloc(
            2, VoiceAssistantStatus::kConversationCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        if (!subtitle_text_) return false;
    }
    if (subtitle_text_identity_ != identity) {
        status_->copyConversation(subtitle_text_.get(), VoiceAssistantStatus::kConversationCapacity);
        response_text_bytes_ = std::strlen(subtitle_text_.get());
        response_text_truncated_ = status_->conversationTruncated();
        subtitle_text_identity_ = identity;
    }
    subtitle_text_.get()[VoiceAssistantStatus::kConversationCapacity] = '\0';
    subtitle_text_visible_ = false;
    return true;
}

void ConversationController::appendResponseText(const char *value)
{
    if (response_text_committed_) return; // llm.done 确定正文后，页面字节不再变化。
    if (subtitle_text_ && subtitle_text_identity_ == reply_ && !response_text_truncated_) {
        const size_t size = std::strlen(value);
        const size_t capacity = VoiceAssistantStatus::kConversationCapacity - 1 - response_text_bytes_;
        size_t retained = std::min(size, capacity);
        // 长度上限可能落在多字节字符内部。
        // 与普通文本存储一样，保留有界且永久截断的完整字符前缀。
        if (retained < size) {
            while (retained && (static_cast<unsigned char>(value[retained]) & 0xc0) == 0x80) --retained;
            response_text_truncated_ = true;
        }
        std::memcpy(subtitle_text_.get() + response_text_bytes_, value, retained);
        response_text_bytes_ += retained;
        subtitle_text_.get()[response_text_bytes_] = '\0';
    }
    if (!subtitle_text_visible_ && !response_text_buffering_) {
        if (response_text_replace_pending_ && subtitle_text_ && subtitle_text_identity_ == reply_) {
            if (response_text_bytes_ != 0) {
                status_->setConversation(subtitle_text_.get(), response_text_truncated_);
                response_text_replace_pending_ = false;
            }
        } else {
            status_->appendConversation(value);
        }
    }
}

void ConversationController::commitResponseText()
{
    if (response_text_buffering_ && subtitle_text_ && subtitle_text_identity_ == reply_) {
        status_->setConversation(subtitle_text_.get(), response_text_truncated_);
        response_text_buffering_ = false;
        response_text_committed_ = true;
    }
}

void ConversationController::restoreResponseText()
{
    if (subtitle_text_visible_ && subtitle_text_ && subtitle_text_identity_ == reply_) {
        if (response_text_bytes_ != 0) {
            status_->setConversation(subtitle_text_.get(), response_text_truncated_);
            response_text_replace_pending_ = false;
        } else {
            // 原始 LLM 后备文本仍为空时，元数据失败不能清除已经可读的文字。
            // 使用保留的规范化文本重置页面；
            // 首个原始文本增量到达后再替换。
            status_->setConversation(subtitle_text_.get() + VoiceAssistantStatus::kConversationCapacity);
            response_text_replace_pending_ = true;
        }
    }
    subtitle_text_visible_ = false;
}

void ConversationController::clearSubtitleTimeline()
{
    if (subtitle_timeline_.enabled || subtitle_timeline_.unavailable) {
        const auto &id = subtitle_timeline_.identity;
        status_->disableSubtitle(id.epoch, id.turn, id.generation,
                                 subtitle_timeline_.stream_id);
    }
    // 不要改动有界词条存储。此处若创建并清零一个 8 KiB 临时对象，
    // 会超过小栈控制任务的剩余栈空间。
    subtitle_timeline_.identity = {};
    subtitle_timeline_.stream_id = 0;
    subtitle_timeline_.next_sequence = 0;
    subtitle_timeline_.text_bytes = 0;
    subtitle_timeline_.word_count = 0;
    subtitle_timeline_.word_head = 0;
    subtitle_timeline_.last_word_start = 0;
    subtitle_timeline_.last_word_offset = 0;
    subtitle_timeline_.have_word = false;
    subtitle_timeline_.current_word = {};
    subtitle_timeline_.have_current_word = false;
    subtitle_timeline_.published_offset = UINT32_MAX;
    subtitle_timeline_.published_active = false;
    subtitle_timeline_.enabled = false;
    subtitle_timeline_.unavailable = false;
    subtitle_timeline_.fixed_response_text = false;
}

void ConversationController::disableSubtitleTimeline(const char *reason)
{
    const auto &id = subtitle_timeline_.identity;
    status_->disableSubtitle(id.epoch, id.turn, id.generation,
                             subtitle_timeline_.stream_id);
    subtitle_timeline_.unavailable = true;
    subtitle_timeline_.word_count = 0;
    // 可选元数据故障不能丢掉正常回答的剩余部分。
    // 恢复完整 LLM 前缀，再正常追加后续增量。
    restoreResponseText();
    ESP_LOGW(kTag, "subtitle unavailable reason=%s epoch=%u turn=%u generation=%u stream=%u",
             reason, static_cast<unsigned>(id.epoch), static_cast<unsigned>(id.turn),
             static_cast<unsigned>(id.generation), static_cast<unsigned>(subtitle_timeline_.stream_id));
}

void ConversationController::onSubtitle(cJSON *event, const voice::Identity &identity, bool escaped_nul)
{
    // 调用者持有 state_mutex_。可选显示元数据绝不能中断正常音频；
    // 批次缺失或格式错误时，只冻结字幕跟随。
    if (!subtitle_timeline_.enabled || subtitle_timeline_.unavailable || !subtitle_text_ ||
        subtitle_timeline_.identity != identity || failure_latched_.load() || resetting_.load()) return;
    uint64_t stream = 0, sequence = 0, text_offset = 0;
    if (!number(event, "stream_id", stream) || stream != subtitle_timeline_.stream_id)
        return;
    if (!number(event, "sequence", sequence) ||
        !number(event, "text_offset", text_offset, VoiceAssistantStatus::kConversationCapacity - 1)) {
        disableSubtitleTimeline("invalid_batch"); return;
    }
    if (sequence < subtitle_timeline_.next_sequence) return; // 重复或迟到的批次。
    if (sequence != subtitle_timeline_.next_sequence || text_offset != subtitle_timeline_.text_bytes) {
        disableSubtitleTimeline("batch_gap"); return;
    }
    if (escaped_nul) { disableSubtitleTimeline("embedded_nul"); return; }
    const cJSON *text_value = cJSON_GetObjectItemCaseSensitive(event, "text");
    if (!cJSON_IsString(text_value) || !text_value->valuestring) {
        disableSubtitleTimeline("missing_text"); return;
    }
    const char *subtitle_text = text_value->valuestring;
    const size_t text_size = std::strlen(subtitle_text);
    const bool fixed_text = subtitle_timeline_.fixed_response_text;
    if (fixed_text && (text_size || text_offset)) {
        disableSubtitleTimeline("fixed_text_modified"); return;
    }
    if (text_size > 1024 || text_offset + text_size >= VoiceAssistantStatus::kConversationCapacity ||
        !validUtf8(subtitle_text, text_size, false)) {
        disableSubtitleTimeline("invalid_text"); return;
    }
    cJSON *words = cJSON_GetObjectItemCaseSensitive(event, "words");
    const int word_count = cJSON_GetArraySize(words);
    if (!cJSON_IsArray(words) || word_count < 0 || word_count > 24 ||
        static_cast<size_t>(word_count) > kSubtitleWordCapacity - subtitle_timeline_.word_count) {
        disableSubtitleTimeline("word_capacity"); return;
    }
    std::array<SubtitleWord, 24> pending{};
    size_t pending_count = 0;
    uint32_t last_start = subtitle_timeline_.last_word_start;
    uint32_t last_offset = subtitle_timeline_.last_word_offset;
    bool have_previous = subtitle_timeline_.have_word;
    const size_t available_text = fixed_text ? response_text_bytes_ : text_offset + text_size;
    const auto display_byte = [&](uint64_t position) -> unsigned char {
        if (fixed_text) return static_cast<unsigned char>(subtitle_text_.get()[position]);
        return static_cast<unsigned char>(position < text_offset ?
            subtitle_text_.get()[VoiceAssistantStatus::kConversationCapacity + position] :
            subtitle_text[position - text_offset]);
    };
    cJSON *word = nullptr;
    cJSON_ArrayForEach(word, words) {
        uint64_t start = 0, end = 0, offset = 0;
        if (!cJSON_IsObject(word) ||
            !number(word, "start_sample", start, 24000ULL * 180) ||
            !number(word, "end_sample", end, 24000ULL * 180) ||
            !number(word, "text_byte_offset", offset,
                   VoiceAssistantStatus::kConversationCapacity - 1) ||
            end <= start || offset >= available_text ||
            (display_byte(offset) & 0xc0) == 0x80 ||
            (have_previous && (start < last_start || offset < last_offset))) {
            disableSubtitleTimeline("invalid_word"); return;
        }

        // 正式协议批次包含准确的词文本。保留不可变的 UTF-8 范围，
        // 防止标点、空格或后来追加的文字
        // 改变已激活时间范围内遍历的字形。
        const cJSON *word_text = cJSON_GetObjectItemCaseSensitive(word, "text");
        size_t word_bytes = 0;
        if (word_text != nullptr) {
            if (!cJSON_IsString(word_text) || !word_text->valuestring) {
                disableSubtitleTimeline("invalid_word_text"); return;
            }
            word_bytes = std::strlen(word_text->valuestring);
            if (!word_bytes || word_bytes > available_text - offset ||
                !validUtf8(word_text->valuestring, word_bytes, false)) {
                disableSubtitleTimeline("invalid_word_text"); return;
            }
            for (size_t i = 0; i < word_bytes; ++i) {
                if (display_byte(offset + i) !=
                    static_cast<unsigned char>(word_text->valuestring[i])) {
                    disableSubtitleTimeline("word_text_mismatch"); return;
                }
            }
        } else {
            // 旧发送端只提供单个锚点。
            // 仅定位该有效字符，不猜测整个词的范围。
            const unsigned char lead = display_byte(offset);
            word_bytes = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        }
        last_start = static_cast<uint32_t>(start);
        last_offset = static_cast<uint32_t>(offset);
        have_previous = true;
        pending[pending_count++] = {static_cast<uint32_t>(start), static_cast<uint32_t>(end),
                                    static_cast<uint32_t>(offset),
                                    static_cast<uint32_t>(offset + word_bytes)};
    }
    if (!fixed_text) {
        status_->setSubtitleText(identity.epoch, identity.turn, identity.generation,
                                 static_cast<uint32_t>(stream), subtitle_text);
        if (text_size) {
            subtitle_text_visible_ = true;
            response_text_replace_pending_ = false;
        }
        std::memcpy(subtitle_text_.get() + VoiceAssistantStatus::kConversationCapacity + text_offset,
                    subtitle_text, text_size + 1);
    }
    for (size_t i = 0; i < pending_count; ++i) {
        subtitle_timeline_.words[(subtitle_timeline_.word_head + subtitle_timeline_.word_count) %
                                  kSubtitleWordCapacity] = pending[i];
        ++subtitle_timeline_.word_count;
    }
    subtitle_timeline_.last_word_start = last_start;
    subtitle_timeline_.last_word_offset = last_offset;
    subtitle_timeline_.have_word = have_previous;
    subtitle_timeline_.text_bytes += static_cast<uint32_t>(text_size);
    ++subtitle_timeline_.next_sequence;
    // 迟到的时间戳批次可能已落后于播放进度。
    // 立即发布当前锚点，不再等待下一个控制周期。
    publishSubtitleProgress(audio_.playbackProgress());
}

void ConversationController::updateSubtitleProgress()
{
    // PCM 所属任务只发布无锁快照；
    // 采集、解码、播放和中断路径都不执行界面或 LVGL 操作。
    const AudioService::PlaybackProgress progress = audio_.playbackProgress();
    std::lock_guard<std::mutex> state(state_mutex_);
    publishSubtitleProgress(progress);
}

void ConversationController::publishSubtitleProgress(const AudioService::PlaybackProgress &progress)
{
    // 两个调用者都持有 state_mutex_；音频快照使用无锁读取。
    if (!subtitle_timeline_.enabled) return;
    if (!accepts(subtitle_timeline_.identity) || failure_latched_.load() ||
        audio_failed_ || status_->playback() == VoicePlaybackState::Idle) {
        clearSubtitleTimeline(); return;
    }
    if (subtitle_timeline_.unavailable || !progress.available || !progress.coherent ||
        progress.identity != subtitle_timeline_.identity ||
        progress.stream_id != subtitle_timeline_.stream_id) return;
    while (subtitle_timeline_.word_count) {
        const auto &word = subtitle_timeline_.words[subtitle_timeline_.word_head];
        if (progress.completed_samples < word.start_sample) break;
        subtitle_timeline_.current_word = word;
        subtitle_timeline_.have_current_word = true;
        subtitle_timeline_.published_offset = word.text_byte_offset;
        subtitle_timeline_.word_head = (subtitle_timeline_.word_head + 1) % kSubtitleWordCapacity;
        --subtitle_timeline_.word_count;
    }
    if (!subtitle_timeline_.have_current_word) return;

    const uint32_t word_end_offset = subtitle_timeline_.current_word.text_end_byte_offset;

    const uint32_t start = subtitle_timeline_.current_word.start_sample;
    const uint32_t end = subtitle_timeline_.current_word.end_sample;
    const uint64_t completed = std::min<uint64_t>(progress.completed_samples, UINT32_MAX);
    const uint32_t playback_sample = static_cast<uint32_t>(completed);
    uint16_t progress_permille = 0;
    if (end > start && completed > start) {
        const uint64_t elapsed = std::min<uint64_t>(completed - start, end - start);
        progress_permille = static_cast<uint16_t>(
            std::min<uint64_t>(1000, (elapsed * 1000) / (end - start)));
    }

    subtitle_timeline_.published_active = true;
    uint32_t next_offset = UINT32_MAX;
    uint16_t next_progress = 0;
    if (subtitle_timeline_.word_count) {
        const auto &next = subtitle_timeline_.words[subtitle_timeline_.word_head];
        if (next.start_sample > start && next.text_byte_offset >= word_end_offset) {
            next_offset = next.text_byte_offset;
            const uint64_t elapsed = completed > start ?
                std::min<uint64_t>(completed - start, next.start_sample - start) : 0;
            next_progress = static_cast<uint16_t>((elapsed * 1000) / (next.start_sample - start));
        }
    }
    const auto &id = subtitle_timeline_.identity;
    status_->setSubtitlePosition(id.epoch, id.turn, id.generation,
                                 subtitle_timeline_.stream_id,
                                 subtitle_timeline_.current_word.text_byte_offset, true,
                                 word_end_offset, start, end, playback_sample,
                                 progress_permille, next_offset, next_progress);
}

void ConversationController::onControl(const uint8_t *data, size_t bytes)
{
    if (!bytes || bytes > 8192) { fail("控制消息超限", "control_size"); return; }
    Json json(cJSON_ParseWithLength(reinterpret_cast<const char *>(data), bytes), cJSON_Delete);
    if (!json || !cJSON_IsObject(json.get())) { fail("控制消息无效", "control_json"); return; }
    const char *type = text(json.get(), "type");
    if (!std::strcmp(type, "session.closed") &&
        !std::strcmp(text(json.get(), "reason"), "idle_timeout")) {
        voice::Identity id{};
        if (!getIdentity(json.get(), id) || id.epoch != epoch_.load()) return;
        if (status_->capture() != VoiceCaptureState::Idle ||
            status_->playback() != VoicePlaybackState::Idle ||
            status_->response() == VoiceResponseState::Waiting ||
            status_->response() == VoiceResponseState::Generating) {
            fail("服务端在语音处理中结束会话", "idle_close_active");
            return;
        }
        ESP_LOGI(kTag, "session_closed reason=idle_timeout attempt=%u epoch=%u turn=%u",
                 static_cast<unsigned>(connection_attempt_.load()),
                 static_cast<unsigned>(id.epoch), static_cast<unsigned>(id.turn));
        idle_closing_.store(true);
        idle_close_pending_.store(true);
        status_->clearSelection();
        return;
    }
    if (!std::strcmp(type, "error")) {
        const char *code = text(json.get(), "code");
        voice::Identity error_id{};
        const bool has_id = getIdentity(json.get(), error_id);
        if (!std::strcmp(code, "tts_failed") || !std::strcmp(code, "response_failed")) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const bool response_failed_before_start = has_id &&
                !std::strcmp(code, "response_failed") &&
                error_id.epoch == epoch_.load() && error_id.turn == turn_.load() &&
                error_id.turn > sealed_turn_ && error_id.generation != 0 &&
                (!reply_.generation || reply_ == error_id);
            if (!has_id || (!accepts(error_id) && !response_failed_before_start)) return;
            reply_revision_.fetch_add(1, std::memory_order_release);
            commitResponseText(); // 生成失败时仍可保留可读的部分回答。
            restoreResponseText();
            clearSubtitleTimeline();
            audio_failed_ = true;
            audio_.stopPlayback();  // 丢弃部分语音，但保留回答文字。
            status_->setPlayback(VoicePlaybackState::Idle);
            status_->setResponse(VoiceResponseState::Failed);
            status_->setPipelineStage(!std::strcmp(code, "tts_failed") ?
                                      "语音合成失败，文字回答仍可查看" :
                                      "回答生成失败，请重新说一句");
            return;
        }
        // 不记录任意服务端载荷、URL 或凭证。
        if (!std::strcmp(code, "uplink_backlog")) {
            fail("服务端上行积压，正在尝试恢复", "server_uplink_backlog"); return;
        }
        if (!std::strcmp(code, "operation_timeout")) {
            fail("语音服务处理超时，正在尝试恢复", "server_operation_timeout"); return;
        }
        if (!std::strcmp(code, "network_failed")) {
            fail("语音服务上游连接失败，正在尝试恢复", "server_network_failed"); return;
        }
        if (!std::strcmp(code, "provider_failed")) {
            fail("语音模型服务失败，正在尝试恢复", "server_provider_failed"); return;
        }
        if (!std::strcmp(code, "asr_events_failed") || !std::strcmp(code, "asr_final_failed")) {
            fail("语音识别服务失败，正在尝试恢复", "server_asr_failed"); return;
        }
        if (!std::strcmp(code, "protocol_invalid") || !std::strcmp(code, "codec_failed")) {
            fail("语音服务协议或音频格式错误，请检查服务配置", "server_protocol_failed"); return;
        }
        fail("语音服务返回错误，正在尝试恢复", "server_error"); return;
    }
    voice::Identity id{};
    if (!getIdentity(json.get(), id)) { fail("会话标识无效", "control_identity"); return; }
    if (!std::strcmp(type, "session.ready")) {
        uint64_t up, down, frame;
        const char *codec = opus_ ? "opus" : "pcm_s16le";
        if (std::strcmp(text(json.get(), "uplink_codec"), codec) ||
            std::strcmp(text(json.get(), "downlink_codec"), codec) ||
            !number(json.get(), "uplink_sample_rate_hz", up) || up != 16000 ||
            !number(json.get(), "downlink_sample_rate_hz", down) || down != 24000 ||
            !number(json.get(), "frame_duration_ms", frame) || frame != 20) {
            fail("协商的音频格式不支持", "session_format"); return;
        }
        const VoiceSessionSelection selection = parseSessionSelection(json.get(), id.epoch, hasEscapedNul(data, bytes));
        bool duplicate = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            // 解析就绪消息时，其他任务也可能报告故障。
            // 元数据发布与 fail() 清除元数据使用同一个锁存屏障。
            std::lock_guard<std::mutex> failure(failure_mutex_);
            if (resetting_.load() || failure_latched_.load() || idle_closing_.load()) return;
            duplicate = epoch_.load() != 0;
            if (!duplicate) {
                reply_revision_.fetch_add(1, std::memory_order_release);
                clearSubtitleTimeline();
                epoch_.store(id.epoch);
                turn_.store(0);
                resetUplinkMetrics(id.epoch, 0);
                sealed_turn_ = 0;
                reply_ = {};
                status_->setSelection(selection);
                status_->setSession(VoiceSessionState::Ready);
            }
        }
        if (duplicate) { fail("重复会话确认", "duplicate_session"); return; }
        status_->setPipelineStage("语音服务已就绪");
        logSessionSelection();
        return;
    }
    if (!std::strcmp(type, "response.cancelled")) {
        std::lock_guard<std::mutex> state(state_mutex_);
        const bool pending = response_cancel_pending_.load();
        const bool matched = pending && id == response_cancel_target_;
        ESP_LOGI(kTag, "barge_in_handoff event=cancel_ack_dispatch at_us=%lld epoch=%u turn=%u generation=%u pending=%u match=%u target_epoch=%u target_turn=%u target_generation=%u",
                 esp_timer_get_time(), static_cast<unsigned>(id.epoch), static_cast<unsigned>(id.turn),
                 static_cast<unsigned>(id.generation), static_cast<unsigned>(pending),
                 static_cast<unsigned>(matched), static_cast<unsigned>(response_cancel_target_.epoch),
                 static_cast<unsigned>(response_cancel_target_.turn),
                 static_cast<unsigned>(response_cancel_target_.generation));
        if (matched) {
            ESP_LOGI(kTag, "barge_in_handoff event=cancel_ack at_us=%lld epoch=%u turn=%u generation=%u",
                     esp_timer_get_time(), static_cast<unsigned>(id.epoch),
                     static_cast<unsigned>(id.turn), static_cast<unsigned>(id.generation));
            response_cancel_pending_.store(false);
        }
        return;
    }
    if (id.epoch != epoch_.load() || id.turn != turn_.load()) return;
    if (!std::strcmp(type, "speech.cancelled")) {
        if (!cancel_pending_.load()) return;
        cancel_pending_.store(false);
        status_->setCapture(VoiceCaptureState::Idle);
        status_->setResponse(VoiceResponseState::Cancelled);
        status_->setPipelineStage("本轮已取消，继续等待讲话");
        return;
    }
    if (!std::strcmp(type, "speech.started")) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (id.turn <= sealed_turn_) {
                // 该封闭回合已交给 speech.cancel 处理；迟到的握手 ACK
                // 不能重新启用采集，也不能提交缓存语音。
                return;
            }
        }
        if (status_->capture() != VoiceCaptureState::Starting || speech_start_queued_.exchange(true)) return;
        const Input input{InputKind::Start, id.epoch, id.turn, {}};
        if (xQueueSend(uplink_, &input, 0) != pdTRUE) fail("录音启动队列已满", "uplink_start_queue_full");
    } else if (!std::strcmp(type, "asr.partial") || !std::strcmp(type, "asr.segment_final")) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (id.turn > sealed_turn_) status_->setConversation(text(json.get(), "text"));
    } else if (!std::strcmp(type, "asr.utterance_final")) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (id.turn <= sealed_turn_) {
            // 旧取消流程可能还在等待最后的采集清理。
            // D2 交接期间，同一旧回合不能覆盖交接中的 Starting 状态。
            if (!barge_in_handoff_.load()) {
                status_->setCapture(VoiceCaptureState::Idle);
                status_->setResponse(VoiceResponseState::Cancelled);
            }
            return;
        }
        status_->setCapture(VoiceCaptureState::Idle);
        status_->setResponse(VoiceResponseState::Waiting);
        status_->setConversation(text(json.get(), "text"));
    } else if (!std::strcmp(type, "response.started") || !std::strcmp(type, "audio.stream.start")) {
        std::unique_lock<std::mutex> lock(state_mutex_);
        if (id.turn <= sealed_turn_) {
            if (!std::strcmp(type, "response.started")) {
                lock.unlock();
                if (!sendControl("response.cancel", id)) fail("取消回复发送失败", "cancel_send");
            }
            return;
        }
        if (!id.generation) return;
        if (reply_.generation && reply_ != id) { fail("回复标识不一致", "response_identity"); return; }
        // 即使回答身份相同，新音频流仍可重置播放。
        reply_revision_.fetch_add(1, std::memory_order_release);
        clearSubtitleTimeline();
        reply_ = id;
        if (!std::strcmp(type, "response.started")) {
            server_done_ = playback_done_ = false;
            audio_failed_ = false;
            clearSubtitleTimeline();
            subtitle_text_identity_ = {};
            response_text_bytes_ = 0;
            response_text_truncated_ = false;
            subtitle_text_visible_ = false;
            response_text_replace_pending_ = false;
            response_text_buffering_ = false;
            response_text_committed_ = false;
            status_->setConversation("");
            if (!std::strcmp(text(json.get(), "text_mode"), "complete_before_audio")) {
                response_text_buffering_ = prepareSubtitleText(id);
                if (!response_text_buffering_)
                    ESP_LOGW(kTag, "response text buffer unavailable; retaining streamed text");
            }
            status_->setResponse(VoiceResponseState::Generating);
        } else {
            // 替换音频流时保留已提交正文；
            // 必要时把旧版规范化字幕文本恢复为原始回答。
            restoreResponseText();
            uint64_t stream, rate, channels, frame, skip;
            if (!number(json.get(), "stream_id", stream) || !stream ||
                !number(json.get(), "sample_rate_hz", rate) || rate != 24000 ||
                !number(json.get(), "channels", channels) || channels != 1 ||
                !number(json.get(), "frame_duration_ms", frame) || frame != 20 ||
                !number(json.get(), "pre_skip_samples", skip, 960) || (!opus_ && skip) ||
                std::strcmp(text(json.get(), "codec"), opus_ ? "opus" : "pcm_s16le")) {
                fail("回复音频格式无效", "downlink_format"); return;
            }
            const esp_err_t err = audio_.beginPlayback(
                id, static_cast<uint32_t>(stream), opus_, static_cast<uint32_t>(skip));
            if (err != ESP_OK) {
                fail("本地回复播放初始化失败", "playback_begin", err); return;
            }
            clearSubtitleTimeline();
            const bool fixed_text = !std::strcmp(text(json.get(), "subtitle_mode"),
                                                 "response_word_timestamps");
            const bool subtitle_requested = fixed_text || !std::strcmp(text(json.get(), "subtitle_mode"),
                                                                       "word_timestamps");
            const bool subtitle_mode = subtitle_requested && (!fixed_text || response_text_committed_) &&
                                       prepareSubtitleText(id);
            subtitle_timeline_.identity = id;
            subtitle_timeline_.stream_id = static_cast<uint32_t>(stream);
            subtitle_timeline_.enabled = subtitle_mode;
            subtitle_timeline_.fixed_response_text = fixed_text;
            if (subtitle_mode) {
                status_->resetSubtitle(id.epoch, id.turn, id.generation,
                                       static_cast<uint32_t>(stream));
            } else if (subtitle_requested) {
                ESP_LOGW(kTag, "subtitle unavailable reason=%s; manual text remains active",
                         fixed_text && !response_text_committed_ ? "text_not_committed" : "text_storage");
            }
            status_->setPlayback(VoicePlaybackState::Buffering);
        }
    } else if (!std::strcmp(type, "audio.subtitle")) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (accepts(id) && !audio_failed_) {
            onSubtitle(json.get(), id, hasEscapedNul(data, bytes));
        }
    } else if (!std::strcmp(type, "audio.subtitle.unavailable")) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        uint64_t stream = 0;
        if (accepts(id) && subtitle_timeline_.identity == id &&
            number(json.get(), "stream_id", stream) && stream == subtitle_timeline_.stream_id)
            disableSubtitleTimeline("server_unavailable");
    } else if (!std::strcmp(type, "llm.delta")) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // 完整正文模式将增量缓存到 llm.done；
        // 旧版回答仍可在规范化字幕之外保留原文后备内容。
        if (accepts(id)) appendResponseText(text(json.get(), "text"));
    } else if (!std::strcmp(type, "llm.done")) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (accepts(id)) commitResponseText();
    } else if (!std::strcmp(type, "audio.stream.end")) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!accepts(id) || audio_failed_) return;
        }
        uint64_t stream, end;
        if (!number(json.get(), "stream_id", stream) ||
            !number(json.get(), "valid_end_sample", end, 24000ULL * 180 + 960)) {
            fail("回复音频结束位置无效", "downlink_end_position"); return;
        }
        const esp_err_t err = audio_.endPlayback(id, static_cast<uint32_t>(stream), end);
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (accepts(id)) {
            if (err != ESP_OK) fail("回复尾部音频无效", "downlink_end", err);
            else if (!playback_done_) status_->setPlayback(VoicePlaybackState::Draining);
        }
    } else if (!std::strcmp(type, "response.done")) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!std::strcmp(text(json.get(), "reason"), "empty_utterance")) {
            if (id.turn <= sealed_turn_) return;
            status_->setCapture(VoiceCaptureState::Idle);
            status_->setConversation("未识别到语音，请重试");
            // 把终态结果保留在结果页。Idle 会切到 Ready 页面，
            // 该页固定提示会遮住这条消息。
            status_->setResponse(VoiceResponseState::Complete);
        } else if (accepts(id)) {
            server_done_ = true;
            if (!std::strcmp(text(json.get(), "reason"), "tts_failed")) {
                reply_revision_.fetch_add(1, std::memory_order_release);
                restoreResponseText();
                clearSubtitleTimeline();
                status_->setPlayback(VoicePlaybackState::Idle);
                status_->setResponse(VoiceResponseState::Failed);
                status_->setPipelineStage("语音合成失败，文字回答仍可查看");
            } else if (playback_done_) status_->setResponse(VoiceResponseState::Complete);
        }
    } // speech.started、中间结果及迟到的取消 ACK 都不能重新打开已封闭的回答。
}
