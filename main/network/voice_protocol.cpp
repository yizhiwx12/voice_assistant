#include "voice_protocol.hpp"

namespace voice {
namespace {

void put32(uint8_t *destination, uint32_t value)
{
    for (unsigned int byte = 0; byte < 4; ++byte) {
        destination[byte] = static_cast<uint8_t>(value >> (8 * byte));
    }
}

uint32_t get32(const uint8_t *source)
{
    uint32_t value = 0;
    for (unsigned int byte = 0; byte < 4; ++byte) {
        value |= static_cast<uint32_t>(source[byte]) << (8 * byte);
    }
    return value;
}

bool valid(const PacketHeader &header)
{
    if (header.identity.epoch == 0 || header.stream_id == 0) {
        return false;
    }
    if (header.kind == kUplink) {
        return header.identity.turn == 0 && header.identity.generation == 0;
    }
    return header.kind == kDownlink && header.identity.turn != 0 && header.identity.generation != 0;
}

} // 匿名命名空间结束

bool packHeader(const PacketHeader &header, uint8_t *destination, size_t capacity)
{
    if (destination == nullptr || capacity < kHeaderBytes || !valid(header)) {
        return false;
    }
    destination[0] = 1;
    destination[1] = header.kind;
    destination[2] = static_cast<uint8_t>(kHeaderBytes);
    destination[3] = 0;
    put32(destination + 4, header.identity.epoch);
    put32(destination + 8, header.stream_id);
    put32(destination + 12, header.identity.turn);
    put32(destination + 16, header.identity.generation);
    put32(destination + 20, header.sequence);
    put32(destination + 24, static_cast<uint32_t>(header.sample_offset));
    put32(destination + 28, static_cast<uint32_t>(header.sample_offset >> 32));
    return true;
}

bool unpackHeader(const uint8_t *packet, size_t packet_bytes, PacketHeader &header)
{
    if (packet == nullptr || packet_bytes <= kHeaderBytes ||
        packet_bytes > kHeaderBytes + kMaxPayloadBytes || packet[0] != 1 ||
        packet[2] != kHeaderBytes || packet[3] != 0) {
        return false;
    }
    PacketHeader candidate{};
    candidate.kind = packet[1];
    candidate.identity.epoch = get32(packet + 4);
    candidate.stream_id = get32(packet + 8);
    candidate.identity.turn = get32(packet + 12);
    candidate.identity.generation = get32(packet + 16);
    candidate.sequence = get32(packet + 20);
    candidate.sample_offset = static_cast<uint64_t>(get32(packet + 24)) |
                              (static_cast<uint64_t>(get32(packet + 28)) << 32);
    if (!valid(candidate)) {
        return false;
    }
    header = candidate;
    return true;
}

} // 命名空间 voice 结束
