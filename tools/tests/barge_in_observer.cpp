#include "audio/barge_in_observer.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
using Observer = voiceaudio::BargeInObserver;
using Phase = Observer::Phase;
constexpr size_t kSamples = 320;

// 用于字节一致性/生命周期夹具的确定性交变波形。平衡正负对替代早先纯正斜坡；
// 真实 VAD/声学判断不在这些测试范围内，每个 320 样本夹具均为零均值。
int16_t value(uint64_t offset)
{
    const int32_t amplitude = static_cast<int32_t>(((offset / 2) * 13U + 19U) % 1000U) + 200;
    return static_cast<int16_t>((offset & 1U) ? -amplitude : amplitude);
}
std::array<int16_t, kSamples> signal(uint64_t offset)
{
    std::array<int16_t, kSamples> pcm{};
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = value(offset + i);
    return pcm;
}
struct Fixture {
    std::vector<Observer::Frame> storage;
    Observer observer;
    uint64_t offset = 0;
    explicit Fixture(size_t capacity = 134, size_t preroll = 20,
                     size_t confirmation = 10, size_t hold = 100,
                     uint32_t minimum_ac = 80) : storage(capacity)
    { observer.configure(storage.data(), capacity, preroll, confirmation, hold, minimum_ac); }
    void enable() { observer.enable(offset); }
    void append(bool speech)
    {
        const auto pcm = signal(offset);
        appendPcm(pcm, speech);
    }
    void appendPcm(const std::array<int16_t, kSamples> &pcm, bool speech)
    {
        observer.append(pcm.data(), pcm.size(), offset, speech);
        offset += pcm.size();
    }
    void frames(size_t count, bool speech)
    { for (size_t i = 0; i < count; ++i) append(speech); }
    Observer::Snapshot snapshot() const { return observer.snapshot(); }
    void confirm() { frames(10, true); assert(snapshot().phase == Phase::Confirmed); }
    void checkStoredInterval(uint64_t first, uint64_t end) const
    {
        // 在调用方存储中定位每个保留 PCM 帧并检查所有样本。
        // 这会检查环回后的内容，不只是计数器。
        for (uint64_t at = first; at < end; at += kSamples) {
            size_t matches = 0;
            for (const auto &frame : storage) if (frame.offset == at) {
                bool valid = true;
                for (size_t i = 0; i < kSamples; ++i)
                    if (frame.pcm[i] != value(at + i)) valid = false;
                if (valid) ++matches;
            }
            assert(matches >= 1);
        }
    }
};

void disabledHasNoEffect()
{
    Fixture f;
    f.frames(2000, true);
    const auto s = f.snapshot();
    assert(s.phase == Phase::Disabled && s.candidates == 0 && s.buffered_frames == 0);
}
void longQuietIsBoundedAndNeverConfirms()
{
    Fixture f;
    f.enable();
    f.frames(10000, false);
    const auto s = f.snapshot();
    assert(s.phase == Phase::Armed && s.candidates == 0 && s.confirmations == 0);
    assert(s.buffered_frames == 20 && s.buffered_peak == 20 && s.overflows == 0);
    assert(s.oldest_offset == f.offset - 20 * kSamples);
    f.checkStoredInterval(s.oldest_offset, f.offset);
}
void initialSpeechDoesNotRequireSilence()
{
    Fixture f;
    f.enable();
    f.frames(9, true);
    assert(f.snapshot().phase == Phase::Candidate && f.snapshot().confirmations == 0);
    f.append(true);
    const auto s = f.snapshot();
    assert(s.phase == Phase::Confirmed && s.candidates == 1 && s.confirmations == 1);
    assert(s.candidate_offset == 0 && s.confirmed_offset == 3200 && s.speech_frames == 10);
}
void confirmationKeepsFullFourHundredMillisecondPreroll()
{
    Fixture f;
    f.enable();
    f.frames(50, false);
    f.confirm();
    const auto s = f.snapshot();
    assert(s.oldest_offset == 30 * kSamples && s.candidate_offset == 50 * kSamples);
    assert(s.confirmed_offset == 60 * kSamples && s.buffered_frames == 30);
    f.checkStoredInterval(s.oldest_offset, s.confirmed_offset);
}
void briefCandidateIsRejected()
{
    Fixture f;
    f.enable();
    f.frames(40, false);
    f.frames(9, true);
    f.append(false);
    const auto s = f.snapshot();
    assert(s.phase == Phase::Armed && s.rejected == 1 && s.confirmations == 0);
    assert(s.buffered_frames == 20 && s.oldest_offset == 30 * kSamples);
    f.checkStoredInterval(s.oldest_offset, f.offset);
}
void separatedBurstsCannotAccumulateConfirmation()
{
    Fixture f;
    f.enable();
    for (size_t i = 0; i < 100; ++i) { f.frames(9, true); f.append(false); }
    const auto s = f.snapshot();
    assert(s.candidates == 100 && s.rejected == 100 && s.confirmations == 0);
    assert(s.phase == Phase::Armed && s.buffered_frames == 20 && s.overflows == 0);
}
void newCandidateHasItsOwnContinuousWindow()
{
    Fixture f;
    f.enable();
    f.frames(9, true);
    f.append(false);
    f.frames(9, true);
    assert(f.snapshot().confirmations == 0);
    f.append(true);
    const auto s = f.snapshot();
    assert(s.candidates == 2 && s.rejected == 1 && s.confirmations == 1);
    assert(s.candidate_offset == 10 * kSamples && s.confirmed_offset == 20 * kSamples);
}
void confirmedSilenceDiscardsCandidateAndStartsQuietCooldown()
{
    Fixture f;
    f.enable();
    f.confirm();
    f.append(false);
    assert(f.snapshot().phase == Phase::Cooldown && f.snapshot().buffered_frames == 1);
    assert(f.snapshot().oldest_offset == 10 * kSamples);
    f.frames(13, false);
    assert(f.snapshot().phase == Phase::Cooldown);
    f.append(false);
    assert(f.snapshot().phase == Phase::Armed && f.snapshot().buffered_frames == 15);
    f.confirm();
    assert(f.snapshot().confirmations == 2 && f.snapshot().candidates == 2);
}
void sustainedSpeechTimesOutOnceWithoutRetriggering()
{
    Fixture f;
    f.enable();
    f.frames(20, false);
    f.confirm();
    f.frames(99, true);
    assert(f.snapshot().phase == Phase::Confirmed && f.snapshot().buffered_frames == 129);
    assert(f.snapshot().timeouts == 0);
    f.append(true);
    assert(f.snapshot().phase == Phase::Cooldown && f.snapshot().timeouts == 1);
    assert(f.snapshot().buffered_frames == 0 && f.snapshot().buffered_peak == 130);
    f.frames(10000, true);
    const auto s = f.snapshot();
    assert(s.phase == Phase::Cooldown && s.confirmations == 1 && s.candidates == 1);
    assert(s.timeouts == 1 && s.overflows == 0 && s.buffered_frames == 0);
}
void cooldownRequiresConsecutiveQuiet()
{
    Fixture f;
    f.enable();
    f.confirm();
    f.frames(14, false);
    f.append(true);
    assert(f.snapshot().buffered_frames == 0);
    f.frames(14, false);
    assert(f.snapshot().phase == Phase::Cooldown && f.snapshot().confirmations == 1);
    f.append(false);
    f.confirm();
    assert(f.snapshot().confirmations == 2);
}
void overflowFailsClosedAndClearsCache()
{
    Fixture f(25);
    f.enable();
    f.frames(20, false);
    f.frames(5, true);
    assert(f.snapshot().buffered_frames == 25 && f.snapshot().phase == Phase::Candidate);
    f.append(true);
    assert(f.snapshot().phase == Phase::Fault && f.snapshot().overflows == 1);
    assert(f.snapshot().buffered_frames == 0 && f.snapshot().confirmations == 0);
    f.frames(100, true);
    assert(f.snapshot().overflows == 1 && f.snapshot().candidates == 1);
}
void discontinuityFailsWithoutJoiningSeparatedAudio()
{
    Fixture f;
    f.enable();
    f.frames(9, true);
    f.offset += kSamples;
    f.append(true);
    assert(f.snapshot().phase == Phase::Fault && f.snapshot().discontinuities == 1);
    assert(f.snapshot().confirmations == 0 && f.snapshot().buffered_frames == 0);
}
void duplicatedFrameIsNotCountedTwice()
{
    Fixture f;
    f.enable();
    f.frames(9, true);
    f.offset -= kSamples;
    f.append(true);
    assert(f.snapshot().phase == Phase::Fault && f.snapshot().confirmations == 0);
    assert(f.snapshot().discontinuities == 1);
}
void enableIgnoresOutputCapturedBeforePlayback()
{
    Fixture f;
    f.observer.enable(5000);
    f.frames(16, true);  // 0..4799 开始于激活边界之前。
    assert(f.snapshot().candidates == 0 && f.snapshot().discontinuities == 0);
    assert(f.snapshot().buffered_frames == 0);
    f.frames(9, true);
    assert(f.snapshot().phase == Phase::Candidate && f.snapshot().candidate_offset == 5120);
    f.append(true);
    assert(f.snapshot().confirmed_offset == 8320 && f.snapshot().confirmations == 1);
}
void disableAndNewPlaybackNeverReuseOldCache()
{
    Fixture f;
    f.enable();
    f.frames(20, false);
    f.confirm();
    f.observer.disable();
    assert(f.snapshot().buffered_frames == 0 && f.snapshot().phase == Phase::Disabled);
    const uint64_t old_offset = f.offset;
    f.frames(50, true);
    const auto delayed = signal(old_offset);
    f.enable();
    f.observer.append(delayed.data(), delayed.size(), old_offset, true);
    assert(f.snapshot().buffered_frames == 0 && f.snapshot().candidates == 1);
    f.confirm();
    const auto s = f.snapshot();
    assert(s.oldest_offset == 80 * kSamples && s.buffered_frames == 10);
    assert(s.candidates == 2 && s.confirmations == 2 && s.discontinuities == 0);
}
void healthFaultCanBeExplicitlyReenabled()
{
    Fixture f;
    f.enable();
    f.frames(4, true);
    f.observer.fault();
    assert(f.snapshot().phase == Phase::Fault && f.snapshot().buffered_frames == 0);
    f.frames(100, true);
    assert(f.snapshot().candidates == 1 && f.snapshot().confirmations == 0);
    f.enable();
    f.confirm();
    assert(f.snapshot().candidates == 2 && f.snapshot().confirmations == 1);
}
void invalidFramesFailClosed()
{
    Fixture f;
    f.enable();
    const auto pcm = signal(0);
    f.observer.append(pcm.data(), 319, 0, true);
    assert(f.snapshot().phase == Phase::Fault && f.snapshot().discontinuities == 1);
    f.enable();
    f.observer.append(nullptr, 320, 0, true);
    assert(f.snapshot().phase == Phase::Fault && f.snapshot().discontinuities == 2);
    f.enable();
    f.observer.append(pcm.data(), 320, std::numeric_limits<uint64_t>::max() - 319, true);
    assert(f.snapshot().phase == Phase::Fault && f.snapshot().discontinuities == 3);
}
void configurableWindowIsRoundedByCallerAndRespected()
{
    Fixture f(40, 5, 3, 20);
    f.enable();
    f.frames(8, false);
    f.frames(2, true);
    assert(f.snapshot().phase == Phase::Candidate);
    f.append(true);
    assert(f.snapshot().phase == Phase::Confirmed && f.snapshot().buffered_frames == 8);
    assert(f.snapshot().oldest_offset == 3 * kSamples && f.snapshot().confirmed_offset == 11 * kSamples);
    f.frames(20, true);
    assert(f.snapshot().phase == Phase::Cooldown && f.snapshot().timeouts == 1);
}
void missingStorageCannotArm()
{
    Observer observer;
    observer.configure(nullptr, 134, 20, 10, 100);
    observer.enable(0);
    assert(observer.snapshot().phase == Phase::Fault && observer.snapshot().buffered_frames == 0);
}
void handoffNeedsExplicitOptInAndContinuousConfirmation()
{
    Fixture f;
    std::array<Observer::Frame, 134> destination{};
    voiceaudio::AutomaticUtterance utterance;
    utterance.configure(destination.data(), destination.size(), 20, 35);
    f.enable();
    f.frames(20, false);
    f.confirm();
    assert(!f.observer.handoff(utterance));  // D1 从不交接 PCM。
    assert(utterance.size() == 0 && f.snapshot().buffered_frames == 30);
    f.observer.setHandoffEnabled(true);
    assert(f.observer.handoff(utterance));
    assert(f.snapshot().phase == Phase::Disabled && f.snapshot().buffered_frames == 0);
    assert(utterance.phase() == voiceaudio::AutomaticUtterance::Phase::Opening);
    assert(utterance.size() == 30);
    assert(!f.observer.handoff(utterance));  // 恰好一次所有权交接。
    f.enable();
    f.frames(9, true);
    assert(!f.observer.handoff(utterance));
    assert(f.snapshot().phase == Phase::Candidate && utterance.size() == 30);
}
void handoffRetainsQuietTailAndEveryWrappedPrerollSample()
{
    Fixture f;
    std::array<Observer::Frame, 134> destination{};
    voiceaudio::AutomaticUtterance utterance;
    utterance.configure(destination.data(), destination.size(), 20, 35);
    f.observer.setHandoffEnabled(true);
    f.enable();
    f.frames(123, false);  // 在起始点之前覆盖源环回绕。
    f.confirm();
    f.frames(7, false);  // 静音不能抹除已确认的 D2 语音段。
    assert(f.snapshot().phase == Phase::Confirmed && f.snapshot().buffered_frames == 37);
    assert(f.snapshot().confirmations == 1 && f.snapshot().candidates == 1);
    assert(f.observer.handoff(utterance));
    Observer::Frame frame{};
    size_t count = 0;
    while (utterance.pop(frame)) {
        assert(frame.offset == (103 + count) * kSamples);
        assert(frame.speech == (count >= 20 && count < 30));
        for (size_t i = 0; i < kSamples; ++i) assert(frame.pcm[i] == value(frame.offset + i));
        ++count;
    }
    assert(count == 37 && utterance.nextOffset() == f.offset);
    for (size_t i = 0; i < 27; ++i) {
        const auto pcm = signal(f.offset);
        assert(utterance.append(pcm.data(), pcm.size(), f.offset, false) ==
               voiceaudio::AutomaticUtterance::Result::Accepted);
        f.offset += kSamples;
    }
    assert(utterance.phase() == voiceaudio::AutomaticUtterance::Phase::Opening);
    const auto pcm = signal(f.offset);
    utterance.append(pcm.data(), pcm.size(), f.offset, false);
    assert(utterance.phase() == voiceaudio::AutomaticUtterance::Phase::Draining);
}
void handoffHoldDeadlineCountsQuietFramesAndNeverReplaysExpiredPcm()
{
    Fixture f;
    std::array<Observer::Frame, 134> destination{};
    voiceaudio::AutomaticUtterance utterance;
    utterance.configure(destination.data(), destination.size(), 20, 35);
    f.observer.setHandoffEnabled(true);
    f.enable();
    f.confirm();
    f.frames(99, false);
    assert(f.snapshot().phase == Phase::Confirmed && f.snapshot().timeouts == 0);
    f.append(false);
    assert(f.snapshot().phase == Phase::Cooldown && f.snapshot().timeouts == 1);
    assert(f.snapshot().buffered_frames == 0 && !f.observer.handoff(utterance));
    assert(utterance.size() == 0);
}
void failedDestinationLeavesConfirmedSourceIntact()
{
    Fixture f;
    std::array<Observer::Frame, 29> destination{};
    voiceaudio::AutomaticUtterance utterance;
    utterance.configure(destination.data(), destination.size(), 20, 35);
    f.observer.setHandoffEnabled(true);
    f.enable();
    f.frames(20, false);
    f.confirm();
    assert(!f.observer.handoff(utterance));
    assert(f.snapshot().phase == Phase::Confirmed && f.snapshot().buffered_frames == 30);
    assert(utterance.phase() == voiceaudio::AutomaticUtterance::Phase::Disabled);
    f.offset += kSamples;
    f.append(true);
    assert(f.snapshot().phase == Phase::Fault && !f.observer.handoff(utterance));
    assert(utterance.size() == 0);
}
void completedQuietHandoffCannotJoinLaterSpeech()
{
    Fixture f;
    std::array<Observer::Frame, 134> destination{};
    voiceaudio::AutomaticUtterance utterance;
    utterance.configure(destination.data(), destination.size(), 20, 35);
    f.observer.setHandoffEnabled(true);
    f.enable();
    f.frames(20, false);
    f.confirm();
    f.frames(40, false);
    f.frames(5, true);
    assert(f.observer.handoff(utterance));
    assert(utterance.phase() == voiceaudio::AutomaticUtterance::Phase::Draining);
    assert(utterance.size() == 65 && utterance.nextOffset() == 65 * kSamples);
    assert(f.snapshot().phase == Phase::Disabled && f.snapshot().buffered_frames == 0);
}

std::array<int16_t, kSamples> alternating(int32_t mean, int32_t amplitude)
{
    std::array<int16_t, kSamples> pcm{};
    assert(mean - amplitude >= INT16_MIN && mean + amplitude <= INT16_MAX);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<int16_t>(mean + ((i & 1U) ? -amplitude : amplitude));
    return pcm;
}

void constantVadIntervalCannotConfirmOrHandoff()
{
    for (const int32_t mean : {0, 400, -400, 32767, -32768}) {
        Fixture f;
        std::array<Observer::Frame, 134> destination{};
        voiceaudio::AutomaticUtterance utterance;
        utterance.configure(destination.data(), destination.size(), 20, 35);
        f.observer.setHandoffEnabled(true);
        f.enable();
        const auto pcm = alternating(mean, 0);
        for (size_t i = 0; i < 10; ++i) f.appendPcm(pcm, true);
        const auto rejected = f.snapshot();
        assert(rejected.phase == Phase::Cooldown && rejected.energy_rejections == 1);
        assert(rejected.confirmations == 0 && rejected.candidates == 1);
        assert(rejected.window_frames == 10 && rejected.window_ac_rms == 0);
        assert(rejected.frame_mean == mean && rejected.frame_ac_rms == 0 && rejected.frame_vad);
        assert(rejected.frame_rms == static_cast<uint32_t>(mean < 0 ? -mean : mean));
        assert(rejected.frame_peak == rejected.frame_rms && rejected.buffered_frames == 0);
        assert(!f.observer.handoff(utterance) && utterance.size() == 0);
        f.frames(1000, true);  // 被拒绝的持续 VAD 区间不能重新布防。
        assert(f.snapshot().energy_rejections == 1 && f.snapshot().candidates == 1);
        assert(f.snapshot().phase == Phase::Cooldown && !f.observer.handoff(utterance));
        f.frames(15, false);
        assert(f.snapshot().phase == Phase::Armed);
        f.confirm();
        assert(f.snapshot().candidates == 2 && f.snapshot().confirmations == 1);
    }
}

void d2TransientVadHangoverCannotConfirm()
{
    Fixture f;
    f.observer.setHandoffEnabled(true);
    f.enable();
    const auto impulse = alternating(0, 3000);
    const auto quiet = alternating(0, 0);
    f.appendPcm(impulse, true);
    for (size_t i = 1; i < 10; ++i) f.appendPcm(quiet, true);
    const auto s = f.snapshot();
    assert(s.phase == Phase::Cooldown && s.confirmations == 0);
    assert(s.energy_rejections == 1 && s.energy_active_frames == 1);
    assert(s.energy_tail_active_frames == 0 && s.window_frames == 10);
}

void actualAcPassesDespiteLargeOffset()
{
    Fixture f;
    f.enable();
    const auto pcm = alternating(2000, 200);
    for (size_t i = 0; i < 10; ++i) f.appendPcm(pcm, true);
    const auto s = f.snapshot();
    assert(s.phase == Phase::Confirmed && s.energy_rejections == 0 && s.confirmations == 1);
    assert(s.min_ac_rms == 80 && s.window_ac_rms == 200 && s.window_frames == 10);
    assert(s.frame_mean == 2000 && s.frame_rms == 2009 && s.frame_ac_rms == 200);
    assert(s.frame_peak == 2200 && s.frame_vad);
    assert(s.last_frame_offset == 9 * kSamples && s.confirmed_offset == 10 * kSamples);
}

void shortLowEnergyPhonemesDoNotResetVadWindow()
{
    Fixture f;
    f.enable();
    for (size_t i = 0; i < 10; ++i)
        f.appendPcm(alternating(400, i == 3 || i == 7 ? 0 : 100), true);
    const auto s = f.snapshot();
    assert(s.phase == Phase::Confirmed && s.confirmations == 1 && s.rejected == 0);
    assert(s.candidates == 1 && s.window_frames == 10 && s.speech_frames == 10);
    assert(s.window_ac_rms == 89 && s.energy_rejections == 0);
    assert(s.candidate_offset == 0 && s.confirmed_offset == 10 * kSamples);
}

void belowAndExactThresholdUsesWindowEnergy()
{
    for (const uint32_t threshold : {80U, 2048U}) {
        for (const int delta : {-1, 0}) {
            Fixture f(134, 20, 10, 100, threshold);
            f.enable();
            const auto pcm = alternating(0, static_cast<int32_t>(threshold) + delta);
            for (size_t i = 0; i < 10; ++i) f.appendPcm(pcm, true);
            const auto s = f.snapshot();
            assert(s.min_ac_rms == threshold && s.window_frames == 10);
            assert(s.window_ac_rms == threshold + delta);
            assert(s.phase == (delta ? Phase::Cooldown : Phase::Confirmed));
            assert(s.energy_rejections == static_cast<uint32_t>(delta != 0));
            assert(s.confirmations == static_cast<uint32_t>(delta == 0));
        }
    }
}

void abandonedCandidateCannotLendEnergyToNextWindow()
{
    Fixture f;
    f.enable();
    const auto strong = alternating(0, 30000);
    const auto weak = alternating(400, 79);
    for (size_t i = 0; i < 9; ++i) f.appendPcm(strong, true);
    f.appendPcm(weak, false);
    assert(f.snapshot().phase == Phase::Armed && f.snapshot().rejected == 1);
    for (size_t i = 0; i < 10; ++i) f.appendPcm(weak, true);
    const auto s = f.snapshot();
    assert(s.phase == Phase::Cooldown && s.candidates == 2 && s.confirmations == 0);
    assert(s.energy_rejections == 1 && s.window_ac_rms == 79 && s.window_frames == 10);
}

void signedPcmExtremesHaveBoundedStatistics()
{
    Fixture f;
    f.enable();
    std::array<int16_t, kSamples> pcm{};
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = (i & 1U) ? INT16_MIN : INT16_MAX;
    for (size_t i = 0; i < 10; ++i) f.appendPcm(pcm, true);
    const auto s = f.snapshot();
    assert(s.phase == Phase::Confirmed && s.overflows == 0 && s.energy_rejections == 0);
    assert(s.frame_mean == 0 && s.frame_rms == 32767 && s.frame_ac_rms == 32767);
    assert(s.frame_peak == 32768 && s.window_ac_rms == 32767 && s.window_frames == 10);
    assert(s.last_frame_offset == 9 * kSamples && s.next_offset == 10 * kSamples);
}

void d2GatePreservesUnmodifiedPcmAndLabels()
{
    Fixture f;
    std::array<Observer::Frame, 134> destination{};
    voiceaudio::AutomaticUtterance utterance;
    utterance.configure(destination.data(), destination.size(), 20, 35);
    f.observer.setHandoffEnabled(true);
    f.enable();
    std::vector<Observer::Frame> expected;
    for (size_t i = 0; i < 33; ++i) {
        const bool vad = i >= 20 && i < 30;
        // 整体活跃 VAD 窗口中的两个零交流能量音素，仍逐字节保留
        // 在 D2 音频中，并保留原始 speech=true 标签。
        const auto pcm = alternating(2000 + static_cast<int32_t>(i),
                                     vad && i != 23 && i != 27 ? 100 : 0);
        Observer::Frame frame;
        frame.pcm = pcm;
        frame.offset = f.offset;
        frame.speech = vad;
        expected.push_back(frame);
        f.appendPcm(pcm, vad);
        if (i == 29) {
            const auto s = f.snapshot();
            assert(s.phase == Phase::Confirmed && s.window_frames == 10 && s.window_ac_rms == 89);
            assert(s.energy_active_frames == 8 && s.energy_tail_active_frames == 2);
            assert(s.last_frame_offset == expected.back().offset && s.frame_mean == 2029);
        }
    }
    assert(f.observer.handoff(utterance));
    Observer::Frame copied;
    size_t count = 0;
    while (utterance.pop(copied)) {
        assert(count < expected.size());
        assert(copied.offset == expected[count].offset && copied.speech == expected[count].speech);
        assert(copied.pcm == expected[count].pcm);
        ++count;
    }
    assert(count == expected.size() && utterance.nextOffset() == f.offset);
    assert(f.snapshot().confirmations == 1 && f.snapshot().energy_rejections == 0);
}

void reenablingClearsEnergyDiagnostics()
{
    Fixture f;
    f.enable();
    const auto pcm = alternating(400, 0);
    for (size_t i = 0; i < 10; ++i) f.appendPcm(pcm, true);
    assert(f.snapshot().energy_rejections == 1 && f.snapshot().window_frames == 10);
    f.observer.disable();
    assert(f.snapshot().window_frames == 10);  // 保留拒绝证据。
    f.enable();
    const auto s = f.snapshot();
    assert(s.energy_rejections == 1 && s.min_ac_rms == 80);  // 计数器为累计值。
    assert(s.last_frame_offset == 0 && s.window_frames == 0 && s.window_ac_rms == 0);
    assert(s.frame_mean == 0 && s.frame_rms == 0 && s.frame_ac_rms == 0);
    assert(s.frame_peak == 0 && !s.frame_vad);
    f.confirm();
    assert(f.snapshot().confirmations == 1 && f.snapshot().energy_rejections == 1);
}
}  // 命名空间

int main()
{
    disabledHasNoEffect();
    longQuietIsBoundedAndNeverConfirms();
    initialSpeechDoesNotRequireSilence();
    confirmationKeepsFullFourHundredMillisecondPreroll();
    briefCandidateIsRejected();
    separatedBurstsCannotAccumulateConfirmation();
    newCandidateHasItsOwnContinuousWindow();
    confirmedSilenceDiscardsCandidateAndStartsQuietCooldown();
    sustainedSpeechTimesOutOnceWithoutRetriggering();
    cooldownRequiresConsecutiveQuiet();
    overflowFailsClosedAndClearsCache();
    discontinuityFailsWithoutJoiningSeparatedAudio();
    duplicatedFrameIsNotCountedTwice();
    enableIgnoresOutputCapturedBeforePlayback();
    disableAndNewPlaybackNeverReuseOldCache();
    healthFaultCanBeExplicitlyReenabled();
    invalidFramesFailClosed();
    configurableWindowIsRoundedByCallerAndRespected();
    missingStorageCannotArm();
    handoffNeedsExplicitOptInAndContinuousConfirmation();
    handoffRetainsQuietTailAndEveryWrappedPrerollSample();
    handoffHoldDeadlineCountsQuietFramesAndNeverReplaysExpiredPcm();
    failedDestinationLeavesConfirmedSourceIntact();
    completedQuietHandoffCannotJoinLaterSpeech();
    constantVadIntervalCannotConfirmOrHandoff();
    d2TransientVadHangoverCannotConfirm();
    actualAcPassesDespiteLargeOffset();
    shortLowEnergyPhonemesDoNotResetVadWindow();
    belowAndExactThresholdUsesWindowEnergy();
    abandonedCandidateCannotLendEnergyToNextWindow();
    signedPcmExtremesHaveBoundedStatistics();
    d2GatePreservesUnmodifiedPcmAndLabels();
    reenablingClearsEnergyDiagnostics();
    std::puts("Barge-in observer: 33 scenarios passed (D1/D2 AC activity gate and PCM handoff; no acoustics).");
}
