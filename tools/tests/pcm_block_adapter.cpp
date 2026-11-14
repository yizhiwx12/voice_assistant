#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#include "audio/pcm_block_adapter.hpp"

namespace {
std::vector<int16_t> signal(size_t count, size_t offset = 0)
{
    std::vector<int16_t> samples(count);
    for (size_t i = 0; i < count; ++i)
        samples[i] = static_cast<int16_t>((i + offset) * 7919U + 17U);
    return samples;
}

template <size_t Capacity>
auto collect(std::vector<int16_t> &output, size_t expected_block)
{
    return [&output, expected_block](const int16_t *samples, size_t count) {
        assert(count == expected_block);
        output.insert(output.end(), samples, samples + count);
        return true;
    };
}

void configurationAndEmptyInput()
{
    voiceaudio::PcmBlockAdapter<512> adapter;
    const auto input = signal(320);
    std::vector<int16_t> output;
    auto consume = collect<512>(output, 512);
    assert(!adapter.append(input.data(), input.size(), consume));
    assert(!adapter.configure(0));
    assert(!adapter.configure(513));
    assert(adapter.configure(512));
    assert(adapter.append(nullptr, 0, consume));
    assert(!adapter.append(nullptr, 1, consume));
    assert(output.empty() && adapter.pendingSamples() == 0);
}

void irregularSourceChunks()
{
    const auto input = signal(81920);
    voiceaudio::PcmBlockAdapter<2048> adapter;
    assert(adapter.configure(512));
    std::vector<int16_t> output;
    auto consume = collect<2048>(output, 512);
    const size_t chunks[] = {1, 17, 320, 777, 13, 2048, 511};
    size_t offset = 0;
    size_t next = 0;
    while (offset < input.size()) {
        const size_t count = std::min(chunks[next++ % 7], input.size() - offset);
        assert(adapter.append(input.data() + offset, count, consume));
        offset += count;
    }
    assert(output == input);
    assert(adapter.pendingSamples() == 0);
}

void twentyMillisecondRoundTrip()
{
    // 320 样本麦克风帧 -> 运行时 AFE 块 -> 320 样本帧。
    // 无语音门控、VAD 缓存插入、重复或补零。
    for (const size_t afe_block : {160U, 256U, 480U, 512U, 1024U, 2048U}) {
        const auto input = signal(30720);  // 可被全部测试大小整除。
        voiceaudio::PcmBlockAdapter<2048> feed;
        voiceaudio::PcmBlockAdapter<320> fetch;
        assert(feed.configure(afe_block));
        assert(fetch.configure(320));
        std::vector<int16_t> output;
        auto emit = collect<320>(output, 320);
        for (size_t offset = 0; offset < input.size(); offset += 320) {
            assert(feed.append(input.data() + offset, 320,
                [&fetch, &emit](const int16_t *pcm, size_t count) {
                    return fetch.append(pcm, count, emit);
                }));
        }
        assert(output == input);
        assert(feed.pendingSamples() == 0 && fetch.pendingSamples() == 0);
    }
}

void resetDiscardsOnlyPartialTail()
{
    voiceaudio::PcmBlockAdapter<512> adapter;
    assert(adapter.configure(512));
    const auto previous = signal(600);
    const auto next = signal(512, 600);
    std::vector<int16_t> output;
    auto emit = collect<512>(output, 512);
    assert(adapter.append(previous.data(), previous.size(), emit));
    assert(output.size() == 512 && adapter.pendingSamples() == 88);
    adapter.reset();
    assert(adapter.pendingSamples() == 0);
    assert(adapter.append(next.data(), next.size(), emit));
    assert(output.size() == 1024);
    assert(std::equal(previous.begin(), previous.begin() + 512, output.begin()));
    assert(std::equal(next.begin(), next.end(), output.begin() + 512));
}

void rejectedConsumerRequiresExplicitRecovery()
{
    voiceaudio::PcmBlockAdapter<512> adapter;
    assert(adapter.configure(512));
    const auto input = signal(1600);
    unsigned calls = 0;
    assert(!adapter.append(input.data(), input.size(), [&calls](const int16_t *, size_t count) {
        assert(count == 512);
        ++calls;
        return false;
    }));
    assert(calls == 1);
    adapter.reset();
    std::vector<int16_t> output;
    assert(adapter.append(input.data() + 512, 512, collect<512>(output, 512)));
    assert(output == std::vector<int16_t>(input.begin() + 512, input.begin() + 1024));
}

void pairedFramesRemainSampleAligned()
{
    const auto microphone = signal(4096, 11);
    const auto reference = signal(4096, 9000);
    voiceaudio::PairedPcmBlockAdapter<2048> adapter;
    assert(!adapter.configure(0));
    assert(!adapter.configure(2049));
    assert(adapter.configure(480));
    std::vector<int16_t> output;
    auto consume = [&output](const int16_t *samples, size_t frames) {
        assert(frames == 480);
        output.insert(output.end(), samples, samples + frames * 2);
        return true;
    };
    size_t offset = 0;
    const size_t chunks[] = {1, 319, 17, 320, 73, 911};
    size_t next = 0;
    while (offset + 480 <= microphone.size()) {
        const size_t count = std::min({chunks[next++ % 6], microphone.size() - offset,
                                      static_cast<size_t>(480 - adapter.pendingFrames())});
        assert(adapter.append(microphone.data() + offset, reference.data() + offset, count, consume));
        offset += count;
    }
    assert(offset == 3840 && adapter.pendingFrames() == 0);
    for (size_t i = 0; i < offset; ++i) {
        assert(output[2 * i] == microphone[i]);
        assert(output[2 * i + 1] == reference[i]);
    }
    adapter.reset();
    const auto next_mic = signal(480, offset);
    const auto next_ref = signal(480, offset + 5000);
    assert(adapter.append(next_mic.data(), next_ref.data(), 480, consume));
    assert(output.size() == (offset + 480) * 2);
    for (size_t i = 0; i < 480; ++i) {
        assert(output[2 * (offset + i)] == next_mic[i]);
        assert(output[2 * (offset + i) + 1] == next_ref[i]);
    }
}
}  // 命名空间

int main()
{
    configurationAndEmptyInput();
    irregularSourceChunks();
    twentyMillisecondRoundTrip();
    resetDiscardsOnlyPartialTail();
    rejectedConsumerRequiresExplicitRecovery();
    pairedFramesRemainSampleAligned();
    std::cout << "PCM block adapter: 6 scenarios, including paired MR alignment, passed\n";
}
