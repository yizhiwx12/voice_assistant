"""检查 P4 模型部署，包含模型和闪存元数据损坏场景。"""

import copy
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "prepare_sr_model", Path(__file__).resolve().parents[1] / "prepare_sr_model.py"
)
manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest)


def packed_model(name="vadnet1_medium"):
    header_size = 4 + 36 + 40
    return (
        struct.pack("<I32sI32sII", 1, name.encode(), 1, b"_MODEL_INFO_", header_size, 5)
        + b"info\n"
    )


def partition(name, offset, size, kind=1, subtype=0x82):
    return struct.pack("<HBBII16sI", 0x50AA, kind, subtype, offset, size, name.encode(), 0)


class ModelManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / "build"
        self.component = self.root / "esp-sr"
        self.component.mkdir()
        (self.component / "idf_component.yml").write_text('version: "2.6.1"\n', encoding="utf-8")
        (self.build / "srmodels").mkdir(parents=True)
        (self.build / "srmodels" / "srmodels.bin").write_bytes(packed_model())
        (self.build / "partition_table").mkdir()
        self.table = partition("factory", 0x10000, 3 * 1024 * 1024, 0, 0) + partition(
            "model", 0x310000, 1024 * 1024
        )
        self.write_table(self.table)
        self.flash = {"flash_files": {"0x10000": "voice_assistant.bin", "0x310000": "srmodels/srmodels.bin"}}
        self.write_flash(self.flash)

    def write_table(self, table):
        (self.build / "partition_table" / "partition-table.bin").write_bytes(table)

    def write_flash(self, flash):
        (self.build / "flasher_args.json").write_text(json.dumps(flash), encoding="utf-8")

    def prepare(self):
        return manifest.prepare(self.build, self.component)

    def test_complete_manifest_contains_matching_header_hash(self):
        report = self.prepare()
        header = (self.build / "generated" / "sr_model_manifest.hpp").read_text()
        self.assertIn(report["sha256"], header)
        self.assertIn(report["model"], header)
        self.assertEqual(report["image_bytes"], len(packed_model()))
        self.assertFalse(report["hardware_verified"])
        self.assertEqual(report["partition"]["offset"], 0x310000)

    def test_output_is_stable_for_unchanged_artifacts(self):
        self.prepare()
        path = self.build / "generated" / "sr_model_manifest.hpp"
        timestamp = path.stat().st_mtime_ns
        self.prepare()
        self.assertEqual(path.stat().st_mtime_ns, timestamp)

    def test_changed_model_payload_produces_new_firmware_hash(self):
        first = self.prepare()["sha256"]
        (self.build / "srmodels" / "srmodels.bin").write_bytes(packed_model()[:-1] + b"!")
        self.assertNotEqual(first, self.prepare()["sha256"])

    def test_missing_model_flash_entry_fails(self):
        flash = copy.deepcopy(self.flash)
        del flash["flash_files"]["0x310000"]
        self.write_flash(flash)
        with self.assertRaisesRegex(ValueError, "Full flash manifest"):
            self.prepare()

    def test_wrong_flash_file_fails(self):
        flash = copy.deepcopy(self.flash)
        flash["flash_files"]["0x310000"] = "old_model.bin"
        self.write_flash(flash)
        with self.assertRaisesRegex(ValueError, "Full flash manifest"):
            self.prepare()

    def test_overlapping_partitions_fail(self):
        self.write_table(self.table + partition("extra", 0x311000, 4096))
        with self.assertRaisesRegex(ValueError, "overlap"):
            self.prepare()

    def test_missing_model_partition_fails(self):
        self.write_table(self.table[:32])
        with self.assertRaisesRegex(ValueError, "Expected one model"):
            self.prepare()

    def test_oversized_image_fails(self):
        self.write_table(partition("model", 0x310000, 64))
        with self.assertRaisesRegex(ValueError, "exceeds model partition"):
            self.prepare()

    def test_unexpected_model_is_not_packaged_silently(self):
        (self.build / "srmodels" / "srmodels.bin").write_bytes(packed_model("wn9_hilexin"))
        with self.assertRaisesRegex(ValueError, "P4.1 requires only"):
            self.prepare()

    def test_truncated_header_and_data_fail(self):
        for image in [b"", b"\xff" * 4, packed_model()[:50], packed_model()[:-1]]:
            with self.subTest(length=len(image)), self.assertRaises(ValueError):
                manifest.parse_models(image)

    def test_file_pointing_into_header_fails(self):
        image = bytearray(packed_model())
        struct.pack_into("<I", image, 72, 4)
        with self.assertRaisesRegex(ValueError, "overlaps header"):
            manifest.parse_models(image)

    def test_unterminated_name_fails(self):
        image = bytearray(packed_model())
        image[4:36] = b"x" * 32
        with self.assertRaisesRegex(ValueError, "NUL terminated"):
            manifest.parse_models(image)


if __name__ == "__main__":
    unittest.main()
