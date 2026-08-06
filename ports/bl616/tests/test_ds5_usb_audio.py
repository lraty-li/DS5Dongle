from pathlib import Path
import re
import unittest


BL616_DIR = Path(__file__).resolve().parents[1]
AUDIO_HEADER = BL616_DIR / "usb" / "ds5_usb_audio.h"
AUDIO_SOURCE = BL616_DIR / "usb" / "ds5_usb_audio.cpp"
ADAPTER_SOURCE = BL616_DIR / "usb" / "ds5_usb_audio_adapter.c"
USB_SOURCE = BL616_DIR / "usb" / "ds5_usb_log.c"
HID_HEADER = BL616_DIR / "usb" / "ds5_usb_hid.h"
AUDIO_MAILBOX = BL616_DIR / "platform" / "ds5_audio_mailbox.c"
HAPTICS_MAILBOX = BL616_DIR / "platform" / "ds5_haptics_mailbox.c"
BT_SOURCE = BL616_DIR / "bluetooth" / "ds5_bt.c"
L2CAP_HEADER = BL616_DIR / "bluetooth" / "ds5_l2cap.h"
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
    def test_native_dualsense_uac_format_matches_audio_contract(self):
        constants = numeric_macros(AUDIO_HEADER)

        self.assertEqual(constants["DS5_USB_AUDIO_CHANNEL_COUNT"], 4)
        self.assertEqual(
            constants["DS5_USB_AUDIO_MICROPHONE_CHANNEL_COUNT"], 2
        )
        self.assertEqual(constants["DS5_USB_AUDIO_SAMPLE_RATE"], 48000)
        self.assertEqual(constants["DS5_USB_AUDIO_SAMPLE_BYTES"], 2)
        self.assertEqual(constants["DS5_USB_AUDIO_PACKET_BYTES"], 384)
        self.assertEqual(constants["DS5_USB_AUDIO_MAX_PACKET_BYTES"], 392)
        self.assertEqual(
            constants["DS5_USB_AUDIO_MICROPHONE_PACKET_BYTES"], 192
        )
        self.assertEqual(
            constants["DS5_USB_AUDIO_CONFIG_DESCRIPTOR_SIZE"], 186
        )

    def test_audio_uses_static_usb_freertos_and_opus_buffers(self):
        source = AUDIO_SOURCE.read_text(encoding="utf-8")

        self.assertIn("USB_NOCACHE_RAM_SECTION", source)
        self.assertIn("xQueueCreateStatic", source)
        self.assertIn("xTaskCreateStatic", source)
        self.assertIn("WDL_Resampler", source)
        self.assertIn("opus_encoder_get_size", source)
        self.assertIn("opus_encoder_init", source)
        self.assertIn("opus_decoder_get_size", source)
        self.assertIn("opus_decoder_init", source)
        self.assertIn("OPUS_SET_EXPERT_FRAME_DURATION", source)
        self.assertIn("codec_initialization_attempted", source)
        self.assertIn("!speaker_stream_open && !microphone_stream_open", source)
        self.assertNotIn("opus_encoder_create", source)
        self.assertNotIn("opus_decoder_create", source)
        self.assertNotIn("malloc(", source)
        self.assertNotIn("free(", source)

    def test_four_host_channels_route_to_speaker_and_haptics(self):
        source = AUDIO_SOURCE.read_text(encoding="utf-8")

        self.assertIn("frame_offset + 0U", source)
        self.assertIn("frame_offset + 2U", source)
        self.assertIn("frame_offset + 4U", source)
        self.assertIn("frame_offset + 6U", source)
        self.assertIn("ds5_usb_audio_encode_speaker", source)
        self.assertIn("ds5_haptics_mailbox_publish", source)
        self.assertIn("ds5_audio_mailbox_publish_speaker_opus", source)

    def test_full_duplex_endpoints_are_armed_and_streams_control_bt(self):
        source = AUDIO_SOURCE.read_text(encoding="utf-8")
        adapter = ADAPTER_SOURCE.read_text(encoding="utf-8")

        self.assertIn("usbd_ep_start_read", source)
        self.assertIn("usbd_ep_start_write", source)
        self.assertIn("ds5_usb_audio_arm_out();", source)
        self.assertIn("ds5_audio_mailbox_set_speaker_stream_active(true)", source)
        self.assertIn(
            "ds5_audio_mailbox_publish_microphone_stream_active(true)", source
        )
        self.assertIn("ds5_usb_audio_on_stream_open", source)
        self.assertIn("ds5_usb_audio_on_stream_close", source)
        self.assertIn("usbd_audio_open", adapter)
        self.assertIn("usbd_audio_close", adapter)
        self.assertIn("usbd_audio_init_intf", adapter)

    def test_diagnostic_feature_observes_audio_pipeline_without_new_endpoint(self):
        header = AUDIO_HEADER.read_text(encoding="utf-8")
        audio = AUDIO_SOURCE.read_text(encoding="utf-8")
        hid = (BL616_DIR / "usb" / "ds5_usb_hid.c").read_text(
            encoding="utf-8"
        )

        self.assertIn("DS5_USB_AUDIO_DIAGNOSTIC_FEATURE_REPORT_ID", header)
        self.assertIn("0xf6U", header)
        self.assertIn("ds5_usb_audio_get_diagnostic_feature", audio)
        self.assertIn("received_audio_packets", audio)
        self.assertIn("published_speaker_frames", audio)
        self.assertIn("ds5_feature_set_mailbox_received_count", audio)
        self.assertIn("ds5_feature_set_mailbox_forwarded_count", audio)
        self.assertIn("DS5_USB_AUDIO_DIAGNOSTIC_FEATURE_REPORT_ID", hid)
        self.assertIn("hid_audio_diagnostic", hid)

    def test_audio_and_haptics_mailboxes_are_static_and_bounded(self):
        audio_source = AUDIO_MAILBOX.read_text(encoding="utf-8")
        haptics_source = HAPTICS_MAILBOX.read_text(encoding="utf-8")

        self.assertIn("DS5_AUDIO_SPEAKER_MAILBOX_LENGTH    4U", audio_source)
        self.assertIn("DS5_AUDIO_MICROPHONE_MAILBOX_LENGTH 8U", audio_source)
        self.assertIn("xQueueCreateStatic", audio_source)
        self.assertIn("xQueueOverwrite", audio_source)
        self.assertNotIn("malloc(", audio_source)
        self.assertNotIn("free(", audio_source)
        self.assertIn("DS5_HAPTICS_MAILBOX_LENGTH 4U", haptics_source)
        self.assertIn("xQueueCreateStatic", haptics_source)

    def test_bluetooth_uses_native_audio_and_microphone_contract(self):
        source = BT_SOURCE.read_text(encoding="utf-8")

        self.assertEqual(source.count("static ds5_output_sequence_t output_sequence"), 1)
        self.assertIn("ds5_build_bt_audio_transaction", source)
        self.assertIn("ds5_build_bt_microphone_status_transaction", source)
        self.assertIn("(event->data[2] & 0x02U)", source)
        self.assertIn("&event->data[4]", source)
        self.assertIn("DS5_AUDIO_MIC_OPUS_SIZE", source)
        self.assertIn("DS5_BT_AUDIO_SPEAKER_FRAME_COUNT", source)

    def test_native_hid_uac_uses_each_bl616_endpoint_number_once(self):
        audio = numeric_macros(AUDIO_HEADER)
        hid = numeric_macros(HID_HEADER)
        source = USB_SOURCE.read_text(encoding="utf-8")
        endpoint_addresses = [
            audio["DS5_USB_AUDIO_OUT_EP"],
            audio["DS5_USB_AUDIO_IN_EP"],
            hid["DS5_USB_HID_IN_EP"],
            hid["DS5_USB_HID_OUT_EP"],
        ]

        self.assertEqual(
            {address & 0x0F for address in endpoint_addresses},
            {1, 2, 3, 4},
        )
        self.assertNotIn("usbd_cdc", source)
        self.assertNotIn("CDC_ACM", source)
        self.assertIn("0x09, 0x02, 0xe3, 0x00", source)
        self.assertIn("0x09, 0x05, 0x01, 0x09, 0x88, 0x01", source)
        self.assertIn("0x09, 0x05, 0x82, 0x05, 0xc4, 0x00", source)
        self.assertIn("0x0a, 0x24, 0x01, 0x00, 0x01, 0x49, 0x00", source)
        self.assertIn("DS5_USB_HID_INTERFACE_NUMBER", source)

    def test_transport_payload_capacity_covers_full_audio_report(self):
        source = L2CAP_HEADER.read_text(encoding="utf-8")

        self.assertRegex(
            source,
            r"#define\s+DS5_L2CAP_MAX_EVENT_PAYLOAD\s+DS5_L2CAP_MTU",
        )
        self.assertRegex(source, r"#define\s+DS5_L2CAP_MTU\s+672U")

    def test_build_includes_fixed_opus_sources_without_subproject(self):
        cmake = CMAKE_PATH.read_text(encoding="utf-8")
        defconfig = DEFCONFIG_PATH.read_text(encoding="utf-8")

        self.assertIn("CONFIG_CHERRYUSB_DEVICE_AUDIO =y", defconfig)
        self.assertIn("CONFIG_BT_L2CAP_TX_MTU       =672", defconfig)
        self.assertIn("../../lib/WDL/WDL/resample.cpp", cmake)
        self.assertIn("OpusFunctions.cmake", cmake)
        self.assertIn("${DS5_OPUS_SOURCES}", cmake)
        self.assertIn("OPUS_BUILD;VAR_ARRAYS", cmake)
        self.assertIn("usb/ds5_usb_audio_adapter.c", cmake)
        self.assertNotIn("add_subdirectory(${DS5_OPUS_ROOT}", cmake)


if __name__ == "__main__":
    unittest.main(verbosity=2)
