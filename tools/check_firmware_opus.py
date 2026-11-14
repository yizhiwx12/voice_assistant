"""在电脑端编译真实固件 Opus 封装，验证原始数据包互通。

使用锁定的托管 78/esp-opus 源码及其 CMake 源文件列表，
并编译真实 main/audio/opus_codec.cpp。小型 esp_err.h 桩只替代 IDF 错误常量。
不使用音频设备、网络、云 API 或硬件。Windows 默认使用 MinGW 编译器，
同时需要安装服务端原生 libopus。
"""

import argparse
import ctypes
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "server"))
from audio.opus_codec import Decoder, Encoder, native_version  # noqa: E402


def find_tool(explicit, name, fallback):
    path = explicit or shutil.which(name) or fallback
    if not path or not Path(path).is_file():
        raise RuntimeError(f"Missing {name}; specify its executable path")
    return str(Path(path).resolve())


def cmake_path(path):
    return str(Path(path).resolve()).replace("\\", "/")


def build_probe(args):
    source = ROOT / "managed_components" / "78__esp-opus"
    if not (source / "include" / "opus.h").is_file():
        raise RuntimeError("Run idf.py reconfigure first to download the locked 78/esp-opus component")
    compiler = find_tool(args.compiler, "gcc", "C:/mingw64/bin/gcc.exe")
    cxx = str(Path(compiler).with_name("g++.exe" if os.name == "nt" else "g++"))
    if not Path(cxx).is_file():
        raise RuntimeError("g++ must be beside the selected gcc")
    cmake = find_tool(args.cmake, "cmake", "C:/Espressif/tools/cmake/4.0.3/bin/cmake.exe")
    ninja = find_tool(args.ninja, "ninja", "C:/Espressif/tools/ninja/1.12.1/ninja.exe")
    build = args.build_dir.resolve()
    project = build / "probe_source"
    project.mkdir(parents=True, exist_ok=True)
    # 复用组件自身的源文件列表，只替代 IDF 的组件注册宏。
    # 电脑端构建不使用 Xtensa 专用内建指令。
    script = f'''cmake_minimum_required(VERSION 3.22)
project(firmware_opus_probe LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(IDF_TARGET host)
function(idf_component_register)
  cmake_parse_arguments(ARG "" "" "SRCS;INCLUDE_DIRS;PRIV_INCLUDE_DIRS" ${{ARGN}})
  add_library(firmware_opus STATIC ${{ARG_SRCS}})
  target_include_directories(firmware_opus PUBLIC ${{ARG_INCLUDE_DIRS}} PRIVATE ${{ARG_PRIV_INCLUDE_DIRS}})
  set(COMPONENT_LIB firmware_opus PARENT_SCOPE)
endfunction()
add_subdirectory("{cmake_path(source)}" opus)
add_library(opus_probe SHARED
  "{cmake_path(ROOT / 'main/audio/opus_codec.cpp')}"
  "{cmake_path(ROOT / 'tools/tests/opus_probe.cpp')}")
target_include_directories(opus_probe PRIVATE
  "{cmake_path(ROOT / 'main/audio')}"
  "{cmake_path(ROOT / 'tools/tests/firmware_stubs')}")
target_link_libraries(opus_probe PRIVATE firmware_opus)
if(MINGW)
  target_link_options(opus_probe PRIVATE -static-libgcc -static-libstdc++)
endif()
'''
    (project / "CMakeLists.txt").write_text(script, encoding="utf-8")
    environment = os.environ.copy()
    environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
    commands = [
        [cmake, "--fresh", "-S", str(project), "-B", str(build / "cmake"), "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_C_COMPILER={cmake_path(compiler)}",
         f"-DCMAKE_CXX_COMPILER={cmake_path(cxx)}", f"-DCMAKE_MAKE_PROGRAM={cmake_path(ninja)}"],
        [cmake, "--build", str(build / "cmake"), "--parallel", str(args.jobs)],
    ]
    for command in commands:
        result = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=180)
        if result.returncode:
            print(result.stdout[-12000:])
            print(result.stderr[-4000:], file=sys.stderr)
            raise RuntimeError("Host codec build failed")
    library_name = "libopus_probe.dll" if os.name == "nt" else "libopus_probe.so"
    return build / "cmake" / library_name


def configure_library(path):
    lib = ctypes.CDLL(str(path))
    p16 = ctypes.POINTER(ctypes.c_int16)
    p8 = ctypes.POINTER(ctypes.c_uint8)
    lib.probe_encode.argtypes = [ctypes.c_uint32, ctypes.c_uint32, p16, ctypes.c_size_t,
                                p8, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
                                ctypes.POINTER(ctypes.c_uint32)]
    lib.probe_encode.restype = ctypes.c_int
    lib.probe_decoder_create.argtypes = [ctypes.c_uint32]
    lib.probe_decoder_create.restype = ctypes.c_void_p
    lib.probe_decode.argtypes = [ctypes.c_void_p, p8, ctypes.c_size_t, p16,
                                ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    lib.probe_decode.restype = ctypes.c_int
    lib.probe_decoder_destroy.argtypes = [ctypes.c_void_p]
    lib.probe_decoder_destroy.restype = None
    return lib


def tone(rate, count):
    return struct.pack("<" + "h" * count, *(
        int(11000 * math.sin(2 * math.pi * 523 * i / rate)) for i in range(count)))


def host_encode(lib, rate, pcm):
    samples = len(pcm) // 2
    raw = (ctypes.c_int16 * samples).from_buffer_copy(pcm)
    output = (ctypes.c_uint8 * ((samples // (rate // 50) + 3) * 1277))()
    written, skip = ctypes.c_size_t(), ctypes.c_uint32()
    assert lib.probe_encode(rate, 24000 if rate == 16000 else 32000, raw, samples,
                            output, len(output), ctypes.byref(written), ctypes.byref(skip)) == 0
    data = bytes(output[:written.value])
    packets = []
    while data:
        assert len(data) >= 2
        size = int.from_bytes(data[:2], "little")
        assert 0 < size <= 1275 and len(data) >= size + 2
        packets.append(data[2:size + 2])
        data = data[size + 2:]
    return packets, skip.value


def host_decode(lib, rate, packets):
    decoder = lib.probe_decoder_create(rate)
    assert decoder
    pcm = bytearray()
    try:
        for packet in packets:
            raw = (ctypes.c_uint8 * len(packet)).from_buffer_copy(packet)
            output = (ctypes.c_int16 * (rate // 50))()
            count = ctypes.c_size_t()
            assert lib.probe_decode(decoder, raw, len(packet), output, len(output),
                                    ctypes.byref(count)) == 0
            assert count.value == rate // 50
            pcm.extend(ctypes.string_at(output, count.value * 2))
        raw = (ctypes.c_uint8 * 1)(0)  # 10 毫秒 TOC 不能进入固定为 20 毫秒的音频流。
        assert lib.probe_decode(decoder, raw, 1, output, len(output), ctypes.byref(count)) != 0
    finally:
        lib.probe_decoder_destroy(decoder)
    return bytes(pcm)


def native_decode(rate, packets):
    decoder = Decoder(rate)
    try:
        return b"".join(decoder.decode(packet) for packet in packets)
    finally:
        decoder.close()


def check_trim(decoded, skip, samples, frame_samples):
    assert 0 < skip <= frame_samples
    assert skip + samples <= len(decoded) // 2 < skip + samples + frame_samples
    pcm = decoded[skip * 2:(skip + samples) * 2]
    assert len(pcm) == samples * 2
    # 较长输入在去除前瞻延迟后，仍必须保留真实尾部能量。
    if samples >= frame_samples * 4:
        tail = struct.unpack("<" + "h" * (frame_samples // 2), pcm[-frame_samples:])
        assert sum(value * value for value in tail) > 100000


def verify(lib):
    cases = 0
    for rate in (16000, 24000):
        frame = rate // 50
        for samples in (1, frame - 1, frame, frame + 1, frame * 2, frame * 2 + 17, frame * 8):
            pcm = tone(rate, samples)
            packets, skip = host_encode(lib, rate, pcm)
            expected = (samples + skip + frame - 1) // frame
            assert len(packets) == expected
            check_trim(native_decode(rate, packets), skip, samples, frame)
            check_trim(host_decode(lib, rate, packets), skip, samples, frame)
            encoder = Encoder(rate, 24000 if rate == 16000 else 32000)
            try:
                packets = encoder.feed(pcm) + encoder.finish()
                check_trim(host_decode(lib, rate, packets), encoder.pre_skip, samples, frame)
            finally:
                encoder.close()
            cases += 1
    # 公开接口拒绝可能回绕成特殊有符号 Opus 控制值的参数。
    pcm = (ctypes.c_int16 * 1)(0)
    output = (ctypes.c_uint8 * 4000)()
    written, skip = ctypes.c_size_t(), ctypes.c_uint32()
    for bitrate in (0, 5999, 128001, 0xFFFFFFFF):
        assert lib.probe_encode(16000, bitrate, pcm, 1, output, len(output),
                                ctypes.byref(written), ctypes.byref(skip)) != 0
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", help="Path to MinGW gcc (g++ must be beside it)")
    parser.add_argument("--cmake", help="Path to CMake executable")
    parser.add_argument("--ninja", help="Path to Ninja executable")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/host_opus")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32:
        parser.error("--jobs must be between 1 and 32")
    path = build_probe(args)
    lib = configure_library(path)
    cases = verify(lib)
    print(f"Passed {cases} length/rate cases in both directions (16k/24k, 20ms, mono)")
    print("Verified actual firmware feed/finish, lookahead/valid-tail trim and malformed-duration rejection")
    print("Host library: 78/esp-opus fixed-point source; native peer: " + native_version())
    print("This does not execute Xtensa code or validate device CPU, I2S, DMA or acoustic quality")


if __name__ == "__main__":
    main()
