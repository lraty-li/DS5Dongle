from pathlib import Path
import re
import unittest


BL616_DIR = Path(__file__).resolve().parents[1]
HID_HEADER = BL616_DIR / "usb" / "ds5_usb_hid.h"
HID_SOURCE = BL616_DIR / "usb" / "ds5_usb_hid.c"
USB_SOURCE = BL616_DIR / "usb" / "ds5_usb_log.c"
MAILBOX_SOURCE = BL616_DIR / "platform" / "ds5_input_mailbox.c"
BT_SOURCE = BL616_DIR / "bluetooth" / "ds5_bt.c"


def numeric_macros(path):
    source = path.read_text(encoding="utf-8")
    matches = re.findall(
        r"^#define\s+([A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+|[0-9]+)U?$",
        source,
        re.MULTILINE,
    )
    return {name: int(value, 0) for name, value in matches}


def report_descriptor_bytes():
    source = HID_SOURCE.read_text(encoding="utf-8")
    match = re.search(
        r"static const uint8_t hid_report_descriptor\[\]\s*=\s*\{(.*?)\};",
        source,
        re.DOTALL,
    )
    if match is None:
        raise AssertionError("HID report descriptor not found")
    body = re.sub(r"/\*.*?\*/|//.*", "", match.group(1), flags=re.DOTALL)
    return bytes(
        int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]+)", body)
    )


class Ds5UsbHidTests(unittest.TestCase):
    def test_report_descriptor_is_input_only_and_has_report_id_one(self):
        constants = numeric_macros(HID_HEADER)
        descriptor = report_descriptor_bytes()

        self.assertEqual(
            len(descriptor), constants["DS5_USB_HID_REPORT_DESCRIPTOR_SIZE"]
        )
        self.assertIn(bytes([0x85, 0x01]), descriptor)
        self.assertEqual(descriptor[-1], 0xC0)
        self.assertNotIn(0x91, descriptor)
        self.assertNotIn(0xB1, descriptor)

    def test_hid_in_transfer_is_report_id_plus_payload(self):
        constants = numeric_macros(HID_HEADER)
        source = HID_SOURCE.read_text(encoding="utf-8")

        self.assertEqual(constants["DS5_USB_HID_IN_REPORT_SIZE"], 64)
        self.assertIn(
            "hid_transmit_report[0] = DS5_USB_INPUT_REPORT_ID", source
        )
        self.assertIn("memcpy(&hid_transmit_report[1], payload", source)

    def test_composite_descriptor_keeps_cdc_and_adds_hid(self):
        source = USB_SOURCE.read_text(encoding="utf-8")

        self.assertIn("CDC_ACM_DESCRIPTOR_INIT", source)
        self.assertIn(
            "USB_CONFIG_DESCRIPTOR_INIT(DS5_USB_CONFIG_SIZE, 0x03", source
        )
        self.assertIn("DS5_USB_HID_INTERFACE_NUMBER", source)
        self.assertIn("DS5_USB_HID_IN_EP", source)

    def test_bridge_uses_static_latest_state_mailbox(self):
        source = MAILBOX_SOURCE.read_text(encoding="utf-8")

        self.assertIn("xQueueCreateStatic", source)
        self.assertIn("xQueueOverwrite", source)
        self.assertNotIn("malloc(", source)
        self.assertNotIn("free(", source)

    def test_bluetooth_publishes_only_validated_payload(self):
        source = BT_SOURCE.read_text(encoding="utf-8")
        valid_start = source.index("if (protocol_result == DS5_PROTOCOL_OK)")
        valid_end = source.index("} else {", valid_start)

        self.assertIn(
            "ds5_input_mailbox_publish", source[valid_start:valid_end]
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
