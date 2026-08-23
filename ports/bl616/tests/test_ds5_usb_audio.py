from pathlib import Path
import random
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
BT_SOURCE_PATHS = (
    BL616_DIR / "bluetooth" / "ds5_bt_link.c",
    BL616_DIR / "bluetooth" / "ds5_bt_discovery.c",
    BL616_DIR / "bluetooth" / "ds5_bt_l2cap_worker.c",
    BL616_DIR / "bluetooth" / "ds5_bt_tx.c",
    BL616_DIR / "bluetooth" / "ds5_bt_policy.c",
    BL616_DIR / "bluetooth" / "ds5_bt_init.c",
    BL616_DIR / "bluetooth" / "ds5_bt.c",
)


def read_bt_sources():
    return "\n".join(
        path.read_text(encoding="utf-8") for path in BT_SOURCE_PATHS
    )
L2CAP_HEADER = BL616_DIR / "bluetooth" / "ds5_l2cap.h"
CMAKE_PATH = BL616_DIR / "CMakeLists.txt"
DEFCONFIG_PATH = BL616_DIR / "defconfig"
SDK_OPUS_ARCHIVE = (
    BL616_DIR.parents[1] / "third_party" / "bouffalo_sdk" / "components" /
    "multimedia" / "opus" / "libopus.a"
)


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
        self.assertNotIn("WDL_Resampler", source)
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

    def test_realtime_ingress_is_decoupled_from_opus_encoding(self):
        source = AUDIO_SOURCE.read_text(encoding="utf-8")
        ingress_start = source.index("static void ds5_usb_audio_task(")
        codec_start = source.index("static void ds5_usb_codec_task(")
        ingress_task = source[ingress_start:codec_start]

        self.assertIn("DS5_USB_AUDIO_QUEUE_LENGTH          4U", source)
        self.assertIn("DS5_USB_SPEAKER_QUEUE_LENGTH        2U", source)
        self.assertIn("DS5_USB_AUDIO_ENCODE_BUDGET_US      10667U", source)
        self.assertIn("DS5_USB_AUDIO_HAPTICS_DECIMATION    16U", source)
        self.assertIn("DS5_USB_AUDIO_SPEAKER_INPUT_STEP    16U", source)
        self.assertIn("DS5_USB_AUDIO_SPEAKER_OUTPUT_STEP   15U", source)
        self.assertIn("xQueueReceive(audio_queue, &packet, 0U)", source)
        self.assertIn("ulTaskNotifyTake(pdTRUE, wait_ticks)", ingress_task)
        self.assertIn("ds5_usb_audio_notify_task();", source)
        self.assertIn("ds5_usb_audio_queue_speaker_frame(packet->generation)", source)
        self.assertIn("xQueueReceiveFromISR(audio_queue", source)
        self.assertIn("DS5_USB_AUDIO_TASK_PRIORITY", source)
        self.assertIn("DS5_USB_CODEC_TASK_PRIORITY", source)
        self.assertIn("total_speaker_encode_us += elapsed_us", source)
        self.assertNotIn("current_average", source)
        self.assertNotIn("opus_encode", ingress_task)
        self.assertNotIn("opus_decode", ingress_task)

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

    def test_audio_out_arm_failure_retries_from_ingress_task(self):
        source = AUDIO_SOURCE.read_text(encoding="utf-8")
        arm_start = source.index("static void ds5_usb_audio_arm_out")
        arm_end = source.index(
            'extern "C" void ds5_usb_audio_on_out_complete', arm_start
        )
        arm = source[arm_start:arm_end]
        task_start = source.index("static void ds5_usb_audio_task")
        task_end = source.index("static void ds5_usb_codec_task", task_start)
        task = source[task_start:task_end]

        self.assertIn("DS5_USB_AUDIO_OUT_RETRY_MS", source)
        self.assertIn("audio_arm_retry_pending = true;", arm)
        self.assertIn("xTaskGetTickCountFromISR()", arm)
        self.assertIn("ds5_usb_audio_notify_task();", arm)
        self.assertIn("audio_arm_retry_pending", task)
        self.assertIn("(int32_t)(now - audio_arm_retry_at)", task)
        self.assertIn("ds5_usb_audio_arm_out();", task)

    def test_audio_and_haptics_mailboxes_are_static_and_bounded(self):
        audio_source = AUDIO_MAILBOX.read_text(encoding="utf-8")
        haptics_source = HAPTICS_MAILBOX.read_text(encoding="utf-8")

        self.assertIn("DS5_AUDIO_SPEAKER_MAILBOX_LENGTH    2U", audio_source)
        self.assertIn("DS5_AUDIO_MICROPHONE_MAILBOX_LENGTH 8U", audio_source)
        self.assertIn("xQueueCreateStatic", audio_source)
        self.assertIn("xQueueOverwrite", audio_source)
        self.assertNotIn("malloc(", audio_source)
        self.assertNotIn("free(", audio_source)
        self.assertIn("DS5_HAPTICS_MAILBOX_LENGTH 1U", haptics_source)
        self.assertIn("xQueueCreateStatic", haptics_source)

    def test_bluetooth_uses_native_audio_and_microphone_contract(self):
        source = read_bt_sources()

        self.assertEqual(source.count("ds5_output_sequence_t output_sequence"), 1)
        self.assertIn("ds5_output_sequence_reset(&output_sequence, 0U)", source)
        self.assertIn("ds5_build_bt_audio_transaction", source)
        self.assertIn("ds5_build_bt_microphone_status_transaction", source)
        self.assertIn("(event->data[2] & 0x02U)", source)
        self.assertIn("&event->data[4]", source)
        self.assertIn("DS5_AUDIO_MIC_OPUS_SIZE", source)
        self.assertIn("DS5_BT_AUDIO_SPEAKER_FRAME_COUNT", source)

    def test_bluetooth_drops_stale_audio_but_retries_latest_state(self):
        source = read_bt_sources()

        self.assertIn("static bool ds5_bt_forward_audio(", source)
        self.assertIn("static bool ds5_bt_forward_usb_output(", source)
        self.assertIn("if (ds5_bt_forward_audio(", source)
        self.assertIn("if (ds5_bt_forward_usb_output(", source)
        self.assertIn("retry_pending = true;", source)
        self.assertIn("pending_haptics = false;", source)
        self.assertNotIn("DS5_BT_DEFAULT_SPEAKER_PREGAIN", source)

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

    def test_build_uses_sdk_e907_opus_fixed_point_backend(self):
        cmake = CMAKE_PATH.read_text(encoding="utf-8")
        defconfig = DEFCONFIG_PATH.read_text(encoding="utf-8")

        self.assertIn("CONFIG_CHERRYUSB_DEVICE_AUDIO =y", defconfig)
        self.assertIn("CONFIG_BT_L2CAP_TX_MTU       =672", defconfig)
        self.assertIn("CONFIG_MULTIMEDIA            =y", defconfig)
        self.assertIn("CONFIG_OPUS                  =y", defconfig)
        self.assertIn("CONFIG_MEMSET_OPTSPEED       =y", defconfig)
        self.assertNotIn("../../lib/WDL/WDL/resample.cpp", cmake)
        self.assertIn("DS5_SDK_OPUS_ROOT", cmake)
        self.assertIn("libopus.a", cmake)
        self.assertNotIn("OpusFunctions.cmake", cmake)
        self.assertNotIn("DS5_OPUS_SOURCES", cmake)
        self.assertNotIn("DS5_OPUS_FLOAT_SOURCES", cmake)
        self.assertNotIn("DS5_OPUS_SILK_FLOAT_SOURCES", cmake)
        self.assertIn("usb/ds5_usb_audio_adapter.c", cmake)
        self.assertTrue(SDK_OPUS_ARCHIVE.is_file())
        self.assertIn(b"libopus 1.3-fixed", SDK_OPUS_ARCHIVE.read_bytes())

    def test_celt_encoder_hot_path_is_linked_into_on_chip_ram(self):
        cmake = CMAKE_PATH.read_text(encoding="utf-8")

        self.assertIn("DS5_OPUS_TCM_OBJECTS", cmake)
        self.assertIn("ds5_usb_audio.cpp.obj", cmake)
        self.assertIn("opus_encoder.c.obj", cmake)
        self.assertIn("opus_decoder.c.obj", cmake)
        self.assertIn("opus.c.obj", cmake)
        self.assertIn("repacketizer.c.obj", cmake)
        self.assertIn("celt_decoder.c.obj", cmake)
        self.assertIn("celt_encoder.c.obj", cmake)
        self.assertIn("entdec.c.obj", cmake)
        self.assertIn("bands.c.obj", cmake)
        self.assertIn("modes.c.obj", cmake)
        self.assertIn("DS5_RUNTIME_TCM_OBJECTS", cmake)
        self.assertIn("lib_vikmemcpy.c.obj", cmake)
        self.assertIn("lib_memset.c.obj", cmake)
        self.assertIn("lib_memmove.c.obj", cmake)
        self.assertIn("lib_abs.c.obj", cmake)
        self.assertIn("DS5_LIBGCC_TCM_OBJECTS", cmake)
        self.assertIn("_clzsi2.o", cmake)
        self.assertIn("muldf3.o", cmake)
        self.assertIn("*libapp.a:${DS5_APP_TCM_OBJECT}(.text*)", cmake)
        self.assertIn("*libopus.a:${DS5_OPUS_TCM_OBJECT}(.text*)", cmake)
        self.assertIn(
            "*liblibc.a:${DS5_RUNTIME_TCM_OBJECT}(.text*)", cmake
        )
        self.assertIn(
            "*libgcc.a:${DS5_LIBGCC_TCM_OBJECT}(.text*)", cmake
        )
        self.assertIn("*libopus.a:${DS5_OPUS_TCM_OBJECT}(.rodata*)", cmake)
        self.assertIn(
            "sdk_set_linker_script_macro(${DS5_LINKER_SCRIPT})", cmake
        )

    def test_opus_keeps_measured_e907_optimization_level(self):
        cmake = CMAKE_PATH.read_text(encoding="utf-8")
        defconfig = DEFCONFIG_PATH.read_text(encoding="utf-8")

        self.assertIn("CONFIG_GCC_OPTIMISE_LEVEL    =-O2", defconfig)
        self.assertNotIn("opus/ds5_opus_e907_dsp.h", cmake)
        self.assertNotIn("-O3", cmake)
        self.assertNotIn("sdk_add_compile_options(-O3", cmake)

    def test_realtime_tasks_do_not_format_periodic_status_logs(self):
        audio = AUDIO_SOURCE.read_text(encoding="utf-8")
        bluetooth = read_bt_sources()

        self.assertNotIn("DS5_USB_AUDIO_LOG_INTERVAL", audio)
        self.assertNotIn("observed_packets %", audio)
        self.assertNotIn("DS5_BT_INPUT_LOG_INTERVAL", bluetooth)
        self.assertNotIn("DS5_BT_OUTPUT_LOG_INTERVAL", bluetooth)
        self.assertNotIn("DS5_BT_HAPTICS_LOG_INTERVAL", bluetooth)
        self.assertNotIn("diagnostic_event_count", bluetooth)
        self.assertNotIn("diagnostic_iteration_count", bluetooth)
        self.assertIn(
            "ds5_usb_audio_stack_high_water_words(audio_task)", audio
        )
        self.assertIn(
            "ds5_usb_audio_stack_high_water_words(codec_task)", audio
        )
        self.assertIn("ds5_bt_stack_high_water_words(worker_task)", bluetooth)
        self.assertIn(
            "ds5_bt_stack_high_water_words(tx_worker_task)", bluetooth
        )

    def test_resampler_advances_exact_16_over_15_phase_without_divide(self):
        source = AUDIO_SOURCE.read_text(encoding="utf-8")
        input_frame = 0
        fraction = 0
        positions = []

        for _ in range(480):
            positions.append((input_frame, fraction))
            input_frame += 1
            fraction += 1
            if fraction == 15:
                fraction = 0
                input_frame += 1

        expected = [(frame * 16 // 15, frame * 16 % 15)
                    for frame in range(480)]
        self.assertEqual(expected, positions)
        self.assertEqual(positions[-1], (510, 14))
        self.assertIn("Advance 16/15 without a divide/modulo pair", source)
        self.assertNotIn("position / DS5_USB_AUDIO_SPEAKER_OUTPUT_STEP", source)

    def test_packed_resampler_uses_exact_limited_range_divide(self):
        source = AUDIO_SOURCE.read_text(encoding="utf-8")

        for magnitude in range(491528):
            self.assertEqual(
                magnitude // 15,
                (magnitude * 0x88889) >> 23,
                magnitude,
            )

        rng = random.Random(0x616)
        for _ in range(4096):
            current = [rng.randrange(-32768, 32768) for _ in range(2)]
            following = [rng.randrange(-32768, 32768) for _ in range(2)]
            fraction = rng.randrange(15)
            for channel in range(2):
                mixed = (current[channel] * (15 - fraction) +
                         following[channel] * fraction)
                expected = ((abs(mixed) + 7) // 15)
                if mixed < 0:
                    expected = -expected
                sign = (mixed & 0xFFFFFFFF) >> 31
                sign_mask = (-sign) & 0xFFFFFFFF
                magnitude = ((((mixed & 0xFFFFFFFF) ^ sign_mask) -
                              sign_mask) & 0xFFFFFFFF) + 7
                quotient = (magnitude * 0x88889) >> 23
                actual = ((quotient ^ sign_mask) + sign) & 0xFFFFFFFF
                if actual & 0x80000000:
                    actual -= 0x100000000
                self.assertEqual(expected, actual)

        self.assertIn("__rv__smul16", source)
        self.assertIn("DS5_USB_AUDIO_DIV15_MULTIPLIER", source)
        self.assertNotRegex(source, r"\([^\n]+\)\s*/\s*denominator")

    def test_speaker_path_stays_int16_into_fixed_point_opus(self):
        source = AUDIO_SOURCE.read_text(encoding="utf-8")

        self.assertIn('#include "opus.h"', source)
        self.assertIn("static int16_t speaker_opus_input", source)
        self.assertIn("int16_t *speaker_data", source)
        self.assertIn("encoded_length = opus_encode(", source)
        self.assertNotIn("opus_encode_float(", source)

    def test_speaker_opus_packet_is_padded_and_validated(self):
        source = AUDIO_SOURCE.read_text(encoding="utf-8")

        self.assertIn("opus_packet_pad(", source)
        self.assertIn("opus_packet_get_nb_frames(", source)
        self.assertIn("opus_packet_get_nb_samples(", source)
        self.assertIn("opus_packet_get_nb_channels(", source)
        self.assertNotIn("memset(&opus_frame[encoded_length]", source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
