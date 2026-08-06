from pathlib import Path
import re
import unittest


BL616_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = BL616_DIR.parents[1]
HID_HEADER = BL616_DIR / "usb" / "ds5_usb_hid.h"
HID_SOURCE = BL616_DIR / "usb" / "ds5_usb_hid.c"
USB_SOURCE = BL616_DIR / "usb" / "ds5_usb_log.c"
MAILBOX_SOURCE = BL616_DIR / "platform" / "ds5_input_mailbox.c"
OUTPUT_MAILBOX_SOURCE = BL616_DIR / "platform" / "ds5_output_mailbox.c"
FEATURE_SET_MAILBOX_SOURCE = BL616_DIR / "platform" / "ds5_feature_set_mailbox.c"
FEATURE_CACHE_SOURCE = BL616_DIR / "ds5" / "ds5_feature_cache.c"
BT_SOURCE = BL616_DIR / "bluetooth" / "ds5_bt.c"
ORIGINAL_DESCRIPTOR_SOURCE = REPO_ROOT / "src" / "usb_descriptors.cpp"


def numeric_macros(path):
    source = path.read_text(encoding="utf-8")
    matches = re.findall(
        r"^#define\s+([A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+|[0-9]+)U?$",
        source,
        re.MULTILINE,
    )
    return {name: int(value, 0) for name, value in matches}


def descriptor_bytes(path, declaration):
    source = path.read_text(encoding="utf-8")
    match = re.search(
        declaration + r"\s*=\s*\{(.*?)\};",
        source,
        re.DOTALL,
    )
    if match is None:
        raise AssertionError("HID report descriptor not found")
    body = re.sub(r"/\*.*?\*/", "", match.group(1), flags=re.DOTALL)
    body = re.sub(r"//[^\r\n]*", "", body)
    return bytes(
        int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]+)", body)
    )


class Ds5UsbHidTests(unittest.TestCase):
    def test_report_descriptor_matches_original_dualsense_model(self):
        constants = numeric_macros(HID_HEADER)
        descriptor = descriptor_bytes(
            HID_SOURCE,
            r"static const uint8_t hid_report_descriptor\[\]",
        )
        original = descriptor_bytes(
            ORIGINAL_DESCRIPTOR_SOURCE,
            r"uint8_t const desc_hid_report_ds\[\]",
        )

        self.assertEqual(
            len(descriptor), constants["DS5_USB_HID_REPORT_DESCRIPTOR_SIZE"]
        )
        self.assertEqual(descriptor, original)
        self.assertIn(bytes([0x85, 0x01]), descriptor)
        self.assertIn(bytes([0x85, 0x02, 0x09, 0x23, 0x95, 0x2F, 0x91]), descriptor)
        self.assertIn(bytes([0x85, 0x05, 0x09, 0x33, 0x95, 0x28, 0xB1]), descriptor)
        self.assertEqual(descriptor[-1], 0xC0)

    def test_hid_in_transfer_is_report_id_plus_payload(self):
        constants = numeric_macros(HID_HEADER)
        source = HID_SOURCE.read_text(encoding="utf-8")

        self.assertEqual(constants["DS5_USB_HID_IN_REPORT_SIZE"], 64)
        self.assertIn(
            "hid_transmit_report[0] = DS5_USB_INPUT_REPORT_ID", source
        )
        self.assertIn("memcpy(&hid_transmit_report[1], payload", source)

    def test_hid_out_reads_exact_wire_report_size(self):
        source = HID_SOURCE.read_text(encoding="utf-8")

        self.assertIn(
            "uint8_t hid_receive_report[DS5_USB_OUTPUT_REPORT_SIZE]", source
        )
        arm_start = source.index("static void ds5_usb_hid_arm_out")
        arm_end = source.index("static void ds5_usb_hid_out", arm_start)
        self.assertIn(
            "sizeof(hid_receive_report)", source[arm_start:arm_end]
        )

    def test_hid_out_preserves_in_band_report_id(self):
        source = HID_SOURCE.read_text(encoding="utf-8")

        self.assertIn(
            "USB interrupt transport includes the report ID", source
        )
        self.assertIn(
            "hid_receive_report, (size_t)transferred_bytes", source
        )
        self.assertNotIn(
            "hid_receive_report[0] != DS5_USB_OUTPUT_REPORT_ID",
            source,
        )

    def test_hid_out_trace_is_copied_in_callback_and_logged_by_task(self):
        source = HID_SOURCE.read_text(encoding="utf-8")

        self.assertIn("DS5_USB_HID_OUTPUT_TRACE_LIMIT", source)
        self.assertIn("ds5_usb_hid_note_output_trace", source)
        self.assertIn("DS5 USB: OUT trace", source)
        self.assertIn("DS5 USB: state flags", source)
        self.assertLess(
            source.index("ds5_usb_hid_note_output_trace"),
            source.index("static void ds5_usb_hid_task"),
        )

    def test_native_dualsense_descriptor_uses_hid_and_full_duplex_audio(self):
        source = USB_SOURCE.read_text(encoding="utf-8")

        self.assertNotIn("CDC_ACM_DESCRIPTOR_INIT", source)
        self.assertIn("#define DS5_USB_CONFIG_SIZE 227U", source)
        self.assertIn("0x09, 0x02, 0xe3, 0x00, 0x04", source)
        self.assertIn("DS5_USB_HID_INTERFACE_NUMBER", source)
        self.assertIn("DS5_USB_HID_IN_EP", source)
        self.assertIn("DS5_USB_HID_OUT_EP", source)
        self.assertIn("0x0a, 0x24, 0x01, 0x00, 0x01, 0x49, 0x00", source)
        self.assertIn("0x09, 0x05, 0x01, 0x09, 0x88, 0x01", source)
        self.assertIn("0x09, 0x05, 0x82, 0x05, 0xc4, 0x00", source)
        self.assertIn("0x09, 0x04, 0x01, 0x00", source)
        self.assertIn("0x09, 0x04, 0x02, 0x00", source)
        self.assertIn("0x09, 0x05, 0x01, 0x09", source)
        self.assertIn("0x09, 0x05, 0x82, 0x05", source)
        self.assertIn("0x054cU", source)
        self.assertIn("0x0ce6U", source)
        self.assertIn('"DualSense Wireless Controller"', source)

    def test_bridge_uses_static_latest_state_mailbox(self):
        source = MAILBOX_SOURCE.read_text(encoding="utf-8")

        self.assertIn("xQueueCreateStatic", source)
        self.assertIn("xQueueOverwrite", source)
        self.assertNotIn("malloc(", source)
        self.assertNotIn("free(", source)

    def test_output_bridge_uses_static_latest_state_mailbox(self):
        source = OUTPUT_MAILBOX_SOURCE.read_text(encoding="utf-8")

        self.assertIn("xQueueCreateStatic", source)
        self.assertIn("xQueueOverwriteFromISR", source)
        self.assertRegex(
            source,
            r"xQueueReceive\(output_queue,\s*report,\s*0U\)",
        )
        self.assertNotIn("malloc(", source)
        self.assertNotIn("free(", source)

    def test_feature_set_uses_a_separate_ordered_mailbox(self):
        source = FEATURE_SET_MAILBOX_SOURCE.read_text(encoding="utf-8")

        self.assertIn("xQueueCreateStatic", source)
        self.assertIn("xQueueSendFromISR", source)
        self.assertIn("xQueueSend(feature_set_queue", source)
        self.assertIn("xQueueReceive(feature_set_queue", source)
        self.assertNotIn("xQueueOverwrite", source)
        self.assertNotIn("malloc(", source)
        self.assertNotIn("free(", source)

    def test_feature_set_report_is_bridged_but_local_audio_diag_is_not(self):
        source = HID_SOURCE.read_text(encoding="utf-8")
        publish_start = source.index("static bool ds5_usb_hid_publish_feature_set(")
        publish_end = source.index("static void ds5_usb_hid_task(", publish_start)
        publish = source[publish_start:publish_end]
        callback_start = source.index("void usbd_hid_set_report(")
        callback_end = source.index("static void ds5_usb_hid_task(", callback_start)
        callback = source[callback_start:callback_end]

        self.assertIn("report_type == HID_REPORT_FEATURE", callback)
        self.assertIn("ds5_usb_hid_publish_feature_set", callback)
        self.assertIn("ds5_feature_set_mailbox_publish", publish)
        self.assertIn("ds5_feature_set_mailbox_note_received", publish)
        self.assertIn("length == (DS5_FEATURE_SET_MAX_PAYLOAD + 1U)", publish)
        self.assertIn("payload = &report[1]", publish)
        self.assertIn("DS5_USB_AUDIO_DIAGNOSTIC_FEATURE_REPORT_ID", publish)
        self.assertLess(
            callback.index("report_type == HID_REPORT_FEATURE"),
            callback.index("report_type != HID_REPORT_OUTPUT"),
        )

    def test_feature_cache_is_bounded_and_double_buffered(self):
        source = FEATURE_CACHE_SOURCE.read_text(encoding="utf-8")

        self.assertIn("DS5_FEATURE_CACHE_ENTRY_COUNT 4U", source)
        self.assertIn("DS5_FEATURE_CACHE_BANK_COUNT  2U", source)
        self.assertNotIn("malloc(", source)

    def test_bluetooth_publishes_only_validated_payload(self):
        source = BT_SOURCE.read_text(encoding="utf-8")
        valid_start = source.index("if (protocol_result == DS5_PROTOCOL_OK)")
        valid_end = source.index("} else {", valid_start)

        self.assertIn(
            "ds5_input_mailbox_publish", source[valid_start:valid_end]
        )

if __name__ == "__main__":
    unittest.main(verbosity=2)
