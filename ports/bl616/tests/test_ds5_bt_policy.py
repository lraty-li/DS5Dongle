from pathlib import Path
import re
import unittest


BL616_DIR = Path(__file__).resolve().parents[1]
HEADER_PATH = BL616_DIR / "bluetooth" / "ds5_bt_policy.h"
SOURCE_PATH = BL616_DIR / "bluetooth" / "ds5_bt_policy.c"
BT_SOURCE_PATH = BL616_DIR / "bluetooth" / "ds5_bt.c"
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
    def test_main_bluetooth_source_stays_within_line_limit(self):
        self.assertLessEqual(
            len(BT_SOURCE_PATH.read_text(encoding="utf-8").splitlines()),
            600,
        )

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
        source = read_bt_sources()
        discovery_start = source.index("static void ds5_bt_discovery_complete")
        discovery_end = source.index("static void ds5_bt_process_l2cap_event")
        connect_start = source.index("int ds5_bt_connect_candidate(void)")
        connect_end = source.index("int ds5_bt_disconnect(void)")

        self.assertNotIn(
            "bt_conn_create_br", source[discovery_start:discovery_end]
        )
        self.assertIn("bt_conn_create_br", source[connect_start:connect_end])

    def test_security_gate_handles_already_encrypted_reconnect(self):
        source = read_bt_sources()
        connected_start = source.index("static void ds5_bt_connected")
        connected_end = source.index("static void ds5_bt_disconnected")
        connected_body = source[connected_start:connected_end]

        self.assertLess(
            connected_body.index("bt_conn_set_security"),
            connected_body.index("bt_conn_get_security"),
        )
        self.assertIn("ds5_bt_security_ready(conn)", connected_body)

    def test_saved_bond_and_pairing_window_are_started_independently(self):
        source = read_bt_sources()
        ready_start = source.index("static void ds5_bt_ready")
        ready_end = source.index("int ds5_bt_init(void)")
        ready_body = source[ready_start:ready_end]

        self.assertLess(
            ready_body.index("bt_br_foreach_bond"),
            ready_body.index("ds5_bt_open_pairing_window"),
        )
        self.assertIn("page_scan_restore_pending = true;", ready_body)
        self.assertNotIn("ds5_bt_enable_bonded_page_scan();", ready_body)
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
        source = read_bt_sources()
        connected_start = source.index("static void ds5_bt_connected")
        connected_end = source.index("static void ds5_bt_disconnected")
        connected_body = source[connected_start:connected_end]
        restore_start = source.index("void ds5_bt_restore_bond")
        restore_end = source.index("static void ds5_bt_discovery_complete")
        restore_body = source[restore_start:restore_end]

        self.assertIn("volatile bool bonded_peer_valid;", source)
        self.assertIn(
            "ds5_bt_connection_matches_saved_peer(conn)", connected_body
        )
        self.assertIn("!bonded_peer_valid", restore_body)
        self.assertIn(
            "bt_addr_copy(&bonded_peer_address, info->addr)", restore_body
        )

    def test_discovery_prefers_saved_peer_without_storing_it_as_candidate(self):
        source = read_bt_sources()
        candidate_start = source.index("static void ds5_bt_consider_candidate")
        candidate_end = source.index("void ds5_bt_restore_bond")
        candidate_body = source[candidate_start:candidate_end]

        self.assertIn("saved_address_match", candidate_body)
        self.assertIn("ds5_bt_policy_candidate_score(", candidate_body)
        self.assertIn("saved_address_match);", candidate_body)
        self.assertNotIn("candidate.saved", candidate_body)

    def test_late_discovery_completion_cannot_replace_active_link(self):
        source = read_bt_sources()
        start = source.index("static void ds5_bt_discovery_complete")
        end = source.index("static int ds5_bt_request_next_feature")
        body = source[start:end]

        self.assertIn("active_connection != NULL", body)
        self.assertIn("!discovery_allowed", body)
        self.assertIn(
            "bluetooth_state != DS5_BT_STATE_DISCOVERING", body
        )

    def test_link_recovery_reasserts_page_scan_and_reopens_pairing_window(self):
        source = read_bt_sources()
        start = source.index("void ds5_bt_recover_after_link")
        end = source.index("int ds5_bt_disconnect_active")
        body = source[start:end]

        self.assertIn("ds5_bt_enable_bonded_page_scan()", body)
        self.assertIn("ds5_bt_open_pairing_window()", body)
        self.assertIn("ds5_bt_set_state(DS5_BT_STATE_IDLE)", body)

    def test_acl_reset_aborts_local_l2cap_channels_without_disconnect_request(self):
        bt_source = read_bt_sources()
        l2cap_source = (BL616_DIR / "bluetooth" / "ds5_l2cap.c").read_text(
            encoding="utf-8"
        )
        reset_start = bt_source.index("void ds5_bt_reset_link_state")
        reset_end = bt_source.index(
            "void ds5_bt_recover_after_link", reset_start
        )
        reset_body = bt_source[reset_start:reset_end]

        self.assertIn("ds5_l2cap_abort_connection(connection)", reset_body)
        self.assertNotIn("ds5_l2cap_disconnect()", reset_body)
        self.assertIn("bt_l2cap_chan_del(&channel->chan)", l2cap_source)

        abort_start = l2cap_source.index("static bool ds5_l2cap_abort_channel")
        abort_end = l2cap_source.index(
            "void ds5_l2cap_abort_connection", abort_start
        )
        abort_body = l2cap_source[abort_start:abort_end]
        self.assertIn("channel_connection != terminated_connection", abort_body)
        self.assertIn("Never let an old ACL callback delete a channel", abort_body)

        disconnected_start = bt_source.index("static void ds5_bt_disconnected")
        disconnected_end = bt_source.index(
            "static void ds5_bt_security_changed", disconnected_start
        )
        disconnected_body = bt_source[disconnected_start:disconnected_end]
        self.assertIn("ds5_l2cap_abort_connection(conn);", disconnected_body)

    def test_tx_owns_sequence_and_uses_a_protected_link_snapshot(self):
        source = read_bt_sources()

        self.assertNotIn("static ds5_output_sequence_t output_sequence", source)
        self.assertIn("ds5_output_sequence_t output_sequence;", source)
        self.assertIn("ds5_bt_get_tx_link_state(&link_state);", source)
        self.assertIn("ds5_bt_complete_initialization(", source)
        self.assertIn("link_state.headset_connected", source)

    def test_policy_task_starts_after_boot_state_is_initialized(self):
        source = read_bt_sources()
        ready_start = source.index("static void ds5_bt_ready")
        ready_end = source.index("int ds5_bt_init", ready_start)
        ready_body = source[ready_start:ready_end]

        self.assertLess(
            ready_body.index("ds5_bt_open_pairing_window();"),
            ready_body.index("ds5_bt_start_worker();"),
        )

        policy_start = source.index("static void ds5_bt_policy_step")
        policy_end = source.index("static TickType_t ds5_bt_policy_wait_ticks")
        policy_body = source[policy_start:policy_end]
        self.assertIn("ds5_bt_process_page_scan_retry(now);", policy_body)

    def test_lifecycle_callbacks_and_policy_use_one_recursive_mutex(self):
        source = read_bt_sources()

        self.assertIn("xSemaphoreCreateRecursiveMutexStatic", source)
        self.assertIn("xSemaphoreTakeRecursive", source)
        self.assertIn("xSemaphoreGiveRecursive", source)
        for function in (
            "static void ds5_bt_connected",
            "static void ds5_bt_disconnected",
            "static void ds5_bt_security_changed",
            "static void ds5_bt_policy_step",
        ):
            start = source.index(function)
            next_function = source.find("\nstatic ", start + len(function))
            body = source[start:] if next_function < 0 else source[start:next_function]
            self.assertIn("ds5_bt_lifecycle_lock();", body)
            self.assertIn("ds5_bt_lifecycle_unlock();", body)

    def test_page_scan_restore_retries_after_a_failed_sdk_request(self):
        source = read_bt_sources()
        start = source.index("void ds5_bt_enable_bonded_page_scan")
        end = source.index("void ds5_bt_disable_page_scan")
        enable_body = source[start:end]

        self.assertIn("page_scan_restore_pending = true;", enable_body)
        self.assertIn("DS5_BT_PAGE_SCAN_RETRY_MS", enable_body)
        self.assertIn("static void ds5_bt_process_page_scan_retry", source)
        self.assertIn("ds5_bt_process_page_scan_retry(now)", source)
        self.assertIn("page_scan_wait", source)

    def test_bond_recovery_requires_an_explicit_missing_key_error(self):
        source = read_bt_sources()
        security_start = source.index("static void ds5_bt_security_changed")
        security_end = source.index("static struct bt_conn_cb")
        pairing_start = source.index("static void ds5_bt_pairing_failed")
        pairing_end = source.index("static const struct bt_conn_auth_cb")
        security_body = source[security_start:security_end]
        pairing_body = source[pairing_start:pairing_end]

        self.assertIn(
            "BT_SECURITY_ERR_PIN_OR_KEY_MISSING", source
        )
        self.assertIn(
            "if (ds5_bt_should_recover_bond(err))", security_body
        )
        self.assertIn(
            "if (ds5_bt_should_recover_bond(reason))", pairing_body
        )
        self.assertNotIn(
            "ds5_bt_request_bond_recovery();\n        (void)ds5_bt_disconnect_active",
            security_body,
        )
        self.assertNotIn(
            "printf(\"DS5 BT: pairing failed (reason %d)\\r\\n\", "
            "(int)reason);\n        ds5_bt_request_bond_recovery();",
            pairing_body,
        )

    def test_all_non_active_br_terminal_paths_restore_bonded_page_scan(self):
        source = read_bt_sources()
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
        source = read_bt_sources()
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
        source = read_bt_sources()
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
        source = read_bt_sources()

        self.assertIn("bt_unpair(BT_ID_DEFAULT, &address)", source)
        self.assertIn("bond_recovery_pending", source)
        self.assertIn("ds5_bt_process_bond_recovery(now)", source)
        self.assertIn("ds5_bt_open_pairing_window()", source)

    def test_deadline_comparisons_are_tick_wrap_safe(self):
        source = read_bt_sources()
        start = source.index("static bool ds5_bt_deadline_expired")
        end = source.index("int ds5_bt_start_discovery_internal")
        body = source[start:end]

        self.assertIn("(int32_t)(now - deadline)", body)
        self.assertNotIn("now >= deadline", body)

    def test_link_phase_timeout_has_a_terminal_recovery_path(self):
        source = read_bt_sources()
        start = source.index("static void ds5_bt_policy_handle_timeout")
        end = source.index("static void ds5_bt_policy_step")
        body = source[start:end]

        self.assertIn("DS5_BT_DISCONNECT_TIMEOUT_MS", source)
        self.assertIn("ds5_bt_reset_link_state();", body)
        self.assertIn("ds5_bt_recover_after_link(bonded_link);", body)

    def test_ready_link_has_a_liveness_fallback(self):
        source = read_bt_sources()
        start = source.index("static void ds5_bt_check_ready_link")
        end = source.index("static void ds5_bt_policy_step", start)
        body = source[start:end]

        self.assertIn("ds5_bt_connection_is_host_connected", body)
        self.assertIn("ds5_l2cap_last_activity_tick", body)
        self.assertIn("DS5_BT_READY_LINK_TIMEOUT_MS", body)
        self.assertIn("ds5_bt_disconnect_active", body)
        self.assertIn("DS5_BT_READY_LINK_POLL_MS", source)

    def test_l2cap_activity_is_updated_before_event_queueing(self):
        source = (BL616_DIR / "bluetooth" / "ds5_l2cap.c").read_text(
            encoding="utf-8"
        )
        start = source.index("static int ds5_l2cap_recv")
        end = source.index("static const struct bt_l2cap_chan_ops", start)
        body = source[start:end]

        self.assertIn("last_activity_tick", body)
        self.assertLess(
            body.index("last_activity_tick"),
            body.index("ds5_l2cap_enqueue_event"),
        )

    def test_l2cap_events_are_bound_to_connection_session(self):
        header = (BL616_DIR / "bluetooth" / "ds5_l2cap.h").read_text(
            encoding="utf-8"
        )
        source = read_bt_sources()

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
        source = read_bt_sources()
        main = (BL616_DIR / "main.c").read_text(encoding="utf-8")

        self.assertIn("void ds5_bt_policy_worker", source)
        self.assertIn("ds5_bt_start_discovery_internal()", source)
        self.assertIn("ds5_bt_connect_candidate()", source)
        self.assertNotIn("ds5_bt_start_discovery()", main)
        self.assertNotIn("ds5_bt_connect_candidate()", main)
        self.assertNotIn("RECONNECT_TIMEOUT", main)

    def test_link_generation_replays_link_scoped_microphone_state(self):
        source = read_bt_sources()

        self.assertIn("pending_microphone_state = true;", source)
        self.assertIn("pending_usb_output = false;", source)
        self.assertIn("ds5_feature_cache_clear();", source)

    def test_link_generation_discards_buffered_speaker_frames(self):
        source = read_bt_sources()
        start = source.index(
            "if (observed_link_generation != link_state.link_generation)"
        )
        end = source.index("if (!pending_feature_set", start)
        body = source[start:end]

        self.assertIn("speaker_frame_count = 0U;", body)
        self.assertIn(
            "ds5_audio_mailbox_try_receive_speaker_opus(", body
        )

    def test_discovery_stop_is_completed_without_waiting_for_callback(self):
        source = read_bt_sources()
        start = source.index("static void ds5_bt_stop_discovery_if_needed")
        end = source.index("static void ds5_bt_policy_handle_timeout")
        body = source[start:end]

        self.assertIn("bt_br_discovery_stop()", body)
        self.assertIn("discovery_in_flight = false;", body)
        self.assertIn("The pinned SDK does not invoke", body)

    def test_pairing_clear_uses_public_pinned_sdk_api(self):
        source = read_bt_sources()
        clear_start = source.index("int ds5_bt_clear_pairing(void)")
        clear_end = source.index("ds5_bt_state_t ds5_bt_get_state(void)")
        clear_body = source[clear_start:clear_end]

        self.assertIn("bt_unpair(BT_ID_DEFAULT, NULL)", clear_body)
        self.assertNotIn("ef_", clear_body)

    def test_connection_pointer_follows_vendor_sticky_ref_contract(self):
        source = read_bt_sources()
        self.assertNotIn("bt_conn_ref(", source)
        self.assertNotIn("bt_conn_unref(", source)

    def test_connection_state_machine_keeps_codecs_and_usb_outside_bluetooth(self):
        source = read_bt_sources().lower()
        for forbidden in ("cherryusb", "tinyusb", "#include \"opus.h\"", "wdl_"):
            self.assertNotIn(forbidden, source)
        self.assertIn('#include "ds5_audio_mailbox.h"', source)

    def test_feature_set_uses_bluetooth_control_l2cap(self):
        source = read_bt_sources()
        helper_start = source.index("static bool ds5_bt_send_feature_set(")
        helper_end = source.index("void ds5_bt_tx_worker", helper_start)
        helper = source[helper_start:helper_end]

        self.assertIn("ds5_build_feature_set_transaction", helper)
        self.assertIn("DS5_L2CAP_CHANNEL_CONTROL", helper)
        self.assertIn("ds5_l2cap_send", helper)
        self.assertIn("ds5_feature_set_mailbox_try_receive", source)

    def test_tx_worker_waits_for_events_when_idle(self):
        source = read_bt_sources()

        self.assertIn("void ds5_bt_tx_wake(void)", source)
        self.assertIn("ulTaskNotifyTake(pdTRUE, wait_ticks)", source)
        self.assertIn("wait_ticks = portMAX_DELAY", source)
        self.assertIn("DS5_BT_TX_RETRY_MS", source)
        self.assertNotIn("DS5_BT_TX_POLL_MS", source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
