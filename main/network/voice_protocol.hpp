#pragma once

#include <cstddef>
#include <cstdint>

namespace voice {

constexpr size_t kHeaderBytes = 32;
constexpr size_t kMaxPayloadBytes = 1275;
constexpr uint8_t kUplink = 1;
constexpr uint8_t kDownlink = 2;

// 对话身份：epoch 随新连接变化，turn 区分用户轮次，generation 区分该轮回答。
// 接收方必须同时校验身份和流序号，防止取消/重连后播放迟到的旧音频。
struct Identity {
    uint32_t epoch = 0;
    uint32_t turn = 0;
    uint32_t generation = 0;

    bool operator==(const Identity &other) const
    {
        return epoch == other.epoch && turn == other.turn && generation == other.generation;
    }
    bool operator!=(const Identity &other) const { return !(*this == other); }
};

struct PacketHeader {
    uint8_t kind = 0;
    Identity identity{};
    uint32_t stream_id = 0;
    uint32_t sequence = 0;
    uint64_t sample_offset = 0;
};

// 不使用紧凑结构体或本机字节序强制转换；这些函数也在电脑端测试中运行。
bool packHeader(const PacketHeader &header, uint8_t *destination, size_t capacity);
bool unpackHeader(const uint8_t *packet, size_t packet_bytes, PacketHeader &header);

} // 命名空间 voice 结束
