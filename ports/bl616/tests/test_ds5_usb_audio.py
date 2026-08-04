from pathlib import Path
import re
import unittest


BL616_DIR = Path(__file__).resolve().parents[1]
AUDIO_HEADER = BL616_DIR / "usb" / "ds5_usb_audio.h"
AUDIO_SOURCE = BL616_DIR / "usb" / "ds5_usb_audio.cpp"
USB_SOURCE = BL616_DIR / "usb" / "ds5_usb_log.c"
HID_HEADER = BL616_DIR / "usb" / "ds5_usb_hid.h"
HAPTICS_MAILBOX = BL616_DIR / "platform" / "ds5_haptics_mailbox.c"
BT_SOURCE = BL616_DIR / "bluetooth" / "ds5_bt.c"
CMAKE_PATH = BL616_DIR / "CMakeLists.txt"
DEFCONFIG_PATH = BL616_DIR / "defconfig"


def numeric_macros(path):
    source = path.read_text(encoding="utf-8")
    matches = re.findall(
        r"^#define\s+([A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+|[0-9]+)U?$",
        source,
        re.MULTILINE,
    )
    return {name: int(value, 0) for name, value in matches}


class Ds5UsbAudioTests(unittest.TestCase):
    def test_native_haptics_usb_format_matches_original_contract(self):
        constants = numeric_macros(AUDIO_HEADER)

        self.assertEqual(constants["DS5_USB_AUDIO_CHANNEL_COUNT"], 4)
        self.assertEqual(constants["DS5_USB_AUDIO_SAMPLE_RATE"], 48000)
        self.assertEqual(constants["DS5_USB_AUDIO_SAMPLE_BYTES"], 2)
        self.assertEqual(constants["DS5_USB_AUDIO_PACKET_BYTES"], 384)
        self.assertEqual(constants["DS5_USB_AUDIO_MAX_PACKET_BYTES"], 392)

    def test_audio_uses_static_usb_and_freertos_buffers(self):
        source = AUDIO_SOURCE.read_text(encoding="utf-8")

        self.assertIn("USB_NOCACHE_RAM_SECTION", source)
        self.assertIn("xQueueCreateStatic", source)
        self.assertIn("xTaskCreateStatic", source)
        self.assertIn("WDL_Resampler", source)
        self.assertIn("SetRates((double)DS5_USB_AUDIO_SAMPLE_RATE, 3000.0)", source)
        self.assertIn("SetFeedMode(true)", source)
        self.assertIn("Prealloc(DS5_USB_AUDIO_OUTPUT_CHANNELS, 64, 8)", source)
        self.assertNotIn("malloc(", source)
        self.assertNotIn("free(", source)
        self.assertNotIn("opus", source.lower())

    def test_audio_extracts_only_native_haptics_channels(self):
        source = AUDIO_SOURCE.read_text(encoding="utf-8")

        self.assertIn("&packet->data[frame_offset + 4U]", source)
        self.assertIn("&packet->data[frame_offset + 6U]", source)
        self.assertNotIn("frame_offset + 0U", source)
        self.assertNotIn("frame_offset + 2U", source)

    def test_audio_stream_rearms_iso_out_and_publishes_fixed_blocks(self):
        source = AUDIO_SOURCE.read_text(encoding="utf-8")

        self.assertIn("usbd_ep_start_read", source)
        self.assertIn("ds5_usb_audio_arm_out();", source)
        self.assertIn("DS5_HAPTICS_DATA_SIZE", source)
        self.assertIn("ds5_haptics_mailbox_publish", source)

    def test_haptics_mailbox_is_static_and_bounded(self):
        source = HAPTICS_MAILBOX.read_text(encoding="utf-8")

        self.assertIn("DS5_HAPTICS_MAILBOX_LENGTH 4U", source)
        self.assertIn("xQueueCreateStatic", source)
        self.assertIn("xQueueSend", source)
        self.assertNotIn("malloc(", source)
        self.assertNotIn("free(", source)

    def test_bluetooth_serializes_haptics_with_hid_output_sequence(self):
        source = BT_SOURCE.read_text(encoding="utf-8")

        self.assertEqual(source.count("static ds5_output_sequence_t output_sequence"), 1)
        self.assertIn("ds5_build_bt_haptics_transaction", source)
        self.assertIn("DS5_BT_HAPTICS_TRANSACTION_SIZE", source)
        self.assertIn("ds5_haptics_mailbox_try_receive", source)

    def test_endpoint_indices_fit_bl616_usb_v2_limit(self):
        audio = numeric_macros(AUDIO_HEADER)
        hid = numeric_macros(HID_HEADER)
        source = USB_SOURCE.read_text(encoding="utf-8")
        cdc = {
            name: int(value, 0)
            for name, value in re.findall(
                r"^#define\s+(DS5_USB_CDC_(?:IN|OUT|INT)_EP)\s+"
                r"(0x[0-9A-Fa-f]+)U?$",
                source,
                re.MULTILINE,
            )
        }
        endpoint_addresses = [
            audio["DS5_USB_AUDIO_OUT_EP"],
            hid["DS5_USB_HID_IN_EP"],
            hid["DS5_USB_HID_OUT_EP"],
            cdc["DS5_USB_CDC_IN_EP"],
            cdc["DS5_USB_CDC_OUT_EP"],
            cdc["DS5_USB_CDC_INT_EP"],
        ]

        self.assertEqual(
            {address & 0x0F for address in endpoint_addresses},
            {1, 2, 3, 4},
        )
        self.assertNotEqual(
            hid["DS5_USB_HID_IN_EP"] & 0x0F,
            hid["DS5_USB_HID_OUT_EP"] & 0x0F,
        )
        self.assertEqual(
            hid["DS5_USB_HID_OUT_EP"] & 0x0F,
            cdc["DS5_USB_CDC_INT_EP"] & 0x0F,
        )

    def test_bl616_shared_vdma_directions_are_not_armed_concurrently(self):
        source = USB_SOURCE.read_text(encoding="utf-8")

        self.assertNotIn("cdc_out_endpoint", source)
        self.assertNotRegex(
            source,
            r"usbd_ep_start_read\s*\([^;]*DS5_USB_CDC_OUT_EP",
        )
        self.assertNotRegex(
            source,
            r"usbd_ep_start_write\s*\([^;]*DS5_USB_CDC_INT_EP",
        )

    def test_build_enables_only_required_audio_dependency(self):
        cmake = CMAKE_PATH.read_text(encoding="utf-8")
        defconfig = DEFCONFIG_PATH.read_text(encoding="utf-8")

        self.assertIn("CONFIG_CHERRYUSB_DEVICE_AUDIO =y", defconfig)
        self.assertIn("CONFIG_BT_L2CAP_TX_MTU       =672", defconfig)
        self.assertIn("../../lib/WDL/WDL/resample.cpp", cmake)
        self.assertNotIn("opus", cmake.lower())


if __name__ == "__main__":
    unittest.main(verbosity=2)
