// 仅主机使用的线上协议测试数据，旁边编译实际固件实现。
#include "network/voice_protocol.hpp"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
bool fromHex(const std::string &text, std::vector<uint8_t> &bytes)
{
    if (text.size() % 2) return false;
    for (size_t i = 0; i < text.size(); i += 2) {
        unsigned int value;
        std::istringstream input(text.substr(i, 2));
        if (!(input >> std::hex >> value) || !input.eof()) return false;
        bytes.push_back(static_cast<uint8_t>(value));
    }
    return true;
}

void printHex(const std::vector<uint8_t> &bytes)
{
    std::ostringstream output;
    for (const uint8_t value : bytes) {
        output << std::hex << std::setfill('0') << std::setw(2)
               << static_cast<unsigned int>(value);
    }
    std::cout << output.str() << '\n';
}
} // 命名空间

int main()
{
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream input(line);
        std::string command, hex;
        input >> command;
        voice::PacketHeader header{};
        if (command == "pack") {
            unsigned int kind;
            if (!(input >> kind >> header.identity.epoch >> header.stream_id
                  >> header.identity.turn >> header.identity.generation >> header.sequence
                  >> header.sample_offset >> hex)) return 2;
            header.kind = static_cast<uint8_t>(kind);
            std::vector<uint8_t> payload;
            if (!fromHex(hex, payload)) return 2;
            std::vector<uint8_t> packet(voice::kHeaderBytes + payload.size());
            if (!voice::packHeader(header, packet.data(), packet.size())) {
                std::cout << "INVALID\n";
            } else {
                std::copy(payload.begin(), payload.end(), packet.begin() + voice::kHeaderBytes);
                printHex(packet);
            }
        } else if (command == "unpack") {
            input >> hex;
            std::vector<uint8_t> packet;
            if (!fromHex(hex, packet)) return 2;
            if (!voice::unpackHeader(packet.data(), packet.size(), header)) {
                std::cout << "INVALID\n";
            } else {
                std::cout << static_cast<unsigned int>(header.kind) << ' '
                          << header.identity.epoch << ' ' << header.stream_id << ' '
                          << header.identity.turn << ' ' << header.identity.generation << ' '
                          << header.sequence << ' ' << header.sample_offset << '\n';
            }
        } else {
            return 2;
        }
    }
}
