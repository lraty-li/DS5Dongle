#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "conn.h"
#include "ds5_bt_internal.h"
#include "ds5_feature_cache.h"
#include "ds5_l2cap.h"
#include "ds5_log.h"

#define printf ds5_log_printf

static bool ds5_bt_get_br_address(const struct bt_conn *conn,
                                  const bt_addr_t **address)
{
    struct bt_conn_info info;

    if ((conn == NULL) || (address == NULL) ||
        (bt_conn_get_info(conn, &info) != 0) ||
        (info.type != BT_CONN_TYPE_BR)) {
        return false;
    }

    *address = info.br.dst;
    return true;
}

static void ds5_bt_print_connection_address(const struct bt_conn *conn,
                                            const char *prefix)
{
    const bt_addr_t *address;
    char address_string[BT_ADDR_STR_LEN];

    if (!ds5_bt_get_br_address(conn, &address)) {
        printf("DS5 BT: %s <unknown BR/EDR address>\r\n", prefix);
        return;
    }

    bt_addr_to_str(address, address_string, sizeof(address_string));
    printf("DS5 BT: %s %s\r\n", prefix, address_string);
}

static bool ds5_bt_connection_matches_candidate(const struct bt_conn *conn)
{
    const bt_addr_t *address;

    return candidate.valid &&
           ds5_bt_get_br_address(conn, &address) &&
           (bt_addr_cmp(address, &candidate.address) == 0);
}

bool ds5_bt_connection_matches_saved_peer(const struct bt_conn *conn)
{
    const bt_addr_t *address;

    return bonded_peer_valid &&
           ds5_bt_get_br_address(conn, &address) &&
           (bt_addr_cmp(address, &bonded_peer_address) == 0);
}

bool ds5_bt_connection_matches_outgoing_target(
    const struct bt_conn *conn)
{
    const bt_addr_t *address;

    return outgoing_create_pending &&
           ds5_bt_get_br_address(conn, &address) &&
           (bt_addr_cmp(address, &outgoing_target_address) == 0);
}

/*
 * The policy task and the BR callback can both observe the end of an ACL
 * create request.  Keep the ownership transition in one small critical
 * section so they cannot both install different active sessions.
 */
bool ds5_bt_claim_active_connection(struct bt_conn *conn,
                                           bool outgoing,
                                           bool bonded,
                                           bool persist_bond)
{
    bool claimed;

    if (conn == NULL) {
        return false;
    }

    ds5_bt_lifecycle_lock();
    if (active_connection == NULL) {
        active_connection = conn;
        outgoing_acl = outgoing;
        active_peer_is_bonded = bonded;
        active_peer_should_persist_bond = persist_bond;
        ++link_generation;
        ds5_l2cap_set_session(conn, link_generation);
    }
    claimed = active_connection == conn;
    ds5_bt_lifecycle_unlock();

    return claimed;
}

static void ds5_bt_remember_bonded_peer(const struct bt_conn *conn)
{
    const bt_addr_t *address;

    if (ds5_bt_get_br_address(conn, &address)) {
        bt_addr_copy(&bonded_peer_address, address);
        bonded_peer_valid = true;
    }
}

void ds5_bt_enable_bonded_page_scan(void)
{
    int err;

    if (!bonded_peer_valid) {
        page_scan_restore_pending = false;
        page_scan_retry_at = 0U;
        return;
    }

    err = bt_br_set_connectable(true);
    if ((err == 0) || (err == -EALREADY)) {
        page_scan_restore_pending = false;
        page_scan_retry_at = 0U;
        return;
    }

    page_scan_restore_pending = true;
    page_scan_retry_at = xTaskGetTickCount() +
                         pdMS_TO_TICKS(DS5_BT_PAGE_SCAN_RETRY_MS);
    printf("DS5 BT: bonded page scan enable failed (err %d); retrying\r\n",
           err);
    ds5_bt_policy_wake();
}

void ds5_bt_disable_page_scan(void)
{
    int err = bt_br_set_connectable(false);

    page_scan_restore_pending = false;
    page_scan_retry_at = 0U;

    if ((err != 0) && (err != -EALREADY)) {
        printf("DS5 BT: page scan disable failed (err %d)\r\n", err);
    }
}

int ds5_bt_forget_bonded_peer(void)
{
    bt_addr_le_t address = {
        .type = BT_ADDR_LE_PUBLIC,
    };

    if (!bonded_peer_valid) {
        return 0;
    }

    /* BR/EDR link keys are addressed as LE public addresses by bt_unpair(). */
    bt_addr_copy(&address.a, &bonded_peer_address);
    return bt_unpair(BT_ID_DEFAULT, &address);
}

static void ds5_bt_request_bond_recovery(void)
{
    if (!active_peer_is_bonded) {
        return;
    }

    bond_recovery_pending = true;
    bond_recovery_retry_at = xTaskGetTickCount();
    ds5_bt_policy_wake();
}

static bool ds5_bt_should_recover_bond(enum bt_security_err reason)
{
    /* Only an explicitly missing key proves that the stored bond is stale. */
    return reason == BT_SECURITY_ERR_PIN_OR_KEY_MISSING;
}

void ds5_bt_close_pairing_window(void)
{
    discovery_allowed = false;
    pairing_window_active = false;
    pairing_window_deadline = 0U;
    ds5_bt_policy_wake();
}

void ds5_bt_open_pairing_window(void)
{
    TickType_t now = xTaskGetTickCount();

    discovery_allowed = true;
    pairing_window_active = true;
    pairing_window_deadline = now + pdMS_TO_TICKS(DS5_BT_PAIRING_WINDOW_MS);
    discovery_retry_at = now;
    candidate_retry_at = now;
    ds5_bt_policy_wake();
}

void ds5_bt_reset_link_state(void)
{
    struct bt_conn *connection;

    ds5_bt_lifecycle_lock();
    connection = active_connection;

    /* The ACL callback may run after the SDK has already lost the link. */
    ds5_l2cap_abort_connection(connection);
    ds5_l2cap_clear_session(connection);
    active_connection = NULL;
    active_peer_is_bonded = false;
    active_peer_should_persist_bond = false;
    outgoing_create_pending = false;
    outgoing_acl = false;
    control_channel_ready = false;
    interrupt_channel_ready = false;
    calibration_response_received = false;
    feature_request_pending = false;
    feature_prefetch_index = 0U;
    initialization_pending = false;
    headset_connected = false;
    microphone_button_pressed = false;
    ds5_feature_cache_clear();
    ++link_generation;
    ds5_bt_tx_wake();
    ds5_bt_policy_wake();
    ds5_bt_lifecycle_unlock();
}

void ds5_bt_recover_after_link(bool bonded_link)
{
    memset(&candidate, 0, sizeof(candidate));
    if (bonded_link) {
        printf("DS5 BT: bonded peer link ended; restoring passive page "
               "scan\r\n");
    }
    /* The pinned SDK clears page scan after every BR terminal event. */
    ds5_bt_enable_bonded_page_scan();
    ds5_bt_open_pairing_window();
    ds5_bt_set_state(DS5_BT_STATE_IDLE);
}

int ds5_bt_disconnect_active(uint8_t reason)
{
    int err;

    ds5_bt_lifecycle_lock();
    if (active_connection == NULL) {
        err = -ENOTCONN;
        goto out;
    }

    ds5_bt_set_state(DS5_BT_STATE_DISCONNECTING);
    err = bt_conn_disconnect(active_connection, reason);
    if (err != 0) {
        printf("DS5 BT: ACL disconnect request failed (err %d)\r\n", err);
    }

out:
    ds5_bt_lifecycle_unlock();
    return err;
}

static void ds5_bt_security_ready(struct bt_conn *conn)
{
    int err;

    if ((conn != active_connection) ||
        (bt_conn_get_security(conn) < BT_SECURITY_L2) ||
        (bluetooth_state == DS5_BT_STATE_L2CAP_CONNECTING) ||
        (bluetooth_state == DS5_BT_STATE_READY)) {
        return;
    }

    ds5_bt_set_state(DS5_BT_STATE_L2CAP_CONNECTING);

    /* A reconnecting controller opens the registered HID servers itself. */
    if (!outgoing_acl) {
        return;
    }

    err = ds5_l2cap_connect(conn);
    if (err != 0) {
        printf("DS5 BT: HID L2CAP connect failed (err %d)\r\n", err);
        (void)ds5_bt_disconnect_active(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

static void ds5_bt_connected(struct bt_conn *conn, u8_t err)
{
    bool matches_candidate;
    bool matches_bonded;
    bool matches_outgoing_target;
    int security_result;

    ds5_bt_lifecycle_lock();

    if (err != 0U) {
        if (conn == active_connection) {
            bool bonded_link = active_peer_is_bonded;

            printf("DS5 BT: ACL connection failed (err %u)\r\n",
                   (unsigned int)err);
            ds5_bt_reset_link_state();
            ds5_bt_recover_after_link(bonded_link);
        } else {
            matches_outgoing_target =
                ds5_bt_connection_matches_outgoing_target(conn);
            if (matches_outgoing_target) {
                printf("DS5 BT: outgoing ACL connection failed (err %u)\r\n",
                       (unsigned int)err);
                outgoing_create_pending = false;
                if (active_connection == NULL) {
                    memset(&candidate, 0, sizeof(candidate));
                    ds5_bt_set_state(DS5_BT_STATE_IDLE);
                    ds5_bt_open_pairing_window();
                }
            }

            /* A rejected/failed BR attempt also disables SDK page scan. */
            ds5_bt_enable_bonded_page_scan();
        }
        goto out;
    }

    matches_candidate = ds5_bt_connection_matches_candidate(conn);
    matches_bonded = ds5_bt_connection_matches_saved_peer(conn);
    matches_outgoing_target =
        ds5_bt_connection_matches_outgoing_target(conn);
    if (active_connection == NULL) {
        if (!matches_candidate && !matches_bonded) {
            ds5_bt_print_connection_address(conn,
                                            "rejecting unselected peer");
            ds5_l2cap_abort_connection(conn);
            (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            ds5_bt_enable_bonded_page_scan();
            goto out;
        }

        if (!matches_candidate) {
            ds5_bt_print_connection_address(conn,
                                            "accepting saved peer");
        }

        if (!ds5_bt_claim_active_connection(
                conn, matches_outgoing_target, matches_bonded,
                matches_candidate || matches_bonded ||
                    matches_outgoing_target)) {
            ds5_bt_print_connection_address(conn,
                                            "rejecting additional peer");
            ds5_l2cap_abort_connection(conn);
            (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            ds5_bt_enable_bonded_page_scan();
            goto out;
        }

        /* A saved incoming peer wins over a still-pending other target. */
        outgoing_create_pending = false;
    } else if (active_connection != conn) {
        ds5_bt_print_connection_address(conn, "rejecting additional peer");
        ds5_l2cap_abort_connection(conn);
        (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        ds5_bt_enable_bonded_page_scan();
        goto out;
    }

    memset(&candidate, 0, sizeof(candidate));
    ds5_bt_close_pairing_window();
    ds5_bt_print_connection_address(conn, "ACL connected to");
    ds5_bt_set_state(DS5_BT_STATE_SECURING);

    security_result = bt_conn_set_security(conn, BT_SECURITY_L2);
    if (security_result != 0) {
        printf("DS5 BT: security request failed (err %d)\r\n",
               security_result);
        (void)ds5_bt_disconnect_active(BT_HCI_ERR_AUTH_FAIL);
        goto out;
    }

    /* The SDK returns 0 without another callback if L2 is already active. */
    if (bt_conn_get_security(conn) >= BT_SECURITY_L2) {
        ds5_bt_security_ready(conn);
    }

out:
    ds5_bt_lifecycle_unlock();
}

static void ds5_bt_disconnected(struct bt_conn *conn, u8_t reason)
{
    /*
     * This callback runs from the SDK HCI receive path.  Do not take the
     * application lifecycle mutex or issue another synchronous HCI command
     * here: the policy task owns link recovery.  The event queue keeps a
     * temporary reference so the policy task can safely abort stale L2CAP
     * channels after the SDK callback returns.
     */
    if (!ds5_bt_enqueue_disconnected_event(conn, reason)) {
        printf("DS5 BT: failed to queue ACL disconnect event; "
               "recovery fallback armed\r\n");
    }
}

static void ds5_bt_security_changed(struct bt_conn *conn, bt_security_t level,
                                    enum bt_security_err err)
{
    ds5_bt_lifecycle_lock();

    if (conn != active_connection) {
        goto out;
    }

    printf("DS5 BT: security changed (level %u, err %d)\r\n",
           (unsigned int)level, (int)err);

    if ((err != BT_SECURITY_ERR_SUCCESS) || (level < BT_SECURITY_L2)) {
        if (ds5_bt_should_recover_bond(err)) {
            ds5_bt_request_bond_recovery();
        }
        (void)ds5_bt_disconnect_active(BT_HCI_ERR_AUTH_FAIL);
        goto out;
    }

    if (active_peer_is_bonded) {
        bond_recovery_pending = false;
        bond_recovery_retry_at = 0U;
    }
    ds5_bt_security_ready(conn);

out:
    ds5_bt_lifecycle_unlock();
}

static struct bt_conn_cb connection_callbacks = {
    .connected = ds5_bt_connected,
    .disconnected = ds5_bt_disconnected,
    .security_changed = ds5_bt_security_changed,
};

static void ds5_bt_auth_cancel(struct bt_conn *conn)
{
    ds5_bt_lifecycle_lock();
    if (conn == active_connection) {
        printf("DS5 BT: authentication cancelled\r\n");
    }
    ds5_bt_lifecycle_unlock();
}

static void ds5_bt_auth_pairing_confirm(struct bt_conn *conn)
{
    int err;

    ds5_bt_lifecycle_lock();
    if (conn != active_connection) {
        (void)bt_conn_auth_cancel(conn);
        goto out;
    }

    err = bt_conn_auth_pairing_confirm(conn);
    if (err != 0) {
        printf("DS5 BT: pairing confirmation failed (err %d)\r\n", err);
    }

out:
    ds5_bt_lifecycle_unlock();
}

static void ds5_bt_auth_pincode_entry(struct bt_conn *conn, bool highsec)
{
    int err;

    ds5_bt_lifecycle_lock();
    if ((conn != active_connection) || highsec) {
        (void)bt_conn_auth_cancel(conn);
        goto out;
    }

    /* Preserve the original project's legacy-controller fallback. */
    err = bt_conn_auth_pincode_entry(conn, "0000");
    if (err != 0) {
        printf("DS5 BT: legacy PIN response failed (err %d)\r\n", err);
    }

out:
    ds5_bt_lifecycle_unlock();
}

static void ds5_bt_pairing_complete(struct bt_conn *conn, bool bonded)
{
    ds5_bt_lifecycle_lock();
    if (conn == active_connection) {
        if (bonded && active_peer_should_persist_bond) {
            ds5_bt_remember_bonded_peer(conn);
            active_peer_is_bonded = true;
            bond_recovery_pending = false;
            bond_recovery_retry_at = 0U;
        }
        printf("DS5 BT: pairing complete (bonded %u)\r\n",
               bonded ? 1U : 0U);
    }
    ds5_bt_lifecycle_unlock();
}

static void ds5_bt_pairing_failed(struct bt_conn *conn,
                                  enum bt_security_err reason)
{
    ds5_bt_lifecycle_lock();
    if (conn == active_connection) {
        printf("DS5 BT: pairing failed (reason %d)\r\n", (int)reason);
        if (ds5_bt_should_recover_bond(reason)) {
            ds5_bt_request_bond_recovery();
        }
    }
    ds5_bt_lifecycle_unlock();
}

static const struct bt_conn_auth_cb authentication_callbacks = {
    .cancel = ds5_bt_auth_cancel,
    .pairing_confirm = ds5_bt_auth_pairing_confirm,
    .pincode_entry = ds5_bt_auth_pincode_entry,
    .pairing_complete = ds5_bt_pairing_complete,
    .pairing_failed = ds5_bt_pairing_failed,
};

int ds5_bt_register_callbacks(void)
{
    bt_conn_cb_register(&connection_callbacks);
    return bt_conn_auth_cb_register(&authentication_callbacks);
}
