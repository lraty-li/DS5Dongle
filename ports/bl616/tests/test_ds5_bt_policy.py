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

    def test_saved_bond_and_pairing_window_are_started_independently(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")
        ready_start = source.index("static void ds5_bt_ready")
        ready_end = source.index("int ds5_bt_init(void)")
        ready_body = source[ready_start:ready_end]

        self.assertLess(
            ready_body.index("bt_br_foreach_bond"),
            ready_body.index("ds5_bt_open_pairing_window"),
        )
        self.assertIn("ds5_bt_enable_bonded_page_scan()", ready_body)
        self.assertIn("ds5_bt_set_state(DS5_BT_STATE_IDLE)", ready_body)
        self.assertIn(
            "pairing_window_deadline = now + pdMS_TO_TICKS("
            "DS5_BT_PAIRING_WINDOW_MS);",
            source,
        )
        header = (BL616_DIR / "bluetooth" / "ds5_bt.h").read_text(
            encoding="utf-8"
        )
        self.assertRegex(header, r"DS5_BT_STATE_OFF\s*=\s*0")
        self.assertRegex(header, r"DS5_BT_STATE_CANDIDATE_READY\s*=\s*3")
        self.assertRegex(header, r"DS5_BT_STATE_ACL_CONNECTING\s*=\s*5")
        self.assertRegex(header, r"DS5_BT_STATE_DISCONNECTING\s*=\s*9")

    def test_bond_is_persistent_policy_state_not_candidate_state(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")
        connected_start = source.index("static void ds5_bt_connected")
        connected_end = source.index("static void ds5_bt_disconnected")
        connected_body = source[connected_start:connected_end]
        restore_start = source.index("static void ds5_bt_restore_bond")
        restore_end = source.index("static void ds5_bt_discovery_complete")
        restore_body = source[restore_start:restore_end]

        self.assertIn("static volatile bool bonded_peer_valid;", source)
        self.assertIn(
            "ds5_bt_connection_matches_saved_peer(conn)", connected_body
        )
        self.assertIn("!bonded_peer_valid", restore_body)
        self.assertIn(
            "bt_addr_copy(&bonded_peer_address, info->addr)", restore_body
        )

    def test_discovery_prefers_saved_peer_without_storing_it_as_candidate(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")
        candidate_start = source.index("static void ds5_bt_consider_candidate")
        candidate_end = source.index("static void ds5_bt_restore_bond")
        candidate_body = source[candidate_start:candidate_end]

        self.assertIn("saved_address_match", candidate_body)
        self.assertIn("ds5_bt_policy_candidate_score(", candidate_body)
        self.assertIn("saved_address_match);", candidate_body)
        self.assertNotIn("candidate.saved", candidate_body)

    def test_late_discovery_completion_cannot_replace_active_link(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")
        start = source.index("static void ds5_bt_discovery_complete")
        end = source.index("static int ds5_bt_request_next_feature")
        body = source[start:end]

        self.assertIn("active_connection != NULL", body)
        self.assertIn("!discovery_allowed", body)
        self.assertIn(
            "bluetooth_state != DS5_BT_STATE_DISCOVERING", body
        )

    def test_link_recovery_reasserts_page_scan_and_reopens_pairing_window(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")
        start = source.index("static void ds5_bt_recover_after_link")
        end = source.index("static int ds5_bt_disconnect_active")
        body = source[start:end]

        self.assertIn("ds5_bt_enable_bonded_page_scan()", body)
        self.assertIn("ds5_bt_open_pairing_window()", body)
        self.assertIn("ds5_bt_set_state(DS5_BT_STATE_IDLE)", body)

    def test_all_non_active_br_terminal_paths_restore_bonded_page_scan(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")
        connected_start = source.index("static void ds5_bt_connected")
        connected_end = source.index("static void ds5_bt_disconnected")
        disconnected_start = connected_end
        disconnected_end = source.index("static void ds5_bt_security_changed")

        self.assertIn(
            "ds5_bt_enable_bonded_page_scan()",
            source[connected_start:connected_end],
        )
        self.assertIn(
            "ds5_bt_enable_bonded_page_scan()",
            source[disconnected_start:disconnected_end],
        )

    def test_newly_paired_peer_is_promoted_to_persistent_bond(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")
        pairing_start = source.index("static void ds5_bt_pairing_complete")
        pairing_end = source.index("static void ds5_bt_pairing_failed")
        connect_start = source.index("static void ds5_bt_connected")
        connect_end = source.index("static void ds5_bt_disconnected")

        self.assertIn("active_peer_should_persist_bond", source)
        self.assertIn(
            "bonded && active_peer_should_persist_bond",
            source[pairing_start:pairing_end],
        )
        self.assertIn(
            "matches_candidate || matches_bonded ||",
            source[connect_start:connect_end],
        )

    def test_outgoing_acl_reservation_covers_create_callback_race(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")
        start = source.index("int ds5_bt_connect_candidate(void)")
        end = source.index("int ds5_bt_disconnect(void)")
        body = source[start:end]
        create_index = body.index("bt_conn_create_br")

        self.assertLess(
            body.index("outgoing_create_pending = true"), create_index
        )
        self.assertGreater(
            body.index("outgoing_create_pending = false", create_index),
            create_index,
        )
        self.assertIn("ds5_bt_claim_active_connection", body)

    def test_rejected_bond_has_a_fresh_pairing_fallback(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")

        self.assertIn("bt_unpair(BT_ID_DEFAULT, &address)", source)
        self.assertIn("bond_recovery_pending", source)
        self.assertIn("ds5_bt_process_bond_recovery(now)", source)
        self.assertIn("ds5_bt_open_pairing_window()", source)

    def test_deadline_comparisons_are_tick_wrap_safe(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")
        start = source.index("static bool ds5_bt_deadline_expired")
        end = source.index("static int ds5_bt_start_discovery_internal")
        body = source[start:end]

        self.assertIn("(int32_t)(now - deadline)", body)
        self.assertNotIn("now >= deadline", body)

    def test_link_phase_timeout_has_a_terminal_recovery_path(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")
        start = source.index("static void ds5_bt_policy_handle_timeout")
        end = source.index("static void ds5_bt_policy_step")
        body = source[start:end]

        self.assertIn("DS5_BT_DISCONNECT_TIMEOUT_MS", source)
        self.assertIn("ds5_bt_reset_link_state();", body)
        self.assertIn("ds5_bt_recover_after_link(bonded_link);", body)

    def test_l2cap_events_are_bound_to_connection_session(self):
        header = (BL616_DIR / "bluetooth" / "ds5_l2cap.h").read_text(
            encoding="utf-8"
        )
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")

        self.assertIn("struct bt_conn *conn;", header)
        self.assertIn("uint32_t session;", header)
        self.assertIn("event->conn != active_connection", source)
        self.assertIn("event->session != link_generation", source)

    def test_immediate_interrupt_connect_failure_clears_channel_metadata(self):
        source = (BL616_DIR / "bluetooth" / "ds5_l2cap.c").read_text(
            encoding="utf-8"
        )
        start = source.index("static void ds5_l2cap_connected")
        end = source.index("static void ds5_l2cap_disconnected")
        body = source[start:end]

        self.assertIn(
            "channel_connections[DS5_L2CAP_CHANNEL_INTERRUPT] = NULL;",
            body,
        )
        self.assertIn(
            "channel_sessions[DS5_L2CAP_CHANNEL_INTERRUPT] = 0U;", body
        )

    def test_policy_task_owns_discovery_and_candidate_retry(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")
        main = (BL616_DIR / "main.c").read_text(encoding="utf-8")

        self.assertIn("static void ds5_bt_policy_worker", source)
        self.assertIn("ds5_bt_start_discovery_internal()", source)
        self.assertIn("ds5_bt_connect_candidate()", source)
        self.assertNotIn("ds5_bt_start_discovery()", main)
        self.assertNotIn("ds5_bt_connect_candidate()", main)
        self.assertNotIn("RECONNECT_TIMEOUT", main)

    def test_link_generation_replays_link_scoped_microphone_state(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")

        self.assertIn("pending_microphone_state = true;", source)
        self.assertIn("pending_usb_output = false;", source)
        self.assertIn("ds5_feature_cache_clear();", source)

    def test_discovery_stop_is_completed_without_waiting_for_callback(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")
        start = source.index("static void ds5_bt_stop_discovery_if_needed")
        end = source.index("static void ds5_bt_policy_handle_timeout")
        body = source[start:end]

        self.assertIn("bt_br_discovery_stop()", body)
        self.assertIn("discovery_in_flight = false;", body)
        self.assertIn("The pinned SDK does not invoke", body)

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

    def test_connection_state_machine_keeps_codecs_and_usb_outside_bluetooth(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8").lower()
        for forbidden in ("cherryusb", "tinyusb", "#include \"opus.h\"", "wdl_"):
            self.assertNotIn(forbidden, source)
        self.assertIn('#include "ds5_audio_mailbox.h"', source)

    def test_feature_set_uses_bluetooth_control_l2cap(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")
        helper_start = source.index("static bool ds5_bt_send_feature_set(")
        helper_end = source.index("static void ds5_bt_tx_worker", helper_start)
        helper = source[helper_start:helper_end]

        self.assertIn("ds5_build_feature_set_transaction", helper)
        self.assertIn("DS5_L2CAP_CHANNEL_CONTROL", helper)
        self.assertIn("ds5_l2cap_send", helper)
        self.assertIn("ds5_feature_set_mailbox_try_receive", source)

    def test_tx_worker_waits_for_events_when_idle(self):
        source = BT_SOURCE_PATH.read_text(encoding="utf-8")

        self.assertIn("void ds5_bt_tx_wake(void)", source)
        self.assertIn("ulTaskNotifyTake(pdTRUE, wait_ticks)", source)
        self.assertIn("wait_ticks = portMAX_DELAY", source)
        self.assertIn("DS5_BT_TX_RETRY_MS", source)
        self.assertNotIn("DS5_BT_TX_POLL_MS", source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
