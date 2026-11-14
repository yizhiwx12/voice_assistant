// 生产控制器使用模拟外设/套接字，并使用真实 cJSON 解析。
#include <array>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#define private public
#include "conversation_controller.hpp"
#undef private
#include "sdkconfig.h"

namespace {
struct Queue { size_t capacity, item_size; std::deque<std::vector<uint8_t>> items; };
struct StopLoop {};
std::function<bool(QueueHandle_t, void *)> receive_hook;
std::function<void()> encoder_finish_hook;
std::string logs;
int64_t clock_us = 1000;
unsigned startup_tasks = 0;
bool fail_subtitle_text_allocation = false;
void control(ConversationController &controller, const char *json) {
    controller.onControl(reinterpret_cast<const uint8_t *>(json), std::strlen(json));
}
void sessionReady(ConversationController &controller, uint32_t epoch, const std::string &selection)
{
    const std::string json = "{\"type\":\"session.ready\",\"session_epoch\":" + std::to_string(epoch) +
        ",\"turn_id\":0,\"generation\":0,\"uplink_codec\":\"opus\",\"downlink_codec\":\"opus\","
        "\"uplink_sample_rate_hz\":16000,\"downlink_sample_rate_hz\":24000,\"frame_duration_ms\":20" +
        (selection.empty() ? "" : ",\"selection\":" + selection) + "}";
    control(controller, json.c_str());
}
struct Fixture {
    std::array<voiceaudio::AutomaticUtterance::Frame, 134> automatic_storage{};
    std::array<voiceaudio::BargeInObserver::Frame, 134> barge_in_storage{};
    VoiceAssistantStatus status;
    WifiStation wifi;
    ConversationController controller;
    Fixture() {
        status.enableRealtime();
        status.setSession(VoiceSessionState::Ready);
        wifi.state_.store(WifiStation::State::Connected);
        controller.status_ = &status;
        controller.wifi_ = &wifi;
        controller.commands_ = xQueueCreate(12, sizeof(ConversationController::Command));
        controller.uplink_ = xQueueCreate(27, sizeof(ConversationController::Input));
        controller.epoch_.store(77);
        controller.connect_since_ = -1;
        controller.automatic_storage_ = automatic_storage.data();
        // 现有协议/样本边界场景用立即起音来隔离握手过程。
        // 下面的专项测试使用固件设置。
        controller.automatic_.configure(automatic_storage.data(), automatic_storage.size(), 10, 35, 1, 0);
        controller.barge_in_storage_ = barge_in_storage.data();
        controller.barge_in_.configure(barge_in_storage.data(), barge_in_storage.size(), 20, 10, 100);
    }
    ~Fixture() {
        receive_hook = {};
        VoiceClient::start_hook = {};
        VoiceClient::text_hook = {};
        VoiceClient::binary_hook = {};
        encoder_finish_hook = {};
        vQueueDelete(controller.commands_);
        vQueueDelete(controller.uplink_);
    }
};

using Phase = voiceaudio::AutomaticUtterance::Phase;
using Input = ConversationController::Input;
using InputKind = ConversationController::InputKind;
size_t sentCount(const Fixture &f, const char *type)
{
    const std::string field = std::string("\"type\":\"") + type + "\"";
    size_t count = 0;
    for (const auto &event : f.controller.client_.sent) if (event.find(field) != std::string::npos) ++count;
    return count;
}
void oneCommand(Fixture &f, char command)
{
    f.controller.command(command);
    unsigned polls = 0;
    receive_hook = [&](QueueHandle_t handle, void *) {
        assert(handle == f.controller.commands_);
        if (++polls > 1) throw StopLoop{};
        return false;
    };
    try { f.controller.controlLoop(); } catch (const StopLoop &) {}
    receive_hook = {};
}
void healthy(Fixture &f)
{
    f.controller.audio_.frontend_metrics.enabled = true;
    f.controller.audio_.frontend_metrics.ready = true;
}
void arm(Fixture &f)
{
    healthy(f);
    f.controller.setAutomatic(true);
    f.controller.updateAutomatic();
    assert(f.controller.automatic_enabled_.load() && f.status.listening());
    assert(f.controller.automatic_.phase() == Phase::Listening);
}
std::array<int16_t, 320> processed(uint64_t offset)
{
    std::array<int16_t, 320> pcm{};
    // 变化的双极性测试信号既表示交流活动，也保留逐样本身份；
    // 正斜坡可能被帧直流分量主导。
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<int16_t>(static_cast<int>((offset + i) * 17U % 32761U) - 16380);
    return pcm;
}
void frontend(Fixture &f, uint64_t offset, bool speech, bool update = true)
{
    // 采集时钟早于 AFE 结果；原始样本使用不同数值，
    // 使意外的自动回退可在队列检查中被发现。
    std::array<int16_t, 320> raw{};
    raw.fill(-1234);
    f.controller.onCapture(raw.data(), raw.size());
    const auto pcm = processed(offset);
    clock_us += 20000;
    f.controller.onFrontend(pcm.data(), pcm.size(), offset, speech);
    if (update) f.controller.updateAutomatic();
}
void trigger(Fixture &f)
{
    arm(f);
    for (uint64_t i = 0; i < 15; ++i) frontend(f, i * 320, false);
    frontend(f, 15 * 320, true);
    assert(f.controller.turn_.load() == 1 && f.status.capture() == VoiceCaptureState::Starting);
    assert(sentCount(f, "speech.start") == 1);
}
std::vector<Input> drainInputs(Fixture &f)
{
    std::vector<Input> output;
    Input input{};
    while (xQueueReceive(f.controller.uplink_, &input, 0) == pdTRUE) output.push_back(input);
    return output;
}
void started(Fixture &f)
{
    control(f.controller, "{\"type\":\"speech.started\",\"session_epoch\":77,\"turn_id\":1,\"generation\":0}");
    const auto inputs = drainInputs(f);
    assert(inputs.size() == 1 && inputs[0].kind == InputKind::Start);
    assert(inputs[0].epoch == 77 && inputs[0].turn == 1);
    // 仅模拟成功的 Start 工作任务边界；编解码/网络任务执行
    // 仍由各自测试隔离验证。
    f.controller.accepting_capture_.store(true);
    f.controller.automatic_.started();
    f.controller.automatic_next_send_us_ = clock_us;
    f.status.setCapture(VoiceCaptureState::Recording);
}
void appendInputs(std::vector<Input> &to, const std::vector<Input> &from)
{ to.insert(to.end(), from.begin(), from.end()); }
void pump(Fixture &f, std::vector<Input> &output, size_t iterations)
{
    for (size_t i = 0; i < iterations; ++i) {
        clock_us += 20000;
        f.controller.updateAutomatic();
        appendInputs(output, drainInputs(f));
    }
}
void checkPcm(const std::vector<Input> &inputs, uint64_t offset, size_t expected_frames)
{
    size_t frames = 0;
    for (const auto &input : inputs) {
        if (input.kind != InputKind::Audio) continue;
        assert(input.epoch == 77 && input.turn == 1);
        assert(input.pcm == processed(offset));
        offset += 320;
        ++frames;
    }
    assert(frames == expected_frames);
}
void sealDuringWrite(Fixture &f)
{
    // 发送者持有 uplink_operation_ 时，模拟取消任务的前半部分。
    // 若此处同步调用完整 cancelSpeech()，会等待同一写者，
    // 与真实独立取消任务的行为不同。
    std::lock_guard<std::mutex> capture(f.controller.capture_mutex_);
    f.controller.accepting_capture_.store(false);
    f.controller.automatic_.disable();
    f.controller.automatic_turn_.store(false);
    std::lock_guard<std::mutex> state(f.controller.state_mutex_);
    f.controller.sealed_turn_ = f.controller.turn_.load();
    f.controller.cancel_pending_.store(true);
    f.status.setCapture(VoiceCaptureState::Idle);
    f.status.setResponse(VoiceResponseState::Cancelled);
}
void runUplink(Fixture &f, const std::vector<Input> &inputs)
{
    size_t index = 0;
    receive_hook = [&](QueueHandle_t handle, void *out) {
        assert(handle == f.controller.uplink_);
        if (index == inputs.size()) throw StopLoop{};
        *static_cast<Input *>(out) = inputs[index++];
        return true;
    };
    try { f.controller.uplinkLoop(); } catch (const StopLoop &) {}
    receive_hook = {};
    assert(index == inputs.size());
}
void observePlayback(Fixture &f)
{
    healthy(f);
    f.controller.audio_.frontend_metrics.aec_requested = true;
    f.controller.audio_.frontend_metrics.aec_active = true;
    f.controller.turn_.store(3);
    f.controller.reply_ = {77, 3, 9};
    f.status.setResponse(VoiceResponseState::Generating);
    f.status.setPlayback(VoicePlaybackState::Playing);
    f.controller.audio_.playing = true;
    f.controller.setBargeInObservation(true);
    f.controller.updateBargeInObservation();
    assert(f.controller.barge_in_enabled_);
}
void observedFrames(Fixture &f, uint64_t &offset, size_t frames, bool speech)
{
    for (size_t i = 0; i < frames; ++i) {
        frontend(f, offset, speech, false);
        offset += 320;
        f.controller.updateBargeInObservation();
        f.controller.updateAutomatic();
    }
}
void armInterruption(Fixture &f)
{
    observePlayback(f);
    f.controller.setBargeIn(true);
    f.controller.updateBargeInObservation();
    assert(f.controller.barge_in_interrupt_enabled_.load() && f.status.bargeInEnabled());
    assert(f.controller.barge_in_enabled_ && f.controller.automatic_enabled_.load());
    assert(f.controller.barge_in_.snapshot().phase == voiceaudio::BargeInObserver::Phase::Armed);
}
void bufferedFrontend(Fixture &f, uint64_t &offset, size_t frames, bool speech)
{
    // 独立于控制节拍模拟 AFE 输出。候选确认不得在前端回调中
    // 执行本地停止或网络操作。
    for (size_t i = 0; i < frames; ++i) {
        frontend(f, offset, speech, false);
        offset += 320;
    }
}
void confirmedInterruption(Fixture &f, uint64_t &offset)
{
    armInterruption(f);
    bufferedFrontend(f, offset, 20, false);
    bufferedFrontend(f, offset, 10, true);
    assert(f.controller.barge_in_.snapshot().confirmations == 1);
    assert(f.controller.audio_.stop_count == 0 && f.controller.client_.sent.empty());
    f.controller.beginBargeInHandoff();
}
void responseCancelled(Fixture &f, uint32_t epoch = 77, uint32_t turn = 3, uint32_t generation = 9)
{
    const auto json = std::string("{\"type\":\"response.cancelled\",\"session_epoch\":") +
        std::to_string(epoch) + ",\"turn_id\":" + std::to_string(turn) +
        ",\"generation\":" + std::to_string(generation) + "}";
    control(f.controller, json.c_str());
}
void subtitleReply(Fixture &f, uint32_t stream = 12)
{
    f.controller.turn_.store(3);
    control(f.controller, "{\"type\":\"response.started\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9}");
    const auto event = std::string("{\"type\":\"audio.stream.start\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":") +
        std::to_string(stream) + ",\"codec\":\"opus\",\"sample_rate_hz\":24000,\"channels\":1,\"frame_duration_ms\":20,\"pre_skip_samples\":0,\"subtitle_mode\":\"word_timestamps\"}";
    control(f.controller, event.c_str());
}
void delta(Fixture &f, const std::string &value)
{
    const auto event = std::string("{\"type\":\"llm.delta\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"text\":\"") + value + "\"}";
    control(f.controller, event.c_str());
}
std::string conversation(Fixture &f)
{
    std::array<char, VoiceAssistantStatus::kConversationCapacity> text{};
    f.status.copyConversation(text.data(), text.size());
    return text.data();
}
void assertD2Pcm(const std::vector<Input> &inputs, uint64_t offset, size_t frames)
{
    size_t seen = 0;
    for (const auto &input : inputs) {
        if (input.kind != InputKind::Audio) continue;
        assert(input.epoch == 77 && input.turn == 4);
        assert(input.pcm == processed(offset));
        offset += 320;
        ++seen;
    }
    assert(seen == frames);
}
}

QueueHandle_t xQueueCreate(unsigned capacity, unsigned bytes) { return new Queue{capacity, bytes, {}}; }
BaseType_t xQueueSend(QueueHandle_t handle, const void *item, TickType_t) {
    auto &queue = *static_cast<Queue *>(handle);
    if (queue.items.size() == queue.capacity) return 0;
    const auto *data = static_cast<const uint8_t *>(item);
    queue.items.emplace_back(data, data + queue.item_size); return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t handle, void *item, TickType_t) {
    if (receive_hook && receive_hook(handle, item)) return pdTRUE;
    auto &queue = *static_cast<Queue *>(handle);
    if (queue.items.empty()) return 0;
    std::memcpy(item, queue.items.front().data(), queue.item_size);
    queue.items.pop_front(); return pdTRUE;
}
void vQueueDelete(QueueHandle_t handle) { delete static_cast<Queue *>(handle); }
unsigned uxQueueMessagesWaiting(QueueHandle_t handle) { return static_cast<Queue *>(handle)->items.size(); }
unsigned uxQueueSpacesAvailable(QueueHandle_t handle) {
    auto &queue = *static_cast<Queue *>(handle); return queue.capacity - queue.items.size();
}
int64_t esp_timer_get_time() { return clock_us += 100; }
unsigned esp_get_free_heap_size() { return 100000; }
unsigned esp_get_minimum_free_heap_size() { return 90000; }
unsigned heap_caps_get_free_size(int) { return 80000; }
void *heap_caps_calloc(size_t count, size_t bytes, int) {
    if (fail_subtitle_text_allocation && count == 2 && bytes == VoiceAssistantStatus::kConversationCapacity)
        return nullptr;
    return std::calloc(count, bytes);
}
void heap_caps_free(void *memory) { std::free(memory); }
// 绝不启动真实任务。后续写入边界测试通过有限输入及确定性产包编解码桩，
// 执行生产上行循环。
BaseType_t xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *) { ++startup_tasks; return pdPASS; }
BaseType_t xTaskCreateWithCaps(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *, int) { ++startup_tasks; return pdPASS; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t) { return 8192; }
esp_err_t voiceaudio::OpusEncoder::open(uint32_t, uint32_t) { return ESP_OK; }
esp_err_t voiceaudio::OpusEncoder::feed(const int16_t *, size_t, const PacketCallback &send) {
    const uint8_t packet[]{0x10, 0x20};
    return send(packet, sizeof(packet));
}
esp_err_t voiceaudio::OpusEncoder::finish(const PacketCallback &send) {
    if (encoder_finish_hook) encoder_finish_hook();
    const uint8_t packet[]{0x30, 0x40};
    return send(packet, sizeof(packet));
}
void voiceaudio::OpusEncoder::close() {}
void stubLog(const char *format, ...) {
    char buffer[2048]; va_list args;
    va_start(args, format); std::vsnprintf(buffer, sizeof(buffer), format, args); va_end(args);
    logs += buffer; logs += '\n';
}
esp_err_t WifiStation::retry() {
    if (state() == State::Failed) state_.store(State::Connecting);
    return ESP_OK;
}

int main() {
    {
        // 使用随项目提供的自动启动选项验证生产 start()。这里只登记任务；
        // 布防必须等到 Ready 且前端健康，不依赖 UART/按键命令，也不创建云端轮次。
        VoiceAssistantStatus status;
        WifiStation wifi;
        ConversationController controller;
        wifi.state_.store(WifiStation::State::Connected);
        controller.audio_.frontend_metrics.enabled = true;
        const unsigned before = startup_tasks;
        assert(controller.start(status, wifi) == ESP_OK && startup_tasks == before + 2);
        assert(controller.requested_mode_ == ConversationController::InteractionMode::BargeIn);
        assert(controller.restore_mode_pending_ && !status.listening());
        controller.restoreInteractionMode();
        assert(controller.restore_mode_pending_ && !status.listening());
        sessionReady(controller, 900, "");
        controller.restoreInteractionMode();
        assert(controller.restore_mode_pending_ && !status.listening());
        controller.audio_.frontend_metrics.ready = true;
        controller.audio_.frontend_metrics.aec_requested = true;
        controller.audio_.frontend_metrics.aec_active = true;
        controller.restoreInteractionMode();
        assert(status.listening() && status.bargeInEnabled() && !controller.restore_mode_pending_);
        assert(controller.client_.sent.empty());
        vQueueDelete(controller.commands_);
        vQueueDelete(controller.uplink_);
        std::free(controller.automatic_storage_);
        std::free(controller.barge_in_storage_);
    }
    {
        // 本地恢复默认使用自动监听 + D2，不需要 UART 字符。
        Fixture f;
        healthy(f);
        f.controller.audio_.frontend_metrics.aec_requested = true;
        f.controller.audio_.frontend_metrics.aec_active = true;
        f.controller.localAction();
        oneCommand(f, '\0');
        assert(f.status.listening() && f.status.bargeInEnabled());
        assert(f.controller.requested_mode_ == ConversationController::InteractionMode::BargeIn);
        assert(f.controller.client_.sent.empty());
        f.controller.localAction();
        oneCommand(f, '\0');
        assert(!f.status.listening() && !f.status.bargeInEnabled());
        assert(f.controller.requested_mode_ == ConversationController::InteractionMode::Manual);
    }
    {
        // 已排队的物理暂停操作优先于已确认的本地 VAD 候选。
        // 不能先开启云端轮次，再把暂停解释为取消。
        Fixture f;
        arm(f);
        f.controller.requested_mode_ = ConversationController::InteractionMode::BargeIn;
        for (uint64_t i = 0; i < 15; ++i) frontend(f, i * 320, false);
        frontend(f, 15 * 320, true, false);
        assert(f.controller.automatic_.phase() == Phase::Opening);
        assert(f.status.capture() == VoiceCaptureState::Idle && f.controller.turn_.load() == 0);
        assert(f.controller.client_.sent.empty());
        f.controller.localAction();
        oneCommand(f, '\0');
        assert(!f.status.listening() && !f.controller.automatic_enabled_.load());
        assert(f.controller.requested_mode_ == ConversationController::InteractionMode::Manual);
        assert(f.controller.automatic_.phase() == Phase::Disabled);
        assert(f.controller.turn_.load() == 0 && f.controller.client_.sent.empty());
        assert(!f.controller.cancel_pending_.load() && !f.controller.failure_latched_.load());
    }
    {
        // 自动握手超时会停止实际监听，但保留请求的 D2 模式。
        // 收到取消 ACK 后，一次物理按键必须能恢复该模式。
        Fixture f;
        f.controller.requested_mode_ = ConversationController::InteractionMode::BargeIn;
        f.controller.audio_.frontend_metrics.aec_requested = true;
        f.controller.audio_.frontend_metrics.aec_active = true;
        trigger(f);
        clock_us += 2100000;
        f.controller.updateAutomatic();
        assert(!f.status.listening() && !f.controller.automatic_enabled_.load());
        assert(f.controller.cancel_pending_.load() && !f.controller.restore_mode_pending_);
        assert(f.controller.requested_mode_ == ConversationController::InteractionMode::BargeIn);
        control(f.controller, "{\"type\":\"speech.cancelled\",\"session_epoch\":77,\"turn_id\":1,\"generation\":0}");
        assert(!f.controller.cancel_pending_.load() && f.status.capture() == VoiceCaptureState::Idle);
        f.controller.localAction();
        oneCommand(f, '\0');
        assert(f.status.listening() && f.status.bargeInEnabled());
        assert(f.controller.automatic_enabled_.load() && !f.controller.restore_mode_pending_);
        assert(f.controller.automatic_.phase() == Phase::Listening);
        assert(f.controller.turn_.load() == 1 && sentCount(f, "speech.start") == 1);
        assert(sentCount(f, "speech.cancel") == 1 && !f.controller.failure_latched_.load());
    }
    {
        // 忙碌期间的物理操作会封存当前回答并恢复空闲 D2；
        // 绝不创建云端轮次，也不重播旧回答 PCM。
        Fixture f;
        armInterruption(f);
        f.controller.requested_mode_ = ConversationController::InteractionMode::BargeIn;
        f.controller.localAction();
        oneCommand(f, '\0');
        assert(sentCount(f, "response.cancel") == 1 && sentCount(f, "speech.start") == 0);
        assert(f.controller.audio_.stop_count == 1 && f.status.listening());
        assert(f.status.bargeInEnabled() && !f.controller.reply_.generation);
    }
    {
        // 缺少 AEC 时明确降级为普通自动监听。
        Fixture f;
        healthy(f);
        f.controller.requested_mode_ = ConversationController::InteractionMode::BargeIn;
        f.controller.restore_mode_pending_ = true;
        f.controller.restoreInteractionMode();
        assert(f.status.listening() && !f.status.bargeInEnabled());
        assert(f.status.bargeIn() == VoiceBargeInState::Unavailable);
    }
    {
        // 有限退避重试五次后提示重启；短暂就绪不补充预算，
        // 只有稳定连接才会补充。
        Fixture f;
        for (unsigned i = 1; i <= 5; ++i) {
            const int64_t before = clock_us;
            f.controller.scheduleRecovery();
            assert(f.controller.reconnect_retries_ == i);
            assert(f.controller.next_connect_us_ > before && f.controller.connect_since_ == 0);
            assert(f.status.session() == VoiceSessionState::Connecting);
        }
        f.controller.scheduleRecovery();
        assert(f.controller.connect_since_ == -1 && f.status.session() == VoiceSessionState::Failed);
        f.status.setSession(VoiceSessionState::Ready);
        f.controller.ready_since_us_ = clock_us;
        oneCommand(f, '\0');
        assert(f.controller.reconnect_retries_ == 5);
        clock_us += 30000001;
        oneCommand(f, '\0');
        assert(f.controller.reconnect_retries_ == 0);
    }
    {
        // 前端失败不能伪装为独立设备已暂停。
        Fixture f;
        healthy(f);
        f.controller.requested_mode_ = ConversationController::InteractionMode::BargeIn;
        f.controller.restore_mode_pending_ = true;
        f.controller.audio_.frontend_metrics.last_error = ESP_FAIL;
        f.controller.restoreInteractionMode();
        assert(f.controller.failed_.load() && !f.controller.failure_retryable_);
        oneCommand(f, '\0');
        assert(f.status.session() == VoiceSessionState::Failed && f.controller.connect_since_ == -1);
        assert(!f.status.listening());
    }
    {
        // 后续硬件失败保留网络首因，同时禁用全部会话重建，
        // 包括明确请求的重试。
        Fixture f;
        f.controller.fail("network first", "network_failure");
        f.controller.fail("hardware second", "audio_device");
        assert(!f.controller.failure_retryable_);
        f.controller.retryConnection();
        oneCommand(f, '\0');
        assert(f.controller.connect_since_ == -1 && f.controller.connection_attempt_.load() == 0);
        assert(f.status.session() == VoiceSessionState::Failed);
    }
    {
        // 可恢复失败清理旧 PCM/身份，等到确切期限，
        // 接受新 epoch 并恢复所选模式。
        Fixture f;
        healthy(f);
        f.controller.audio_.frontend_metrics.aec_requested = true;
        f.controller.audio_.frontend_metrics.aec_active = true;
        f.controller.requested_mode_ = ConversationController::InteractionMode::BargeIn;
        f.controller.fail("network", "network_failure");
        oneCommand(f, '\0');
        assert(!f.controller.epoch_.load() && !f.status.listening());
        assert(f.controller.connection_attempt_.load() == 0);
        clock_us = f.controller.next_connect_us_ + 1;
        VoiceClient::start_hook = [](const VoiceClient::Handler &handler) {
            handler(VoiceClient::Event::Connected, nullptr, 0);
            const char ready[] = R"({"type":"session.ready","session_epoch":900,"turn_id":0,"generation":0,"uplink_codec":"opus","downlink_codec":"opus","uplink_sample_rate_hz":16000,"downlink_sample_rate_hz":24000,"frame_duration_ms":20})";
            handler(VoiceClient::Event::Text, reinterpret_cast<const uint8_t *>(ready), sizeof(ready) - 1);
        };
        oneCommand(f, '\0');
        assert(f.controller.epoch_.load() == 900 && !f.controller.turn_.load());
        assert(f.status.listening() && f.status.bargeInEnabled());
        assert(sentCount(f, "session.start") == 1 && sentCount(f, "speech.start") == 0);
    }
    {
        // 后续设备失败会取消已经安排的网络重试。
        Fixture f;
        f.controller.fail("network first", "network_failure");
        oneCommand(f, '\0');
        assert(f.controller.reconnect_retries_ == 1 && !f.controller.failed_.load());
        f.controller.fail("hardware later", "audio_device");
        assert(f.controller.failed_.load() && !f.controller.failure_retryable_);
        clock_us = f.controller.next_connect_us_ + 1;
        oneCommand(f, '\0');
        assert(f.controller.connect_since_ == -1 && f.controller.connection_attempt_.load() == 0);
        assert(f.status.session() == VoiceSessionState::Failed);
        assert(f.controller.first_failure_stage_ == std::string("network first"));
    }
    {
        // 预算耗尽后，明确重试可开启新的有限预算。
        Fixture f;
        f.controller.reconnect_retries_ = 5;
        f.controller.fail("network", "network_failure");
        f.controller.retryConnection();
        oneCommand(f, '\0');
        assert(f.controller.reconnect_retries_ == 0 && f.controller.connection_attempt_.load() == 1);
    }
    // 流元数据有效而本地播放失败时，必须保留硬件错误，
    // 不能归咎于服务端音频格式。
    {
        Fixture f;
        observePlayback(f);
        f.controller.opus_ = true;
        f.controller.audio_.begin_result = ESP_ERR_INVALID_STATE;
        logs.clear();
        control(f.controller, "{\"type\":\"audio.stream.start\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"codec\":\"opus\",\"sample_rate_hz\":24000,\"channels\":1,\"frame_duration_ms\":20,\"pre_skip_samples\":0}");
        assert(f.controller.audio_.begin_count == 1 && f.controller.failure_latched_.load());
        assert(logs.find("first_failure reason=playback_begin") != std::string::npos);
        assert(logs.find("result=259") != std::string::npos);
        assert(logs.find("reason=downlink_format") == std::string::npos);
    }
    {
        Fixture f;
        observePlayback(f);
        f.controller.opus_ = true;
        logs.clear();
        control(f.controller, "{\"type\":\"audio.stream.start\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"codec\":\"opus\",\"sample_rate_hz\":24000,\"channels\":1,\"frame_duration_ms\":20,\"pre_skip_samples\":961}");
        assert(f.controller.audio_.begin_count == 0 && f.controller.failure_latched_.load());
        assert(logs.find("first_failure reason=downlink_format") != std::string::npos);
        assert(logs.find("reason=playback_begin") == std::string::npos);
    }
    {
        Fixture f;
        f.controller.turn_.store(1);
        f.status.setCapture(VoiceCaptureState::Finishing);
        control(f.controller, "{\"type\":\"asr.utterance_final\",\"session_epoch\":77,\"turn_id\":1,\"generation\":0,\"text\":\"\"}");
        control(f.controller, "{\"type\":\"response.done\",\"session_epoch\":77,\"turn_id\":1,\"generation\":0,\"reason\":\"empty_utterance\"}");
        std::array<char, 128> text{};
        f.status.copyConversation(text.data(), text.size());
        assert(std::string(text.data()) == "未识别到语音，请重试");
        // 检查 render() 实际使用的生产状态映射，而不只是文本存储；
        // 此前 Ready 的固定说明曾遮住此结果。
        assert(f.status.state() == VoiceAssistantState::Complete);
        assert(f.status.capture() == VoiceCaptureState::Idle);
        assert(f.status.playback() == VoicePlaybackState::Idle);
        assert(!f.controller.failed_.load() && f.status.session() == VoiceSessionState::Ready);
        auto &local = f.controller.audio_.frontend_metrics;
        local.enabled = local.ready = true;
        for (int transition = 0; transition < 4; ++transition) {
            local.speech = transition % 2;
            f.controller.updateFrontendStatus();
            assert(f.status.state() == VoiceAssistantState::Complete);
            assert(f.status.frontend() == (local.speech ? VoiceFrontendState::Speech : VoiceFrontendState::Silence));
            f.status.copyConversation(text.data(), text.size());
            assert(std::string(text.data()) == "未识别到语音，请重试");
        }
        f.controller.startSpeech();
        assert(f.controller.turn_.load() == 2 && f.status.capture() == VoiceCaptureState::Starting);
        f.status.copyConversation(text.data(), text.size());
        assert(text[0] == '\0' && f.status.state() == VoiceAssistantState::Preparing);
        assert(f.controller.client_.sent.size() == 1);
        assert(f.controller.client_.sent[0].find("speech.start") != std::string::npos);
    }
    {
        Fixture f;
        f.controller.turn_.store(1);
        f.status.setCapture(VoiceCaptureState::Finishing);
        f.controller.interrupt();
        control(f.controller, "{\"type\":\"asr.utterance_final\",\"session_epoch\":77,\"turn_id\":1,\"generation\":0,\"text\":\"\"}");
        control(f.controller, "{\"type\":\"response.done\",\"session_epoch\":77,\"turn_id\":1,\"generation\":0,\"reason\":\"empty_utterance\"}");
        assert(f.status.response() == VoiceResponseState::Cancelled);
        assert(f.status.state() == VoiceAssistantState::Ready);
    }
    {
        Fixture f;
        f.controller.updateFrontendStatus();
        assert(f.status.frontend() == VoiceFrontendState::Disabled);
        auto &local = f.controller.audio_.frontend_metrics;
        local.enabled = true;
        f.controller.updateFrontendStatus();
        assert(f.status.frontend() == VoiceFrontendState::Starting);
        local.ready = true;
        for (int transition = 0; transition < 20; ++transition) {
            local.speech = transition % 2;
            f.controller.updateFrontendStatus();
            assert(f.status.frontend() == (local.speech ? VoiceFrontendState::Speech : VoiceFrontendState::Silence));
            assert(f.status.capture() == VoiceCaptureState::Idle);
            assert(f.controller.turn_.load() == 0 && !f.controller.accepting_capture_.load());
            assert(uxQueueMessagesWaiting(f.controller.uplink_) == 0);
        }
        local.last_error = ESP_FAIL;
        f.controller.updateFrontendStatus();
        assert(f.status.frontend() == VoiceFrontendState::Failed);
        assert(!f.controller.failed_.load() && f.status.session() == VoiceSessionState::Ready);
        f.controller.logFrontendMetrics();
        assert(logs.find("mode=manual_raw enabled=1 ready=1") != std::string::npos);
    }
    {
        Fixture f;
        // 确定性复现竞争：第 1 轮 End 已开始，
        // 随后控制任务先重置到第 2 轮，End 才发布统计。
        ConversationController::Input old_end{ConversationController::InputKind::End, 77, 1, {}};
        f.controller.resetUplinkMetrics(77, 2);
        assert(!f.controller.recordUplinkMetrics(old_end, 1, 5000, 5000, ESP_OK, ESP_OK, 0, 5000));
        assert(f.controller.uplink_metrics_.packets == 0 && f.controller.uplink_metrics_.send_us == 0);
        assert(f.controller.metrics_pending_.load() == 0);
        ConversationController::Input current_audio{ConversationController::InputKind::Audio, 77, 2, {}};
        assert(f.controller.recordUplinkMetrics(current_audio, 1, 2000, 2000, ESP_OK, ESP_OK, 7000, 9000));
        assert(f.controller.uplink_metrics_.frames == 1 && f.controller.uplink_metrics_.packets == 1);
        assert(f.controller.uplink_metrics_.encode_us == 7000 && f.controller.uplink_metrics_.frame_us == 9000);
        assert(f.controller.metrics_pending_.load() == 0);
        ConversationController::Input current_end{ConversationController::InputKind::End, 77, 2, {}};
        assert(f.controller.recordUplinkMetrics(current_end, 1, 1000, 1000, ESP_OK, ESP_OK, 0, 1000));
        assert(f.controller.uplink_metrics_.packets == 2 && f.controller.metrics_pending_.load() == 2);
        f.controller.resetUplinkMetrics(88, 0);
        assert(!f.controller.recordUplinkMetrics(current_end, 1, 1000, 1000, ESP_OK, ESP_OK, 0, 1000));
        assert(f.controller.uplink_metrics_.frames == 0 && f.controller.uplink_metrics_.packets == 0);
        assert(f.controller.metrics_pending_.load() == 0);
    }
    {
        Fixture f;
        f.controller.uplink_metrics_.epoch = 66;
        f.controller.uplink_metrics_.turn = 4;
        f.controller.uplink_metrics_.frames = 46;
        f.controller.uplink_queue_peak_.store(11);
        f.controller.metrics_pending_.store(2);
        f.controller.resetUplinkMetrics(77, 0);
        f.controller.logUplinkMetrics("manual");
        assert(f.controller.uplink_metrics_.epoch == 77 && f.controller.uplink_metrics_.turn == 0);
        assert(f.controller.uplink_metrics_.frames == 0 && f.controller.uplink_queue_peak_.load() == 0);
        assert(f.controller.metrics_pending_.load() == 0);
        assert(logs.find("active_epoch=77 active_turn=0 metrics_current=1 frames=0") != std::string::npos);
    }
    {
        Fixture f;
        f.controller.epoch_.store(0);
        const char *ready =
            "{\"type\":\"session.ready\",\"session_epoch\":88,\"turn_id\":0,\"generation\":0,"
            "\"uplink_codec\":\"opus\",\"downlink_codec\":\"opus\",\"uplink_sample_rate_hz\":16000,"
            "\"downlink_sample_rate_hz\":24000,\"frame_duration_ms\":20,\"selection\":{"
            "\"profile\":\"realtime_default\",\"mode\":\"realtime\",\"require_streaming_asr\":true,"
            "\"asr\":{\"id\":\"doubao_2\",\"adapter\":\"doubao_streaming\",\"model\":\"bigmodel\","
            "\"resource_id\":\"volc.seedasr.sauc.duration\",\"capabilities\":{\"streaming_input\":true,"
            "\"streaming_output\":true,\"partial_results\":true}},"
            "\"llm\":{\"id\":\"deepseek_default\",\"adapter\":\"chat_completions\",\"model\":\"deepseek-flash\","
            "\"capabilities\":{\"streaming_output\":true}},"
            "\"tts\":{\"id\":\"volc_default\",\"adapter\":\"volcengine_bidirectional\",\"voice\":\"zh_female\","
            "\"resource_id\":\"seed-tts-2.0\",\"capabilities\":{\"streaming_input\":true,\"streaming_output\":true}}}}";
        control(f.controller, ready);
        const VoiceSessionSelection selection = f.status.selection();
        assert(f.status.session() == VoiceSessionState::Ready && f.controller.epoch_.load() == 88);
        assert(selection.epoch == 88 && selection.metadata_present && !selection.rejected_fields);
        assert(std::string(selection.profile.data()) == "realtime_default");
        assert(std::string(selection.asr.id.data()) == "doubao_2");
        assert(selection.asr.capabilities.partial_results == VoiceSelectionFlag::Yes);
        assert(selection.llm.capabilities.streaming_input == VoiceSelectionFlag::Unknown);
        assert(std::string(selection.tts.voice.data()) == "zh_female");
        f.controller.resetConnection();
        assert(f.status.selection().epoch == 0 && !f.status.selection().metadata_present);
    }
    {
        // 元数据仅供诊断：选择信息缺失或格式错误，
        // 不能拒绝原本支持的音频握手。
        for (const char *selection : {"", "\"selection\":\"wrong-type\""}) {
            Fixture f;
            f.controller.epoch_.store(0);
            std::string ready =
                "{\"type\":\"session.ready\",\"session_epoch\":89,\"turn_id\":0,\"generation\":0,"
                "\"uplink_codec\":\"opus\",\"downlink_codec\":\"opus\",\"uplink_sample_rate_hz\":16000,"
                "\"downlink_sample_rate_hz\":24000,\"frame_duration_ms\":20";
            if (selection[0] != '\0') ready += std::string(",") + selection;
            ready += "}";
            control(f.controller, ready.c_str());
            const VoiceSessionSelection snapshot = f.status.selection();
            assert(f.status.session() == VoiceSessionState::Ready && f.controller.epoch_.load() == 89);
            assert(snapshot.epoch == 89 && !snapshot.metadata_present);
            if (selection[0] != '\0') assert(snapshot.rejected_fields);
        }
    }
    {
        Fixture f;
        f.controller.epoch_.store(0);
        std::string oversized(81, 'x');
        const std::string ready = std::string(
            "{\"type\":\"session.ready\",\"session_epoch\":90,\"turn_id\":0,\"generation\":0,"
            "\"uplink_codec\":\"opus\",\"downlink_codec\":\"opus\",\"uplink_sample_rate_hz\":16000,"
            "\"downlink_sample_rate_hz\":24000,\"frame_duration_ms\":20,\"selection\":{\"profile\":\"" ) +
            oversized + "\",\"mode\":\"realtime\"}}";
        control(f.controller, ready.c_str());
        const VoiceSessionSelection snapshot = f.status.selection();
        assert(f.status.session() == VoiceSessionState::Ready && snapshot.metadata_present);
        assert(snapshot.rejected_fields && snapshot.profile[0] == '\0');
    }
    {
        Fixture f;
        f.controller.epoch_.store(0);
        logs.clear();
        sessionReady(f.controller, 91, R"({"profile":"公开组合","mode":"realtime","asr":{"id":"asr_one","capabilities":{"streaming_input":false,"streaming_output":1,"partial_results":true,"future_capability":true},"endpoint":"SECRET_ENDPOINT","credential_ref":"SECRET_REF"},"llm":{"id":3,"model":{}},"tts":{"id":"none","adapter":null},"require_streaming_asr":"true","credentials":"SECRET_CREDENTIALS"})");
        auto snapshot = f.status.selection();
        assert(snapshot.epoch == 91 && snapshot.metadata_present && snapshot.rejected_fields);
        assert(std::string(snapshot.profile.data()) == "公开组合");
        assert(snapshot.require_streaming_asr == VoiceSelectionFlag::Unknown);
        assert(snapshot.asr.capabilities.streaming_input == VoiceSelectionFlag::No);
        assert(snapshot.asr.capabilities.streaming_output == VoiceSelectionFlag::Unknown);
        assert(snapshot.asr.capabilities.partial_results == VoiceSelectionFlag::Yes);
        assert(snapshot.llm.id[0] == '\0' && snapshot.llm.model[0] == '\0');
        assert(std::string(snapshot.tts.id.data()) == "none" && snapshot.tts.adapter[0] == '\0');
        assert(logs.find("SECRET") == std::string::npos);
        assert(logs.find("streaming_output=unknown") != std::string::npos);
        const size_t log_start = logs.size();
        f.controller.command('?');
        assert(logs.substr(log_start).find("session_selection epoch=91") != std::string::npos);
        assert(logs.substr(log_start).find("profile=\"公开组合\"") != std::string::npos);
        assert(logs.substr(log_start).find("session_selection_asr epoch=91 id=\"asr_one\"") != std::string::npos);
        assert(logs.find("SECRET") == std::string::npos);
        f.controller.resetConnection();
        assert(f.status.selection().epoch == 0);
        sessionReady(f.controller, 92, R"({"asr":{"id":"replacement"}})");
        snapshot = f.status.selection();
        assert(snapshot.epoch == 92 && snapshot.metadata_present && !snapshot.rejected_fields);
        assert(snapshot.profile[0] == '\0' && snapshot.tts.id[0] == '\0');
        assert(std::string(snapshot.asr.id.data()) == "replacement");
        // 旧的普通事件不能替换本会话的元数据。
        control(f.controller, "{\"type\":\"llm.delta\",\"session_epoch\":91,\"turn_id\":1,\"generation\":1,\"text\":\"old\"}");
        assert(f.status.selection().epoch == 92);
        f.controller.onNetwork(VoiceClient::Event::Disconnected, nullptr, 0);
        assert(f.status.selection().epoch == 0 && f.controller.failure_latched_.load());
    }
    {
        // 容量以 UTF-8 字节计；接受文本不能被截断为
        // 另一个模型/选项标识符。
        const std::string utf8 = "中文";
        const std::string maximum = std::string(74, 'm') + utf8;
        for (bool overlong : {false, true}) {
            Fixture f;
            f.controller.epoch_.store(0);
            sessionReady(f.controller, 93, "{\"profile\":\"" + maximum + (overlong ? "x" : "") +
                "\",\"asr\":{\"id\":\"valid_id\"}}");
            const auto snapshot = f.status.selection();
            assert(snapshot.rejected_fields == overlong);
            assert(std::string(snapshot.profile.data()) == (overlong ? "" : maximum));
            assert(std::string(snapshot.asr.id.data()) == "valid_id");
            assert(!f.controller.failure_latched_.load());
        }
        for (const std::string &invalid : {std::string("\xc0\xaf"), std::string("\xed\xa0\x80"),
                                           std::string("\xf4\x90\x80\x80"), std::string("\xe4\xb8")}) {
            Fixture f;
            f.controller.epoch_.store(0);
            sessionReady(f.controller, 94, "{\"asr\":{\"id\":\"" + invalid + "\"}}");
            assert(f.status.selection().rejected_fields && f.status.selection().asr.id[0] == '\0');
            assert(!f.controller.failure_latched_.load());
        }
    }
    {
        Fixture f;
        f.controller.epoch_.store(0);
        logs.clear();
        sessionReady(f.controller, 95, R"({"asr":{"id":"line\nINJECTED_LOG"},"llm":{"id":"normal"}})");
        assert(f.status.selection().rejected_fields && f.status.selection().asr.id[0] == '\0');
        assert(logs.find("INJECTED_LOG") == std::string::npos);
        assert(std::string(f.status.selection().llm.id.data()) == "normal");
        // 预期的空闲关闭立即使当前生效组合失效。
        control(f.controller, "{\"type\":\"session.closed\",\"reason\":\"idle_timeout\",\"session_epoch\":95,\"turn_id\":0,\"generation\":0}");
        assert(f.status.selection().epoch == 0 && f.controller.idle_close_pending_.load());
    }
    {
        Fixture f;
        f.controller.epoch_.store(0);
        f.controller.resetting_.store(true);
        sessionReady(f.controller, 96, R"({"profile":"stale_ready"})");
        assert(f.controller.epoch_.load() == 0 && f.status.selection().epoch == 0);
        f.controller.resetting_.store(false);
        sessionReady(f.controller, 97, R"({"tts":{"id":"none","adapter":null}})");
        assert(!f.status.selection().rejected_fields);
        sessionReady(f.controller, 96, R"({"profile":"stale_ready"})");
        assert(f.controller.failure_latched_.load() && f.status.selection().epoch == 0);
    }
    {
        Fixture f;
        f.controller.epoch_.store(0);
        sessionReady(f.controller, 98, R"({"asr":{"id":"asr\u0000different"}})");
        assert(f.status.selection().epoch == 98 && f.status.selection().rejected_fields);
        assert(!f.status.selection().metadata_present && f.status.selection().asr.id[0] == '\0');
        assert(!f.controller.failure_latched_.load());
        f.controller.resetConnection();
        // 忽略字段中的字面反斜杠 u 序列不是 NUL，
        // 不能因此屏蔽其他有效白名单元数据。
        sessionReady(f.controller, 99, R"({"profile":"first","ignored":"literal\\u0000text"})");
        assert(f.status.selection().metadata_present && !f.status.selection().rejected_fields);
        assert(std::string(f.status.selection().profile.data()) == "first");
        // 同 epoch 的重复就绪仍属于协议失败，
        // 不能悄悄替换元数据，也不算第二次就绪成功。
        sessionReady(f.controller, 99, R"({"profile":"second"})");
        assert(f.controller.failure_latched_.load() && f.status.selection().epoch == 0);
    }
    {
        Fixture f;
        control(f.controller, "{\"type\":\"session.closed\",\"reason\":\"idle_timeout\",\"session_epoch\":66,\"turn_id\":0,\"generation\":0}");
        assert(!f.controller.idle_close_pending_.load() && !f.controller.failed_.load());
        control(f.controller, "{\"type\":\"session.closed\",\"reason\":\"idle_timeout\",\"session_epoch\":77,\"turn_id\":0,\"generation\":0}");
        assert(f.controller.idle_close_pending_.load() && f.controller.idle_closing_.load());
        f.controller.onNetwork(VoiceClient::Event::Disconnected, nullptr, 0);
        f.controller.onNetwork(VoiceClient::Event::Failure, nullptr, 0);
        assert(!f.controller.failed_.load() && !f.controller.failure_latched_.load());
    }
    {
        Fixture f;
        f.status.setCapture(VoiceCaptureState::Recording);
        control(f.controller, "{\"type\":\"session.closed\",\"reason\":\"idle_timeout\",\"session_epoch\":77,\"turn_id\":1,\"generation\":0}");
        assert(f.controller.failed_.load() && !f.controller.idle_close_pending_.load());
    }
    {
        Fixture f;
        f.controller.onNetwork(VoiceClient::Event::Disconnected, nullptr, 0);
        assert(f.controller.failed_.load());  // 未预告的关闭仍属于错误。
    }
    {
        Fixture f;
        control(f.controller, "{\"type\":\"error\",\"code\":\"uplink_backlog\",\"text\":\"SECRET\"}");
        assert(f.controller.failed_.load());
        assert(logs.find("reason=server_uplink_backlog") != std::string::npos);
        control(f.controller, "{\"type\":\"error\",\"code\":\"SECRET\"}");
        assert(logs.find("SECRET") == std::string::npos);
    }
    {
        Fixture f;
        // 空闲后重连必须接受一次 r，并等到新 session.ready 才开始新轮次；
        // 不能重播旧流。
        f.controller.turn_.store(5);
        f.controller.uplink_metrics_.epoch = 77;
        f.controller.uplink_metrics_.turn = 5;
        f.controller.uplink_metrics_.frames = 46;
        int polls = 0;
        receive_hook = [&](QueueHandle_t handle, void *item) {
            assert(handle == f.controller.commands_);
            if (++polls == 1) {
                // 空闲预告与命令轮询竞争，控制任务尚未完成清理。
                // 这个 r 必须保留。
                control(f.controller, "{\"type\":\"session.closed\",\"reason\":\"idle_timeout\",\"session_epoch\":77,\"turn_id\":5,\"generation\":0}");
                assert(f.controller.idle_close_pending_.load());
                auto *command = static_cast<ConversationController::Command *>(item);
                command->kind = ConversationController::CommandKind::Start;
                return true;
            }
            throw StopLoop{};
        };
        VoiceClient::start_hook = [](const VoiceClient::Handler &handler) {
            handler(VoiceClient::Event::Connected, nullptr, 0);
            const char ready[] = "{\"type\":\"session.ready\",\"session_epoch\":88,\"turn_id\":0,\"generation\":0,\"uplink_codec\":\"opus\",\"downlink_codec\":\"opus\",\"uplink_sample_rate_hz\":16000,\"downlink_sample_rate_hz\":24000,\"frame_duration_ms\":20}";
            handler(VoiceClient::Event::Text, reinterpret_cast<const uint8_t *>(ready), sizeof(ready) - 1);
        };
        try { f.controller.controlLoop(); } catch (const StopLoop &) {}
        assert(f.controller.connection_attempt_.load() == 1);
        assert(f.controller.epoch_.load() == 88 && f.controller.turn_.load() == 1);
        assert(f.status.capture() == VoiceCaptureState::Starting);
        assert(f.controller.uplink_metrics_.epoch == 88 && f.controller.uplink_metrics_.turn == 1);
        assert(f.controller.uplink_metrics_.frames == 0 && uxQueueMessagesWaiting(f.controller.uplink_) == 0);
        assert(f.controller.client_.sent.size() == 2);
        assert(f.controller.client_.sent[0].find("session.start") != std::string::npos);
        assert(f.controller.client_.sent[1].find("speech.start") != std::string::npos);
        assert(f.controller.client_.sent[1].find("\"session_epoch\":88") != std::string::npos);
    }
    {
        Fixture f;
        f.controller.setAutomatic(true);
        assert(!f.controller.automatic_enabled_.load() && !f.status.listening());
        healthy(f);
        f.controller.audio_.frontend_metrics.last_error = ESP_FAIL;
        f.controller.setAutomatic(true);
        assert(!f.status.listening());
        f.controller.audio_.frontend_metrics.last_error = ESP_OK;
        f.status.setSession(VoiceSessionState::Connecting);
        f.controller.setAutomatic(true);
        assert(!f.status.listening());
        f.status.setSession(VoiceSessionState::Ready);
        f.controller.automatic_storage_ = nullptr;
        f.controller.setAutomatic(true);
        assert(!f.status.listening());
        f.controller.automatic_storage_ = f.automatic_storage.data();
        oneCommand(f, 'a');
        assert(f.status.listening() && f.controller.automatic_.phase() == Phase::Listening);
        assert(f.controller.client_.sent.empty() && f.controller.turn_.load() == 0);
        oneCommand(f, 'm');
        assert(!f.status.listening() && f.controller.automatic_.phase() == Phase::Disabled);
    }
    {
        Fixture f;
        arm(f);
        std::array<int16_t, 320> raw{};
        raw.fill(-1234);
        for (size_t i = 0; i < 50; ++i) f.controller.onCapture(raw.data(), raw.size());
        f.controller.updateAutomatic();
        assert(f.controller.client_.sent.empty() && f.controller.turn_.load() == 0);
        oneCommand(f, 'r');
        assert(!f.status.listening() && !f.controller.automatic_turn_.load());
        control(f.controller, "{\"type\":\"speech.started\",\"session_epoch\":77,\"turn_id\":1,\"generation\":0}");
        assert(drainInputs(f).front().kind == InputKind::Start);
        f.controller.accepting_capture_.store(true);
        f.status.setCapture(VoiceCaptureState::Recording);
        f.controller.onCapture(raw.data(), raw.size());
        const auto input = drainInputs(f);
        assert(input.size() == 1 && input.front().kind == InputKind::Audio && input.front().pcm == raw);
        const auto pcm = processed(0);
        f.controller.onFrontend(pcm.data(), pcm.size(), 0, true);
        assert(drainInputs(f).empty());
    }
    {
        Fixture f;
        arm(f);
        for (uint64_t i = 0; i < 2000; ++i) frontend(f, i * 320, false);
        assert(f.controller.turn_.load() == 0 && f.controller.client_.sent.empty());
        assert(f.controller.automatic_.phase() == Phase::Listening && f.status.listening());
        assert(drainInputs(f).empty() && !f.controller.failed_.load());
    }
    {
        Fixture f;
        arm(f);
        for (uint64_t i = 0; i < 14; ++i) frontend(f, i * 320, false);
        frontend(f, 14 * 320, true);
        assert(sentCount(f, "speech.start") == 0); // 初始安静只有 280 ms。
        for (uint64_t i = 15; i < 30; ++i) frontend(f, i * 320, false);
        frontend(f, 30 * 320, true);
        assert(sentCount(f, "speech.start") == 1 && f.controller.automatic_.size() == 11);
        for (uint64_t i = 31; i < 35; ++i) frontend(f, i * 320, true);
        assert(sentCount(f, "speech.start") == 1 && f.controller.turn_.load() == 1);
        assert(drainInputs(f).empty()); // 云端 ACK 前不发送任何数据。
    }
    {
        Fixture f;
        trigger(f);
        started(f);
        std::vector<Input> output;
        f.controller.updateAutomatic();
        appendInputs(output, drainInputs(f));
        assert(output.size() == 1 && output.front().kind == InputKind::Audio);
        // 未推进 20 ms 时，下一缓存帧必须等待。
        f.controller.updateAutomatic();
        assert(drainInputs(f).empty());
        pump(f, output, 10);
        checkPcm(output, 5 * 320, 11);
        assert(f.controller.automatic_.size() == 0 && f.status.capture() == VoiceCaptureState::Recording);
    }
    {
        Fixture f;
        trigger(f);
        // 一段短话在 ASR 握手仍待完成时结束。
        for (uint64_t i = 16; i < 51; ++i) frontend(f, i * 320, false);
        assert(f.controller.automatic_.phase() == Phase::Draining);
        assert(f.status.capture() == VoiceCaptureState::Starting && sentCount(f, "speech.start") == 1);
        started(f);
        assert(f.controller.automatic_.phase() == Phase::Draining);
        std::vector<Input> output;
        pump(f, output, 60);
        checkPcm(output, 5 * 320, 46);
        size_t ends = 0;
        for (const auto &input : output) if (input.kind == InputKind::End) ++ends;
        assert(ends == 1 && output.back().kind == InputKind::End);
        assert(f.status.capture() == VoiceCaptureState::Finishing && !f.controller.accepting_capture_.load());
        f.controller.endSpeech();
        pump(f, output, 5);
        size_t after = 0;
        for (const auto &input : output) if (input.kind == InputKind::End) ++after;
        assert(after == 1 && sentCount(f, "speech.start") == 1);
    }
    {
        Fixture f;
        trigger(f);
        started(f);
        std::array<int16_t, 320> raw{};
        for (size_t i = 0; i < 3; ++i) f.controller.onCapture(raw.data(), raw.size());
        f.controller.endSpeech();
        assert(f.controller.automatic_.phase() == Phase::Streaming);
        for (uint64_t i = 16; i < 18; ++i) {
            const auto pcm = processed(i * 320);
            f.controller.onFrontend(pcm.data(), pcm.size(), i * 320, true);
            assert(f.controller.automatic_.phase() == Phase::Streaming);
        }
        const auto last = processed(18 * 320);
        f.controller.onFrontend(last.data(), last.size(), 18 * 320, true);
        assert(f.controller.automatic_.phase() == Phase::Draining);
        std::vector<Input> output;
        pump(f, output, 20);
        checkPcm(output, 5 * 320, 14);
        assert(output.back().kind == InputKind::End && output.size() == 15);
    }
    for (char command : {'c', 'm'}) {
        Fixture f;
        trigger(f);
        oneCommand(f, command);
        assert(!f.controller.automatic_enabled_.load() && !f.status.listening());
        assert(f.controller.cancel_pending_.load() && f.controller.sealed_turn_ == 1);
        assert(sentCount(f, "speech.cancel") == 1 && f.controller.automatic_.size() == 0);
        assert(f.status.capture() == VoiceCaptureState::Idle);
        control(f.controller, "{\"type\":\"speech.started\",\"session_epoch\":77,\"turn_id\":1,\"generation\":0}");
        assert(drainInputs(f).empty() && !f.controller.accepting_capture_.load());
        assert(sentCount(f, "speech.end") == 0); // 绝不提交已取消的握手。
        f.controller.startSpeech();
        assert(f.controller.turn_.load() == 1); // 等待取消确认。
        control(f.controller, "{\"type\":\"speech.cancelled\",\"session_epoch\":77,\"turn_id\":1,\"generation\":0}");
        assert(!f.controller.cancel_pending_.load());
        f.controller.startSpeech();
        assert(f.controller.turn_.load() == 2 && sentCount(f, "speech.start") == 2);
    }
    {
        Fixture f;
        f.controller.automatic_.configure(f.automatic_storage.data(), 12, 10, 35, 1, 0);
        trigger(f);
        frontend(f, 16 * 320, true, false);
        frontend(f, 17 * 320, true, false);
        assert(f.controller.automatic_buffer_failed_.load());
        f.controller.updateAutomatic();
        assert(!f.controller.automatic_enabled_.load() && !f.status.listening());
        assert(f.controller.cancel_pending_.load() && sentCount(f, "speech.cancel") == 1);
        assert(!f.controller.accepting_capture_.load() && drainInputs(f).empty());
        std::array<int16_t, 320> raw{};
        f.controller.onCapture(raw.data(), raw.size());
        assert(drainInputs(f).empty() && !f.controller.failed_.load());
    }
    {
        Fixture f;
        trigger(f);
        clock_us += 2100000;
        f.controller.updateAutomatic();
        assert(f.controller.cancel_pending_.load() && !f.status.listening());
        assert(sentCount(f, "speech.cancel") == 1 && sentCount(f, "speech.end") == 0);
    }
    {
        Fixture f;
        healthy(f);
        f.status.setPlayback(VoicePlaybackState::Playing);
        f.controller.setAutomatic(true);
        f.controller.updateAutomatic();
        assert(f.controller.automatic_.phase() == Phase::Disabled);
        for (uint64_t i = 0; i < 30; ++i) frontend(f, i * 320, i >= 15);
        assert(sentCount(f, "speech.start") == 0);
        f.status.setPlayback(VoicePlaybackState::Idle);
        f.status.setResponse(VoiceResponseState::Generating);
        f.controller.updateAutomatic();
        assert(f.controller.automatic_.phase() == Phase::Disabled);
        f.status.setResponse(VoiceResponseState::Complete);
        f.controller.updateAutomatic();
        assert(f.controller.automatic_.phase() == Phase::Listening);
        for (uint64_t i = 30; i < 45; ++i) frontend(f, i * 320, true);
        assert(sentCount(f, "speech.start") == 0); // 播放后需要新的安静区间。
        for (uint64_t i = 45; i < 60; ++i) frontend(f, i * 320, false);
        frontend(f, 60 * 320, true);
        assert(sentCount(f, "speech.start") == 1);
    }
    {
        Fixture f;
        f.controller.opus_ = false;
        f.controller.turn_.store(1);
        f.status.setCapture(VoiceCaptureState::Starting);
        Input audio{InputKind::Audio, 77, 1, processed(0)};
        runUplink(f, {{InputKind::Start, 77, 1, {}}, audio, {InputKind::End, 77, 1, {}}});
        assert(sentCount(f, "audio.stream.start") == 1 && f.controller.client_.sent_binary == 1);
        assert(sentCount(f, "audio.stream.end") == 1 && sentCount(f, "speech.end") == 1);
        assert(!f.controller.failure_latched_.load()); // 写入方正向对照。
    }
    for (esp_err_t send_result : {ESP_OK, ESP_FAIL}) {
        Fixture f;
        trigger(f);
        f.controller.opus_ = false;
        VoiceClient::text_hook = [&](const std::string &event) {
            if (event.find("\"type\":\"audio.stream.start\"") != std::string::npos) {
                sealDuringWrite(f);
                return send_result;
            }
            return ESP_OK;
        };
        runUplink(f, {{InputKind::Start, 77, 1, {}},
                      {InputKind::Audio, 77, 1, processed(0)}, {InputKind::End, 77, 1, {}}});
        assert(!f.controller.accepting_capture_.load() && f.status.capture() == VoiceCaptureState::Idle);
        assert(f.status.response() == VoiceResponseState::Cancelled && f.controller.cancel_pending_.load());
        assert(f.controller.client_.sent_binary == 0 && sentCount(f, "audio.stream.end") == 0);
        assert(sentCount(f, "speech.end") == 0 && !f.controller.failure_latched_.load());
    }
    {
        Fixture f;
        f.controller.opus_ = false;
        f.controller.turn_.store(1);
        f.status.setCapture(VoiceCaptureState::Starting);
        VoiceClient::text_hook = [&](const std::string &event) {
            if (event.find("\"type\":\"audio.stream.end\"") != std::string::npos) sealDuringWrite(f);
            return ESP_OK;
        };
        runUplink(f, {{InputKind::Start, 77, 1, {}},
                      {InputKind::Audio, 77, 1, processed(0)}, {InputKind::End, 77, 1, {}}});
        assert(sentCount(f, "audio.stream.end") == 1 && sentCount(f, "speech.end") == 0);
        assert(f.controller.client_.sent_binary == 1 && f.controller.sealed_turn_ == 1);
        assert(!f.controller.accepting_capture_.load() && !f.controller.failure_latched_.load());
    }
    {
        Fixture f;
        f.controller.turn_.store(1);
        f.status.setCapture(VoiceCaptureState::Starting);
        encoder_finish_hook = [&] { sealDuringWrite(f); };
        runUplink(f, {{InputKind::Start, 77, 1, {}}, {InputKind::End, 77, 1, {}}});
        // Opus 尾部排空期间发生取消时，须在套接字写入前拒绝数据包，
        // 随后跳过流完成及语音完成控制消息。
        assert(f.controller.client_.sent_binary == 0);
        assert(sentCount(f, "audio.stream.end") == 0 && sentCount(f, "speech.end") == 0);
        assert(f.controller.cancel_pending_.load() && !f.controller.failure_latched_.load());
    }
    {
        Fixture f;
        f.controller.opus_ = false;
        f.controller.turn_.store(1);
        f.status.setCapture(VoiceCaptureState::Starting);
        VoiceClient::binary_hook = [&](const uint8_t *, size_t bytes) {
            assert(bytes == voice::kHeaderBytes + 640);
            sealDuringWrite(f);
            return ESP_FAIL;
        };
        runUplink(f, {{InputKind::Start, 77, 1, {}},
                      {InputKind::Audio, 77, 1, processed(0)}, {InputKind::End, 77, 1, {}}});
        assert(f.controller.client_.sent_binary == 1 && f.controller.cancel_pending_.load());
        assert(sentCount(f, "audio.stream.end") == 0 && sentCount(f, "speech.end") == 0);
        assert(!f.controller.failure_latched_.load()); // 主动取消保留自身原因及界面状态。
    }
    {
        Fixture f;
        healthy(f);
        f.status.setPlayback(VoicePlaybackState::Playing);
        f.controller.raw_samples_ = 640000;
        f.controller.setAutomatic(true);
        f.controller.updateAutomatic();
        assert(f.controller.automatic_.phase() == Phase::Disabled);
        f.status.setPlayback(VoicePlaybackState::Idle);
        f.status.setResponse(VoiceResponseState::Complete);
        f.controller.updateAutomatic();
        assert(f.controller.automatic_resume_sample_ == 640000);
        assert(f.controller.automatic_.phase() == Phase::Listening);
        // 这些块在播放结束后输出，但样本采集于播放期间。
        // 即使完整安静门限后又出现 VAD 语音候选，
        // 也不得进入下一轮历史或触发 ASR。
        for (uint64_t i = 0; i < 21; ++i) {
            const auto offset = 640000 - (21 - i) * 320;
            const auto pcm = processed(offset);
            f.controller.onFrontend(pcm.data(), pcm.size(), offset, i >= 15);
            f.controller.updateAutomatic();
        }
        assert(f.controller.automatic_.size() == 0 && sentCount(f, "speech.start") == 0);
        assert(!f.controller.automatic_buffer_failed_.load());
        // 恢复屏障处，只有新鲜音频才能满足 300 ms 安静门限；
        // 280 ms 后紧接语音仍不够。
        for (uint64_t i = 0; i < 14; ++i) frontend(f, 640000 + i * 320, false);
        frontend(f, 640000 + 14 * 320, true);
        assert(sentCount(f, "speech.start") == 0);
        for (uint64_t i = 15; i < 30; ++i) frontend(f, 640000 + i * 320, false);
        frontend(f, 640000 + 30 * 320, true);
        assert(sentCount(f, "speech.start") == 1 && f.controller.turn_.load() == 1);
        assert(!f.controller.automatic_buffer_failed_.load());
        const auto cache = f.controller.automatic_.frames_;
        const auto head = f.controller.automatic_.head_;
        assert(cache[head].offset == 640000 + 20 * 320);
        assert(cache[head].pcm == processed(640000 + 20 * 320));
    }
    {
        Fixture f;
        arm(f);
        f.status.setConversation("previous result");
        f.status.setResponse(VoiceResponseState::Complete);
        oneCommand(f, 'c');
        std::array<char, 64> retained{};
        f.status.copyConversation(retained.data(), retained.size());
        assert(!f.status.listening() && f.status.response() == VoiceResponseState::Complete);
        assert(std::string(retained.data()) == "previous result");
        assert(f.controller.client_.sent.empty() && !f.controller.cancel_pending_.load());
        assert(!f.controller.failure_latched_.load());
    }
    {
        Fixture f;
        assert(!f.controller.barge_in_enabled_ && f.status.bargeIn() == VoiceBargeInState::Disabled);
        f.controller.setBargeInObservation(true); // 单麦克风 AFE 不足以满足要求。
        assert(!f.controller.barge_in_enabled_ && f.status.bargeIn() == VoiceBargeInState::Unavailable);
        assert(f.controller.client_.sent.empty() && f.controller.audio_.stop_count == 0);
        healthy(f);
        f.controller.audio_.frontend_metrics.aec_requested = true;
        f.controller.audio_.frontend_metrics.aec_active = true;
        f.controller.barge_in_storage_ = nullptr;
        f.controller.setBargeInObservation(true);
        assert(!f.controller.barge_in_enabled_ && f.controller.client_.sent.empty());
    }
    {
        Fixture f;
        observePlayback(f);
        uint64_t offset = 0;
        observedFrames(f, offset, 150, false);
        const auto s = f.controller.barge_in_.snapshot();
        assert(s.candidates == 0 && s.confirmations == 0 && s.buffered_frames == 20);
        assert(f.controller.client_.sent.empty() && f.controller.client_.sent_binary == 0);
        assert(f.controller.audio_.stop_count == 0 && f.status.playback() == VoicePlaybackState::Playing);
    }
    {
        Fixture f;
        observePlayback(f);
        f.controller.setAutomatic(true);
        uint64_t offset = 0;
        observedFrames(f, offset, 20, false);
        observedFrames(f, offset, 9, true);
        assert(f.status.bargeIn() == VoiceBargeInState::Candidate);
        observedFrames(f, offset, 1, true);
        assert(f.status.bargeIn() == VoiceBargeInState::Confirmed);
        assert(f.controller.barge_in_.snapshot().confirmations == 1);
        assert(f.controller.automatic_.phase() == Phase::Disabled && f.status.listening());
        assert(f.controller.turn_.load() == 3 && f.controller.reply_ == (voice::Identity{77, 3, 9}));
        assert(f.status.capture() == VoiceCaptureState::Idle && f.controller.audio_.stop_count == 0);
        assert(f.controller.client_.sent.empty() && drainInputs(f).empty());
    }
    {
        Fixture f;
        f.controller.raw_samples_ = 640000;
        observePlayback(f);
        for (uint64_t offset = 633600; offset < 640000; offset += 320) {
            const auto pcm = processed(offset);
            f.controller.onFrontend(pcm.data(), pcm.size(), offset, true);
        }
        assert(f.controller.barge_in_.snapshot().candidates == 0);
        uint64_t offset = 640000;
        observedFrames(f, offset, 10, true);
        assert(f.controller.barge_in_.snapshot().candidate_offset == 640000);
        assert(f.controller.barge_in_.snapshot().confirmations == 1);
    }
    {
        Fixture f;
        observePlayback(f);
        uint64_t offset = 0;
        observedFrames(f, offset, 10, true);
        f.controller.audio_.playing = false;
        f.status.setPlayback(VoicePlaybackState::Idle);
        f.controller.updateBargeInObservation();
        assert(f.controller.barge_in_.snapshot().buffered_frames == 0);
        assert(f.controller.barge_in_enabled_ && f.status.bargeIn() == VoiceBargeInState::Armed);
        // 新的实际播放必须使用新的原始时钟激活屏障。
        f.controller.raw_samples_ += 6400;
        offset = f.controller.raw_samples_;
        f.controller.reply_revision_.fetch_add(1, std::memory_order_release);
        f.controller.turn_.store(4);
        f.controller.reply_ = {77, 4, 10};
        f.controller.audio_.playing = true;
        f.status.setPlayback(VoicePlaybackState::Playing);
        f.controller.updateBargeInObservation();
        observedFrames(f, offset, 10, true);
        assert(f.controller.barge_in_.snapshot().candidate_offset == offset - 3200);
        assert(f.controller.barge_in_.snapshot().confirmations == 2 && f.controller.client_.sent.empty());
    }
    {
        Fixture f;
        observePlayback(f);
        uint64_t offset = 0;
        observedFrames(f, offset, 9, true);
        f.controller.audio_.frontend_metrics.aec_active = false;
        observedFrames(f, offset, 1, true);
        assert(!f.controller.barge_in_enabled_ && f.status.bargeIn() == VoiceBargeInState::Unavailable);
        assert(f.controller.barge_in_.snapshot().buffered_frames == 0);
        assert(f.controller.barge_in_.snapshot().confirmations == 0);
        assert(std::string(f.controller.barge_in_reason_) == "aec_unavailable");
        assert(f.controller.audio_.stop_count == 0 && !f.controller.failure_latched_.load());
        // 仅恢复健康状态不能悄悄重新启用观测。
        f.controller.audio_.frontend_metrics.aec_active = true;
        f.controller.updateBargeInObservation();
        assert(!f.controller.barge_in_enabled_);
        oneCommand(f, 'b');
        assert(f.controller.barge_in_enabled_);
    }
    {
        Fixture f;
        observePlayback(f);
        uint64_t offset = 0;
        observedFrames(f, offset, 9, true);
        offset += 320; // 缺少 AEC 输出不能算作连续语音。
        observedFrames(f, offset, 1, true);
        assert(!f.controller.barge_in_enabled_ && f.controller.barge_in_.snapshot().discontinuities == 1);
        assert(f.controller.barge_in_.snapshot().buffered_frames == 0 && f.controller.client_.sent.empty());
    }
    {
        Fixture f;
        observePlayback(f);
        uint64_t offset = 0;
        observedFrames(f, offset, 110, true);
        assert(f.controller.barge_in_.snapshot().timeouts == 1);
        observedFrames(f, offset, 200, true);
        assert(f.controller.barge_in_.snapshot().confirmations == 1);
        observedFrames(f, offset, 15, false);
        observedFrames(f, offset, 10, true);
        assert(f.controller.barge_in_.snapshot().confirmations == 2);
        assert(f.controller.client_.sent.empty() && f.controller.audio_.stop_count == 0);
    }
    {
        for (char command : {'c', 'm', 'r', 'n'}) {
            Fixture f;
            observePlayback(f);
            uint64_t offset = 0;
            observedFrames(f, offset, 10, true);
            oneCommand(f, command);
            assert(!f.controller.barge_in_enabled_ && f.status.bargeIn() == VoiceBargeInState::Disabled);
            assert(f.controller.barge_in_.snapshot().buffered_frames == 0);
            assert(f.controller.client_.sent_binary == 0); // 不重放候选。
        }
    }
    {
        Fixture f;
        observePlayback(f);
        f.controller.setAutomatic(true);
        oneCommand(f, 'b');
        assert(!f.controller.barge_in_enabled_ && f.controller.automatic_enabled_.load());
        assert(f.status.listening() && f.controller.reply_ == (voice::Identity{77, 3, 9}));
        oneCommand(f, 'b');
        assert(f.controller.barge_in_enabled_ && f.controller.automatic_enabled_.load());
        assert(f.controller.client_.sent.empty() && f.controller.audio_.stop_count == 0);
    }
    {
        Fixture f;
        observePlayback(f);
        uint64_t offset = 0;
        observedFrames(f, offset, 9, true);
        clock_us += 500001;
        f.controller.updateBargeInObservation();
        assert(!f.controller.barge_in_enabled_ && f.status.bargeIn() == VoiceBargeInState::Unavailable);
        assert(f.controller.barge_in_.snapshot().buffered_frames == 0 && f.controller.client_.sent.empty());
    }
    {
        Fixture f;
        observePlayback(f);
        uint64_t offset = 0;
        observedFrames(f, offset, 9, true);
        f.controller.reply_revision_.fetch_add(1, std::memory_order_release);
        f.controller.sealed_turn_ = 3; // 模拟迟到输出前的生产封存操作。
        observedFrames(f, offset, 1, true);
        assert(f.controller.barge_in_.snapshot().confirmations == 0);
        assert(f.controller.barge_in_.snapshot().buffered_frames == 0);
        assert(f.controller.client_.sent.empty() && f.controller.audio_.stop_count == 0);
    }
    {
        Fixture f;
        observePlayback(f);
        uint64_t offset = 0;
        observedFrames(f, offset, 9, true);
        const auto pcm = processed(offset);
        {
            std::lock_guard<std::mutex> state(f.controller.state_mutex_);
            // 模拟 RX 任务停留在较慢的错误/扬声器屏障。
            // AFE 必须不等待该互斥锁就返回，运行器会执行超时限制。
            std::thread fetch([&] { f.controller.onFrontend(pcm.data(), pcm.size(), offset, true); });
            fetch.join();
        }
        f.controller.updateBargeInObservation();
        assert(f.controller.barge_in_enabled_ && f.controller.barge_in_.snapshot().confirmations == 1);
        assert(f.status.bargeIn() == VoiceBargeInState::Confirmed);
        assert(f.controller.barge_in_.snapshot().buffered_frames == 10);
        assert(f.controller.client_.sent.empty() && f.controller.audio_.stop_count == 0);
    }
    {
        Fixture f;
        observePlayback(f);
        f.controller.barge_in_.disable(); // 观测已布防，但尚未活动。
        {
            std::lock_guard<std::mutex> state(f.controller.state_mutex_);
            std::thread update([&] { f.controller.updateBargeInObservation(); });
            update.join();
        }
        assert(f.controller.barge_in_enabled_ && f.controller.barge_in_.snapshot().phase == voiceaudio::BargeInObserver::Phase::Disabled);
        f.controller.updateBargeInObservation();
        assert(f.controller.barge_in_.snapshot().phase == voiceaudio::BargeInObserver::Phase::Armed);
        {
            std::lock_guard<std::mutex> state(f.controller.state_mutex_);
            std::thread update([&] { f.controller.updateBargeInObservation(); });
            update.join();
        }
        assert(f.controller.barge_in_enabled_ && f.status.bargeIn() == VoiceBargeInState::Armed);
        // 即使状态被锁定，真实输出断供仍是致命错误。
        clock_us += 500001;
        {
            std::lock_guard<std::mutex> state(f.controller.state_mutex_);
            std::thread update([&] { f.controller.updateBargeInObservation(); });
            update.join();
        }
        assert(!f.controller.barge_in_enabled_ && f.status.bargeIn() == VoiceBargeInState::Unavailable);
        assert(std::string(f.controller.barge_in_reason_) == "frontend_stalled");
        assert(f.controller.client_.sent.empty() && f.controller.audio_.stop_count == 0);
    }
    {
        Fixture f;
        observePlayback(f);
        uint64_t offset = 0;
        {
            std::lock_guard<std::mutex> state(f.controller.state_mutex_);
            std::thread fetch([&] {
                observedFrames(f, offset, 20, false);
                observedFrames(f, offset, 10, true);
            });
            fetch.join();
        }
        const auto snapshot = f.controller.barge_in_.snapshot();
        assert(f.controller.barge_in_enabled_ && snapshot.confirmations == 1);
        assert(snapshot.buffered_frames == 30 && snapshot.oldest_offset == 0 && snapshot.next_offset == 9600);
        assert(snapshot.discontinuities == 0 && snapshot.overflows == 0);
        // 跨忙碌状态帧比较每个存储样本，不只比较计数器。
        const auto &observer = f.controller.barge_in_;
        for (size_t i = 0; i < snapshot.buffered_frames; ++i) {
            const auto &frame = observer.frames_[(observer.head_ + i) % observer.capacity_];
            assert(frame.offset == i * 320 && frame.pcm == processed(i * 320));
        }
        assert(f.controller.client_.sent.empty() && drainInputs(f).empty() && f.controller.audio_.stop_count == 0);
    }
    {
        Fixture f;
        observePlayback(f);
        uint64_t offset = 0;
        {
            std::lock_guard<std::mutex> state(f.controller.state_mutex_);
            std::thread fetch([&] {
                observedFrames(f, offset, 110, true);
                assert(f.controller.barge_in_.snapshot().timeouts == 1);
                observedFrames(f, offset, 60, true);
                assert(f.controller.barge_in_.snapshot().confirmations == 1);
                observedFrames(f, offset, 15, false);
                observedFrames(f, offset, 10, true);
            });
            fetch.join();
        }
        const auto snapshot = f.controller.barge_in_.snapshot();
        assert(f.controller.barge_in_enabled_ && snapshot.confirmations == 2 && snapshot.timeouts == 1);
        assert(snapshot.discontinuities == 0 && snapshot.next_offset == offset);
        assert(f.status.bargeIn() == VoiceBargeInState::Confirmed && f.controller.client_.sent.empty());
    }
    {
        Fixture f;
        observePlayback(f);
        uint64_t offset = 0;
        observedFrames(f, offset, 9, true);
        {
            std::lock_guard<std::mutex> state(f.controller.state_mutex_);
            f.controller.reply_revision_.fetch_add(1, std::memory_order_release);
            f.controller.turn_.store(4);
            f.controller.reply_ = {77, 4, 10};
            std::thread fetch([&] { observedFrames(f, offset, 1, true); });
            fetch.join();
            assert(f.controller.barge_in_.snapshot().phase == voiceaudio::BargeInObserver::Phase::Disabled);
            assert(f.controller.barge_in_.snapshot().buffered_frames == 0);
            assert(f.controller.barge_in_.snapshot().confirmations == 0);
        }
        f.controller.updateBargeInObservation();
        const auto fresh = offset;
        observedFrames(f, offset, 10, true);
        assert(f.controller.barge_in_reply_ == (voice::Identity{77, 4, 10}));
        assert(f.controller.barge_in_.snapshot().candidate_offset == fresh);
        assert(f.controller.barge_in_.snapshot().confirmations == 1 && f.controller.barge_in_boundary_resets_ == 1);
        assert(f.controller.client_.sent.empty() && f.controller.audio_.stop_count == 0);
    }
    {
        Fixture f;
        observePlayback(f);
        uint64_t offset = 0;
        observedFrames(f, offset, 9, true);
        const auto before = f.controller.reply_revision_.load();
        control(f.controller, "{\"type\":\"audio.stream.start\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"codec\":\"opus\",\"sample_rate_hz\":24000,\"channels\":1,\"frame_duration_ms\":20,\"pre_skip_samples\":0}");
        assert(f.controller.reply_revision_.load() == before + 1);
        // 即使回答身份相同，仍是新的播放绑定；
        // 其首帧不能补完前一个流的九帧候选。
        frontend(f, offset, true, false);
        offset += 320;
        assert(f.controller.barge_in_.snapshot().buffered_frames == 0);
        assert(f.controller.barge_in_.snapshot().confirmations == 0);
        f.controller.updateBargeInObservation();
        const auto fresh = offset;
        observedFrames(f, offset, 10, true);
        assert(f.controller.barge_in_.snapshot().candidate_offset == fresh);
        assert(f.controller.barge_in_.snapshot().confirmations == 1 && !f.controller.failure_latched_.load());
    }
    {
        Fixture f;
        observePlayback(f);
        uint64_t offset = 0;
        observedFrames(f, offset, 9, true);
        const auto revision = f.controller.reply_revision_.load();
        control(f.controller, "{\"type\":\"llm.delta\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"text\":\"ordinary reply text\"}");
        std::array<uint8_t, voice::kHeaderBytes + 1> packet{};
        const voice::PacketHeader header{voice::kDownlink, {77, 3, 9}, 1, 0, 0};
        assert(voice::packHeader(header, packet.data(), packet.size()));
        f.controller.onBinary(packet.data(), packet.size());
        assert(f.controller.reply_revision_.load() == revision);
        observedFrames(f, offset, 1, true);
        assert(f.controller.barge_in_.snapshot().confirmations == 1 && f.controller.barge_in_boundary_resets_ == 0);
        assert(f.controller.client_.sent.empty() && f.controller.audio_.stop_count == 0);
    }
    {
        for (const char *code : {"tts_failed", "response_failed"}) {
            Fixture f;
            observePlayback(f);
            uint64_t offset = 0;
            observedFrames(f, offset, 9, true);
            const auto revision = f.controller.reply_revision_.load();
            const std::string error = std::string("{\"type\":\"error\",\"code\":\"") + code +
                "\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9}";
            control(f.controller, error.c_str());
            assert(f.controller.reply_revision_.load() == revision + 1 && f.controller.audio_failed_);
            assert(f.controller.audio_.stop_count == 1 && f.status.response() == VoiceResponseState::Failed);
            f.controller.audio_.playing = true; // 即使 DMA 仍有残留，也不能重新布防失败回答。
            observedFrames(f, offset, 1, true);
            assert(f.controller.barge_in_.snapshot().confirmations == 0);
            assert(f.controller.barge_in_.snapshot().buffered_frames == 0);
            assert(f.controller.barge_in_.snapshot().phase == voiceaudio::BargeInObserver::Phase::Disabled);
            assert(f.controller.client_.sent.empty());
        }
    }
    {
        Fixture f;
        observePlayback(f);
        uint64_t offset = 0;
        observedFrames(f, offset, 9, true);
        const auto revision = f.controller.reply_revision_.load();
        control(f.controller, "{\"type\":\"response.done\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"reason\":\"completed\"}");
        assert(f.controller.reply_revision_.load() == revision && f.controller.server_done_);
        observedFrames(f, offset, 1, true);
        assert(f.controller.barge_in_.snapshot().confirmations == 1 && f.controller.barge_in_enabled_);
        assert(f.controller.audio_.playing && f.controller.audio_.stop_count == 0);
    }
    {
        for (unsigned lifecycle = 0; lifecycle < 3; ++lifecycle) {
            Fixture f;
            observePlayback(f);
            uint64_t offset = 0;
            observedFrames(f, offset, 9, true);
            const auto revision = f.controller.reply_revision_.load();
            if (lifecycle == 0) f.controller.startSpeech();
            else if (lifecycle == 1) f.controller.cancelSpeech();
            else f.controller.resetConnection();
            assert(f.controller.reply_revision_.load() > revision);
            f.controller.updateBargeInObservation();
            assert(f.controller.barge_in_.snapshot().buffered_frames == 0);
            assert(f.controller.barge_in_.snapshot().confirmations == 0);
            assert(f.controller.client_.sent_binary == 0);
        }
    }
    {
        Fixture f;
        observePlayback(f);
        uint64_t offset = 0;
        observedFrames(f, offset, 9, true);
        const auto revision = f.controller.reply_revision_.load();
        f.controller.onPlayed({77, 3, 9});
        oneCommand(f, '?'); // 处理生产代码排队的 Played 回调。
        assert(f.controller.reply_revision_.load() == revision + 1 && f.controller.playback_done_);
        observedFrames(f, offset, 1, true);
        assert(f.controller.barge_in_.snapshot().buffered_frames == 0);
        assert(f.controller.barge_in_.snapshot().confirmations == 0);
        assert(sentCount(f, "playback.done") == 1 && f.controller.audio_.stop_count == 0);
    }
    {
        Fixture f;
        observePlayback(f);
        uint64_t offset = 0;
        observedFrames(f, offset, 9, true);
        const auto revision = f.controller.reply_revision_.load();
        control(f.controller, "{\"type\":\"response.done\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"reason\":\"tts_failed\"}");
        assert(f.controller.reply_revision_.load() == revision + 1);
        assert(f.status.response() == VoiceResponseState::Failed);
        observedFrames(f, offset, 1, true);
        assert(f.controller.barge_in_.snapshot().confirmations == 0);
        assert(f.controller.barge_in_.snapshot().buffered_frames == 0);
        assert(f.controller.barge_in_.snapshot().phase == voiceaudio::BargeInObserver::Phase::Disabled);
    }
    {
        Fixture f;
        assert(!f.controller.barge_in_interrupt_enabled_.load() && !f.status.bargeInEnabled());
        healthy(f); // 非 AEC 前端不能布防 D2。
        f.controller.setBargeIn(true);
        assert(!f.controller.barge_in_interrupt_enabled_.load() && !f.status.bargeInEnabled());
        assert(f.controller.client_.sent.empty() && f.controller.audio_.stop_count == 0);
    }
    {
        Fixture f;
        armInterruption(f);
        uint64_t offset = 0;
        bufferedFrontend(f, offset, 120, false);
        f.controller.beginBargeInHandoff();
        assert(f.controller.barge_in_.snapshot().candidates == 0);
        assert(!f.controller.barge_in_handoff_.load() && !f.controller.response_cancel_pending_.load());
        assert(f.controller.client_.sent.empty() && f.controller.audio_.stop_count == 0);
    }
    {
        Fixture f;
        armInterruption(f);
        uint64_t offset = 0;
        bufferedFrontend(f, offset, 20, false);
        bufferedFrontend(f, offset, 9, true);
        f.controller.beginBargeInHandoff();
        bufferedFrontend(f, offset, 1, false);
        f.controller.beginBargeInHandoff();
        assert(f.controller.barge_in_.snapshot().confirmations == 0);
        assert(f.controller.client_.sent.empty() && f.controller.audio_.stop_count == 0);
    }
    {
        Fixture f;
        armInterruption(f);
        uint64_t offset = 0;
        bufferedFrontend(f, offset, 20, false);
        bufferedFrontend(f, offset, 10, true);
        assert(f.controller.audio_.stop_count == 0 && f.controller.client_.sent.empty());
        bool stopped_before_cancel = false, callback_ran_during_stop = false;
        f.controller.audio_.stop_hook = [&] {
            assert(f.controller.sealed_turn_ == 3 && !f.controller.accepts({77, 3, 9}));
            assert(sentCount(f, "response.cancel") == 0 && sentCount(f, "speech.start") == 0);
            // 控制任务执行本地停止时，新鲜近端语音仍在到达。
            // 这些帧必须跨越新封存边界保留下来。
            bufferedFrontend(f, offset, 3, true);
            callback_ran_during_stop = true;
        };
        VoiceClient::text_hook = [&](const std::string &event) {
            if (event.find("response.cancel") != std::string::npos) {
                assert(f.controller.audio_.stop_count == 1 && !f.controller.audio_.playing);
                assert(f.controller.sealed_turn_ == 3 && f.controller.response_cancel_pending_.load());
                assert(f.controller.automatic_.size() == 33);
                stopped_before_cancel = true;
            }
            return ESP_OK;
        };
        f.controller.beginBargeInHandoff();
        assert(stopped_before_cancel && callback_ran_during_stop);
        assert(f.controller.barge_in_handoff_.load() && f.controller.response_cancel_pending_.load());
        assert(f.controller.turn_.load() == 3 && sentCount(f, "speech.start") == 0);
        assert(f.controller.client_.sent_binary == 0 && f.status.capture() == VoiceCaptureState::Starting);
        f.controller.beginBargeInHandoff(); // 同一次确认只消费一次。
        assert(f.controller.audio_.stop_count == 1 && sentCount(f, "response.cancel") == 1);
    }
    {
        Fixture f;
        uint64_t offset = 0;
        confirmedInterruption(f, offset);
        for (const auto id : {voice::Identity{88, 3, 9}, voice::Identity{77, 2, 9},
                              voice::Identity{77, 3, 8}, voice::Identity{77, 4, 9}}) {
            responseCancelled(f, id.epoch, id.turn, id.generation);
            f.controller.updateBargeInHandoff();
            assert(f.controller.response_cancel_pending_.load() && f.controller.turn_.load() == 3);
            assert(sentCount(f, "speech.start") == 0);
        }
        responseCancelled(f);
        assert(!f.controller.response_cancel_pending_.load());
        f.controller.updateBargeInHandoff();
        assert(f.controller.turn_.load() == 4 && sentCount(f, "speech.start") == 1);
        responseCancelled(f); // 迟到的重复消息不能开启第二轮。
        f.controller.updateBargeInHandoff();
        f.controller.beginBargeInHandoff();
        assert(f.controller.turn_.load() == 4 && sentCount(f, "speech.start") == 1);
        assert(sentCount(f, "response.cancel") == 1);
    }
    {
        Fixture f;
        uint64_t offset = 0;
        confirmedInterruption(f, offset);
        bufferedFrontend(f, offset, 3, true);
        responseCancelled(f);
        f.controller.updateBargeInHandoff();
        control(f.controller, "{\"type\":\"speech.started\",\"session_epoch\":77,\"turn_id\":4,\"generation\":0}");
        const auto start = drainInputs(f);
        assert(start.size() == 1 && start[0].kind == InputKind::Start && start[0].turn == 4);
        runUplink(f, start);
        std::vector<Input> sent;
        for (size_t i = 0; i < 45; ++i) {
            frontend(f, offset, i < 10, false);
            offset += 320;
            f.controller.updateBargeInHandoff();
            f.controller.updateAutomatic();
            const auto at_tick = drainInputs(f);
            assert(at_tick.size() <= 2); // 一个按节奏发送的音频帧，可选带 End。
            appendInputs(sent, at_tick);
        }
        pump(f, sent, 40);
        assertD2Pcm(sent, 0, 78); // 预录 + 确认 + 取消等待 + 语音 + 静音。
        size_t ends = 0;
        for (const auto &input : sent) ends += input.kind == InputKind::End;
        assert(ends == 1 && sentCount(f, "speech.start") == 1);
        assert(!f.controller.automatic_buffer_failed_.load() && !f.controller.failure_latched_.load());
    }
    {
        Fixture f;
        uint64_t offset = 0;
        confirmedInterruption(f, offset);
        bufferedFrontend(f, offset, 35, false); // 话语先于取消 ACK 结束。
        assert(f.controller.automatic_.phase() == Phase::Draining);
        assert(f.controller.turn_.load() == 3 && sentCount(f, "speech.start") == 0);
        responseCancelled(f);
        f.controller.updateBargeInHandoff();
        control(f.controller, "{\"type\":\"speech.started\",\"session_epoch\":77,\"turn_id\":4,\"generation\":0}");
        const auto start = drainInputs(f);
        assert(start.size() == 1 && start[0].kind == InputKind::Start);
        runUplink(f, start);
        std::vector<Input> sent;
        pump(f, sent, 70);
        assertD2Pcm(sent, 0, 65);
        size_t ends = 0;
        for (const auto &input : sent) ends += input.kind == InputKind::End;
        assert(ends == 1 && f.status.capture() == VoiceCaptureState::Finishing);
        assert(sentCount(f, "speech.start") == 1 && !f.controller.failure_latched_.load());
    }
    {
        Fixture f;
        uint64_t offset = 0;
        confirmedInterruption(f, offset);
        std::array<char, 64> retained{};
        control(f.controller, "{\"type\":\"llm.delta\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"text\":\"late old text\"}");
        control(f.controller, "{\"type\":\"audio.stream.start\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"codec\":\"opus\",\"sample_rate_hz\":24000,\"channels\":1,\"frame_duration_ms\":20,\"pre_skip_samples\":0}");
        std::array<uint8_t, voice::kHeaderBytes + 1> packet{};
        const voice::PacketHeader header{voice::kDownlink, {77, 3, 9}, 1, 0, 0};
        assert(voice::packHeader(header, packet.data(), packet.size()));
        f.controller.onBinary(packet.data(), packet.size());
        f.status.copyConversation(retained.data(), retained.size());
        assert(std::string(retained.data()).find("late old text") == std::string::npos);
        assert(f.controller.audio_.feed_count == 0 && f.controller.audio_.begin_count == 0);
        assert(f.controller.response_cancel_pending_.load() && !f.controller.failure_latched_.load());
    }
    {
        Fixture f;
        uint64_t offset = 0;
        confirmedInterruption(f, offset);
        clock_us += 2000001;
        f.controller.updateBargeInHandoff();
        assert(f.controller.failure_latched_.load() && f.controller.turn_.load() == 3);
        assert(sentCount(f, "speech.start") == 0 && f.controller.client_.sent_binary == 0);
    }
    {
        Fixture f;
        uint64_t offset = 0;
        confirmedInterruption(f, offset);
        responseCancelled(f);
        f.controller.updateBargeInHandoff();
        assert(f.controller.turn_.load() == 4 && sentCount(f, "speech.start") == 1);
        clock_us += 2000001; // 新 ASR 就绪过程共用原缓冲预算。
        f.controller.updateBargeInHandoff();
        assert(f.controller.failure_latched_.load());
        assert(sentCount(f, "speech.start") == 1 && f.controller.client_.sent_binary == 0);
    }
    {
        Fixture f;
        uint64_t offset = 0;
        confirmedInterruption(f, offset);
        f.controller.audio_.frontend_metrics.aec_active = false;
        responseCancelled(f);
        f.controller.updateBargeInHandoff();
        assert(sentCount(f, "speech.start") == 0 && f.controller.turn_.load() == 3);
        assert(f.controller.failure_latched_.load() || !f.controller.barge_in_handoff_.load());
    }
    {
        Fixture f;
        uint64_t offset = 0;
        confirmedInterruption(f, offset);
        // 有意耗尽真实有限缓冲；不能悄悄淘汰 PCM，
        // 从而允许缺少起音的新识别轮次继续。
        f.controller.automatic_.capacity_ = f.controller.automatic_.size();
        bufferedFrontend(f, offset, 1, true);
        f.controller.updateBargeInHandoff();
        assert(f.controller.failure_latched_.load() && sentCount(f, "speech.start") == 0);
    }
    {
        Fixture f;
        uint64_t offset = 0;
        confirmedInterruption(f, offset);
        offset += 320; // 缺失 AFE 帧不能被悄悄拼接掉。
        bufferedFrontend(f, offset, 1, true);
        f.controller.updateBargeInHandoff();
        assert(f.controller.failure_latched_.load() && sentCount(f, "speech.start") == 0);
    }
    {
        Fixture f;
        uint64_t offset = 0;
        confirmedInterruption(f, offset);
        clock_us += 600000; // 独立的真实前端断供保护。
        f.controller.updateBargeInHandoff();
        assert(f.controller.failure_latched_.load() && sentCount(f, "speech.start") == 0);
    }
    {
        Fixture f;
        armInterruption(f);
        uint64_t offset = 0;
        bufferedFrontend(f, offset, 20, false);
        bufferedFrontend(f, offset, 10, true);
        f.controller.audio_.stop_result = ESP_FAIL;
        f.controller.beginBargeInHandoff();
        assert(f.controller.failure_latched_.load() && sentCount(f, "speech.start") == 0);
    }
    {
        Fixture f;
        armInterruption(f);
        uint64_t offset = 0;
        bufferedFrontend(f, offset, 20, false);
        bufferedFrontend(f, offset, 10, true);
        VoiceClient::text_hook = [](const std::string &) { return ESP_FAIL; };
        f.controller.beginBargeInHandoff();
        assert(f.controller.failure_latched_.load() && sentCount(f, "speech.start") == 0);
    }
    {
        Fixture f;
        armInterruption(f);
        uint64_t offset = 0;
        bufferedFrontend(f, offset, 20, false);
        bufferedFrontend(f, offset, 10, true);
        f.controller.reply_revision_.fetch_add(1, std::memory_order_release);
        f.controller.beginBargeInHandoff();
        assert(!f.controller.barge_in_handoff_.load() && sentCount(f, "speech.start") == 0);
        assert(f.controller.audio_.stop_count == 0 && sentCount(f, "response.cancel") == 0);
    }
    {
        for (const char command : {'a', 'b', 'c', 'm', 'r', 'n'}) {
            Fixture f;
            armInterruption(f);
            oneCommand(f, command);
            assert(!f.controller.barge_in_interrupt_enabled_.load() && !f.status.bargeInEnabled());
            assert(!f.controller.barge_in_handoff_.load());
        }
    }
    {
        Fixture f;
        uint64_t offset = 0;
        confirmedInterruption(f, offset);
        oneCommand(f, 'r');
        assert(!f.controller.barge_in_interrupt_enabled_.load() && !f.controller.barge_in_handoff_.load());
        assert(f.controller.response_cancel_pending_.load() && f.controller.start_after_ready_);
        assert(f.controller.automatic_.size() == 0 && sentCount(f, "speech.start") == 0);
        responseCancelled(f);
        oneCommand(f, '?');
        assert(!f.controller.response_cancel_pending_.load() && f.controller.turn_.load() == 4);
        assert(sentCount(f, "speech.start") == 1 && !f.controller.automatic_turn_.load());
        assert(f.controller.client_.sent_binary == 0);
    }
    {
        Fixture f;
        uint64_t offset = 0;
        confirmedInterruption(f, offset);
        control(f.controller, "{\"type\":\"speech.cancelled\",\"session_epoch\":77,\"turn_id\":3,\"generation\":0}");
        control(f.controller, "{\"type\":\"asr.utterance_final\",\"session_epoch\":77,\"turn_id\":3,\"generation\":0,\"text\":\"old sealed utterance\"}");
        assert(f.status.capture() == VoiceCaptureState::Starting);
        assert(f.controller.response_cancel_pending_.load() && f.controller.barge_in_handoff_.load());
        assert(sentCount(f, "speech.start") == 0 && !f.controller.failure_latched_.load());
        responseCancelled(f);
        f.controller.updateBargeInHandoff();
        assert(f.controller.turn_.load() == 4 && sentCount(f, "speech.start") == 1);
    }
    {
        Fixture f;
        uint64_t offset = 0;
        confirmedInterruption(f, offset);
        responseCancelled(f);
        f.controller.updateBargeInHandoff();
        const char *ack = "{\"type\":\"speech.started\",\"session_epoch\":77,\"turn_id\":4,\"generation\":0}";
        control(f.controller, ack);
        control(f.controller, ack);
        const auto start = drainInputs(f);
        assert(start.size() == 1 && start[0].kind == InputKind::Start);
        runUplink(f, start);
        control(f.controller, ack);
        assert(drainInputs(f).empty() && sentCount(f, "audio.stream.start") == 1);
        assert(f.status.capture() == VoiceCaptureState::Recording && !f.controller.failure_latched_.load());
    }
    {
        Fixture f;
        uint64_t offset = 0;
        confirmedInterruption(f, offset);
        bufferedFrontend(f, offset, 35, false);
        const auto deadline = f.controller.automatic_trigger_us_ + 2000000;
        clock_us = deadline - 10000;
        responseCancelled(f);
        f.controller.updateBargeInHandoff();
        assert(sentCount(f, "speech.start") == 1 && !f.controller.failure_latched_.load());
        clock_us = deadline + 10000;
        f.controller.updateBargeInHandoff();
        assert(f.controller.failure_latched_.load() && f.controller.client_.sent_binary == 0);
        assert(sentCount(f, "speech.start") == 1);
    }
    {
        Fixture f;
        uint64_t offset = 0;
        confirmedInterruption(f, offset);
        bufferedFrontend(f, offset, 35, false);
        const auto deadline = f.controller.automatic_trigger_us_ + 2000000;
        clock_us = deadline - 10000;
        responseCancelled(f);
        f.controller.updateBargeInHandoff();
        control(f.controller, "{\"type\":\"speech.started\",\"session_epoch\":77,\"turn_id\":4,\"generation\":0}");
        const auto start = drainInputs(f);
        assert(start.size() == 1 && start[0].kind == InputKind::Start);
        // 工作任务可能在上一次控制节拍检查之后才获得调度。
        // 它自己的启动屏障也必须拒绝陈旧缓存语音。
        clock_us = deadline + 10000;
        runUplink(f, start);
        assert(f.controller.failure_latched_.load() && f.controller.client_.sent_binary == 0);
        assert(sentCount(f, "audio.stream.start") == 0 && !f.controller.accepting_capture_.load());
    }
    {
        // 发生硬件失败的 epoch 超过 INT32_MAX。使用这些值及 UINT32_MAX
        // 验证真实 cJSON/控制器路径，确保取消 ACK 不匹配或重复时仍精确隔离。
        for (const uint32_t epoch : {3090991631U, 2165675190U, UINT32_MAX}) {
            Fixture f;
            f.controller.epoch_.store(epoch);
            f.controller.turn_.store(3);
            f.controller.sealed_turn_ = 3;
            f.controller.response_cancel_target_ = {epoch, 3, 9};
            f.controller.response_cancel_pending_.store(true);
            const size_t start = logs.size();
            responseCancelled(f, epoch, 3, 8);
            assert(f.controller.response_cancel_pending_.load());
            assert(logs.substr(start).find("pending=1 match=0") != std::string::npos);
            responseCancelled(f, epoch, 3, 9);
            assert(!f.controller.response_cancel_pending_.load());
            assert(logs.substr(start).find("pending=1 match=1 target_epoch=" +
                std::to_string(epoch)) != std::string::npos);
            responseCancelled(f, epoch, 3, 9);
            assert(logs.substr(start).find("pending=0 match=0") != std::string::npos);
            assert(sentCount(f, "speech.start") == 0 && f.controller.turn_.load() == 3);
            assert(!f.controller.failure_latched_.load());
        }
    }
    {
        // 神经 VAD 对直流的误报不能停止真实扬声器或取消回答，
        // 即使 D2 已布防且总 RMS 很高也一样。
        Fixture f;
        armInterruption(f);
        std::array<int16_t, 320> pcm{};
        pcm.fill(400);
        f.controller.audio_.frontend_metrics.microphone_mean = 300;
        f.controller.audio_.frontend_metrics.microphone_ac_rms = 5;
        f.controller.audio_.frontend_metrics.reference_mean = -20;
        f.controller.audio_.frontend_metrics.reference_ac_rms = 150;
        for (uint64_t frame = 0; frame < 10; ++frame) {
            clock_us += 20000;
            f.controller.onFrontend(pcm.data(), pcm.size(), frame * pcm.size(), true);
            f.controller.updateBargeInObservation();
        }
        const auto s = f.controller.barge_in_.snapshot();
        assert(s.confirmations == 0 && s.energy_rejections == 1);
        assert(s.window_frames == 10 && s.window_ac_rms == 0);
        assert(f.controller.audio_.stop_count == 0 && f.controller.client_.sent.empty());
        assert(!f.controller.response_cancel_pending_.load() && !f.controller.barge_in_handoff_.load());
        // 延迟日志保留事件发生时的输入观测，而非之后的观测。
        f.controller.audio_.frontend_metrics.microphone_ac_rms = 999;
        const size_t start = logs.size();
        f.controller.logBargeInObservation();
        const auto observed = logs.substr(start);
        assert(observed.find("event=rejected_low_ac") != std::string::npos);
        assert(observed.find("epoch=77 turn=3 generation=9") != std::string::npos);
        assert(observed.find("frame_sample=2880 frame_mean=400 frame_rms=400 frame_ac_rms=0 frame_peak=400") != std::string::npos);
        assert(observed.find("window_ac_rms=0 window_frames=10 min_ac_rms=80") != std::string::npos);
        assert(observed.find("frame_stats_aligned=1") != std::string::npos);
        assert(observed.find("mic_mean=300 mic_ac_rms=5 ref_mean=-20 ref_ac_rms=150") != std::string::npos);
        assert(observed.find("mic_ref_output_sample_aligned=0") != std::string::npos);
        // 自动报告不得重放同一份缓存证据。
        // 明确的指标请求仍可重放最新事件。
        logs.clear();
        f.controller.logBargeInObservation();
        assert(logs.find("barge_in_energy") == std::string::npos);
        logs.clear();
        f.controller.logBargeInObservation(true);
        assert(logs.find("event=rejected_low_ac") != std::string::npos);
    }
    {
        // 门限评估整个 VAD 窗口，因此两个弱音节帧不会重置它。
        // 直流偏置不改变交流判断或 PCM。
        Fixture f;
        armInterruption(f);
        std::vector<std::array<int16_t, 320>> expected;
        for (uint64_t frame = 0; frame < 30; ++frame) {
            std::array<int16_t, 320> pcm{};
            for (size_t i = 0; i < pcm.size(); ++i)
                pcm[i] = static_cast<int16_t>(2000 + ((frame == 22 || frame == 25) ? 0 : (i % 2 ? 200 : -200)));
            expected.push_back(pcm);
            clock_us += 20000;
            f.controller.onFrontend(pcm.data(), pcm.size(), frame * pcm.size(), frame >= 20);
        }
        assert(f.controller.barge_in_.snapshot().confirmations == 1);
        assert(f.controller.barge_in_.snapshot().window_frames == 10);
        assert(f.controller.barge_in_.snapshot().window_ac_rms >= 178);
        assert(f.controller.audio_.stop_count == 0 && f.controller.client_.sent.empty());
        f.controller.beginBargeInHandoff();
        assert(f.controller.audio_.stop_count == 1 && sentCount(f, "response.cancel") == 1);
        assert(f.controller.automatic_.size() == expected.size());
        voiceaudio::AutomaticUtterance::Frame retained;
        for (size_t frame = 0; frame < expected.size(); ++frame) {
            assert(f.controller.automatic_.pop(retained));
            assert(retained.offset == frame * retained.pcm.size() && retained.pcm == expected[frame]);
            assert(retained.speech == (frame >= 20));
        }
    }
    {
        // 取消可在 AEC 仍健康时解除观测布防；
        // 诊断不能把该转换误报为 AEC 失败。
        Fixture f;
        observePlayback(f);
        f.controller.cancel_pending_.store(true);
        f.controller.updateBargeInObservation();
        assert(!f.controller.barge_in_enabled_ && f.status.bargeIn() == VoiceBargeInState::Unavailable);
        assert(f.controller.audio_.frontend_metrics.ready && f.controller.audio_.frontend_metrics.aec_active);
        assert(f.controller.audio_.frontend_metrics.last_error == ESP_OK);
        assert(std::string(f.controller.barge_in_reason_) == "speech_cancel_pending");
        assert(f.controller.audio_.stop_count == 0 && f.controller.client_.sent.empty());
    }
    {
        // 在确认事件发生时冻结有界顺序源证据；
        // 诊断证据不能否决语音，也不能替换已保留 PCM。
        Fixture f;
        armInterruption(f);
        auto &source = f.controller.audio_.source_window;
        source.valid = true;
        source.covered_first = 4800;
        source.covered_end = 9600;
        source.available_end = 10240;
        source.frames = 15;
        source.samples = 4800;
        source.raw = {272, 274, 35, 354, 51};
        source.dc = {1, 34, 34, 90, 49};
        source.reference = {13, 179, 178, 617, 300};
        std::array<int16_t, 320> pcm{};
        for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = i % 2 ? 200 : -200;
        for (uint64_t frame = 0; frame < 30; ++frame) {
            clock_us += 20000;
            f.controller.onFrontend(pcm.data(), pcm.size(), frame * pcm.size(), frame >= 20);
        }
        assert(f.controller.barge_in_.snapshot().confirmations == 1);
        assert(f.controller.audio_.source_requested_first == 6400 - 1536);
        assert(f.controller.audio_.source_requested_end == 9600);
        source.raw.ac_rms = 999;
        const size_t start = logs.size();
        f.controller.logBargeInObservation();
        const auto observed = logs.substr(start);
        assert(observed.find("barge_in_source event=confirmed") != std::string::npos);
        assert(observed.find("valid=1 requested_first=4864 requested_end=9600") != std::string::npos);
        assert(observed.find("raw_mean=272 raw_rms=274 raw_ac=35 raw_peak=354 raw_frame_ac_max=51") != std::string::npos);
        assert(observed.find("dc_mean=1 dc_rms=34 dc_ac=34") != std::string::npos);
        assert(observed.find("ordinal_mapping=1 lag_calibrated=0 waveform_aligned=0 diagnostic_only=1") != std::string::npos);
        assert(f.controller.audio_.stop_count == 0 && f.controller.client_.sent.empty());
    }
    {
        // 源覆盖缺失仍表示未知；既不伪造静音，
        // 也不改变既有输出/VAD 确认路径。
        Fixture f;
        armInterruption(f);
        std::array<int16_t, 320> pcm{};
        for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = i % 2 ? 200 : -200;
        for (uint64_t frame = 0; frame < 10; ++frame) {
            clock_us += 20000;
            f.controller.onFrontend(pcm.data(), pcm.size(), frame * pcm.size(), true);
        }
        assert(f.controller.audio_.source_requested_first == 0);
        assert(!f.controller.barge_in_confirmed_energy_.source_window.valid);
        assert(f.controller.barge_in_.snapshot().confirmations == 1);
        f.controller.beginBargeInHandoff();
        assert(f.controller.audio_.stop_count == 1 && sentCount(f, "response.cancel") == 1);
    }
    {
        // 事件类型及回答身份的每个字段都参与自动去重。
        // 相同时间戳不能隐藏其他事件类型或回答。
        Fixture f;
        const auto rows = [](const std::string &prefix) {
            size_t count = 0, position = 0;
            while ((position = logs.find(prefix, position)) != std::string::npos) {
                ++count;
                position += prefix.size();
            }
            return count;
        };
        auto &candidate = f.controller.barge_in_candidate_energy_;
        candidate.valid = true;
        candidate.at_us = 123456;
        candidate.reply = {77, 3, 9};
        f.controller.barge_in_confirmed_energy_ = candidate;
        f.controller.barge_in_rejected_energy_ = candidate;
        logs.clear();
        f.controller.logBargeInObservation();
        assert(rows("barge_in_observation ") == 1);
        assert(rows("barge_in_energy ") == 3 && rows("barge_in_source ") == 3);
        for (const char *event : {"candidate", "confirmed", "rejected_low_ac"})
            assert(rows(std::string("barge_in_energy event=") + event + " ") == 1);

        logs.clear();
        f.controller.logBargeInObservation();
        assert(rows("barge_in_observation ") == 1);
        assert(rows("barge_in_energy ") == 0 && rows("barge_in_source ") == 0);
        // 解除布防清除观测作用域，不清除日志标记。
        // 该阶段转换不能重放其旧的最新快照。
        f.controller.setBargeInObservation(false);
        clock_us += 500000;
        f.controller.updateBargeInObservation();
        assert(rows("barge_in_observation ") == 2 && rows("barge_in_energy ") == 0);

        for (const voice::Identity reply : {voice::Identity{78, 3, 9}, {78, 4, 9}, {78, 4, 10}}) {
            candidate.reply = reply;
            logs.clear();
            f.controller.logBargeInObservation();
            assert(rows("barge_in_energy event=candidate ") == 1);
            assert(rows("barge_in_energy ") == 1 && rows("barge_in_source ") == 1);
        }
        ++candidate.at_us;
        logs.clear();
        f.controller.logBargeInObservation();
        assert(rows("barge_in_energy event=candidate ") == 1);
        // 实际手动命令有意重放三个缓存事件，
        // 但不让它们在自动报告中重新被视为新事件。
        logs.clear();
        f.controller.command('?');
        assert(rows("barge_in_energy ") == 3 && rows("barge_in_source ") == 3);
        logs.clear();
        f.controller.logBargeInObservation();
        assert(rows("barge_in_energy ") == 0 && rows("barge_in_source ") == 0);
    }
    {
        // 即使 VAD 未创建候选或 AEC 随后故障，仍保留整轮回答证据。
        // 一次 fetch 产生的两个 20 ms 输出属于同一个 VAD 窗口。
        Fixture f;
        armInterruption(f);
        auto &metrics = f.controller.audio_.frontend_metrics;
        metrics.vad_thresholds_valid = true;
        metrics.vad_energy_threshold_cdbfs = -6000;
        metrics.vad_input_energy_valid = true;
        metrics.vad_input_energy_cdbfs = -6700;
        metrics.vad_input_energy_frames = 1;
        metrics.microphone_ac_rms = 70;
        metrics.microphone_peak = 400;
        std::array<int16_t, 320> pcm{};
        for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = i % 2 ? 40 : -40;
        f.controller.onFrontend(pcm.data(), pcm.size(), 0, false);
        f.controller.onFrontend(pcm.data(), pcm.size(), 320, false);
        assert(f.controller.barge_in_signal_.output_frames == 2);
        assert(f.controller.barge_in_signal_.vad_windows == 1);
        assert(f.controller.barge_in_signal_.vad_above_floor == 0);
        metrics.vad_input_energy_frames = 2;
        metrics.vad_input_energy_cdbfs = -5000;
        f.controller.onFrontend(pcm.data(), pcm.size(), 640, false);
        metrics.vad_input_energy_valid = false;
        metrics.vad_input_energy_invalid_frames = 1;
        f.controller.onFrontend(pcm.data(), pcm.size(), 960, false);
        assert(f.controller.barge_in_.snapshot().candidates == 0);
        assert(f.controller.barge_in_signal_.vad_windows == 2);
        assert(f.controller.barge_in_signal_.vad_above_floor == 1);
        assert(f.controller.barge_in_signal_.vad_energy_peak_cdbfs == -5000);
        assert(f.controller.barge_in_signal_.output_ac_peak == 40);
        assert(f.controller.barge_in_signal_.microphone_ac_peak == 70);
        metrics.aec_active = false;
        f.controller.updateBargeInObservation();
        logs.clear();
        f.controller.command('?');
        assert(logs.find("output_frames=4 speech_frames=0 output_ac_peak=40") != std::string::npos);
        assert(logs.find("vad_windows=2 vad_above_floor=1 vad_peak_cdbfs=-5000") != std::string::npos);
        // 另一个回答从空聚合开始；空闲麦克风/启动峰值
        // 不会泄漏到该回答的观测。
        metrics.aec_active = true;
        f.controller.reply_ = {77, 3, 10};
        f.controller.setBargeInObservation(true);
        f.controller.updateBargeInObservation();
        assert(f.controller.barge_in_signal_.output_frames == 0);
        assert(f.controller.barge_in_signal_.vad_windows == 0);
    }
    {
        // 生产空闲起音配置：脉冲后的 VAD 拖尾必须留在本地，
        // 不得触发 speech.start、上传 PCM 或产生空 ASR 往返。
        // 上面的既有握手测试单独隔离单帧起音。
        Fixture f;
        f.controller.automatic_.configure(f.automatic_storage.data(), f.automatic_storage.size(), 20, 35,
            (CONFIG_VOICE_AUTO_START_CONFIRM_MS + 19) / 20, CONFIG_VOICE_AUTO_START_MIN_AC_RMS);
        arm(f);
        uint64_t offset = 0;
        int64_t last_callback_us = 0;
        std::vector<voiceaudio::AutomaticUtterance::Frame> source;
        const auto append = [&](bool speech, int amplitude) {
            std::array<int16_t, 320> pcm{}, raw{};
            for (size_t i = 0; i < pcm.size(); ++i)
                pcm[i] = static_cast<int16_t>(1000 + (i % 2 ? amplitude : -amplitude));
            source.push_back({pcm, offset, speech});
            f.controller.onCapture(raw.data(), raw.size());
            clock_us += 20000;
            f.controller.onFrontend(pcm.data(), pcm.size(), offset, speech);
            last_callback_us = clock_us;
            offset += pcm.size();
            f.controller.updateAutomatic();
        };
        for (size_t i = 0; i < 20; ++i) append(false, 0);
        for (size_t i = 0; i < 20; ++i) append(true, i == 0 ? 2000 : 0);
        assert(sentCount(f, "speech.start") == 0 && f.controller.client_.sent.empty());
        assert(f.controller.turn_.load() == 0 && drainInputs(f).empty());
        assert(f.status.capture() == VoiceCaptureState::Idle && f.status.listening());
        assert(f.controller.automatic_trigger_us_ == 0 && !f.controller.automatic_buffer_failed_.load());
        auto onset = f.controller.automatic_.startMetrics();
        assert(onset.candidates == 1 && onset.energy_rejections == 1 && onset.confirmations == 0);
        assert(onset.energy_active_frames == 1 && onset.energy_tail_active_frames == 0);
        assert(f.controller.automatic_.size() == 20 && !onset.pending);
        const size_t log_start = logs.size();
        f.controller.logFrontendMetrics();
        assert(logs.substr(log_start).find("auto_start confirm_ms=160 min_ac_rms=80 candidates=1 confirmations=0") != std::string::npos);

        for (size_t i = 0; i < 15; ++i) append(false, 0);
        const size_t first = source.size() - 20;
        for (size_t i = 0; i < 8; ++i) {
            // 保留真实的微弱起始音素及窗口内弱帧；
            // 同窗口内足够的后续活动只确认一次。
            append(true, i < 3 || i == 6 ? 20 : 200);
            assert(sentCount(f, "speech.start") == (i == 7 ? 1U : 0U));
        }
        assert(f.controller.automatic_trigger_us_ == last_callback_us);
        assert(f.status.capture() == VoiceCaptureState::Starting && f.controller.turn_.load() == 1);
        assert(drainInputs(f).empty()); // 仍须等待真实云端 ACK。
        assert(f.controller.automatic_.size() == 28);
        started(f);
        std::vector<Input> uploaded;
        pump(f, uploaded, 29);
        assert(uploaded.size() == 28);
        for (size_t i = 0; i < uploaded.size(); ++i) {
            assert(uploaded[i].kind == InputKind::Audio && uploaded[i].pcm == source[first + i].pcm);
        }
        assert(sentCount(f, "speech.start") == 1 && !f.controller.failure_latched_.load());
    }
    {
        // 已确认的 D2 使用 adoptConfirmed；不得再加一道起音门限、
        // 丢失预录，或重新开始共享 ACK 预算。
        Fixture f;
        f.controller.automatic_.configure(f.automatic_storage.data(), f.automatic_storage.size(), 20, 35,
            (CONFIG_VOICE_AUTO_START_CONFIRM_MS + 19) / 20, CONFIG_VOICE_AUTO_START_MIN_AC_RMS);
        armInterruption(f);
        for (uint64_t i = 0; i < 10; ++i) frontend(f, i * 320, true, false);
        f.controller.beginBargeInHandoff();
        assert(f.controller.audio_.stop_count == 1 && sentCount(f, "response.cancel") == 1);
        assert(f.controller.automatic_.phase() == Phase::Opening && f.controller.automatic_.size() == 10);
        assert(f.controller.automatic_.startMetrics().candidates == 0);
        assert(!f.controller.automatic_.startMetrics().pending);
    }
    {
        // 完整文本模式在音频前发布一份不可变回答。
        // 后续时间戳批次仅移动 DMA 游标，绝不改变页的源文本、修订号或对话身份。
        Fixture f;
        f.controller.turn_.store(3);
        control(f.controller, R"({"type":"response.started","session_epoch":77,"turn_id":3,"generation":9,"text_mode":"complete_before_audio"})");
        delta(f, "**first** sentence. ");
        delta(f, "second sentence.");
        assert(conversation(f).empty() && f.controller.response_text_buffering_);
        const auto pending_revision = f.status.conversationRevision();
        control(f.controller, R"({"type":"llm.done","session_epoch":77,"turn_id":3,"generation":9})");
        assert(conversation(f) == "**first** sentence. second sentence.");
        assert(f.status.conversationRevision() != pending_revision && f.controller.response_text_committed_);
        const auto committed_revision = f.status.conversationRevision();
        const auto committed_id = f.status.conversationId();
        control(f.controller, R"({"type":"audio.stream.start","session_epoch":77,"turn_id":3,"generation":9,"stream_id":12,"codec":"opus","sample_rate_hz":24000,"channels":1,"frame_duration_ms":20,"pre_skip_samples":0,"subtitle_mode":"response_word_timestamps"})");
        assert(f.controller.subtitle_timeline_.enabled && f.controller.subtitle_timeline_.fixed_response_text);
        control(f.controller, R"({"type":"audio.subtitle","session_epoch":77,"turn_id":3,"generation":9,"stream_id":12,"sequence":0,"text_offset":0,"text":"","words":[{"text":"first","start_sample":0,"end_sample":1000,"text_byte_offset":2}]})");
        f.controller.audio_.playback_progress = {{77, 3, 9}, 12, 500, true, true, true};
        f.controller.updateSubtitleProgress();
        assert(f.status.subtitlePosition().active && f.status.subtitlePosition().text_byte_offset == 2);
        assert(f.status.subtitlePosition().word_end_byte_offset == 7 && f.status.subtitlePosition().word_progress_permille == 500);
        assert(f.status.conversationRevision() == committed_revision && f.status.conversationId() == committed_id);
        control(f.controller, R"({"type":"audio.subtitle","session_epoch":77,"turn_id":3,"generation":9,"stream_id":12,"sequence":1,"text_offset":0,"text":"","words":[{"text":"second","start_sample":2000,"end_sample":3000,"text_byte_offset":20}]})");
        assert(!f.controller.subtitle_timeline_.unavailable);
        delta(f, "late invalid extra"); // 已提交正文不能继续增长。
        control(f.controller, R"({"type":"llm.done","session_epoch":77,"turn_id":3,"generation":9})");
        assert(f.status.conversationRevision() == committed_revision && f.status.conversationId() == committed_id);
        control(f.controller, R"({"type":"audio.subtitle.unavailable","session_epoch":77,"turn_id":3,"generation":9,"stream_id":12})");
        assert(!f.status.subtitlePosition().active && conversation(f) == "**first** sentence. second sentence.");
        assert(f.status.conversationRevision() == committed_revision && f.status.conversationId() == committed_id);
        // 重新启动流保留同一份完整正文及手动翻页身份，
        // 仅重置播放时间线。
        control(f.controller, R"({"type":"audio.stream.start","session_epoch":77,"turn_id":3,"generation":9,"stream_id":13,"codec":"opus","sample_rate_hz":24000,"channels":1,"frame_duration_ms":20,"pre_skip_samples":0,"subtitle_mode":"response_word_timestamps"})");
        assert(f.controller.subtitle_timeline_.enabled && !f.controller.subtitle_timeline_.unavailable);
        assert(f.status.conversationId() == committed_id);
        control(f.controller, R"({"type":"audio.subtitle","session_epoch":77,"turn_id":3,"generation":9,"stream_id":13,"sequence":0,"text_offset":0,"text":"replacement","words":[]})");
        assert(f.controller.subtitle_timeline_.unavailable && f.controller.audio_.stop_count == 0);
        assert(f.status.conversationId() == committed_id && f.status.conversationRevision() == committed_revision);
    }
    {
        // 提前开始的流不能取得尚未提交的正文。
        // 音频仍可运行，llm.done 可在稍后补全可读文本。
        Fixture f;
        f.controller.turn_.store(3);
        control(f.controller, R"({"type":"response.started","session_epoch":77,"turn_id":3,"generation":9,"text_mode":"complete_before_audio"})");
        delta(f, "retained answer");
        control(f.controller, R"({"type":"audio.stream.start","session_epoch":77,"turn_id":3,"generation":9,"stream_id":12,"codec":"opus","sample_rate_hz":24000,"channels":1,"frame_duration_ms":20,"pre_skip_samples":0,"subtitle_mode":"response_word_timestamps"})");
        assert(!f.controller.subtitle_timeline_.enabled && !f.controller.failure_latched_.load());
        assert(conversation(f).empty());
        control(f.controller, R"({"type":"llm.done","session_epoch":77,"turn_id":3,"generation":9})");
        assert(conversation(f) == "retained answer");
    }
    {
        // 合成错误保留已提交回答。生成错误则显示已保留的部分文本，
        // 但不启动播放。
        for (bool complete : {false, true}) {
            Fixture f;
            f.controller.turn_.store(3);
            control(f.controller, R"({"type":"response.started","session_epoch":77,"turn_id":3,"generation":9,"text_mode":"complete_before_audio"})");
            delta(f, "still readable");
            if (complete) control(f.controller, R"({"type":"llm.done","session_epoch":77,"turn_id":3,"generation":9})");
            control(f.controller, complete ? R"({"type":"error","session_epoch":77,"turn_id":3,"generation":9,"code":"tts_failed"})" :
                                            R"({"type":"error","session_epoch":77,"turn_id":3,"generation":9,"code":"response_failed"})");
            assert(conversation(f) == "still readable" && f.status.response() == VoiceResponseState::Failed);
            assert(!f.controller.failure_latched_.load());
        }
    }
    {
        // UTF-8 有界缓存与普通显示存储提交相同的永久截断前缀，
        // 包括四字节码点边界。
        Fixture f;
        f.controller.turn_.store(3);
        control(f.controller, R"({"type":"response.started","session_epoch":77,"turn_id":3,"generation":9,"text_mode":"complete_before_audio"})");
        const std::string retained(VoiceAssistantStatus::kConversationCapacity - 3, 'a');
        for (size_t offset = 0; offset < retained.size(); offset += 512) delta(f, retained.substr(offset, 512));
        delta(f, "\xF0\x9F\x98\x80");
        control(f.controller, R"({"type":"llm.done","session_epoch":77,"turn_id":3,"generation":9})");
        assert(conversation(f) == retained && f.status.conversationTruncated());
    }
    {
        // 真实 JSON 解析、回答绑定及按样本推进的字幕游标。
        Fixture f;
        f.controller.turn_.store(3);
        f.controller.reply_ = {77, 3, 9};
        f.status.setResponse(VoiceResponseState::Generating);
        control(f.controller, "{\"type\":\"audio.stream.start\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"codec\":\"opus\",\"sample_rate_hz\":24000,\"channels\":1,\"frame_duration_ms\":20,\"pre_skip_samples\":0,\"subtitle_mode\":\"word_timestamps\"}");
        assert(f.controller.subtitle_timeline_.enabled && !f.status.subtitlePosition().active);
        const char *cue = "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"sequence\":0,\"text_offset\":0,\"text\":\"one two\",\"words\":[{\"text\":\"one\",\"start_sample\":0,\"end_sample\":1000,\"text_byte_offset\":0},{\"text\":\"two\",\"start_sample\":2000,\"end_sample\":3000,\"text_byte_offset\":4}]}";
        control(f.controller, cue);
        assert(f.controller.subtitle_timeline_.word_count == 2);
        char content[64]{};
        f.status.copyConversation(content, sizeof(content));
        assert(std::string(content) == "one two");
        const auto conversation = f.status.conversationId();
        control(f.controller, cue); // 重复消息不得追加文本或词。
        assert(f.status.conversationId() == conversation && f.controller.subtitle_timeline_.word_count == 2);
        f.controller.audio_.playback_progress = {{77, 3, 9}, 12, 0, true, true, true};
        f.controller.updateSubtitleProgress();
        assert(f.status.subtitlePosition().active && f.status.subtitlePosition().text_byte_offset == 0);
        assert(f.status.subtitlePosition().word_end_byte_offset == 3 &&
               f.status.subtitlePosition().word_start_sample == 0 &&
               f.status.subtitlePosition().word_end_sample == 1000 &&
               f.status.subtitlePosition().word_progress_permille == 0 &&
               f.status.subtitlePosition().next_word_byte_offset == 4 &&
               f.status.subtitlePosition().next_word_progress_permille == 0);
        f.controller.audio_.playback_progress.completed_samples = 500;
        f.controller.updateSubtitleProgress();
        assert(f.status.subtitlePosition().text_byte_offset == 0 &&
               f.status.subtitlePosition().word_progress_permille == 500 &&
               f.status.subtitlePosition().playback_sample == 500 &&
               f.status.subtitlePosition().next_word_progress_permille == 250);
        const auto held_revision = f.status.subtitlePosition().revision;
        clock_us += 1000000; // 仅实际经过时间不能让欠载字幕滚动。
        f.controller.updateSubtitleProgress();
        assert(f.status.subtitlePosition().revision == held_revision);
        f.controller.audio_.playback_progress.completed_samples = 1500;
        const auto revision = f.status.subtitlePosition().revision;
        f.controller.updateSubtitleProgress();
        assert(f.status.subtitlePosition().revision != revision &&
               f.status.subtitlePosition().word_progress_permille == 1000 &&
               f.status.subtitlePosition().next_word_progress_permille == 750);
        f.controller.audio_.playback_progress.completed_samples = 2000;
        f.controller.updateSubtitleProgress();
        assert(f.status.subtitlePosition().text_byte_offset == 4 && f.controller.subtitle_timeline_.word_count == 0);
        assert(f.status.subtitlePosition().word_end_byte_offset == 7 &&
               f.status.subtitlePosition().word_progress_permille == 0 &&
               f.status.subtitlePosition().next_word_byte_offset == UINT32_MAX);
        control(f.controller, "{\"type\":\"llm.delta\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"text\":\"raw markdown\"}");
        f.status.copyConversation(content, sizeof(content));
        assert(std::string(content) == "one two");
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"sequence\":1,\"text_offset\":7,\"text\":\" three\",\"words\":[{\"start_sample\":4000,\"end_sample\":5000,\"text_byte_offset\":8}]}");
        f.status.copyConversation(content, sizeof(content));
        assert(std::string(content) == "one two three" && f.status.conversationId() == conversation);
        control(f.controller, "{\"type\":\"audio.subtitle.unavailable\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12}");
        assert(!f.status.subtitlePosition().active && f.controller.subtitle_timeline_.unavailable);
        f.controller.audio_.playback_progress.completed_samples = 4500;
        f.controller.updateSubtitleProgress();
        assert(f.status.subtitlePosition().text_byte_offset == 4 && !f.controller.failure_latched_.load());
        f.controller.interrupt();
        assert(!f.controller.subtitle_timeline_.enabled && !f.status.subtitlePosition().active);
    }
    {
        Fixture f;
        f.controller.turn_.store(3);
        f.controller.reply_ = {77, 3, 9};
        f.status.setResponse(VoiceResponseState::Generating);
        control(f.controller, "{\"type\":\"audio.stream.start\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"codec\":\"opus\",\"sample_rate_hz\":24000,\"channels\":1,\"frame_duration_ms\":20,\"pre_skip_samples\":0,\"subtitle_mode\":\"word_timestamps\"}");
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":76,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"sequence\":0,\"text_offset\":0,\"text\":\"old\",\"words\":[]}");
        assert(f.controller.subtitle_timeline_.next_sequence == 0 && !f.controller.subtitle_timeline_.unavailable);
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"sequence\":1,\"text_offset\":0,\"text\":\"gap\",\"words\":[]}");
        assert(f.controller.subtitle_timeline_.unavailable && !f.controller.failure_latched_.load());
        assert(f.controller.audio_.stop_count == 0);
        // 新流重置元数据；嵌入 NUL 会被真实 JSON 解析边界拒绝，
        // 但不停止音频流。
        control(f.controller, "{\"type\":\"audio.stream.start\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":13,\"codec\":\"opus\",\"sample_rate_hz\":24000,\"channels\":1,\"frame_duration_ms\":20,\"pre_skip_samples\":0,\"subtitle_mode\":\"word_timestamps\"}");
        assert(f.controller.subtitle_timeline_.enabled && !f.controller.subtitle_timeline_.unavailable);
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":13,\"sequence\":0,\"text_offset\":0,\"text\":\"bad\\u0000text\",\"words\":[]}");
        assert(f.controller.subtitle_timeline_.unavailable && !f.controller.failure_latched_.load());
    }
    {
        // 真实提供方元数据可在全部词时间戳到达前显示完整文本。
        // 后续仅含时序的批次引用先前 UTF-8 字节，
        // 不会追加重复文本，也不会禁用跟随。
        Fixture f;
        subtitleReply(f);
        delta(f, "raw answer");
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"sequence\":0,\"text_offset\":0,\"text\":\"\\u4f60\\u597d\",\"words\":[{\"start_sample\":0,\"end_sample\":1000,\"text_byte_offset\":0}]}");
        const auto id = f.status.conversationId();
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"sequence\":1,\"text_offset\":6,\"text\":\"\",\"words\":[{\"start_sample\":2000,\"end_sample\":3000,\"text_byte_offset\":3}]}");
        assert(f.controller.subtitle_timeline_.next_sequence == 2 && !f.controller.subtitle_timeline_.unavailable);
        assert(conversation(f) == "\xE4\xBD\xA0\xE5\xA5\xBD" && f.status.conversationId() == id);
        // 不同流、重复的坏文本及陈旧时钟快照
        // 不能禁用或推进本回答的活动游标。
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":11,\"sequence\":2,\"text_offset\":6,\"text\":\"bad\\u0000old\",\"words\":[]}");
        assert(!f.controller.subtitle_timeline_.unavailable);
        f.controller.audio_.playback_progress = {{77, 3, 9}, 11, 9999, true, true, true};
        f.controller.updateSubtitleProgress();
        assert(!f.status.subtitlePosition().active);
        f.controller.audio_.playback_progress = {{77, 3, 9}, 12, 2000, true, false, true};
        f.controller.updateSubtitleProgress();
        assert(!f.status.subtitlePosition().active);
        f.controller.audio_.playback_progress.coherent = true;
        f.controller.updateSubtitleProgress();
        assert(f.status.subtitlePosition().active && f.status.subtitlePosition().text_byte_offset == 3);
        assert(f.status.subtitlePosition().word_end_byte_offset == 6);
        // 落在已显示码点中间的偏移仍无效，
        // 但只使显示跟随失败，语音继续运行。
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"sequence\":2,\"text_offset\":6,\"text\":\"\",\"words\":[{\"start_sample\":4000,\"end_sample\":5000,\"text_byte_offset\":4}]}");
        assert(f.controller.subtitle_timeline_.unavailable && !f.status.subtitlePosition().active);
        assert(conversation(f) == "raw answer" && f.controller.audio_.stop_count == 0 && !f.controller.failure_latched_.load());
    }
    {
        // 累计提供方元数据追上进度时，一个词范围可能跨越
        // 早先批次保留的文本和新追加后缀。校验须比较两部分，不能重复文本。
        Fixture f;
        subtitleReply(f);
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"sequence\":0,\"text_offset\":0,\"text\":\"\\u4f60\",\"words\":[]}");
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"sequence\":1,\"text_offset\":3,\"text\":\"\\u597d\",\"words\":[{\"text\":\"\\u4f60\\u597d\",\"start_sample\":0,\"end_sample\":1000,\"text_byte_offset\":0}]}");
        f.controller.audio_.playback_progress = {{77, 3, 9}, 12, 750, true, true, true};
        f.controller.updateSubtitleProgress();
        const auto position = f.status.subtitlePosition();
        assert(position.active && position.text_byte_offset == 0 &&
               position.word_end_byte_offset == 6 && position.word_progress_permille == 750);
        assert(conversation(f) == "\xE4\xBD\xA0\xE5\xA5\xBD");
    }
    {
        // 当前生产批次携带精确词文本，而非只有下一个锚点。
        // 其不可变范围排除标点，在当前时段内后续文本到达时仍保持稳定。
        Fixture f;
        subtitleReply(f);
        delta(f, "raw backup");
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"sequence\":0,\"text_offset\":0,\"text\":\"\\u4f60\\u597d, \",\"words\":[{\"text\":\"\\u4f60\\u597d\",\"start_sample\":0,\"end_sample\":1000,\"text_byte_offset\":0}]}");
        f.controller.audio_.playback_progress = {{77, 3, 9}, 12, 500, true, true, true};
        f.controller.updateSubtitleProgress();
        const auto before = f.status.subtitlePosition();
        assert(before.word_end_byte_offset == 6 && before.word_progress_permille == 500);
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"sequence\":1,\"text_offset\":8,\"text\":\"world\",\"words\":[{\"text\":\"world\",\"start_sample\":2000,\"end_sample\":3000,\"text_byte_offset\":8}]}");
        f.controller.updateSubtitleProgress();
        const auto after = f.status.subtitlePosition();
        assert(after.text_byte_offset == before.text_byte_offset &&
               after.word_end_byte_offset == before.word_end_byte_offset &&
               after.word_progress_permille == before.word_progress_permille);
        // 文本/偏移不匹配仅是显示失败；不能把错误字形范围
        // 绑定到健康播放，也不能丢失原始回答。
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"sequence\":2,\"text_offset\":13,\"text\":\"!\",\"words\":[{\"text\":\"wrong\",\"start_sample\":4000,\"end_sample\":5000,\"text_byte_offset\":8}]}");
        assert(f.controller.subtitle_timeline_.unavailable && !f.status.subtitlePosition().active);
        assert(conversation(f) == "raw backup" && f.controller.audio_.stop_count == 0);
    }
    {
        // 部分字幕提示之后元数据失败时，恢复全部原始 LLM 增量，
        // 包括规范化文本占有显示期间隐藏的增量。
        Fixture f;
        subtitleReply(f);
        delta(f, "first ");
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"sequence\":0,\"text_offset\":0,\"text\":\"normalized\",\"words\":[{\"start_sample\":0,\"end_sample\":1000,\"text_byte_offset\":0}]}");
        delta(f, "second ");
        assert(conversation(f) == "normalized");
        control(f.controller, "{\"type\":\"audio.subtitle.unavailable\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12}");
        assert(conversation(f) == "first second ");
        const auto id = f.status.conversationId();
        delta(f, "third");
        assert(conversation(f) == "first second third" && f.status.conversationId() == id);
        assert(!f.controller.failure_latched_.load() && f.controller.audio_.stop_count == 0);
    }
    {
        // 即使尚无原始 LLM 增量，规范化字幕提示也须在元数据失败后保留。
        // 迟到提示还会立即使用当前 DMA 时钟，不必等待下一控制周期。
        Fixture f;
        subtitleReply(f);
        f.status.setPlayback(VoicePlaybackState::Playing);
        f.controller.audio_.playback_progress = {{77, 3, 9}, 12, 500, true, true, true};
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"sequence\":0,\"text_offset\":0,\"text\":\"normalized text\",\"words\":[{\"text\":\"normalized\",\"start_sample\":0,\"end_sample\":1000,\"text_byte_offset\":0}]}");
        const auto position = f.status.subtitlePosition();
        assert(position.active && position.text_byte_offset == 0 &&
               position.word_progress_permille == 500);
        control(f.controller, "{\"type\":\"audio.subtitle.unavailable\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12}");
        assert(conversation(f) == "normalized text");
        assert(f.controller.response_text_replace_pending_);
        delta(f, "raw answer");
        assert(conversation(f) == "raw answer");
        assert(!f.controller.response_text_replace_pending_);
        delta(f, " continues");
        assert(conversation(f) == "raw answer continues");
        assert(!f.controller.failure_latched_.load() && f.controller.audio_.stop_count == 0);
    }
    for (const char *failure : {
            "{\"type\":\"error\",\"code\":\"tts_failed\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9}",
            "{\"type\":\"response.done\",\"reason\":\"tts_failed\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9}"}) {
        Fixture f;
        subtitleReply(f);
        delta(f, "complete LLM answer");
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"sequence\":0,\"text_offset\":0,\"text\":\"partial\",\"words\":[{\"start_sample\":0,\"end_sample\":1000,\"text_byte_offset\":0}]}");
        control(f.controller, failure);
        assert(conversation(f) == "complete LLM answer" && !f.status.subtitlePosition().active);
        assert(f.status.response() == VoiceResponseState::Failed && !f.controller.failure_latched_.load());
    }
    {
        // 播放完成命令可能早于迟到 LLM 内容。
        // 清除其时间线不能把原始 Markdown 追加到规范化文本中。
        Fixture f;
        subtitleReply(f);
        delta(f, "raw prefix");
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"sequence\":0,\"text_offset\":0,\"text\":\"normalized\",\"words\":[{\"start_sample\":0,\"end_sample\":1000,\"text_byte_offset\":0}]}");
        f.controller.onPlayed({77, 3, 9});
        oneCommand(f, '\0');
        assert(f.controller.playback_done_ && !f.status.subtitlePosition().active);
        delta(f, " and late tail");
        assert(conversation(f) == "normalized");
        // 同回答的替换流保留原始备份；首个字幕提示
        // 开启新的规范化字节布局，不重复文本。
        const char *replacement = "{\"type\":\"audio.stream.start\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":13,\"codec\":\"opus\",\"sample_rate_hz\":24000,\"channels\":1,\"frame_duration_ms\":20,\"pre_skip_samples\":0,\"subtitle_mode\":\"word_timestamps\"}";
        control(f.controller, replacement);
        assert(conversation(f) == "raw prefix and late tail");
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":13,\"sequence\":0,\"text_offset\":0,\"text\":\"new normalized\",\"words\":[{\"start_sample\":0,\"end_sample\":1000,\"text_byte_offset\":0}]}");
        control(f.controller, "{\"type\":\"audio.subtitle.unavailable\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":13}");
        assert(conversation(f) == "raw prefix and late tail");
    }
    {
        // 可选 PSRAM 分配失败不影响普通 LLM 翻页和语音。
        Fixture f;
        fail_subtitle_text_allocation = true;
        subtitleReply(f);
        fail_subtitle_text_allocation = false;
        assert(!f.controller.subtitle_timeline_.enabled && !f.controller.subtitle_text_);
        delta(f, "ordinary answer");
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"sequence\":0,\"text_offset\":0,\"text\":\"ignored normalized\",\"words\":[]}");
        assert(conversation(f) == "ordinary answer" && !f.controller.failure_latched_.load());
        assert(f.controller.audio_.begin_count == 1 && f.controller.audio_.stop_count == 0);
    }
    {
        // 完整原始备份在规范化替换及元数据回退过程中，
        // 保留完整 UTF-8 前缀和截断状态。
        Fixture f;
        subtitleReply(f);
        const std::string prefix(VoiceAssistantStatus::kConversationCapacity - 2, 'a');
        delta(f, prefix.substr(0, 6000));
        delta(f, prefix.substr(6000));
        control(f.controller, "{\"type\":\"audio.subtitle\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12,\"sequence\":0,\"text_offset\":0,\"text\":\"short subtitle\",\"words\":[{\"start_sample\":0,\"end_sample\":1000,\"text_byte_offset\":0}]}");
        delta(f, "\xE4\xBD\xA0");
        delta(f, "must stay clipped");
        control(f.controller, "{\"type\":\"audio.subtitle.unavailable\",\"session_epoch\":77,\"turn_id\":3,\"generation\":9,\"stream_id\":12}");
        assert(conversation(f) == prefix && f.status.conversationTruncated());
        delta(f, "later");
        assert(conversation(f) == prefix);
    }
    std::puts("Passed production conversation lifecycle scenarios, including DMA subtitle timing, UTF-8 late cues, complete-text fallback, PSRAM failure and cancellation");
}
