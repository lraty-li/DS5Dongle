from pathlib import Path
import re
import unittest


BL616_DIR = Path(__file__).resolve().parents[1]
HEADER_PATH = BL616_DIR / "ds5" / "ds5_protocol.h"
SOURCE_PATH = BL616_DIR / "ds5" / "ds5_protocol.c"


def read_numeric_macros():
    header = HEADER_PATH.read_text(encoding="utf-8")
    matches = re.findall(
        r"^#define\s+(DS5_[A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+|[0-9]+)U?$",
        header,
        re.MULTILINE,
    )
    return {name: int(value, 0) for name, value in matches}


CONSTANTS = read_numeric_macros()


def crc32_seeded(data, seed):
    crc = (~seed) & 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
    return (~crc) & 0xFFFFFFFF


def extract_usb_input(bt_transaction):
    if len(bt_transaction) < CONSTANTS["DS5_BT_INPUT_MIN_SIZE"]:
        raise ValueError("short Bluetooth input transaction")
    if bt_transaction[0] != CONSTANTS["DS5_BT_INPUT_TRANSACTION_HEADER"]:
        raise ValueError("unexpected Bluetooth HID transaction header")
    if (
        bt_transaction[CONSTANTS["DS5_BT_INPUT_REPORT_ID_OFFSET"]]
        != CONSTANTS["DS5_BT_INPUT_REPORT_ID"]
    ):
        raise ValueError("unexpected Bluetooth input report ID")
    offset = CONSTANTS["DS5_BT_INPUT_PAYLOAD_OFFSET"]
    size = CONSTANTS["DS5_USB_INPUT_PAYLOAD_SIZE"]
    return bytes(bt_transaction[offset : offset + size])


def build_bt_output(usb_report, sequence):
    if len(usb_report) != CONSTANTS["DS5_USB_OUTPUT_REPORT_SIZE"]:
        raise ValueError("unexpected USB output report length")
    if usb_report[0] != CONSTANTS["DS5_USB_OUTPUT_REPORT_ID"]:
        raise ValueError("unexpected USB output report ID")

    report = bytearray(CONSTANTS["DS5_BT_OUTPUT_REPORT_SIZE"])
    report[0] = CONSTANTS["DS5_BT_OUTPUT_REPORT_ID"]
    report[CONSTANTS["DS5_BT_OUTPUT_SEQUENCE_OFFSET"]] = (
        sequence & 0x0F
    ) << 4
    report[CONSTANTS["DS5_BT_OUTPUT_TAG_OFFSET"]] = CONSTANTS[
        "DS5_BT_OUTPUT_TAG"
    ]

    state_offset = CONSTANTS["DS5_BT_OUTPUT_STATE_OFFSET"]
    state_size = CONSTANTS["DS5_USB_OUTPUT_STATE_SIZE"]
    report[state_offset : state_offset + state_size] = usb_report[1:]

    crc_offset = CONSTANTS["DS5_BT_OUTPUT_CRC_OFFSET"]
    crc = crc32_seeded(report[:crc_offset], 0xEADA2D49)
    report[crc_offset:] = crc.to_bytes(4, "little")

    transaction = bytes([CONSTANTS["DS5_BT_OUTPUT_TRANSACTION_HEADER"]]) + bytes(
        report
    )
    return transaction, ((sequence & 0x0F) + 1) & 0x0F


def build_usb_microphone_mute_report(muted):
    report = bytearray(CONSTANTS["DS5_USB_OUTPUT_REPORT_SIZE"])
    report[0] = CONSTANTS["DS5_USB_OUTPUT_REPORT_ID"]
    report[CONSTANTS["DS5_USB_OUTPUT_VALID_FLAGS1_OFFSET"]] = (
        CONSTANTS["DS5_USB_OUTPUT_ALLOW_MUTE_LIGHT"]
        | CONSTANTS["DS5_USB_OUTPUT_ALLOW_AUDIO_MUTE"]
    )
    report[CONSTANTS["DS5_USB_OUTPUT_MUTE_LIGHT_OFFSET"]] = (
        CONSTANTS["DS5_USB_OUTPUT_MUTE_LIGHT_ON"] if muted else 0
    )
    report[CONSTANTS["DS5_USB_OUTPUT_MUTE_CONTROL_OFFSET"]] = (
        CONSTANTS["DS5_USB_OUTPUT_MIC_MUTE"] if muted else 0
    )
    return bytes(report)


def build_bt_haptics(haptics_data, sequence, packet_counter):
    if len(haptics_data) != CONSTANTS["DS5_HAPTICS_DATA_SIZE"]:
        raise ValueError("unexpected haptics data length")

    report = bytearray(CONSTANTS["DS5_BT_HAPTICS_REPORT_SIZE"])
    report[0] = CONSTANTS["DS5_BT_HAPTICS_REPORT_ID"]
    report[CONSTANTS["DS5_BT_HAPTICS_SEQUENCE_OFFSET"]] = (
        sequence & 0x0F
    ) << 4
    report[2] = CONSTANTS["DS5_BT_HAPTICS_STREAM_FLAGS"]
    report[3] = CONSTANTS["DS5_BT_HAPTICS_HEADER_LENGTH"]
    report[4] = CONSTANTS["DS5_BT_HAPTICS_ROUTING"]
    report[5:9] = bytes([CONSTANTS["DS5_BT_HAPTICS_BUFFER_LENGTH"]]) * 4
    packet_counter = (packet_counter + 2) & 0xFF
    report[9] = packet_counter
    report[10] = CONSTANTS["DS5_BT_HAPTICS_BLOCK_FLAGS"]
    report[11] = CONSTANTS["DS5_BT_HAPTICS_BLOCK_LENGTH"]

    data_offset = CONSTANTS["DS5_BT_HAPTICS_DATA_OFFSET"]
    report[data_offset : data_offset + len(haptics_data)] = haptics_data
    crc_offset = CONSTANTS["DS5_BT_HAPTICS_CRC_OFFSET"]
    crc = crc32_seeded(report[:crc_offset], 0xEADA2D49)
    report[crc_offset:] = crc.to_bytes(4, "little")

    transaction = bytes([CONSTANTS["DS5_BT_OUTPUT_TRANSACTION_HEADER"]]) + bytes(
        report
    )
    return (
        transaction,
        ((sequence & 0x0F) + 1) & 0x0F,
        packet_counter,
    )


def build_bt_audio(
    haptics_data,
    sequence,
    packet_counter,
    microphone_enabled,
    route_to_headphones,
    speaker_opus_data,
):
    if len(haptics_data) != CONSTANTS["DS5_HAPTICS_DATA_SIZE"]:
        raise ValueError("unexpected haptics data length")
    if len(speaker_opus_data) not in (0, 400):
        raise ValueError("unexpected speaker Opus length")

    report = bytearray(CONSTANTS["DS5_BT_HAPTICS_REPORT_SIZE"])
    report[0] = CONSTANTS["DS5_BT_HAPTICS_REPORT_ID"]
    report[1] = (sequence & 0x0F) << 4
    report[2] = CONSTANTS["DS5_BT_HAPTICS_STREAM_FLAGS"]
    report[3] = CONSTANTS["DS5_BT_HAPTICS_HEADER_LENGTH"]
    report[4] = (
        CONSTANTS["DS5_BT_AUDIO_MIC_ENABLED_ROUTING"]
        if microphone_enabled
        else CONSTANTS["DS5_BT_HAPTICS_ROUTING"]
    )
    report[5:9] = bytes([CONSTANTS["DS5_BT_HAPTICS_BUFFER_LENGTH"]]) * 4
    packet_counter = (packet_counter + 2) & 0xFF
    report[9] = packet_counter
    report[10] = CONSTANTS["DS5_BT_HAPTICS_BLOCK_FLAGS"]
    report[11] = CONSTANTS["DS5_BT_HAPTICS_BLOCK_LENGTH"]
    report[12 : 12 + len(haptics_data)] = haptics_data
    if speaker_opus_data:
        report[CONSTANTS["DS5_BT_AUDIO_SPEAKER_FLAGS_OFFSET"]] = (
            CONSTANTS["DS5_BT_AUDIO_HEADPHONE_FLAGS"]
            if route_to_headphones
            else CONSTANTS["DS5_BT_AUDIO_SPEAKER_FLAGS"]
        )
        report[CONSTANTS["DS5_BT_AUDIO_SPEAKER_LENGTH_OFFSET"]] = 200
        offset = CONSTANTS["DS5_BT_AUDIO_SPEAKER_DATA_OFFSET"]
        report[offset : offset + len(speaker_opus_data)] = speaker_opus_data
    crc_offset = CONSTANTS["DS5_BT_HAPTICS_CRC_OFFSET"]
    crc = crc32_seeded(report[:crc_offset], 0xEADA2D49)
    report[crc_offset:] = crc.to_bytes(4, "little")
    return (
        bytes([CONSTANTS["DS5_BT_OUTPUT_TRANSACTION_HEADER"]]) + bytes(report),
        ((sequence & 0x0F) + 1) & 0x0F,
        packet_counter,
    )


def build_bt_microphone_status(microphone_enabled, sequence):
    report = bytearray(CONSTANTS["DS5_BT_INITIALIZATION_REPORT_SIZE"])
    report[0] = CONSTANTS["DS5_BT_MIC_STATUS_REPORT_ID"]
    report[1] = (sequence & 0x0F) << 4
    report[2] = CONSTANTS["DS5_BT_MIC_STATUS_FLAGS"]
    report[3] = CONSTANTS["DS5_BT_MIC_STATUS_LENGTH"]
    report[4] = (
        CONSTANTS["DS5_BT_MIC_STATUS_ENABLED"]
        if microphone_enabled
        else CONSTANTS["DS5_BT_MIC_STATUS_DISABLED"]
    )
    crc_offset = CONSTANTS["DS5_BT_INITIALIZATION_CRC_OFFSET"]
    crc = crc32_seeded(report[:crc_offset], 0xEADA2D49)
    report[crc_offset:] = crc.to_bytes(4, "little")
    return (
        bytes([CONSTANTS["DS5_BT_OUTPUT_TRANSACTION_HEADER"]]) + bytes(report),
        ((sequence & 0x0F) + 1) & 0x0F,
    )


def build_bt_initialization(mic_select=0):
    if not 0 <= mic_select <= 3:
        raise ValueError("unexpected microphone selection")

    report = bytearray(CONSTANTS["DS5_BT_INITIALIZATION_REPORT_SIZE"])
    report[0] = CONSTANTS["DS5_BT_INITIALIZATION_REPORT_ID"]
    report[1] = CONSTANTS["DS5_BT_INITIALIZATION_TAG"]
    report[2] = CONSTANTS["DS5_BT_INITIALIZATION_FLAGS"]
    report[3] = CONSTANTS["DS5_BT_INITIALIZATION_MODE"]
    state = CONSTANTS["DS5_BT_INITIALIZATION_STATE_OFFSET"]
    report[state + 0] = 0x80
    report[state + 1] = 0x07
    report[state + 7] = mic_select
    report[state + 38] = 0x03
    report[state + 41] = 0x02
    report[state + 42] = 0x00
    report[state + 44 : state + 47] = bytes.fromhex("ffd700")
    crc_offset = CONSTANTS["DS5_BT_INITIALIZATION_CRC_OFFSET"]
    crc = crc32_seeded(report[:crc_offset], 0xEADA2D49)
    report[crc_offset:] = crc.to_bytes(4, "little")
    return bytes([CONSTANTS["DS5_BT_OUTPUT_TRANSACTION_HEADER"]]) + bytes(report)


def build_feature_set(report_id, payload):
    body = bytes([report_id]) + bytes(payload)
    crc = crc32_seeded(body, 0x2060EFC3)
    return (
        bytes([CONSTANTS["DS5_FEATURE_SET_HEADER"]])
        + body
        + crc.to_bytes(4, "little")
    )


class Ds5ProtocolContractTests(unittest.TestCase):
    def test_wire_size_relationships(self):
        self.assertEqual(
            CONSTANTS["DS5_BT_INPUT_PAYLOAD_OFFSET"]
            + CONSTANTS["DS5_USB_INPUT_PAYLOAD_SIZE"],
            CONSTANTS["DS5_BT_INPUT_MIN_SIZE"],
        )
        self.assertEqual(
            CONSTANTS["DS5_BT_OUTPUT_REPORT_OFFSET"]
            + CONSTANTS["DS5_BT_OUTPUT_REPORT_SIZE"],
            CONSTANTS["DS5_BT_OUTPUT_TRANSACTION_SIZE"],
        )
        self.assertEqual(
            CONSTANTS["DS5_BT_OUTPUT_CRC_OFFSET"] + 4,
            CONSTANTS["DS5_BT_OUTPUT_REPORT_SIZE"],
        )
        self.assertEqual(
            CONSTANTS["DS5_USB_OUTPUT_STATE_OFFSET"]
            + CONSTANTS["DS5_USB_OUTPUT_STATE_SIZE"],
            CONSTANTS["DS5_USB_OUTPUT_REPORT_SIZE"],
        )
        self.assertEqual(
            CONSTANTS["DS5_BT_HAPTICS_REPORT_SIZE"] + 1,
            CONSTANTS["DS5_BT_HAPTICS_TRANSACTION_SIZE"],
        )
        self.assertEqual(
            CONSTANTS["DS5_BT_HAPTICS_CRC_OFFSET"] + 4,
            CONSTANTS["DS5_BT_HAPTICS_REPORT_SIZE"],
        )
        self.assertLessEqual(
            CONSTANTS["DS5_BT_HAPTICS_DATA_OFFSET"]
            + CONSTANTS["DS5_HAPTICS_DATA_SIZE"],
            CONSTANTS["DS5_BT_HAPTICS_CRC_OFFSET"],
        )
        self.assertEqual(
            CONSTANTS["DS5_BT_INITIALIZATION_REPORT_SIZE"] + 1,
            CONSTANTS["DS5_BT_INITIALIZATION_TRANSACTION_SIZE"],
        )
        self.assertEqual(
            CONSTANTS["DS5_BT_INITIALIZATION_CRC_OFFSET"] + 4,
            CONSTANTS["DS5_BT_INITIALIZATION_REPORT_SIZE"],
        )

    def test_extracts_63_byte_input_payload(self):
        payload = bytes(range(CONSTANTS["DS5_USB_INPUT_PAYLOAD_SIZE"]))
        packet = bytes(
            [
                CONSTANTS["DS5_BT_INPUT_TRANSACTION_HEADER"],
                CONSTANTS["DS5_BT_INPUT_REPORT_ID"],
                0x7F,
            ]
        ) + payload
        self.assertEqual(extract_usb_input(packet), payload)

    def test_rejects_short_or_wrong_input_header_or_report(self):
        with self.assertRaises(ValueError):
            extract_usb_input(bytes(CONSTANTS["DS5_BT_INPUT_MIN_SIZE"] - 1))

        packet = bytearray(CONSTANTS["DS5_BT_INPUT_MIN_SIZE"])
        packet[CONSTANTS["DS5_BT_INPUT_REPORT_ID_OFFSET"]] = CONSTANTS[
            "DS5_BT_INPUT_REPORT_ID"
        ]
        with self.assertRaises(ValueError):
            extract_usb_input(packet)

        packet[0] = CONSTANTS["DS5_BT_INPUT_TRANSACTION_HEADER"]
        packet[CONSTANTS["DS5_BT_INPUT_REPORT_ID_OFFSET"]] = 0x30
        with self.assertRaises(ValueError):
            extract_usb_input(packet)

    def test_zero_output_report_matches_golden_crc(self):
        usb_report = bytes([CONSTANTS["DS5_USB_OUTPUT_REPORT_ID"]]) + bytes(
            CONSTANTS["DS5_USB_OUTPUT_STATE_SIZE"]
        )
        transaction, next_sequence = build_bt_output(usb_report, 0)

        self.assertEqual(len(transaction), 79)
        self.assertEqual(transaction[:4], bytes.fromhex("a2310010"))
        self.assertEqual(transaction[-4:], bytes.fromhex("b5011523"))
        self.assertEqual(next_sequence, 1)

    def test_output_preserves_state_and_wraps_sequence(self):
        state = bytes(range(CONSTANTS["DS5_USB_OUTPUT_STATE_SIZE"]))
        usb_report = bytes([CONSTANTS["DS5_USB_OUTPUT_REPORT_ID"]]) + state
        transaction, next_sequence = build_bt_output(usb_report, 0x0F)

        report_offset = CONSTANTS["DS5_BT_OUTPUT_REPORT_OFFSET"]
        state_offset = report_offset + CONSTANTS["DS5_BT_OUTPUT_STATE_OFFSET"]
        self.assertEqual(transaction[2], 0xF0)
        self.assertEqual(transaction[state_offset : state_offset + len(state)], state)
        padding_start = state_offset + len(state)
        padding_end = state_offset + CONSTANTS["DS5_SET_STATE_SIZE"]
        self.assertEqual(transaction[padding_start:padding_end], bytes(16))
        self.assertEqual(next_sequence, 0)

    def test_startup_state_matches_original_update_state_contract(self):
        transaction = build_bt_initialization()
        state = CONSTANTS["DS5_BT_INITIALIZATION_STATE_OFFSET"] + 1

        self.assertEqual(
            len(transaction), CONSTANTS["DS5_BT_INITIALIZATION_TRANSACTION_SIZE"]
        )
        self.assertEqual(transaction[:5], bytes.fromhex("a23210903f"))
        self.assertEqual(transaction[state + 0], 0x80)
        self.assertEqual(transaction[state + 1], 0x07)
        self.assertEqual(transaction[state + 38], 0x03)
        self.assertEqual(transaction[state + 41], 0x02)
        self.assertEqual(transaction[state + 44 : state + 47], bytes.fromhex("ffd700"))
        self.assertEqual(
            int.from_bytes(transaction[-4:], "little"),
            crc32_seeded(transaction[1:-4], 0xEADA2D49),
        )

    def test_microphone_button_report_keeps_mute_and_yellow_led_in_sync(self):
        muted = build_usb_microphone_mute_report(True)
        unmuted = build_usb_microphone_mute_report(False)

        self.assertEqual(
            muted[CONSTANTS["DS5_USB_OUTPUT_VALID_FLAGS1_OFFSET"]], 0x03
        )
        self.assertEqual(
            muted[CONSTANTS["DS5_USB_OUTPUT_MUTE_LIGHT_OFFSET"]], 0x01
        )
        self.assertEqual(
            muted[CONSTANTS["DS5_USB_OUTPUT_MUTE_CONTROL_OFFSET"]], 0x10
        )
        self.assertEqual(
            unmuted[CONSTANTS["DS5_USB_OUTPUT_VALID_FLAGS1_OFFSET"]], 0x03
        )
        self.assertEqual(
            unmuted[CONSTANTS["DS5_USB_OUTPUT_MUTE_LIGHT_OFFSET"]], 0x00
        )
        self.assertEqual(
            unmuted[CONSTANTS["DS5_USB_OUTPUT_MUTE_CONTROL_OFFSET"]], 0x00
        )

        transaction, _ = build_bt_output(muted, 0)
        state = (
            CONSTANTS["DS5_BT_OUTPUT_REPORT_OFFSET"]
            + CONSTANTS["DS5_BT_OUTPUT_STATE_OFFSET"]
        )
        self.assertEqual(transaction[state + 1], 0x03)
        self.assertEqual(transaction[state + 8], 0x01)
        self.assertEqual(transaction[state + 9], 0x10)

    def test_startup_state_preserves_two_bit_microphone_setting(self):
        transaction = build_bt_initialization(3)
        state = CONSTANTS["DS5_BT_INITIALIZATION_STATE_OFFSET"] + 1

        self.assertEqual(transaction[state + 7], 3)
        with self.assertRaises(ValueError):
            build_bt_initialization(4)

    def test_rejects_wrong_usb_output_shape(self):
        with self.assertRaises(ValueError):
            build_bt_output(bytes(CONSTANTS["DS5_USB_OUTPUT_REPORT_SIZE"] - 1), 0)

        report = bytearray(CONSTANTS["DS5_USB_OUTPUT_REPORT_SIZE"])
        report[0] = 0x01
        with self.assertRaises(ValueError):
            build_bt_output(report, 0)

    def test_zero_haptics_report_matches_golden_crc(self):
        transaction, next_sequence, next_counter = build_bt_haptics(
            bytes(CONSTANTS["DS5_HAPTICS_DATA_SIZE"]), 0, 0
        )

        self.assertEqual(len(transaction), 548)
        self.assertEqual(
            transaction[:13],
            bytes.fromhex("a2390091067e4040404002d240"),
        )
        self.assertEqual(transaction[-4:], bytes.fromhex("fbb8ff6e"))
        self.assertEqual(next_sequence, 1)
        self.assertEqual(next_counter, 2)

    def test_haptics_preserves_samples_and_wraps_counters(self):
        samples = bytes(range(CONSTANTS["DS5_HAPTICS_DATA_SIZE"]))
        transaction, next_sequence, next_counter = build_bt_haptics(
            samples, 0x0F, 0xFE
        )
        report_offset = 1
        data_offset = (
            report_offset + CONSTANTS["DS5_BT_HAPTICS_DATA_OFFSET"]
        )

        self.assertEqual(transaction[2], 0xF0)
        self.assertEqual(
            transaction[data_offset : data_offset + len(samples)], samples
        )
        self.assertEqual(next_sequence, 0)
        self.assertEqual(next_counter, 0)

    def test_audio_report_embeds_two_fixed_opus_frames(self):
        haptics = bytes(range(CONSTANTS["DS5_HAPTICS_DATA_SIZE"]))
        speaker = bytes(range(200)) + bytes(range(200))
        transaction, next_sequence, next_counter = build_bt_audio(
            haptics, 0x0F, 0xFE, True, True, speaker
        )
        report = transaction[1:]
        offset = CONSTANTS["DS5_BT_AUDIO_SPEAKER_DATA_OFFSET"]

        self.assertEqual(len(transaction), 548)
        self.assertEqual(report[1], 0xF0)
        self.assertEqual(
            report[4], CONSTANTS["DS5_BT_AUDIO_MIC_ENABLED_ROUTING"]
        )
        self.assertEqual(
            report[CONSTANTS["DS5_BT_AUDIO_SPEAKER_FLAGS_OFFSET"]],
            CONSTANTS["DS5_BT_AUDIO_HEADPHONE_FLAGS"],
        )
        self.assertEqual(
            report[CONSTANTS["DS5_BT_AUDIO_SPEAKER_LENGTH_OFFSET"]], 200
        )
        self.assertEqual(report[offset : offset + len(speaker)], speaker)
        self.assertEqual(next_sequence, 0)
        self.assertEqual(next_counter, 0)

    def test_microphone_status_uses_audio_state_report_and_crc(self):
        transaction, next_sequence = build_bt_microphone_status(True, 3)
        report = transaction[1:]

        self.assertEqual(len(transaction), 143)
        self.assertEqual(transaction[:6], bytes.fromhex("a23230910103"))
        self.assertEqual(report[4], CONSTANTS["DS5_BT_MIC_STATUS_ENABLED"])
        self.assertEqual(next_sequence, 4)
        self.assertEqual(
            int.from_bytes(report[-4:], "little"),
            crc32_seeded(report[:-4], 0xEADA2D49),
        )

    def test_rejects_wrong_haptics_shape(self):
        with self.assertRaises(ValueError):
            build_bt_haptics(
                bytes(CONSTANTS["DS5_HAPTICS_DATA_SIZE"] - 1), 0, 0
            )

    def test_feature_transactions_match_golden_vector(self):
        get_transaction = bytes(
            [CONSTANTS["DS5_FEATURE_GET_HEADER"], 0x20]
        )
        self.assertEqual(get_transaction, bytes.fromhex("4320"))
        self.assertEqual(
            build_feature_set(0x20, bytes.fromhex("010203")),
            bytes.fromhex("532001020337b45eb3"),
        )
        self.assertEqual(CONSTANTS["DS5_FEATURE_SET_MAX_PAYLOAD"], 63)
        self.assertEqual(
            len(build_feature_set(0x80, bytes(63))),
            CONSTANTS["DS5_FEATURE_SET_MAX_PAYLOAD"]
            + CONSTANTS["DS5_FEATURE_SET_OVERHEAD"],
        )

    def test_protocol_c_has_no_platform_includes(self):
        includes = re.findall(
            r'^#include\s+[<"]([^>"]+)[>"]',
            SOURCE_PATH.read_text(encoding="utf-8"),
            re.MULTILINE,
        )
        self.assertEqual(includes, ["ds5_protocol.h", "string.h"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
