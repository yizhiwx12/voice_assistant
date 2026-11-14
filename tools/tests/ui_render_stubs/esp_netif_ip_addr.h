#pragma once
#include <cstdint>
struct esp_ip4_addr_t { uint32_t addr; };
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(address) static_cast<unsigned>((address)->addr & 0xff), static_cast<unsigned>(((address)->addr >> 8) & 0xff), static_cast<unsigned>(((address)->addr >> 16) & 0xff), static_cast<unsigned>(((address)->addr >> 24) & 0xff)
