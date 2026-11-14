#include "audio/automatic_utterance.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

namespace {
using Utterance = voiceaudio::AutomaticUtterance;
using Phase = Utterance::Phase;
using Result = Utterance::Result;
constexpr size_t kSamples = 320;

int16_t value(uint64_t offset) { return static_cast<int16_t>((offset * 17U + 7U) % 32761U); }
std::array<int16_t, kSamples> signal(uint64_t offset)
{
    std::array<int16_t, kSamples> pcm{};
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = value(offset + i);
    return pcm;
}
struct Fixture {
    std::vector<Utterance::Frame> storage;
    Utterance input;
    uint64_t offset = 0;
    explicit Fixture(size_t capacity = 114, size_t preroll = 10, size_t silence = 35,
                     size_t confirmation = 1, uint32_t min_ac_rms = 0) : storage(capacity)
    { input.configure(storage.data(), capacity, preroll, silence, confirmation, min_ac_rms); }
    Result append(bool speech)
    {
        const auto pcm = signal(offset);
        const auto result = input.append(pcm.data(), pcm.size(), offset, speech);
        offset += pcm.size();
        return result;
    }
    Result appendLevel(bool speech, int32_t amplitude, int32_t dc = 0)
    {
        std::array<int16_t, kSamples> pcm{};
        for (size_t i = 0; i < pcm.size(); ++i)
            pcm[i] = static_cast<int16_t>(dc + (i % 2 ? amplitude : -amplitude));
        const auto result = input.append(pcm.data(), pcm.size(), offset, speech);
        offset += pcm.size();
        return result;
    }
    void quiet(size_t frames)
    { for (size_t i = 0; i < frames; ++i) assert(append(false) == Result::Accepted); }
    void trigger()
    {
        input.listen();
        quiet(15);
        assert(append(true) == Result::Triggered);
        assert(input.phase() == Phase::Opening);
    }
};
std::vector<Utterance::Frame> drain(Utterance &input)
{
    std::vector<Utterance::Frame> output;
    Utterance::Frame frame{};
    while (input.pop(frame)) output.push_back(frame);
    return output;
}
void checkSequence(const std::vector<Utterance::Frame> &frames, uint64_t first)
{
    for (const auto &frame : frames) {
        assert(frame.offset == first);
        for (size_t i = 0; i < frame.pcm.size(); ++i) assert(frame.pcm[i] == value(first + i));
        first += kSamples;
    }
}

void explicitIdleAndQuietHaveNoTurn()
{
    Fixture f;
    assert(f.input.phase() == Phase::Disabled);
    for (size_t i = 0; i < 100; ++i) assert(f.append(true) == Result::Accepted);
    assert(f.input.size() == 0 && f.input.utteranceFrames() == 0);
    f.input.listen();
    f.quiet(2000);  // 超过 30 秒无语音仍须保持空闲。
    assert(f.input.phase() == Phase::Listening && f.input.utteranceFrames() == 0);
    assert(f.input.size() == 10);
}
void quietGateRequiresThreeHundredMilliseconds()
{
    Fixture f;
    f.input.listen();
    for (size_t i = 0; i < 100; ++i) assert(f.append(true) == Result::Accepted);
    f.quiet(14);
    assert(f.append(true) == Result::Accepted);  // 280 ms 不足以布防。
    f.quiet(15);
    assert(f.append(true) == Result::Triggered);
    assert(f.input.phase() == Phase::Opening);
}
void largerPrerollCoversDelayedVadOnset()
{
    Fixture f(124, 20);
    f.input.listen();
    f.quiet(15);  // 播放后的静音门限已通过。
    // 模拟 280 ms 真实起始音频，但其 VAD 标签晚到；
    // 这些连续 AFE 帧仍经过应用预录历史。
    for (size_t i = 0; i < 14; ++i) assert(f.append(false) == Result::Accepted);
    assert(f.append(true) == Result::Triggered);
    const auto frames = drain(f.input);
    assert(frames.size() == 21);  // 400 ms 历史加触发帧。
    checkSequence(frames, 9 * kSamples);
    assert(frames[6].offset == 15 * kSamples);  // 延迟 VAD 的语音起始仍然存在。
    assert(f.input.phase() == Phase::Opening);
}
void prerollRetainsHistoryAndContinuousSamples()
{
    Fixture f;
    f.trigger();
    assert(f.input.size() == 11);  // 前十帧加触发帧。
    auto output = drain(f.input);
    assert(output.size() == 11 && f.input.utteranceFrames() == 11);
    checkSequence(output, 5 * kSamples);
    f.input.started();
    assert(f.append(true) == Result::Accepted);
    const auto live = drain(f.input);
    assert(live.size() == 1);
    assert(live.front().offset == output.back().offset + kSamples);
    checkSequence(live, 16 * kSamples);
}
void silenceEndpointKeepsAllTailAudio()
{
    Fixture f;
    f.trigger();
    f.input.started();
    auto output = drain(f.input);
    f.quiet(34);
    assert(f.input.phase() == Phase::Streaming);
    assert(f.append(false) == Result::Accepted);
    assert(f.input.phase() == Phase::Draining);
    auto tail = drain(f.input);
    output.insert(output.end(), tail.begin(), tail.end());
    assert(output.size() == 46 && f.input.utteranceFrames() == 46);
    checkSequence(output, 5 * kSamples);
    const auto stopped_offset = f.input.nextOffset();
    assert(f.append(true) == Result::Accepted);
    assert(f.input.size() == 0 && f.input.nextOffset() == stopped_offset);
}
void speechResetsSilenceCountdown()
{
    Fixture f;
    f.trigger();
    f.input.started();
    drain(f.input);
    f.quiet(34);
    assert(f.append(true) == Result::Accepted);
    f.quiet(34);
    assert(f.input.phase() == Phase::Streaming);
    f.quiet(1);
    assert(f.input.phase() == Phase::Draining);
}
void maximumIncludesPrerollAndEndsOnce()
{
    Fixture f;
    f.trigger();
    f.input.started();
    auto output = drain(f.input);
    while (f.input.utteranceFrames() < 1499) {
        assert(f.append(true) == Result::Accepted);
        assert(f.input.phase() == Phase::Streaming);
        const auto live = drain(f.input);
        output.insert(output.end(), live.begin(), live.end());
    }
    assert(f.append(true) == Result::Accepted);
    assert(f.input.phase() == Phase::Draining && f.input.utteranceFrames() == 1500);
    const auto last = drain(f.input);
    output.insert(output.end(), last.begin(), last.end());
    assert(output.size() == 1500);
    checkSequence(output, 5 * kSamples);
    assert(f.append(true) == Result::Accepted && f.input.size() == 0);
}
void explicitFinishWaitsForPendingAfeSamples()
{
    Fixture f;
    f.trigger();
    f.input.started();
    drain(f.input);
    // 请求的采集边界位于未来第三个输出帧内部。
    f.input.finishAfter(f.input.nextOffset() + 2 * kSamples + 150);
    for (size_t i = 0; i < 2; ++i) {
        assert(f.append(true) == Result::Accepted);
        assert(f.input.phase() == Phase::Streaming);
    }
    assert(f.append(true) == Result::Accepted && f.input.phase() == Phase::Draining);
    auto output = drain(f.input);
    assert(output.size() == 3);
    checkSequence(output, 16 * kSamples);
}
void alreadyAvailableFinishBoundaryDrainsImmediately()
{
    Fixture f;
    f.trigger();
    f.input.finishAfter(f.input.nextOffset());
    assert(f.input.phase() == Phase::Draining && f.input.size() == 11);
    f.input.started();
    assert(f.input.phase() == Phase::Draining);
    auto output = drain(f.input);
    checkSequence(output, 5 * kSamples);
}
void utteranceMayFinishBeforeHandshakeReady()
{
    Fixture f;
    f.trigger();
    f.quiet(34);
    assert(f.input.phase() == Phase::Opening);
    f.quiet(1);
    assert(f.input.phase() == Phase::Draining);
    f.input.started();  // 迟到的 speech.started 不能重新开启此语音段。
    assert(f.input.phase() == Phase::Draining);
    auto output = drain(f.input);
    assert(output.size() == 46);
    checkSequence(output, 5 * kSamples);
}
void overflowIsBoundedAndDoesNotOverwrite()
{
    Fixture f(12, 2);
    f.trigger();
    assert(f.input.size() == 3);
    for (size_t i = 0; i < 9; ++i) assert(f.append(true) == Result::Accepted);
    assert(f.input.size() == 12);
    assert(f.append(true) == Result::Overflow && f.input.size() == 12);
    auto output = drain(f.input);
    assert(output.size() == 12);
    checkSequence(output, 13 * kSamples);
}
void malformedAndMissingFramesAreRejected()
{
    Fixture f;
    f.trigger();
    const auto offset = f.input.nextOffset();
    auto pcm = signal(offset);
    const auto size = f.input.size();
    assert(f.input.append(nullptr, 320, offset, true) == Result::Discontinuity);
    assert(f.input.append(pcm.data(), 319, offset, true) == Result::Discontinuity);
    assert(f.input.append(pcm.data(), 320, offset + 320, true) == Result::Discontinuity);
    assert(f.input.append(pcm.data(), 320, offset - 320, true) == Result::Discontinuity);
    assert(f.input.size() == size && f.input.nextOffset() == offset);
    assert(f.append(true) == Result::Accepted);
    auto output = drain(f.input);
    checkSequence(output, 5 * kSamples);
}
void resumedListeningDiscardsPriorReplyAndHistory()
{
    Fixture f;
    f.trigger();
    f.input.finishAfter(f.input.nextOffset());
    f.input.listen();
    assert(f.input.size() == 0 && f.input.utteranceFrames() == 0 && f.input.nextOffset() == 0);
    f.offset = 100000;  // 回答播放后任意的当前 AFE 时钟。
    assert(f.append(true) == Result::Accepted);
    f.quiet(15);
    assert(f.append(true) == Result::Triggered);
    auto output = drain(f.input);
    assert(output.size() == 11);
    checkSequence(output, 100000 + 6 * kSamples);
    f.input.disable();
    assert(f.input.phase() == Phase::Disabled && f.input.size() == 0);
    assert(f.append(true) == Result::Accepted && f.input.size() == 0);
}
void ringWrapAndSlowConsumerPreserveOrder()
{
    Fixture f(18);
    f.trigger();
    f.input.started();
    auto output = drain(f.input);
    for (size_t batch = 0; batch < 40; ++batch) {
        for (size_t i = 0; i < 17; ++i) assert(f.append(true) == Result::Accepted);
        auto buffered = drain(f.input);
        output.insert(output.end(), buffered.begin(), buffered.end());
    }
    assert(output.size() == 691);
    checkSequence(output, 5 * kSamples);
}

std::vector<Utterance::Frame> confirmedSource(size_t count, size_t speech_start,
                                             size_t speech_end, uint64_t first = 100000)
{
    std::vector<Utterance::Frame> source(count);
    for (size_t i = 0; i < count; ++i) {
        auto &frame = source[i];
        frame.offset = first + i * kSamples;
        frame.pcm = signal(frame.offset);
        frame.speech = i >= speech_start && i < speech_end;
    }
    return source;
}
void confirmedHandoffBypassesQuietGateAndPreservesRingOnset()
{
    Fixture f(134, 20);
    auto linear = confirmedSource(40, 20, 35);
    std::vector<Utterance::Frame> ring(47);
    const size_t head = 43;
    for (size_t i = 0; i < linear.size(); ++i) ring[(head + i) % ring.size()] = linear[i];
    assert(f.input.adoptConfirmed(ring.data(), ring.size(), head, linear.size(),
                                 linear[20].offset, linear[30].offset));
    assert(f.input.phase() == Phase::Opening && f.input.utteranceFrames() == 40);
    assert(f.input.nextOffset() == 100000 + 40 * kSamples);
    auto output = drain(f.input);
    checkSequence(output, 100000);
    assert(output.size() == 40);
    for (size_t i = 0; i < output.size(); ++i) assert(output[i].speech == linear[i].speech);
    f.input.started();
    f.offset = f.input.nextOffset();
    f.quiet(29);  // 确认语音后保留五个安静帧。
    assert(f.input.phase() == Phase::Streaming);
    f.quiet(1);
    assert(f.input.phase() == Phase::Draining);
}
void confirmedHandoffHistoryCannotEndBeforeConfirmedSpeech()
{
    Fixture f(150, 60);
    // 有意保留超过结束点阈值的安静历史。
    // 确认片段仍须保有完整起始部分。
    auto source = confirmedSource(75, 60, 70);
    assert(f.input.adoptConfirmed(source.data(), source.size(), 0, source.size(),
                                 source[60].offset, source[70].offset));
    assert(f.input.phase() == Phase::Opening && f.input.size() == 75);
    auto output = drain(f.input);
    checkSequence(output, 100000);
}
void completedHandoffStopsAtFirstQuietEndpoint()
{
    Fixture f(150, 20);
    auto source = confirmedSource(90, 20, 30);
    // 首个 700 ms 静音之后的新无关语音不能加入。
    source[75].speech = true;
    assert(f.input.adoptConfirmed(source.data(), source.size(), 0, source.size(),
                                 source[20].offset, source[30].offset));
    assert(f.input.phase() == Phase::Draining && f.input.size() == 65);
    assert(f.input.nextOffset() == 100000 + 65 * kSamples);
    f.input.started();
    assert(f.input.phase() == Phase::Draining);
    const auto output = drain(f.input);
    assert(output.size() == 65);
    checkSequence(output, 100000);
}
void rejectedHandoffsCannotAlterExistingDestination()
{
    Fixture f(50);
    f.trigger();
    const auto size = f.input.size();
    const auto next = f.input.nextOffset();
    auto source = confirmedSource(40, 20, 35);
    const auto candidate = source[20].offset, confirmed = source[30].offset;
    const auto unchanged = [&] {
        assert(f.input.phase() == Phase::Opening && f.input.size() == size);
        assert(f.input.nextOffset() == next);
    };
    assert(!f.input.adoptConfirmed(nullptr, 40, 0, 40, candidate, confirmed));
    unchanged();
    assert(!f.input.adoptConfirmed(source.data(), 40, 40, 40, candidate, confirmed));
    unchanged();
    assert(!f.input.adoptConfirmed(source.data(), 40, 0, 41, candidate, confirmed));
    unchanged();
    assert(!f.input.adoptConfirmed(source.data(), 40, 0, 0, candidate, confirmed));
    unchanged();
    assert(!f.input.adoptConfirmed(source.data(), 40, 0, 40, candidate, candidate));
    unchanged();
    source[23].speech = false;
    assert(!f.input.adoptConfirmed(source.data(), 40, 0, 40, candidate, confirmed));
    unchanged();
    source[23].speech = true;
    source[39].offset += kSamples;
    assert(!f.input.adoptConfirmed(source.data(), 40, 0, 40, candidate, confirmed));
    unchanged();
    assert(!f.input.adoptConfirmed(f.storage.data(), f.storage.size(), 0, size,
                                  5 * kSamples, next));
    unchanged();
    const auto output = drain(f.input);
    checkSequence(output, 5 * kSamples);
}
void confirmedHandoffHonorsCapacityAndThirtySecondLimit()
{
    Fixture too_small(29);
    auto source = confirmedSource(30, 20, 30);
    assert(!too_small.input.adoptConfirmed(source.data(), source.size(), 0, source.size(),
                                          source[20].offset, source.back().offset + kSamples));
    assert(too_small.input.phase() == Phase::Disabled && too_small.input.size() == 0);
    Fixture full(1501);
    source = confirmedSource(1501, 0, 1501);
    assert(!full.input.adoptConfirmed(source.data(), source.size(), 0, source.size(),
                                     source[0].offset, source[10].offset));
    assert(full.input.adoptConfirmed(source.data(), source.size(), 0, 1500,
                                    source[0].offset, source[10].offset));
    assert(full.input.phase() == Phase::Draining && full.input.utteranceFrames() == 1500);
    assert(full.input.size() == 1500);
}
void sampleOffsetOverflowIsRejectedWithoutChangingClock()
{
    Fixture f;
    f.input.listen();
    auto pcm = signal(0);
    const uint64_t offset = UINT64_MAX - 319;
    assert(f.input.append(pcm.data(), pcm.size(), offset, true) == Result::Discontinuity);
    assert(f.input.size() == 0 && f.input.nextOffset() == 0);
}
void transientWithVadHangoverNeverOpensACloudTurn()
{
    Fixture f(134, 20, 35, 8, 80);
    f.input.listen();
    f.quiet(20);
    assert(f.appendLevel(true, 1500) == Result::Accepted);
    for (size_t i = 0; i < 7; ++i) assert(f.appendLevel(true, 10) == Result::Accepted);
    assert(f.input.phase() == Phase::Listening && f.input.utteranceFrames() == 0);
    const auto rejected = f.input.startMetrics();
    assert(rejected.candidates == 1 && rejected.confirmations == 0);
    assert(rejected.rejected == 0 && rejected.energy_rejections == 1);
    assert(rejected.energy_active_frames == 1 && rejected.energy_tail_active_frames == 0);
    assert(!rejected.pending && f.input.size() <= 20);
    // SDK 可将同一个脉冲持续标记为语音，远超确认窗口。
    // 不能因此反复创建新候选。
    for (size_t i = 0; i < 60; ++i) assert(f.appendLevel(true, 10) == Result::Accepted);
    assert(f.input.startMetrics().candidates == 1 && f.input.size() <= 20);
    assert(f.input.phase() == Phase::Listening);
}
void defaultConfigurationUsesTheProductionOnsetGuard()
{
    Fixture f(134, 20);
    f.input.configure(f.storage.data(), f.storage.size(), 20, 35);
    assert(f.input.startMetrics().confirm_frames == 8 && f.input.startMetrics().min_ac_rms == 80);
    f.input.listen();
    f.quiet(20);
    for (size_t i = 0; i < 8; ++i)
        assert(f.appendLevel(true, 80) == (i == 7 ? Result::Triggered : Result::Accepted));
    assert(f.input.phase() == Phase::Opening && f.input.startMetrics().confirmations == 1);
}
void steadyDcCannotPassIdleEnergyPersistence()
{
    Fixture f(134, 20, 35, 8, 80);
    f.input.listen();
    f.quiet(20);
    for (size_t i = 0; i < 8; ++i) assert(f.appendLevel(true, 0, 32767) == Result::Accepted);
    const auto start = f.input.startMetrics();
    assert(start.confirmations == 0 && start.energy_rejections == 1);
    assert(start.frame_ac_rms == 0 && start.window_ac_rms == 0);
    assert(f.input.phase() == Phase::Listening && f.input.size() <= 20);
}
void quietOnsetAndWeakPhonemesCanStillConfirm()
{
    Fixture f(134, 20, 35, 8, 80);
    f.input.listen();
    f.quiet(20);
    const uint64_t candidate = f.offset;
    // 八帧中四帧活跃，包括末三帧中的两帧。
    // 较弱起始音素或中间一个安静帧都不能造成 PCM 丢失。
    const std::array<int32_t, 8> levels{{20, 30, 50, 120, 140, 0, 160, 180}};
    for (size_t i = 0; i < levels.size(); ++i)
        assert(f.appendLevel(true, levels[i], 300) ==
               (i + 1 == levels.size() ? Result::Triggered : Result::Accepted));
    const auto start = f.input.startMetrics();
    assert(start.confirmations == 1 && start.energy_rejections == 0);
    assert(start.energy_active_frames == 4 && start.energy_tail_active_frames == 2);
    assert(start.candidate_offset == candidate && start.confirmed_offset == f.offset);
    assert(f.input.phase() == Phase::Opening && f.input.size() == 28);
    const auto output = drain(f.input);
    assert(output.size() == 28);
    for (size_t frame = 0; frame < levels.size(); ++frame) {
        const auto &retained = output[20 + frame];
        assert(retained.offset == candidate + frame * kSamples && retained.speech);
        for (size_t i = 0; i < kSamples; ++i)
            assert(retained.pcm[i] == 300 + (i % 2 ? levels[frame] : -levels[frame]));
    }
}
void loudEarlyFramesCannotHideAQuietConfirmationTail()
{
    Fixture f(134, 20, 35, 8, 80);
    f.input.listen();
    f.quiet(20);
    for (size_t i = 0; i < 8; ++i)
        assert(f.appendLevel(true, i < 6 ? 1200 : 10) == Result::Accepted);
    const auto start = f.input.startMetrics();
    assert(start.window_ac_rms > 80 && start.energy_active_frames == 6);
    assert(start.energy_tail_active_frames == 1 && start.energy_rejections == 1);
    assert(start.confirmations == 0 && f.input.phase() == Phase::Listening);
}
void brokenCandidateRearmsAfterFreshQuietAndPreservesFullPreroll()
{
    Fixture f(134, 20, 35, 8, 80);
    f.input.listen();
    f.quiet(20);
    for (size_t i = 0; i < 3; ++i) assert(f.append(true) == Result::Accepted);
    assert(f.input.startMetrics().pending && f.input.size() == 23);
    assert(f.append(false) == Result::Accepted);
    assert(!f.input.startMetrics().pending && f.input.startMetrics().rejected == 1);
    assert(f.input.size() <= 20);
    f.quiet(13);  // 拒绝帧加后续 13 帧仅为 280 ms。
    assert(f.append(true) == Result::Accepted);
    assert(!f.input.startMetrics().pending && f.input.startMetrics().candidates == 1);
    f.quiet(20);
    const uint64_t candidate = f.offset;
    for (size_t i = 0; i < 8; ++i)
        assert(f.append(true) == (i == 7 ? Result::Triggered : Result::Accepted));
    assert(f.input.startMetrics().candidates == 2 && f.input.startMetrics().confirmations == 1);
    const auto output = drain(f.input);
    assert(output.size() == 28 && f.input.utteranceFrames() == 28);
    checkSequence(output, candidate - 20 * kSamples);
    for (size_t i = 0; i < output.size(); ++i) assert(output[i].speech == (i >= 20));
}
void rejectedImpulsesKeepStorageBoundedAcrossManyAttempts()
{
    Fixture f(28, 20, 35, 8, 80);
    f.input.listen();
    for (size_t attempt = 0; attempt < 50; ++attempt) {
        f.quiet(15);
        for (size_t i = 0; i < 8; ++i)
            assert(f.appendLevel(true, i == 0 ? 1800 : 0) == Result::Accepted);
        assert(f.input.phase() == Phase::Listening && f.input.size() <= 20);
        assert(!f.input.startMetrics().pending);
    }
    const auto start = f.input.startMetrics();
    assert(start.candidates == 50 && start.energy_rejections == 50 && start.confirmations == 0);
    assert(f.input.utteranceFrames() == 0);
}
void guardedResetAndDiscontinuityCannotReuseAnOldCandidate()
{
    Fixture f(134, 20, 35, 8, 80);
    f.input.listen();
    f.quiet(20);
    for (size_t i = 0; i < 3; ++i) assert(f.append(true) == Result::Accepted);
    const auto before = f.input.startMetrics();
    const auto before_size = f.input.size(), before_offset = f.input.nextOffset();
    const auto pcm = signal(f.offset);
    assert(f.input.append(pcm.data(), pcm.size(), f.offset + kSamples, true) == Result::Discontinuity);
    assert(f.input.size() == before_size && f.input.nextOffset() == before_offset);
    assert(f.input.startMetrics().pending && f.input.startMetrics().window_frames == before.window_frames);
    f.input.disable();
    assert(!f.input.startMetrics().pending && f.input.size() == 0);
    f.input.listen();
    assert(f.append(true) == Result::Accepted);  // 旧安静/候选不能重新布防。
    assert(!f.input.startMetrics().pending);
    f.quiet(20);
    for (size_t i = 0; i < 8; ++i)
        assert(f.append(true) == (i == 7 ? Result::Triggered : Result::Accepted));
    assert(f.input.phase() == Phase::Opening);
}
void exactIdleEnergyBoundaryAndInt16ExtremesAreSafe()
{
    for (const int32_t amplitude : {79, 80}) {
        Fixture f(134, 20, 35, 8, 80);
        f.input.listen();
        f.quiet(20);
        for (size_t i = 0; i < 8; ++i)
            assert(f.appendLevel(true, amplitude) ==
                   (amplitude == 80 && i == 7 ? Result::Triggered : Result::Accepted));
        const auto start = f.input.startMetrics();
        assert(start.frame_ac_rms == static_cast<uint32_t>(amplitude));
        assert(start.window_ac_rms == static_cast<uint32_t>(amplitude));
        assert(start.confirmations == static_cast<uint32_t>(amplitude == 80));
    }
    Fixture f(134, 20, 35, 8, 80);
    f.input.listen();
    f.quiet(20);
    std::array<int16_t, kSamples> extremes{};
    for (size_t i = 0; i < extremes.size(); ++i) extremes[i] = i % 2 ? INT16_MAX : INT16_MIN;
    for (size_t i = 0; i < 8; ++i) {
        assert(f.input.append(extremes.data(), extremes.size(), f.offset, true) ==
               (i == 7 ? Result::Triggered : Result::Accepted));
        f.offset += kSamples;
    }
    assert(f.input.startMetrics().frame_ac_rms == 32767 && f.input.startMetrics().window_ac_rms == 32767);
    assert(f.input.startMetrics().confirmations == 1 && f.input.phase() == Phase::Opening);
}
void guardedDestinationTrustsAnAlreadyConfirmedD2Handoff()
{
    Fixture f(134, 20, 35, 8, 80);
    auto source = confirmedSource(40, 20, 35);
    // D2 已执行自身语音/能量确认。空闲检测器
    // 不能再要求更多语音、安静或第二道能量门限。
    for (auto &frame : source) frame.pcm.fill(5);
    assert(f.input.adoptConfirmed(source.data(), source.size(), 0, source.size(),
                                 source[20].offset, source[28].offset));
    assert(f.input.phase() == Phase::Opening && f.input.size() == source.size());
    assert(!f.input.startMetrics().pending);
    const auto output = drain(f.input);
    for (size_t i = 0; i < output.size(); ++i) {
        assert(output[i].offset == source[i].offset && output[i].speech == source[i].speech);
        assert(output[i].pcm == source[i].pcm);
    }
}
}  // 命名空间

int main()
{
    explicitIdleAndQuietHaveNoTurn();
    quietGateRequiresThreeHundredMilliseconds();
    largerPrerollCoversDelayedVadOnset();
    prerollRetainsHistoryAndContinuousSamples();
    silenceEndpointKeepsAllTailAudio();
    speechResetsSilenceCountdown();
    maximumIncludesPrerollAndEndsOnce();
    explicitFinishWaitsForPendingAfeSamples();
    alreadyAvailableFinishBoundaryDrainsImmediately();
    utteranceMayFinishBeforeHandshakeReady();
    overflowIsBoundedAndDoesNotOverwrite();
    malformedAndMissingFramesAreRejected();
    resumedListeningDiscardsPriorReplyAndHistory();
    ringWrapAndSlowConsumerPreserveOrder();
    confirmedHandoffBypassesQuietGateAndPreservesRingOnset();
    confirmedHandoffHistoryCannotEndBeforeConfirmedSpeech();
    completedHandoffStopsAtFirstQuietEndpoint();
    rejectedHandoffsCannotAlterExistingDestination();
    confirmedHandoffHonorsCapacityAndThirtySecondLimit();
    sampleOffsetOverflowIsRejectedWithoutChangingClock();
    transientWithVadHangoverNeverOpensACloudTurn();
    defaultConfigurationUsesTheProductionOnsetGuard();
    steadyDcCannotPassIdleEnergyPersistence();
    quietOnsetAndWeakPhonemesCanStillConfirm();
    loudEarlyFramesCannotHideAQuietConfirmationTail();
    brokenCandidateRearmsAfterFreshQuietAndPreservesFullPreroll();
    rejectedImpulsesKeepStorageBoundedAcrossManyAttempts();
    guardedResetAndDiscontinuityCannotReuseAnOldCandidate();
    exactIdleEnergyBoundaryAndInt16ExtremesAreSafe();
    guardedDestinationTrustsAnAlreadyConfirmedD2Handoff();
    std::puts("Automatic utterance: 30 production sample-boundary and onset scenarios passed");
}
