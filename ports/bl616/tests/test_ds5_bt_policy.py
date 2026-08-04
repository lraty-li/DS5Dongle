from pathlib import Path
import re
import unittest


BL616_DIR = Path(__file__).resolve().parents[1]
HEADER_PATH = BL616_DIR / "bluetooth" / "ds5_bt_policy.h"
SOURCE_PATH = BL616_DIR / "bluetooth" / "ds5_bt_policy.c"
BT_SOURCE_PATH = BL616_DIR / "bluetooth" / "ds5_bt.c"


def read_numeric_macros():
    header = HEADER_PATH.read_text(encoding="utf-8")
    matches = re.findall(
        r"^#define\s+(DS5_BT_[A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+|[0-9]+)U?$",
        header,
        re.MULTILINE,
    )
    return {name: int(value, 0) for name, value in matches}


CONSTANTS = read_numeric_macros()
KNOWN_NAMES = {
    "Wireless Controller",
    "DualSense Wireless Controller",
    "DualSense Edge Wireless Controller",
}


def is_gamepad(device_class):
    return (
        device_class & CONSTANTS["DS5_BT_COD_MAJOR_MASK"]
        == CONSTANTS["DS5_BT_COD_MAJOR_PERIPHERAL"]
        and device_class & CONSTANTS["DS5_BT_COD_MINOR_TYPE_MASK"]
        == CONSTANTS["DS5_BT_COD_MINOR_GAMEPAD"]
    )


def candidate_score(device_class, name=None, saved=False):
    if saved:
        score = CONSTANTS["DS5_BT_CANDIDATE_SCORE_SAVED"]
    elif is_gamepad(device_class):
        score = CONSTANTS["DS5_BT_CANDIDATE_SCORE_GAMEPAD"]
    else:
        return 0

    if name in KNOWN_NAMES:
        score += CONSTANTS["DS5_BT_CANDIDATE_SCORE_NAME"]
    return score


def extract_name(eir):
    offset = 0
    while offset < len(eir):
        field_length = eir[offset]
        offset += 1
        if field_length == 0:
            return None
        if field_length > len(eir) - offset:
            raise ValueError("malformed EIR")
        field_type = eir[offset]
        if field_type in (
            CONSTANTS["DS5_BT_EIR_SHORT_NAME"],
            CONSTANTS["DS5_BT_EIR_COMPLETE_NAME"],
        ):
            return bytes(eir[offset + 1 : offset + field_length]).decode("ascii")
        offset += field_length
    return None


class Ds5BtPolicyTests(unittest.TestCase):
    def test_dualsense_gamepad_class_is_candidate(self):
        self.assertTrue(is_gamepad(0x002508))
        self.assertEqual(
            candidate_score(0x002508, "Wireless Controller"),
            CONSTANTS["DS5_BT_CANDIDATE_SCORE_GAMEPAD"]
            + CONSTANTS["DS5_BT_CANDIDATE_SCORE_NAME"],
        )

    def test_name_alone_never_selects_device(self):
        self.assertEqual(candidate_score(0x000000, "Wireless Controller"), 0)

    def test_other_peripherals_do_not_match_gamepad_minor_class(self):
        self.assertFalse(is_gamepad(0x002504))
        self.assertFalse(is_gamepad(0x001F08))

    def test_saved_address_has_priority(self):
        self.assertGreater(
            candidate_score(0, saved=True),
            candidate_score(0x002508, "Wireless Controller"),
        )

    def test_extracts_complete_name_after_other_eir_field(self):
        name = b"Wireless Controller"
        eir = bytes([2, 0x01, 0x02, len(name) + 1, 0x09]) + name + bytes([0])
        self.assertEqual(extract_name(eir), name.decode("ascii"))

    def test_rejects_malformed_eir(self):
        with self.assertRaises(ValueError):
            extract_name(bytes([5, 0x09, ord("D")]))

    def test_policy_c_has_no_sdk_or_rtos_includes(self):
        includes = re.findall(
            r'^#include\s+[<"]([^>"]+)[>"]',
            SOURCE_PATH.read_text(encoding="utf-8"),
            re.MULTILINE,
        )
        self.assertEqual(includes, ["ds5_bt_policy.h", "string.h"])

    def test_discovery_only_selects_and_explicit_api_creates_acl(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")
        discovery_start = source.index("static void ds5_bt_discovery_complete")
        discovery_end = source.index("static void ds5_bt_process_l2cap_event")
        connect_start = source.index("int ds5_bt_connect_candidate(void)")
        connect_end = source.index("int ds5_bt_disconnect(void)")

        self.assertNotIn(
            "bt_conn_create_br", source[discovery_start:discovery_end]
        )
        self.assertIn("bt_conn_create_br", source[connect_start:connect_end])

    def test_security_gate_handles_already_encrypted_reconnect(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")
        connected_start = source.index("static void ds5_bt_connected")
        connected_end = source.index("static void ds5_bt_disconnected")
        connected_body = source[connected_start:connected_end]

        self.assertLess(
            connected_body.index("bt_conn_set_security"),
            connected_body.index("bt_conn_get_security"),
        )
        self.assertIn("ds5_bt_security_ready(conn)", connected_body)

    def test_saved_bond_is_restored_before_falling_back_to_discovery(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")
        ready_start = source.index("static void ds5_bt_ready")
        ready_end = source.index("int ds5_bt_init(void)")
        ready_body = source[ready_start:ready_end]

        self.assertLess(
            ready_body.index("bt_br_foreach_bond"),
            ready_body.index("ds5_bt_start_discovery"),
        )
        self.assertIn("bt_br_set_connectable(true)", ready_body)

    def test_pairing_clear_uses_public_pinned_sdk_api(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")
        clear_start = source.index("int ds5_bt_clear_pairing(void)")
        clear_end = source.index("ds5_bt_state_t ds5_bt_get_state(void)")
        clear_body = source[clear_start:clear_end]

        self.assertIn("bt_unpair(BT_ID_DEFAULT, NULL)", clear_body)
        self.assertNotIn("ef_", clear_body)

    def test_connection_pointer_follows_vendor_sticky_ref_contract(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")
        self.assertNotIn("bt_conn_ref(", source)
        self.assertNotIn("bt_conn_unref(", source)

    def test_connection_state_machine_has_no_usb_or_audio_dependency(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8").lower()
        for forbidden in ("cherryusb", "tinyusb", "opus", "wdl", "audio"):
            self.assertNotIn(forbidden, source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
