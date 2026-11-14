"""本机编解码检查与共用的 32 字节设备音频协议测试样例。

使用 .venv-voice/Scripts/python.exe 运行，以测试项目锁定的本机 Opus DLL。
缺少 libopus 时明确跳过本机测试，协议测试始终运行。自带 DLL 报告
'libopus unknown'，因此验证功能而不推断版本。音频为有损解码，不能断言样本相等。
"""

import ctypes
import math
import struct
import unittest
from unittest.mock import patch

from audio.opus_codec import CodecError, Decoder, Encoder, library, native_version
from transport.voice_protocol import AudioPacket, DOWNLINK, HEADER, ProtocolError, UPLINK, uint32


def signal(rate, count):
    values = [int(11000 * math.sin(2 * math.pi * 580 * index / rate)) for index in range(count)]
    return struct.pack("<" + "h" * count, *values)


class NativeOpusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.version = native_version()
        except CodecError as exc:
            raise unittest.SkipTest(f"Native libopus is unavailable: {exc}") from None

    def encoder(self, rate, bitrate):
        encoder = Encoder(rate, bitrate, vbr=True)
        self.addCleanup(encoder.close)
        return encoder

    def decoder(self, rate):
        decoder = Decoder(rate)
        self.addCleanup(decoder.close)
        return decoder

    def test_native_version_is_readable_without_claiming_release_number(self):
        self.assertTrue(self.version.startswith("libopus"))

    def test_rates_20ms_vbr_packets_and_exact_cropped_sample_count(self):
        for rate, bitrate in ((16000, 24000), (24000, 32000)):
            frame = rate // 50
            # 覆盖单样本、不足整帧、恰好一帧或多帧，以及带尾部的输入。
            for count in (1, frame - 1, frame, frame * 5, frame * 5 + 73):
                with self.subTest(rate=rate, count=count):
                    encoder, decoder = self.encoder(rate, bitrate), self.decoder(rate)
                    source = signal(rate, count)
                    packets = []
                    # 任意 PCM 分块边界都不能重置原生编解码流状态。
                    for offset in range(0, len(source), 142):
                        packets.extend(encoder.feed(source[offset:offset + 142]))
                    self.assertEqual(encoder.valid_samples, count)
                    tail = encoder.finish()
                    packets.extend(tail)
                    self.assertGreater(encoder.pre_skip, 0)
                    expected_packets = math.ceil((count + encoder.pre_skip) / frame)
                    self.assertEqual(len(packets), expected_packets)
                    self.assertTrue(tail, "Codec lookahead must drain, including exact frame multiples")
                    self.assertTrue(all(1 <= len(packet) <= 1275 for packet in packets))
                    decoded_frames = [decoder.decode(packet) for packet in packets]
                    self.assertTrue(all(len(pcm) == frame * 2 for pcm in decoded_frames))
                    decoded = b"".join(decoded_frames)
                    cropped = decoded[encoder.pre_skip * 2:(encoder.pre_skip + count) * 2]
                    self.assertEqual(len(cropped) // 2, count)
                    values = struct.unpack("<" + "h" * count, cropped)
                    rms = math.sqrt(sum(value * value for value in values) / count)
                    self.assertTrue(math.isfinite(rms))
                    if count >= frame:
                        self.assertGreater(rms, 1000)
                        self.assertGreater(max(abs(value) for value in values[-min(count, 100):]), 1000,
                                           "Drained audio must retain the end of the signal")
                    with self.assertRaises(CodecError):
                        encoder.feed(b"\0\0")
                    with self.assertRaises(CodecError):
                        encoder.finish()

    def test_vbr_varies_packet_length_and_silence_keeps_frame_timeline(self):
        for rate, bitrate in ((16000, 24000), (24000, 32000)):
            with self.subTest(rate=rate):
                frame = rate // 50
                encoder = self.encoder(rate, bitrate)
                packets = encoder.feed(signal(rate, frame * 6))
                packets.extend(encoder.feed(bytes(frame * 2 * 12)))
                packets.extend(encoder.finish())
                self.assertGreater(len(set(map(len, packets))), 1, "VBR must permit variable packet sizes")
                self.assertEqual(len(packets), math.ceil((frame * 18 + encoder.pre_skip) / frame))
                decoder = self.decoder(rate)
                self.assertTrue(all(len(decoder.decode(packet)) == frame * 2 for packet in packets))

    def test_empty_stream_odd_pcm_and_closed_codec(self):
        encoder = self.encoder(16000, 24000)
        self.assertEqual(encoder.feed(b""), [])
        with self.assertRaises(CodecError):
            encoder.feed(b"\0")
        self.assertEqual(encoder.valid_samples, 0)
        self.assertEqual(encoder.finish(), [])
        encoder.close()
        encoder.close()
        decoder = self.decoder(24000)
        decoder.close()
        decoder.close()
        with self.assertRaises(CodecError):
            decoder.decode(b"\0")

    def test_rejects_packet_length_malformed_payload_and_wrong_duration(self):
        decoder = self.decoder(16000)
        for packet in (b"", b"x" * 1276, b"\x03", b"OggS" + bytes(100)):
            with self.subTest(length=len(packet)), self.assertRaises(CodecError):
                decoder.decode(packet)
        # 生成有效的原生 10/40 ms Opus 包；设备协议只接受
        # 20 ms，即使其他时长的包本身有效也必须拒绝。
        encoder = self.encoder(16000, 24000)
        for sample_count in (160, 640):
            samples = (ctypes.c_int16 * sample_count).from_buffer_copy(signal(16000, sample_count))
            output = ctypes.create_string_buffer(1275)
            size = encoder.lib.opus_encode(encoder.state, samples, sample_count, output, len(output))
            self.assertGreater(size, 0)
            with self.assertRaisesRegex(CodecError, "20 ms"):
                decoder.decode(output.raw[:size])

    def test_canceled_downlink_is_independent_of_continuous_uplink(self):
        uplink_encoder = self.encoder(16000, 24000)
        uplink_decoder = self.decoder(16000)
        old_encoder = self.encoder(24000, 32000)
        old_decoder = self.decoder(24000)
        uplink_states = (uplink_encoder.state, uplink_decoder.state)
        first_up = uplink_encoder.feed(signal(16000, 320))
        self.assertEqual(len(uplink_decoder.decode(first_up[0])), 640)
        old_down = old_encoder.feed(signal(24000, 480))[0]
        old_decoder.decode(old_down)
        old_encoder.close()
        old_decoder.close()
        with self.assertRaises(CodecError):
            old_decoder.decode(old_down)
        replacement_encoder = self.encoder(24000, 32000)
        replacement_decoder = self.decoder(24000)
        new_down = replacement_encoder.feed(signal(24000, 480))
        new_down.extend(replacement_encoder.finish())
        self.assertTrue(all(len(replacement_decoder.decode(packet)) == 960 for packet in new_down))
        second_up = uplink_encoder.feed(signal(16000, 320))
        second_up.extend(uplink_encoder.finish())
        self.assertEqual(uplink_states, (uplink_encoder.state, uplink_decoder.state))
        self.assertEqual(uplink_encoder.valid_samples, 640)
        self.assertTrue(all(len(uplink_decoder.decode(packet)) == 640 for packet in second_up))


class WireProtocolTests(unittest.TestCase):
    def packet(self, **changes):
        values = dict(kind=UPLINK, epoch=1, stream_id=2, turn_id=0, generation=0,
                      sequence=3, sample_offset=320, payload=b"\x01")
        return AudioPacket(**(values | changes))

    def test_32byte_header_known_little_endian_fixture(self):
        packet = self.packet(kind=DOWNLINK, epoch=0x11223344, stream_id=0x55667788,
                             turn_id=0x99AABBCC, generation=0xDDEEFF00,
                             sequence=0x01020304, sample_offset=0x0102030405060708,
                             payload=b"\xaa\xbb")
        expected_header = bytes.fromhex(
            "01 02 20 00 44 33 22 11 88 77 66 55 cc bb aa 99 "
            "00 ff ee dd 04 03 02 01 08 07 06 05 04 03 02 01"
        )
        self.assertEqual(HEADER.size, 32)
        self.assertEqual(packet.pack(), expected_header + b"\xaa\xbb")
        self.assertEqual(AudioPacket.unpack(expected_header + b"\xaa\xbb"), packet)

    def test_packet_lengths_and_header_validation_before_audio(self):
        valid = self.packet().pack()
        for message in (b"", valid[:31], valid[:32], valid[:32] + bytes(1276)):
            with self.subTest(length=len(message)), self.assertRaises(ProtocolError):
                AudioPacket.unpack(message)
        for index, value in ((0, 2), (1, 0), (1, 3), (2, 31), (2, 33), (3, 1)):
            malformed = bytearray(valid)
            malformed[index] = value
            with self.subTest(index=index, value=value), self.assertRaises(ProtocolError):
                AudioPacket.unpack(bytes(malformed))
        maximal = self.packet(payload=bytes(1275)).pack()
        self.assertEqual(len(AudioPacket.unpack(maximal).payload), 1275)

    def test_identity_fields_survive_wire_for_stale_frame_rejection(self):
        old = self.packet(kind=DOWNLINK, epoch=10, stream_id=7, turn_id=2, generation=1)
        new_generation = self.packet(kind=DOWNLINK, epoch=10, stream_id=8, turn_id=2, generation=2)
        reconnected = self.packet(kind=DOWNLINK, epoch=11, stream_id=1, turn_id=1, generation=1)
        identities = []
        for packet in (old, new_generation, reconnected):
            restored = AudioPacket.unpack(packet.pack())
            identities.append((restored.epoch, restored.stream_id, restored.turn_id, restored.generation))
            self.assertEqual(restored, packet)
        self.assertEqual(len(set(identities)), 3)
        # 实际判断过期帧是否可接受由 WebSocket/会话层负责；
        # 本二进制编解码层必须保留判断所需字段。
        uplink = AudioPacket.unpack(self.packet(turn_id=0, generation=0).pack())
        self.assertEqual((uplink.turn_id, uplink.generation), (0, 0))

    def test_uint32_control_fields_reject_booleans_negative_and_overflow(self):
        self.assertEqual(uint32(0, "stream"), 0)
        self.assertEqual(uint32(0xFFFFFFFF, "stream", nonzero=True), 0xFFFFFFFF)
        for value in (True, False, -1, 0x100000000, 1.0, "1", None):
            with self.subTest(value=value), self.assertRaises(ProtocolError):
                uint32(value, "stream")
        with self.assertRaises(ProtocolError):
            uint32(0, "stream", nonzero=True)

    def test_missing_native_dependency_is_an_explainable_error(self):
        library.cache_clear()
        try:
            with patch.dict("os.environ", {"OPUS_LIBRARY": "C:/missing-test-opus/opus.dll"}):
                with self.assertRaisesRegex(CodecError, "load libopus"):
                    library()
        finally:
            library.cache_clear()


if __name__ == "__main__":
    unittest.main()
