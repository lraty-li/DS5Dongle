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

    def test_rejects_wrong_usb_output_shape(self):
        with self.assertRaises(ValueError):
            build_bt_output(bytes(CONSTANTS["DS5_USB_OUTPUT_REPORT_SIZE"] - 1), 0)

        report = bytearray(CONSTANTS["DS5_USB_OUTPUT_REPORT_SIZE"])
        report[0] = 0x01
        with self.assertRaises(ValueError):
            build_bt_output(report, 0)

    def test_feature_transactions_match_golden_vector(self):
        get_transaction = bytes(
            [CONSTANTS["DS5_FEATURE_GET_HEADER"], 0x20]
        )
        self.assertEqual(get_transaction, bytes.fromhex("4320"))
        self.assertEqual(
            build_feature_set(0x20, bytes.fromhex("010203")),
            bytes.fromhex("532001020337b45eb3"),
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
