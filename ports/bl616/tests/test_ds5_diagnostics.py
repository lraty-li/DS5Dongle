from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import struct
import unittest


ROOT = Path(__file__).resolve().parents[3]
HOST_SCRIPT = ROOT / "tools" / "host" / "read_bl616_diagnostics.py"
HID_SOURCE = ROOT / "ports" / "bl616" / "usb" / "ds5_usb_hid.c"
L2CAP_SOURCE = ROOT / "ports" / "bl616" / "bluetooth" / "ds5_l2cap.c"

SPEC = spec_from_file_location("read_bl616_diagnostics", HOST_SCRIPT)
DIAGNOSTICS = module_from_spec(SPEC)
SPEC.loader.exec_module(DIAGNOSTICS)


class Ds5DiagnosticsTests(unittest.TestCase):
    def test_pipeline_wire_format_decodes_little_endian_counters(self):
        report = bytearray(64)
        report[0:7] = bytes([0xF0]) + b"D5D0" + bytes([1, 0x1D])
        report[7] = 3
        struct.pack_into("<IIIIIIIIIIII", report, 8, *range(10, 22))
        struct.pack_into("<HHHH", report, 56, 7000, 12000, 100, 200)

        decoded = DIAGNOSTICS.decode_pipeline(bytes(report))

        self.assertTrue(decoded["speaker_stream_open"])
        self.assertTrue(decoded["codec_ready"])
        self.assertEqual(decoded["usb_packets"], 10)
        self.assertEqual(decoded["encode_count"], 21)
        self.assertEqual(decoded["encode_average_us"], 7000)
        self.assertEqual(decoded["codec_task_stack_free_bytes"], 800)

    def test_transport_wire_format_reports_hci_completion_latency(self):
        report = bytearray(64)
        report[0:8] = bytes([0xF1]) + b"D5D1" + bytes([1, 8, 3])
        struct.pack_into("<H", report, 8, 251)
        report[10:16] = bytes([1, 4, 0, 2, 3, 1])
        struct.pack_into("<IIIIIIIIIII", report, 16, *range(30, 41))
        struct.pack_into("<HH", report, 60, 300, 200)

        decoded = DIAGNOSTICS.decode_transport(bytes(report))

        self.assertEqual(decoded["bt_state"], "ready")
        self.assertEqual(decoded["estimated_acl_fragments_per_audio_report"], 3)
        self.assertEqual(decoded["audio_send_attempts"], 30)
        self.assertEqual(decoded["completion_average_us"], 40)
        self.assertEqual(decoded["bt_tx_task_stack_free_bytes"], 1200)

    def test_diagnostics_use_unused_read_only_feature_reports(self):
        hid_source = HID_SOURCE.read_text(encoding="utf-8")
        l2cap_source = L2CAP_SOURCE.read_text(encoding="utf-8")

        self.assertIn("DS5_USB_HID_DIAGNOSTIC_PIPELINE_ID", hid_source)
        self.assertIn("DS5_USB_HID_DIAGNOSTIC_TRANSPORT_ID", hid_source)
        self.assertIn("ds5_usb_hid_build_pipeline_diagnostics", hid_source)
        self.assertIn("ds5_usb_hid_build_transport_diagnostics", hid_source)
        self.assertIn("bt_l2cap_send_cb", l2cap_source)
        self.assertIn("ds5_l2cap_audio_completed", l2cap_source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
