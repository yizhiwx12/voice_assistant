#include "voice_assistant_status.hpp"
#include "display/device_status_display.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
size_t checks = 0;

void require(bool condition, const char *message)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "UI text check failed: %s\n", message);
        std::exit(1);
    }
}

std::string stored(const VoiceAssistantStatus &status)
{
    std::array<char, VoiceAssistantStatus::kConversationCapacity> result{};
    status.copyConversation(result.data(), result.size());
    return result.data();
}

void checkLongTextAndCapacity()
{
    VoiceAssistantStatus status;
    const std::string long_text = std::string(1024, 'a') + " long answer tail";
    status.setConversation(long_text.c_str());
    require(stored(status) == long_text, "answers longer than 320 bytes retain their tail");
    require(!status.conversationTruncated(), "long answer below the bound is complete");

    const std::string exact(VoiceAssistantStatus::kConversationCapacity - 1, 'x');
    status.setConversation(exact.c_str());
    require(stored(status) == exact, "exact byte capacity retains all content");
    require(!status.conversationTruncated(), "exact byte capacity is not marked truncated");
    status.appendConversation("");
    require(!status.conversationTruncated(), "empty append at exact capacity remains complete");
    status.appendConversation("overflow");
    require(stored(status) == exact, "append beyond byte capacity preserves the prefix");
    require(status.conversationTruncated(), "append beyond byte capacity marks truncation");
    const auto revision = status.textRevision();
    status.appendConversation("later");
    require(status.textRevision() == revision && stored(status) == exact,
            "saturated answer ignores all later fragments");
}

void checkUtf8Boundaries(const std::string &codepoint)
{
    // 在存储及复制边界放入每一种可能的不完整编码。
    for (size_t retained = 1; retained < codepoint.size(); ++retained) {
        VoiceAssistantStatus status;
        const std::string prefix(VoiceAssistantStatus::kConversationCapacity - 1 - retained, 'a');
        status.setConversation((prefix + codepoint).c_str());
        require(stored(status) == prefix, "partial final UTF-8 encoding is removed from storage");
        require(status.conversationTruncated(), "UTF-8 clipping reports a truncated answer");
        status.appendConversation("later fragment");
        require(stored(status) == prefix, "UTF-8 clipping seals the truncated prefix");

        status.setConversation(prefix.c_str());
        const auto append_id = status.conversationId();
        status.appendConversation(codepoint.c_str());
        require(stored(status) == prefix && status.conversationTruncated(),
                "append crossing a UTF-8 capacity boundary drops the partial encoding");
        require(status.conversationId() == append_id, "clipped append keeps the answer identity");

        status.setConversation((std::string("a") + codepoint + "b").c_str());
        std::array<char, 16> destination{};
        destination.fill('#');
        const size_t capacity = 2 + retained;
        status.copyConversation(destination.data(), capacity);
        require(std::string(destination.data()) == "a", "small copies remove partial UTF-8 tails");
        require(destination[capacity] == '#', "bounded copy does not write past its capacity");
    }

    VoiceAssistantStatus status;
    const std::string exact = std::string(VoiceAssistantStatus::kConversationCapacity - 1 -
                                         codepoint.size(), 'a') + codepoint;
    status.setConversation(exact.c_str());
    require(stored(status) == exact && !status.conversationTruncated(),
            "complete UTF-8 encoding ending exactly at capacity is retained");

    status.setConversation("prefix ");
    const auto id = status.conversationId();
    status.appendConversation(codepoint.c_str());
    require(stored(status) == "prefix " + codepoint, "append retains complete UTF-8 codepoints");
    require(status.conversationId() == id, "append keeps the current answer identity");
}

void checkResetAndCopies()
{
    VoiceAssistantStatus status;
    status.setConversation(std::string(VoiceAssistantStatus::kConversationCapacity + 20, 'z').c_str());
    const auto old_id = status.conversationId();
    status.setConversation("new answer");
    require(!status.conversationTruncated() && stored(status) == "new answer",
            "new answer clears previous truncation");
    require(status.conversationId() != old_id, "replacement advances answer identity");
    const auto id = status.conversationId();
    const auto revision = status.textRevision();
    status.appendConversation(nullptr);
    require(status.conversationId() == id && status.textRevision() == revision,
            "null append is a no-op");

    char sentinel = '#';
    status.copyConversation(&sentinel, 0);
    status.copyConversation(nullptr, 0);
    status.copyConversation(nullptr, 64);
    require(sentinel == '#', "null and zero-capacity copies are no-ops");
    status.copyConversation(&sentinel, 1);
    require(sentinel == '\0', "one-byte copy produces only the terminator");

    std::array<char, 5> short_copy{'#', '#', '#', '#', '#'};
    status.copyConversation(short_copy.data(), 4);
    require(std::string(short_copy.data()) == "new" && short_copy[4] == '#',
            "small ASCII copy is bounded and terminated");
    status.setConversation(nullptr);
    require(stored(status).empty() && !status.conversationTruncated(),
            "null replacement clears text and truncation");
    require(status.conversationId() != id, "clear advances answer identity");
}

void checkBodyRevisionAndPacketUpdates()
{
    VoiceAssistantStatus status;
    require(status.conversationRevision() == 0, "empty body has no published text revision");
    status.setConversation("answer ");
    const auto body_revision = status.conversationRevision();
    const auto answer_id = status.conversationId();
    status.setSession(VoiceSessionState::Ready);
    status.setCapture(VoiceCaptureState::Starting);
    status.setResponse(VoiceResponseState::Generating);
    status.setPlayback(VoicePlaybackState::Playing);
    const auto status_revision = status.textRevision();
    // 下行数据包交付不能破坏渲染器的文本缓存。
    for (size_t packet = 0; packet < 1000; ++packet) {
        status.setSession(VoiceSessionState::Ready);
        status.setCapture(VoiceCaptureState::Starting);
        status.setResponse(VoiceResponseState::Generating);
        status.setPlayback(VoicePlaybackState::Playing);
    }
    require(status.textRevision() == status_revision,
            "repeated session/capture/response/playback reports do not invalidate rendering");
    require(status.conversationRevision() == body_revision && status.conversationId() == answer_id,
            "packet and state updates do not replace or revise the body text");
    status.setFrontend(VoiceFrontendState::Speech);
    status.setListening(true);
    status.setBargeIn(VoiceBargeInState::Candidate);
    status.setPipelineStage("connection diagnostics");
    require(status.textRevision() != status_revision && status.conversationRevision() == body_revision,
            "other visible status changes retain the cached body text");
    status.appendConversation("");
    status.appendConversation(nullptr);
    require(status.conversationRevision() == body_revision,
            "empty body fragments do not invalidate the cached answer");
    status.appendConversation("\xe4\xbd\xa0");
    status.appendConversation("\xf0\x9f\x98\x80");
    require(stored(status) == "answer \xe4\xbd\xa0\xf0\x9f\x98\x80" &&
            status.conversationRevision() == body_revision + 2 && status.conversationId() == answer_id,
            "UTF-8 body appends advance only the body revision and preserve identity");
    std::array<char, 12> partial{};
    partial.fill('#');
    status.copyConversation(partial.data(), 10);
    require(std::string(partial.data()) == "answer " && partial[10] == '#',
            "cached-length copy drops a partial multibyte suffix without overrunning the target");
    const auto updated_revision = status.conversationRevision();
    status.setConversation("replacement");
    require(status.conversationRevision() == updated_revision + 1 && status.conversationId() == answer_id + 1,
            "answer replacement publishes a new identity and body revision");
    status.setConversation("replacement");
    require(status.conversationRevision() == updated_revision + 2 && status.conversationId() == answer_id + 2,
            "same-text replacement still represents a distinct answer");

    const std::string exact(VoiceAssistantStatus::kConversationCapacity - 1, 'x');
    status.setConversation(exact.c_str());
    const auto full_revision = status.conversationRevision();
    status.appendConversation("overflow");
    require(status.conversationRevision() == full_revision + 1 && status.conversationTruncated(),
            "first overflow publishes truncation even when no additional text fits");
    status.appendConversation("later");
    require(status.conversationRevision() == full_revision + 1 && stored(status) == exact,
            "a sealed truncated body ignores later fragments and their revisions");

    VoiceAssistantStatus offline;
    const auto offline_revision = offline.textRevision();
    offline.setSession(VoiceSessionState::Offline);
    offline.setCapture(VoiceCaptureState::Idle);
    offline.setResponse(VoiceResponseState::Idle);
    offline.setPlayback(VoicePlaybackState::Idle);
    require(offline.textRevision() == offline_revision,
            "unchanged default state reports do not schedule rendering");
}

void checkSubtitleTransitionSnapshot()
{
    VoiceAssistantStatus status;
    status.resetSubtitle(77, 3, 9, 12);
    require(status.subtitlePosition().next_word_byte_offset == UINT32_MAX,
            "new subtitle streams do not invent a future word anchor");
    status.setSubtitleText(77, 3, 9, 12, "one two");
    const auto body_revision = status.conversationRevision();
    status.setSubtitlePosition(77, 3, 9, 12, 0, true, 3, 0, 1000, 500, 500, 4, 250);
    const auto position = status.subtitlePosition();
    require(position.active && position.next_word_byte_offset == 4 &&
            position.next_word_progress_permille == 250 && position.word_progress_permille == 500,
            "subtitle snapshot retains both true word timing and the next-anchor transition");
    require(status.conversationRevision() == body_revision,
            "playback-only subtitle movement does not invalidate the answer copy");
    status.setSubtitlePosition(77, 3, 9, 12, 0, true, 3, 0, 1000, 500, 500, 4, 250);
    require(status.subtitlePosition().revision == position.revision,
            "unchanged transition snapshots do not schedule another display update");
    status.setSubtitlePosition(77, 3, 9, 12, 0, true, 3, 0, 1000, 500, 500, 4, 300);
    require(status.subtitlePosition().revision == position.revision + 1,
            "next-anchor progress updates render even if the current-word fields stay unchanged");
    status.setSubtitlePosition(77, 3, 9, 12, 0, true, 3, 0, 1000, 500, 500);
    require(status.subtitlePosition().next_word_byte_offset == UINT32_MAX &&
            status.subtitlePosition().next_word_progress_permille == 0,
            "legacy position publishers explicitly remove unavailable next-word metadata");
    const auto current = status.subtitlePosition();
    status.setSubtitlePosition(77, 3, 8, 12, 4, true, 7, 2000, 3000, 2500, 500, 8, 500);
    require(status.subtitlePosition().revision == current.revision &&
            status.subtitlePosition().next_word_byte_offset == UINT32_MAX,
            "old reply generations cannot publish transition anchors into a new stream");
}

void checkLocalDetectionIsObservationOnly()
{
    VoiceAssistantStatus status;
    status.enableRealtime();
    status.setSession(VoiceSessionState::Ready);
    const auto revision = status.textRevision();
    status.setFrontend(VoiceFrontendState::Speech);
    require(status.frontend() == VoiceFrontendState::Speech && status.textRevision() == revision + 1,
            "local speech invalidates the display once");
    status.setFrontend(VoiceFrontendState::Speech);
    require(status.textRevision() == revision + 1, "unchanged VAD state does not force redraws");
    require(status.capture() == VoiceCaptureState::Idle && status.state() == VoiceAssistantState::Ready,
            "VAD speech does not start recording");
    status.setFrontend(VoiceFrontendState::Failed);
    require(status.state() == VoiceAssistantState::Ready && status.session() == VoiceSessionState::Ready,
            "local observation failure does not claim a network failure");
}

void checkExplicitAutomaticListening()
{
    VoiceAssistantStatus status;
    status.enableRealtime();
    status.setSession(VoiceSessionState::Ready);
    require(!status.listening() && status.state() == VoiceAssistantState::Ready,
            "automatic listening is disarmed at boot");
    const auto revision = status.textRevision();
    status.setListening(true);
    require(status.listening() && status.textRevision() == revision + 1,
            "arming invalidates the display");
    status.setListening(true);
    require(status.textRevision() == revision + 1, "unchanged arming is a no-op");
    require(status.state() == VoiceAssistantState::Listening,
            "armed idle uses listening rather than recording");
    status.setFrontend(VoiceFrontendState::Speech);
    require(status.state() == VoiceAssistantState::Listening && status.capture() == VoiceCaptureState::Idle,
            "the status model does not manufacture a turn from VAD observation");
    status.setCapture(VoiceCaptureState::Starting);
    require(status.state() == VoiceAssistantState::Preparing, "startup remains visible while armed");
    status.setCapture(VoiceCaptureState::Recording);
    require(status.state() == VoiceAssistantState::Recording, "active capture takes priority over listening");
    status.setCapture(VoiceCaptureState::Finishing);
    require(status.state() == VoiceAssistantState::Transcribing, "endpoint commit takes priority over listening");
    status.setCapture(VoiceCaptureState::Idle);
    status.setResponse(VoiceResponseState::Waiting);
    require(status.state() == VoiceAssistantState::Uploading, "waiting result does not claim listening");
    status.setResponse(VoiceResponseState::Generating);
    require(status.state() == VoiceAssistantState::Uploading, "generation does not claim listening");
    status.setPlayback(VoicePlaybackState::Draining);
    require(status.state() == VoiceAssistantState::Speaking, "actual playback drain takes priority");
    status.setResponse(VoiceResponseState::Complete);
    require(status.state() == VoiceAssistantState::Speaking, "server completion does not hide undrained audio");
    status.setPlayback(VoicePlaybackState::Idle);
    status.setConversation("empty result or retained answer");
    require(status.state() == VoiceAssistantState::Complete && status.listening() &&
            stored(status) == "empty result or retained answer",
            "rearmed listening preserves the completed result page");
    status.setListening(false);
    require(status.state() == VoiceAssistantState::Complete, "manual mode preserves the completed result page");
    status.setListening(true);
    status.setResponse(VoiceResponseState::Cancelled);
    require(status.state() == VoiceAssistantState::Listening, "cancelled response can resume armed idle");
    status.setSession(VoiceSessionState::Connecting);
    require(status.state() == VoiceAssistantState::Preparing, "armed connection wait is not listening");
    status.setSession(VoiceSessionState::Failed);
    require(status.state() == VoiceAssistantState::UploadError, "network failure remains visible");
    status.setSession(VoiceSessionState::Ready);
    status.setResponse(VoiceResponseState::Failed);
    require(status.state() == VoiceAssistantState::UploadError, "reply failure remains visible");
}

void checkBargeInObservationIsIndependent()
{
    VoiceAssistantStatus status;
    status.enableRealtime();
    status.setSession(VoiceSessionState::Ready);
    status.setPlayback(VoicePlaybackState::Playing);
    status.setConversation("retained playing answer");
    const auto id = status.conversationId();
    require(status.bargeIn() == VoiceBargeInState::Disabled,
            "barge-in observation is disabled at boot");
    for (const auto value : {VoiceBargeInState::Armed, VoiceBargeInState::Candidate,
                             VoiceBargeInState::Confirmed, VoiceBargeInState::Cooldown,
                             VoiceBargeInState::Unavailable, VoiceBargeInState::Disabled}) {
        const auto revision = status.textRevision();
        status.setBargeIn(value);
        require(status.bargeIn() == value && status.textRevision() == revision + 1,
                "candidate observation transitions invalidate the display once");
        status.setBargeIn(value);
        require(status.textRevision() == revision + 1,
                "unchanged candidate observation does not force redraws");
        require(status.state() == VoiceAssistantState::Speaking &&
                status.playback() == VoicePlaybackState::Playing,
                "candidate confirmation and faults do not interrupt playback");
        require(status.capture() == VoiceCaptureState::Idle && !status.listening(),
                "observation states do not arm listening or create capture");
        require(status.conversationId() == id && stored(status) == "retained playing answer",
                "candidate observation preserves answer identity and text for paging");
    }
    status.setSession(VoiceSessionState::Failed);
    status.setBargeIn(VoiceBargeInState::Unavailable);
    require(status.state() == VoiceAssistantState::UploadError,
            "observation cannot hide a network failure");
}

void checkExplicitBargeInMode()
{
    VoiceAssistantStatus status;
    status.enableRealtime();
    status.setSession(VoiceSessionState::Ready);
    status.setPlayback(VoicePlaybackState::Playing);
    status.setConversation("retained answer while interrupt mode is armed");
    const auto id = status.conversationId();
    require(!status.bargeInEnabled(), "automatic interruption is disabled at boot");
    auto revision = status.textRevision();
    status.setBargeInEnabled(true);
    require(status.bargeInEnabled() && status.textRevision() == revision + 1,
            "explicit interruption mode changes invalidate the display once");
    status.setBargeInEnabled(true);
    require(status.textRevision() == revision + 1,
            "unchanged interruption mode does not force redraws");
    require(status.state() == VoiceAssistantState::Speaking && !status.listening() &&
            status.capture() == VoiceCaptureState::Idle,
            "mode display alone neither stops playback nor arms input");
    for (const auto value : {VoiceBargeInState::Cancelling, VoiceBargeInState::Starting}) {
        revision = status.textRevision();
        status.setBargeIn(value);
        require(status.bargeIn() == value && status.textRevision() == revision + 1,
                "handoff display phases invalidate once");
        require(status.state() == VoiceAssistantState::Speaking &&
                status.conversationId() == id &&
                stored(status) == "retained answer while interrupt mode is armed",
                "handoff status does not claim completion or replace the answer");
    }
    status.setPlayback(VoicePlaybackState::Idle);
    status.setCapture(VoiceCaptureState::Starting);
    require(status.state() == VoiceAssistantState::Preparing,
            "controller capture state takes priority during handoff");
    status.setSession(VoiceSessionState::Failed);
    require(status.state() == VoiceAssistantState::UploadError,
            "handoff mode cannot hide a network failure");
    revision = status.textRevision();
    status.setBargeInEnabled(false);
    require(!status.bargeInEnabled() && status.textRevision() == revision + 1,
            "disarming interruption updates the mode display");
}

void checkSessionSnapshotAndDeviceStatus()
{
    VoiceAssistantStatus status;
    status.enableRealtime();
    status.setSession(VoiceSessionState::Ready);
    VoiceSessionSelection selection;
    selection.epoch = 77;
    selection.metadata_present = true;
    std::snprintf(selection.profile.data(), selection.profile.size(), "%s", "realtime_default");
    std::snprintf(selection.mode.data(), selection.mode.size(), "%s", "realtime");
    std::snprintf(selection.asr.id.data(), selection.asr.id.size(), "%s", "doubao_2");
    std::snprintf(selection.llm.id.data(), selection.llm.id.size(), "%s", "deepseek_default");
    std::snprintf(selection.tts.id.data(), selection.tts.id.size(), "%s", "qwen_tts");
    std::snprintf(selection.asr.model.data(), selection.asr.model.size(), "%s", "bigmodel");
    std::snprintf(selection.tts.voice.data(), selection.tts.voice.size(), "%s", "Cherry");
    selection.asr.capabilities.streaming_input = VoiceSelectionFlag::Yes;
    selection.asr.capabilities.streaming_output = VoiceSelectionFlag::No;
    selection.require_streaming_asr = VoiceSelectionFlag::Yes;
    const auto conversation_id = status.conversationId();
    status.setSelection(selection);
    selection.asr.id[0] = 'x';
    require(std::string(status.selection().asr.id.data()) == "doubao_2",
            "selection publication stores a copy independent of the parser buffer");
    auto copied = status.selection();
    copied.llm.id[0] = 'x';
    require(std::string(status.selection().llm.id.data()) == "deepseek_default",
            "UI snapshot copies cannot mutate the shared active selection");

    require(status.conversationId() == conversation_id,
            "diagnostic selection changes do not replace the answer identity");
    status.setSession(VoiceSessionState::Connecting);
    require(status.selection().epoch == 0 && !status.selection().metadata_present,
            "new connection status invalidates the previous provider snapshot");
    std::array<char, 96> summary{};
    for (const auto session : {VoiceSessionState::Offline, VoiceSessionState::Connecting,
                               VoiceSessionState::Ready, VoiceSessionState::Failed}) {
        voiceui::formatModeConnection(session, false, false, VoiceBargeInState::Disabled,
                                      summary.data(), summary.size());
        require(std::string(summary.data()).find("已暂停 | ") == 0,
                "paused mode remains explicit throughout connection states");
    }
    voiceui::formatModeConnection(VoiceSessionState::Ready, true, false, VoiceBargeInState::Disabled,
                                  summary.data(), summary.size());
    require(std::string(summary.data()) == "自动监听 | 已连接", "automatic interaction mode is visible");
    voiceui::formatModeConnection(VoiceSessionState::Ready, true, false, VoiceBargeInState::Armed,
                                  summary.data(), summary.size());
    require(std::string(summary.data()) == "插话观察 | 已连接", "debug observation remains separate from automatic mode");
    voiceui::formatModeConnection(VoiceSessionState::Ready, true, true, VoiceBargeInState::Starting,
                                  summary.data(), summary.size());
    require(std::string(summary.data()) == "自动插话 | 已连接", "automatic interruption has explicit mode priority");
}
}

int main()
{
    checkLongTextAndCapacity();
    checkUtf8Boundaries("\xc3\xa9");       // 双字节码点。
    checkUtf8Boundaries("\xe4\xbd\xa0");  // 三字节码点。
    checkUtf8Boundaries("\xf0\x9f\x98\x80"); // 四字节码点。
    checkResetAndCopies();
    checkBodyRevisionAndPacketUpdates();
    checkSubtitleTransitionSnapshot();
    checkLocalDetectionIsObservationOnly();
    checkExplicitAutomaticListening();
    checkBargeInObservationIsIndependent();
    checkExplicitBargeInMode();
    checkSessionSnapshotAndDeviceStatus();
    std::printf("UI text status: %zu checks passed\n", checks);
}
