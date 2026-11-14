"""第 1 版设备通信格式；所有偏移量按解码后的单声道样本计数。"""

import struct
from dataclasses import dataclass

HEADER = struct.Struct("<BBHIIIIIQ")
VERSION = 1
UPLINK = 1
DOWNLINK = 2


class ProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class AudioPacket:
    kind: int
    epoch: int
    stream_id: int
    turn_id: int
    generation: int
    sequence: int
    sample_offset: int
    payload: bytes

    def pack(self):
        return HEADER.pack(VERSION, self.kind, HEADER.size, self.epoch, self.stream_id,
                           self.turn_id, self.generation, self.sequence, self.sample_offset) + self.payload

    @classmethod
    def unpack(cls, message):
        if not HEADER.size < len(message) <= HEADER.size + 1275:
            raise ProtocolError("Invalid audio message length")
        version, kind, size, *fields = HEADER.unpack_from(message)
        if version != VERSION or size != HEADER.size or kind not in (UPLINK, DOWNLINK):
            raise ProtocolError("Unsupported audio header")
        return cls(kind, *fields, message[HEADER.size:])


def uint32(value, name, *, nonzero=False):
    if type(value) is not int or not (int(nonzero) <= value <= 0xFFFFFFFF):
        raise ProtocolError(f"{name} must be a valid uint32")
    return value
