"""交叉核对真实 C++ 线上协议实现与 Python 服务端。

使用电脑端 C++ 编译器，不烧录硬件，也不连接云服务。
在项目根目录使用服务端 Python 环境运行。编译输出和可执行文件
保存在临时目录，不写入固件源码目录。
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "server"))
from transport.voice_protocol import AudioPacket, DOWNLINK, UPLINK  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=shutil.which("g++"))
    args = parser.parse_args()
    if not args.compiler:
        parser.error("A host C++ compiler is required; pass --compiler")
    commands, expected = [], []
    valid = [
        AudioPacket(UPLINK, 1, 1, 0, 0, 0, 0, b"\x00"),
        AudioPacket(UPLINK, 0xF2345678, 0x87654321, 0, 0, 0xFFFFFFFF,
                    0xFEDCBA9876543210, bytes(range(255)) * 5),
        AudioPacket(DOWNLINK, 0xFFFFFFFF, 0x80000001, 0xABCDEF01, 0xDEADBEEF,
                    0xF0E1D2C3, 0xFFFFFFFFFFFFFFFF, b"\x55\xaa" * 480),
        AudioPacket(UPLINK, 2, 3, 0, 0, 5, 0x100000001, bytes(640)),
    ]
    for packet in valid:
        fields = (packet.kind, packet.epoch, packet.stream_id, packet.turn_id,
                  packet.generation, packet.sequence, packet.sample_offset)
        commands.append("pack " + " ".join(map(str, fields)) + " " + packet.payload.hex())
        expected.append(packet.pack().hex())
        commands.append("unpack " + packet.pack().hex())
        expected.append(" ".join(map(str, fields)))

    baseline = valid[0].pack()
    invalid = [baseline[:31], baseline[:32], baseline + bytes(1275)]
    for position, value in ((0, 2), (1, 3), (2, 31), (3, 1)):
        bad = bytearray(baseline)
        bad[position] = value
        invalid.append(bytes(bad))
    # 身份校验比 Python 外层封装解析更严格；
    # 服务端传输层在把音频交给服务商前也会检查这些约束。
    invalid.extend([
        AudioPacket(UPLINK, 0, 1, 0, 0, 0, 0, b"x").pack(),
        AudioPacket(UPLINK, 1, 0, 0, 0, 0, 0, b"x").pack(),
        AudioPacket(UPLINK, 1, 1, 1, 0, 0, 0, b"x").pack(),
        AudioPacket(DOWNLINK, 1, 1, 0, 1, 0, 0, b"x").pack(),
        AudioPacket(DOWNLINK, 1, 1, 1, 0, 0, 0, b"x").pack(),
    ])
    commands.extend("unpack " + packet.hex() for packet in invalid)
    expected.extend("INVALID" for _ in invalid)
    commands.append("pack 1 1 1 1 0 0 0 00")
    expected.append("INVALID")

    with tempfile.TemporaryDirectory(prefix="voice-protocol-") as temporary:
        executable = Path(temporary) / "wire_fixture.exe"
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-static-libgcc", "-static-libstdc++", "-I", str(ROOT / "main"),
            str(ROOT / "main/network/voice_protocol.cpp"),
            str(ROOT / "tools/tests/voice_protocol_fixture.cpp"),
            "-o", str(executable),
        ], check=True, timeout=60)
        completed = subprocess.run(
            [str(executable)], input="\n".join(commands) + "\n",
            text=True, capture_output=True, check=True, timeout=15,
        )
        actual = completed.stdout.splitlines()
        if actual != expected:
            for index, (want, got) in enumerate(zip(expected, actual), 1):
                if want != got:
                    raise AssertionError(f"Wire vector {index} differs")
            raise AssertionError("C++ wire fixture returned an unexpected number of results")
        # 使用真实 Python 解码器独立解析 C++ 输出。
        for index, packet in enumerate(valid):
            assert AudioPacket.unpack(bytes.fromhex(actual[index * 2])) == packet
    print(f"Passed {len(expected)} C++/Python wire vectors; no hardware or cloud was used")


if __name__ == "__main__":
    main()
