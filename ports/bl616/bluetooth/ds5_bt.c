#include "ds5_bt.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include "portmacro.h"
#include "task.h"

#include "bluetooth.h"
#include "btble_lib_api.h"
#include "conn.h"
#include "hci_driver.h"
#include "hci_err.h"

#include "ds5_bt_policy.h"
#include "ds5_audio_mailbox.h"
#include "ds5_feature_cache.h"
#include "ds5_feature_set_mailbox.h"
#include "ds5_haptics_mailbox.h"
#include "ds5_l2cap.h"
#include "ds5_input_mailbox.h"
#include "ds5_output_mailbox.h"
#include "ds5_protocol.h"
#include "ds5_log.h"

#define printf ds5_log_printf

#define DS5_BT_DISCOVERY_RESULT_COUNT 10U
#define DS5_BT_DISCOVERY_LENGTH       0x05U
#define DS5_BT_CANDIDATE_NAME_SIZE    64U
#define DS5_BT_WORKER_STACK_DEPTH     (configMINIMAL_STACK_SIZE * 4U)
#define DS5_BT_TX_WORKER_STACK_DEPTH  (configMINIMAL_STACK_SIZE * 6U)
#define DS5_BT_POLICY_STACK_DEPTH     (configMINIMAL_STACK_SIZE * 3U)
#define DS5_BT_OUTPUT_READY_TIMEOUT_MS 5000U
#define DS5_BT_TX_RETRY_MS             1U
#define DS5_BT_DEFAULT_MIC_SELECT      0U
#define DS5_BT_PAIRING_WINDOW_MS      20000U
#define DS5_BT_DISCOVERY_RETRY_MS     750U
#define DS5_BT_ACL_TIMEOUT_MS          10000U
#define DS5_BT_SECURITY_TIMEOUT_MS     5000U
#define DS5_BT_L2CAP_TIMEOUT_MS        5000U
#define DS5_BT_DISCONNECT_TIMEOUT_MS   5000U

static const uint8_t feature_prefetch_ids[] = {
    0x09U,
    0x20U,
    0x22U,
    0x05U,
};

typedef struct {
    bool valid;
    bt_addr_t address;
    uint32_t device_class;
    int8_t rssi;
    uint16_t score;
    char name[DS5_BT_CANDIDATE_NAME_SIZE];
} ds5_bt_candidate_t;

static struct bt_br_discovery_result
    discovery_results[DS5_BT_DISCOVERY_RESULT_COUNT];
static ds5_bt_candidate_t candidate;
/*
 * Policy has three independent pieces of state:
 *
 *   - bonded_peer_* is persistent identity policy restored from link keys;
 *   - candidate is only the result of one active inquiry;
 *   - active_connection/link_generation is the current ACL session.
 *
 * In particular, a passive page scan for the bonded peer is not represented
 * by a blocking state such as RECONNECT_WAIT.  Active inquiry is a separate,
 * time-limited pairing window.
 */
static bt_addr_t bonded_peer_address;
static volatile bool bonded_peer_valid;

/*
 * BouffaloSDK v2.3.30 keeps the locally-created BR connection's sticky
 * reference until its failure/disconnect notification completes. This is a
 * borrowed pointer and must be cleared in those callbacks, not unreferenced by
 * the application (matching bredr_cli_cmds.c in this SDK).
 */
static struct bt_conn *active_connection;
static bool active_peer_is_bonded;
static bool active_peer_should_persist_bond;
static bool outgoing_create_pending;
static bt_addr_t outgoing_target_address;

static volatile ds5_bt_state_t bluetooth_state = DS5_BT_STATE_OFF;
static bool outgoing_acl;
static bool control_channel_ready;
static bool interrupt_channel_ready;
static bool calibration_response_received;
static bool feature_request_pending;
static size_t feature_prefetch_index;
static volatile uint32_t received_l2cap_packets;
static uint32_t received_control_packets;
static uint32_t received_interrupt_packets;
static uint32_t valid_input_reports;
static uint32_t invalid_input_reports;
static uint32_t input_mailbox_publish_failures;
static uint32_t transmitted_output_reports;
static uint32_t failed_output_reports;
static uint32_t transmitted_haptics_reports;
static uint32_t failed_haptics_reports;
static uint32_t discarded_haptics_not_ready;
static uint32_t received_microphone_packets;
static uint32_t rejected_microphone_packets;
static uint32_t transmitted_microphone_status_reports;
static uint32_t failed_microphone_status_reports;
static uint32_t received_microphone_button_presses;
static uint32_t dropped_microphone_button_presses;
static uint32_t transmitted_microphone_mute_reports;
static uint32_t failed_microphone_mute_reports;
static uint32_t transmitted_feature_set_reports;
static uint32_t failed_feature_set_reports;
static uint8_t latest_usb_input_payload[DS5_USB_INPUT_PAYLOAD_SIZE];
static uint32_t failed_initialization_reports;
static volatile bool initialization_pending;
static ds5_output_sequence_t output_sequence;
static uint8_t haptics_packet_counter;
static bool headset_connected;
static bool microphone_button_pressed;
static volatile uint32_t link_generation;
static volatile bool discovery_in_flight;
static volatile bool discovery_allowed;
static volatile bool discovery_stop_requested;
static volatile bool pairing_window_active;
static TickType_t pairing_window_deadline;
static TickType_t discovery_retry_at;
static TickType_t candidate_retry_at;
static TickType_t link_deadline;
static TickType_t bond_recovery_retry_at;
static uint32_t discovery_session;
static volatile bool bond_recovery_pending;

static const struct bt_br_discovery_param discovery_param = {
    .length = DS5_BT_DISCOVERY_LENGTH,
    .limited = false,
};

static StaticTask_t worker_task_storage;
static StackType_t worker_task_stack[DS5_BT_WORKER_STACK_DEPTH];
static TaskHandle_t worker_task;
static StaticTask_t tx_worker_task_storage;
static StackType_t tx_worker_task_stack[DS5_BT_TX_WORKER_STACK_DEPTH];
static TaskHandle_t tx_worker_task;
static StaticTask_t policy_task_storage;
static StackType_t policy_task_stack[DS5_BT_POLICY_STACK_DEPTH];
static TaskHandle_t policy_task;

static void ds5_bt_policy_wake(void);
static void ds5_bt_open_pairing_window(void);
static void ds5_bt_recover_after_link(bool bonded_link);
static bool ds5_bt_deadline_expired(TickType_t now, TickType_t deadline);

void ds5_bt_tx_wake(void)
{
    if (tx_worker_task == NULL) {
        return;
    }

    if (xPortIsInsideInterrupt()) {
        BaseType_t task_woken = pdFALSE;

        vTaskNotifyGiveFromISR(tx_worker_task, &task_woken);
        portYIELD_FROM_ISR(task_woken);
    } else {
        xTaskNotifyGive(tx_worker_task);
    }
}

static void ds5_bt_policy_wake(void)
{
    if (policy_task == NULL) {
        return;
    }

    if (xPortIsInsideInterrupt()) {
        BaseType_t task_woken = pdFALSE;

        vTaskNotifyGiveFromISR(policy_task, &task_woken);
        portYIELD_FROM_ISR(task_woken);
    } else {
        xTaskNotifyGive(policy_task);
    }
}

static uint16_t ds5_bt_stack_high_water_words(TaskHandle_t task)
{
    UBaseType_t words;

    if (task == NULL) {
        return 0U;
    }

    words = uxTaskGetStackHighWaterMark(task);

    return words > UINT16_MAX ? UINT16_MAX : (uint16_t)words;
}

static const char *ds5_bt_state_name(ds5_bt_state_t state)
{
    switch (state) {
    case DS5_BT_STATE_OFF:
        return "off";
    case DS5_BT_STATE_DISCOVERING:
        return "discovering";
    case DS5_BT_STATE_IDLE:
        return "idle";
    case DS5_BT_STATE_CANDIDATE_READY:
        return "candidate-ready";
    case DS5_BT_STATE_ACL_CONNECTING:
        return "acl-connecting";
    case DS5_BT_STATE_SECURING:
        return "securing";
    case DS5_BT_STATE_L2CAP_CONNECTING:
        return "l2cap-connecting";
    case DS5_BT_STATE_READY:
        return "ready";
    case DS5_BT_STATE_DISCONNECTING:
        return "disconnecting";
    default:
        return "unknown";
    }
}

static void ds5_bt_set_state(ds5_bt_state_t state)
{
    ds5_bt_state_t previous = bluetooth_state;

    if (previous == state) {
        return;
    }

    bluetooth_state = state;
    link_deadline = 0U;
    switch (state) {
    case DS5_BT_STATE_ACL_CONNECTING:
        link_deadline = xTaskGetTickCount() +
                        pdMS_TO_TICKS(DS5_BT_ACL_TIMEOUT_MS);
        break;
    case DS5_BT_STATE_SECURING:
        link_deadline = xTaskGetTickCount() +
                        pdMS_TO_TICKS(DS5_BT_SECURITY_TIMEOUT_MS);
        break;
    case DS5_BT_STATE_L2CAP_CONNECTING:
        link_deadline = xTaskGetTickCount() +
                        pdMS_TO_TICKS(DS5_BT_L2CAP_TIMEOUT_MS);
        break;
    case DS5_BT_STATE_DISCONNECTING:
        link_deadline = xTaskGetTickCount() +
                        pdMS_TO_TICKS(DS5_BT_DISCONNECT_TIMEOUT_MS);
        break;
    default:
        break;
    }
    printf("DS5 BT: state %s -> %s\r\n",
           ds5_bt_state_name(previous), ds5_bt_state_name(state));
    ds5_bt_policy_wake();
}

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

static bool ds5_bt_connection_matches_saved_peer(const struct bt_conn *conn)
{
    const bt_addr_t *address;

    return bonded_peer_valid &&
           ds5_bt_get_br_address(conn, &address) &&
           (bt_addr_cmp(address, &bonded_peer_address) == 0);
}

static bool ds5_bt_connection_matches_outgoing_target(
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
static bool ds5_bt_claim_active_connection(struct bt_conn *conn,
                                           bool outgoing,
                                           bool bonded,
                                           bool persist_bond)
{
    bool claimed;

    if (conn == NULL) {
        return false;
    }

    taskENTER_CRITICAL();
    if (active_connection == NULL) {
        active_connection = conn;
        outgoing_acl = outgoing;
        active_peer_is_bonded = bonded;
        active_peer_should_persist_bond = persist_bond;
        ++link_generation;
        ds5_l2cap_set_session(conn, link_generation);
    }
    claimed = active_connection == conn;
    taskEXIT_CRITICAL();

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

static void ds5_bt_enable_bonded_page_scan(void)
{
    int err;

    if (!bonded_peer_valid) {
        return;
    }

    err = bt_br_set_connectable(true);
    if ((err != 0) && (err != -EALREADY)) {
        printf("DS5 BT: bonded page scan enable failed (err %d)\r\n", err);
    }
}

static void ds5_bt_disable_page_scan(void)
{
    int err = bt_br_set_connectable(false);

    if ((err != 0) && (err != -EALREADY)) {
        printf("DS5 BT: page scan disable failed (err %d)\r\n", err);
    }
}

static int ds5_bt_forget_bonded_peer(void)
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

static void ds5_bt_close_pairing_window(void)
{
    discovery_allowed = false;
    pairing_window_active = false;
    pairing_window_deadline = 0U;
    ds5_bt_policy_wake();
}

static void ds5_bt_open_pairing_window(void)
{
    TickType_t now = xTaskGetTickCount();

    discovery_allowed = true;
    pairing_window_active = true;
    pairing_window_deadline = now + pdMS_TO_TICKS(DS5_BT_PAIRING_WINDOW_MS);
    discovery_retry_at = now;
    candidate_retry_at = now;
    ds5_bt_policy_wake();
}

static void ds5_bt_reset_link_state(void)
{
    struct bt_conn *connection = active_connection;

    /* Tear down any old channels before a new ACL can reuse their objects. */
    (void)ds5_l2cap_disconnect();
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
    ds5_output_sequence_reset(&output_sequence, 0U);
    haptics_packet_counter = 0U;
    headset_connected = false;
    microphone_button_pressed = false;
    ds5_feature_cache_clear();
    ++link_generation;
    ds5_bt_tx_wake();
    ds5_bt_policy_wake();
}

static void ds5_bt_recover_after_link(bool bonded_link)
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

static int ds5_bt_disconnect_active(uint8_t reason)
{
    int err;

    if (active_connection == NULL) {
        return -ENOTCONN;
    }

    ds5_bt_set_state(DS5_BT_STATE_DISCONNECTING);
    err = bt_conn_disconnect(active_connection, reason);
    if (err != 0) {
        printf("DS5 BT: ACL disconnect request failed (err %d)\r\n", err);
    }

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
        return;
    }

    matches_candidate = ds5_bt_connection_matches_candidate(conn);
    matches_bonded = ds5_bt_connection_matches_saved_peer(conn);
    matches_outgoing_target =
        ds5_bt_connection_matches_outgoing_target(conn);
    if (active_connection == NULL) {
        if (!matches_candidate && !matches_bonded) {
            ds5_bt_print_connection_address(conn,
                                            "rejecting unselected peer");
            (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            ds5_bt_enable_bonded_page_scan();
            return;
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
            (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            ds5_bt_enable_bonded_page_scan();
            return;
        }

        /* A saved incoming peer wins over a still-pending other target. */
        outgoing_create_pending = false;
    } else if (active_connection != conn) {
        ds5_bt_print_connection_address(conn, "rejecting additional peer");
        (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        ds5_bt_enable_bonded_page_scan();
        return;
    }

    memset(&candidate, 0, sizeof(candidate));
    ds5_bt_close_pairing_window();
    ds5_bt_print_connection_address(conn, "ACL connected to");
    ds5_bt_set_state(DS5_BT_STATE_SECURING);

    security_result = bt_conn_set_security(conn, BT_SECURITY_L2);
    if (security_result != 0) {
        printf("DS5 BT: security request failed (err %d)\r\n",
               security_result);
        ds5_bt_request_bond_recovery();
        (void)ds5_bt_disconnect_active(BT_HCI_ERR_AUTH_FAIL);
        return;
    }

    /* The SDK returns 0 without another callback if L2 is already active. */
    if (bt_conn_get_security(conn) >= BT_SECURITY_L2) {
        ds5_bt_security_ready(conn);
    }
}

static void ds5_bt_disconnected(struct bt_conn *conn, u8_t reason)
{
    bool bonded_link;

    if (conn != active_connection) {
        if (ds5_bt_connection_matches_outgoing_target(conn)) {
            outgoing_create_pending = false;
            if (active_connection == NULL) {
                memset(&candidate, 0, sizeof(candidate));
                ds5_bt_set_state(DS5_BT_STATE_IDLE);
                ds5_bt_open_pairing_window();
            }
        }

        /* The SDK clears page scan for every BR disconnection, including
         * rejected/short-lived peers that never became active_connection. */
        ds5_bt_enable_bonded_page_scan();
        return;
    }

    printf("DS5 BT: ACL disconnected (reason 0x%02x)\r\n",
           (unsigned int)reason);
    bonded_link = active_peer_is_bonded;
    ds5_bt_reset_link_state();
    ds5_bt_recover_after_link(bonded_link);
}

static void ds5_bt_security_changed(struct bt_conn *conn, bt_security_t level,
                                    enum bt_security_err err)
{
    if (conn != active_connection) {
        return;
    }

    printf("DS5 BT: security changed (level %u, err %d)\r\n",
           (unsigned int)level, (int)err);

    if ((err != BT_SECURITY_ERR_SUCCESS) || (level < BT_SECURITY_L2)) {
        ds5_bt_request_bond_recovery();
        (void)ds5_bt_disconnect_active(BT_HCI_ERR_AUTH_FAIL);
        return;
    }

    if (active_peer_is_bonded) {
        bond_recovery_pending = false;
        bond_recovery_retry_at = 0U;
    }
    ds5_bt_security_ready(conn);
}

static struct bt_conn_cb connection_callbacks = {
    .connected = ds5_bt_connected,
    .disconnected = ds5_bt_disconnected,
    .security_changed = ds5_bt_security_changed,
};

static void ds5_bt_auth_cancel(struct bt_conn *conn)
{
    if (conn == active_connection) {
        printf("DS5 BT: authentication cancelled\r\n");
    }
}

static void ds5_bt_auth_pairing_confirm(struct bt_conn *conn)
{
    int err;

    if (conn != active_connection) {
        (void)bt_conn_auth_cancel(conn);
        return;
    }

    err = bt_conn_auth_pairing_confirm(conn);
    if (err != 0) {
        printf("DS5 BT: pairing confirmation failed (err %d)\r\n", err);
    }
}

static void ds5_bt_auth_pincode_entry(struct bt_conn *conn, bool highsec)
{
    int err;

    if ((conn != active_connection) || highsec) {
        (void)bt_conn_auth_cancel(conn);
        return;
    }

    /* Preserve the original project's legacy-controller fallback. */
    err = bt_conn_auth_pincode_entry(conn, "0000");
    if (err != 0) {
        printf("DS5 BT: legacy PIN response failed (err %d)\r\n", err);
    }
}

static void ds5_bt_pairing_complete(struct bt_conn *conn, bool bonded)
{
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
}

static void ds5_bt_pairing_failed(struct bt_conn *conn,
                                  enum bt_security_err reason)
{
    if (conn == active_connection) {
        printf("DS5 BT: pairing failed (reason %d)\r\n", (int)reason);
        ds5_bt_request_bond_recovery();
    }
}

static const struct bt_conn_auth_cb authentication_callbacks = {
    .cancel = ds5_bt_auth_cancel,
    .pairing_confirm = ds5_bt_auth_pairing_confirm,
    .pincode_entry = ds5_bt_auth_pincode_entry,
    .pairing_complete = ds5_bt_pairing_complete,
    .pairing_failed = ds5_bt_pairing_failed,
};

static uint32_t ds5_bt_result_device_class(
    const struct bt_br_discovery_result *result)
{
    return (uint32_t)result->cod[0] |
           ((uint32_t)result->cod[1] << 8U) |
           ((uint32_t)result->cod[2] << 16U);
}

static void ds5_bt_consider_candidate(
    const struct bt_br_discovery_result *result,
    uint32_t device_class, const char *name)
{
    bool saved_address_match = bonded_peer_valid &&
        (bt_addr_cmp(&result->addr, &bonded_peer_address) == 0);
    uint16_t score = ds5_bt_policy_candidate_score(
        device_class, name, saved_address_match);

    if (score == 0U) {
        return;
    }

    if (candidate.valid &&
        ((score < candidate.score) ||
         ((score == candidate.score) && (result->rssi <= candidate.rssi)))) {
        return;
    }

    candidate.valid = true;
    bt_addr_copy(&candidate.address, &result->addr);
    candidate.device_class = device_class;
    candidate.rssi = result->rssi;
    candidate.score = score;
    if (name != NULL) {
        strncpy(candidate.name, name, sizeof(candidate.name) - 1U);
        candidate.name[sizeof(candidate.name) - 1U] = '\0';
    } else {
        candidate.name[0] = '\0';
    }
}

static void ds5_bt_restore_bond(const struct bt_br_bond_info *info,
                                void *user_data)
{
    size_t *bond_count = user_data;

    if ((info == NULL) || (info->addr == NULL) || (bond_count == NULL)) {
        return;
    }

    ++(*bond_count);
    if (!bonded_peer_valid) {
        bt_addr_copy(&bonded_peer_address, info->addr);
        bonded_peer_valid = true;
    }
}

static void ds5_bt_discovery_complete(struct bt_br_discovery_result *results,
                                      size_t count)
{
    size_t index;

    discovery_in_flight = false;
    discovery_stop_requested = false;
    discovery_retry_at = xTaskGetTickCount() +
                         pdMS_TO_TICKS(DS5_BT_DISCOVERY_RETRY_MS);

    /* A late inquiry completion must never overwrite an active link. */
    if ((active_connection != NULL) || !discovery_allowed ||
        (bluetooth_state != DS5_BT_STATE_DISCOVERING)) {
        memset(&candidate, 0, sizeof(candidate));
        printf("DS5 BT: discovery completion ignored for current link/policy\r\n");
        if (bluetooth_state == DS5_BT_STATE_DISCOVERING) {
            ds5_bt_set_state(DS5_BT_STATE_IDLE);
        }
        ds5_bt_policy_wake();
        return;
    }

    memset(&candidate, 0, sizeof(candidate));
    printf("DS5 BT: discovery complete (%u result(s))\r\n",
           (unsigned int)count);

    for (index = 0U; (results != NULL) && (index < count); ++index) {
        char address[BT_ADDR_STR_LEN];
        char name[DS5_BT_CANDIDATE_NAME_SIZE];
        uint32_t device_class = ds5_bt_result_device_class(&results[index]);
        ds5_bt_eir_name_result_t name_result;

        bt_addr_to_str(&results[index].addr, address, sizeof(address));
        name_result = ds5_bt_policy_extract_eir_name(
            results[index].eir, sizeof(results[index].eir),
            name, sizeof(name));

        printf("  addr: %s, rssi: %d, class: 0x%06lx",
               address, (int)results[index].rssi,
               (unsigned long)device_class);
        if (name_result == DS5_BT_EIR_NAME_FOUND) {
            printf(", name: %s", name);
        } else if (name_result == DS5_BT_EIR_NAME_MALFORMED) {
            printf(", name: <malformed EIR>");
        }
        printf("\r\n");

        ds5_bt_consider_candidate(&results[index], device_class,
                                  name_result == DS5_BT_EIR_NAME_FOUND
                                      ? name
                                      : NULL);
    }

    if (candidate.valid) {
        char address[BT_ADDR_STR_LEN];

        bt_addr_to_str(&candidate.address, address, sizeof(address));
        printf("DS5 BT: selected explicit-connect candidate %s "
               "(score %u, class 0x%06lx, name %s)\r\n",
               address, (unsigned int)candidate.score,
               (unsigned long)candidate.device_class,
               candidate.name[0] != '\0' ? candidate.name : "<unknown>");
        ds5_bt_set_state(DS5_BT_STATE_CANDIDATE_READY);
    } else {
        printf("DS5 BT: no gamepad-class candidate found\r\n");
        ds5_bt_set_state(DS5_BT_STATE_IDLE);
    }
    ds5_bt_policy_wake();
}

static int ds5_bt_request_next_feature(void)
{
    uint8_t transaction[DS5_FEATURE_GET_TRANSACTION_SIZE];
    uint8_t report_id;
    size_t transaction_length;
    ds5_protocol_result_t protocol_result;
    int err;

    if (!control_channel_ready || feature_request_pending ||
        (feature_prefetch_index >=
         (sizeof(feature_prefetch_ids) / sizeof(feature_prefetch_ids[0])))) {
        return 0;
    }

    report_id = feature_prefetch_ids[feature_prefetch_index];
    protocol_result = ds5_build_feature_get_transaction(
        report_id,
        transaction, sizeof(transaction), &transaction_length);
    if (protocol_result != DS5_PROTOCOL_OK) {
        return (int)protocol_result;
    }

    err = ds5_l2cap_send(DS5_L2CAP_CHANNEL_CONTROL,
                         transaction, transaction_length);
    if (err < 0) {
        return err;
    }

    feature_request_pending = true;
    printf("DS5 BT: Feature 0x%02x requested (%u/%u)\r\n",
           (unsigned int)report_id,
           (unsigned int)(feature_prefetch_index + 1U),
           (unsigned int)(sizeof(feature_prefetch_ids) /
                          sizeof(feature_prefetch_ids[0])));
    return 0;
}

static void ds5_bt_process_l2cap_event(const ds5_l2cap_event_t *event)
{
    ds5_protocol_result_t protocol_result;

    if ((event == NULL) || (event->conn != active_connection) ||
        (event->session != link_generation)) {
        return;
    }

    switch (event->type) {
    case DS5_L2CAP_EVENT_CONNECTED:
        if (event->channel == DS5_L2CAP_CHANNEL_CONTROL) {
            control_channel_ready = true;
            printf("DS5 BT: HID Control L2CAP ready\r\n");
        } else {
            interrupt_channel_ready = true;
            printf("DS5 BT: HID Interrupt L2CAP ready\r\n");
        }

        if (control_channel_ready && interrupt_channel_ready &&
            (active_connection != NULL)) {
            int request_err = ds5_bt_request_next_feature();

            if (request_err != 0) {
                printf("DS5 BT: Feature prefetch request failed "
                       "(err %d)\r\n",
                       request_err);
            }
            initialization_pending = true;
            ds5_bt_set_state(DS5_BT_STATE_READY);
        }
        ds5_bt_tx_wake();
        break;
    case DS5_L2CAP_EVENT_DISCONNECTED:
        if (event->channel == DS5_L2CAP_CHANNEL_CONTROL) {
            control_channel_ready = false;
        } else {
            interrupt_channel_ready = false;
        }

        if ((active_connection != NULL) &&
            (bluetooth_state != DS5_BT_STATE_DISCONNECTING)) {
            (void)ds5_bt_disconnect_active(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
        ds5_bt_tx_wake();
        break;
    case DS5_L2CAP_EVENT_DATA:
        ++received_l2cap_packets;
        if (event->channel == DS5_L2CAP_CHANNEL_CONTROL) {
            uint8_t report_id;

            ++received_control_packets;
            if ((event->length >= 2U) &&
                (event->data[0] == DS5_FEATURE_DATA_HEADER)) {
                report_id = event->data[1];
                if (ds5_feature_cache_store(report_id, &event->data[2],
                                            event->length - 2U)) {
                    printf("DS5 BT: Feature 0x%02x cached (length %u)\r\n",
                           (unsigned int)report_id,
                           (unsigned int)(event->length - 2U));
                }

                if ((report_id == DS5_FEATURE_CALIBRATION_REPORT_ID) &&
                    !calibration_response_received) {
                    printf("DS5 BT: calibration Feature 0x05 response "
                           "received (length %u)\r\n",
                           (unsigned int)event->length);
                    calibration_response_received = true;
                }

                if (feature_request_pending &&
                    (feature_prefetch_index <
                     (sizeof(feature_prefetch_ids) /
                      sizeof(feature_prefetch_ids[0]))) &&
                    (report_id == feature_prefetch_ids[feature_prefetch_index])) {
                    int request_err;

                    feature_request_pending = false;
                    ++feature_prefetch_index;
                    request_err = ds5_bt_request_next_feature();
                    if (request_err != 0) {
                        printf("DS5 BT: Feature prefetch request failed "
                               "(err %d)\r\n",
                               request_err);
                    }
                }
            } else if (received_control_packets <= 4U) {
                if (event->length >= 2U) {
                    printf("DS5 BT: unhandled HID Control packet "
                           "(length %u, prefix %02x %02x)\r\n",
                           (unsigned int)event->length,
                           (unsigned int)event->data[0],
                           (unsigned int)event->data[1]);
                } else {
                    printf("DS5 BT: short HID Control packet "
                           "(length %u)\r\n",
                           (unsigned int)event->length);
                }
            }
            break;
        }

        ++received_interrupt_packets;
        if (received_interrupt_packets == 1U) {
            if (event->length >= 3U) {
                printf("DS5 BT: first HID Interrupt packet length %u, "
                       "prefix %02x %02x %02x\r\n",
                       (unsigned int)event->length,
                       (unsigned int)event->data[0],
                       (unsigned int)event->data[1],
                       (unsigned int)event->data[2]);
            } else {
                printf("DS5 BT: first HID Interrupt packet is short "
                       "(length %u)\r\n",
                       (unsigned int)event->length);
            }
        }

        /*
         * The controller marks an Opus microphone payload in byte 2 of its
         * 0x31 Interrupt report.  It starts at byte 4, not at the normal USB
         * HID input payload offset.  Keep decode work out of the Bluetooth
         * worker by handing the fixed-size frame to the audio task.
         */
        if ((event->length >= 3U) &&
            (event->data[0] == DS5_BT_INPUT_TRANSACTION_HEADER) &&
            (event->data[1] == DS5_BT_INPUT_REPORT_ID) &&
            ((event->data[2] & 0x02U) != 0U)) {
            if (event->length < (4U + DS5_AUDIO_MIC_OPUS_SIZE)) {
                ++rejected_microphone_packets;
                if (rejected_microphone_packets <= 4U) {
                    printf("DS5 BT: short microphone packet (length %u)\r\n",
                           (unsigned int)event->length);
                }
            } else if (!ds5_audio_mailbox_publish_microphone_opus(
                           &event->data[4], DS5_AUDIO_MIC_OPUS_SIZE)) {
                ++rejected_microphone_packets;
                if (rejected_microphone_packets <= 4U) {
                    printf("DS5 BT: microphone audio mailbox full\r\n");
                }
            } else {
                ++received_microphone_packets;
                if (received_microphone_packets == 1U) {
                    printf("DS5 BT: first microphone Opus frame queued\r\n");
                }
            }
            break;
        }

        protocol_result = ds5_extract_usb_input_payload(
            event->data, event->length, latest_usb_input_payload,
            sizeof(latest_usb_input_payload));
        if (protocol_result == DS5_PROTOCOL_OK) {
            ++valid_input_reports;
            if (!ds5_input_mailbox_publish(latest_usb_input_payload,
                                           sizeof(latest_usb_input_payload))) {
                ++input_mailbox_publish_failures;
                if (input_mailbox_publish_failures <= 4U) {
                    printf("DS5 BT: input mailbox publish failed (%lu)\r\n",
                           (unsigned long)input_mailbox_publish_failures);
                }
            }
            if (valid_input_reports == 1U) {
                printf("DS5 BT: first valid 0x31 input report accepted "
                       "(length %u)\r\n",
                       (unsigned int)event->length);
            }

            /* Original src/main.cpp uses report byte 53 bit 0 for routing. */
            headset_connected =
                (latest_usb_input_payload[53U] & 0x01U) != 0U;

            /*
             * A DualSense reports the microphone button as a momentary HID
             * input.  The host must toggle both the hardware mute state and
             * its yellow LED on the rising edge; the controller does not do
             * that state machine for us while bridged over Bluetooth.
             */
            {
                bool pressed =
                    (latest_usb_input_payload[
                         DS5_USB_INPUT_BUTTONS2_OFFSET] &
                     DS5_USB_INPUT_MIC_BUTTON_MASK) != 0U;

                if (pressed && !microphone_button_pressed) {
                    if (ds5_output_mailbox_publish_microphone_button_press()) {
                        ++received_microphone_button_presses;
                        ds5_bt_tx_wake();
                    } else {
                        ++dropped_microphone_button_presses;
                        if (dropped_microphone_button_presses <= 4U) {
                            printf("DS5 BT: microphone button event queue "
                                   "full\r\n");
                        }
                    }
                }
                microphone_button_pressed = pressed;
            }

        } else {
            ++invalid_input_reports;
            if (invalid_input_reports <= 4U) {
                printf("DS5 BT: rejected HID Interrupt packet "
                       "(result %d, length %u)\r\n",
                       (int)protocol_result,
                       (unsigned int)event->length);
            }
        }
        break;
    case DS5_L2CAP_EVENT_ERROR:
        printf("DS5 BT: L2CAP channel %u failed (err %d)\r\n",
               (unsigned int)event->channel, event->status);
        if (active_connection != NULL) {
            (void)ds5_bt_disconnect_active(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
        ds5_bt_tx_wake();
        break;
    default:
        break;
    }
}

static void ds5_bt_worker(void *parameter)
{
    ds5_l2cap_event_t event;

    (void)parameter;

    while (1) {
        if (ds5_l2cap_event_receive(&event)) {
            ds5_bt_process_l2cap_event(&event);
        }
    }
}

static bool ds5_bt_interrupt_ready(void)
{
    return (bluetooth_state == DS5_BT_STATE_READY) &&
           interrupt_channel_ready;
}

static bool ds5_bt_forward_usb_output(
    const uint8_t *usb_report,
    uint8_t *bt_transaction,
    size_t bt_transaction_capacity)
{
    size_t transaction_length;
    ds5_protocol_result_t protocol_result;
    ds5_output_sequence_t previous_sequence = output_sequence;
    int send_result;

    if (usb_report == NULL) {
        ++failed_output_reports;
        return false;
    }

    protocol_result = ds5_build_bt_output_transaction(
        &output_sequence, usb_report, DS5_USB_OUTPUT_REPORT_SIZE,
        bt_transaction, bt_transaction_capacity, &transaction_length);
    if (protocol_result != DS5_PROTOCOL_OK) {
        ++failed_output_reports;
        if (failed_output_reports <= 4U) {
            printf("DS5 BT: USB output report rejected (result %d)\r\n",
                   (int)protocol_result);
        }
        return false;
    }

    send_result = ds5_l2cap_send(DS5_L2CAP_CHANNEL_INTERRUPT,
                                 bt_transaction, transaction_length);
    if (send_result < 0) {
        output_sequence = previous_sequence;
        ++failed_output_reports;
        if (failed_output_reports <= 4U) {
            printf("DS5 BT: HID output send failed (err %d)\r\n",
                   send_result);
        }
        return false;
    }

    ++transmitted_output_reports;
    if (transmitted_output_reports == 1U) {
        printf("DS5 BT: first USB output report forwarded\r\n");
    }

    return true;
}

static bool ds5_bt_send_microphone_mute(
    bool muted,
    uint8_t *usb_report,
    size_t usb_report_capacity,
    uint8_t *bt_transaction,
    size_t bt_transaction_capacity)
{
    ds5_protocol_result_t protocol_result;

    protocol_result = ds5_build_usb_microphone_mute_report(
        muted, usb_report, usb_report_capacity);
    if (protocol_result != DS5_PROTOCOL_OK) {
        ++failed_microphone_mute_reports;
        return false;
    }

    if (!ds5_bt_forward_usb_output(usb_report, bt_transaction,
                                   bt_transaction_capacity)) {
        ++failed_microphone_mute_reports;
        return false;
    }

    ++transmitted_microphone_mute_reports;
    return true;
}

static bool ds5_bt_send_initialization(uint8_t *bt_transaction,
                                       size_t bt_transaction_capacity)
{
    size_t transaction_length;
    ds5_protocol_result_t protocol_result;
    int send_result;

    protocol_result = ds5_build_bt_initialization_transaction(
        DS5_BT_DEFAULT_MIC_SELECT, bt_transaction, bt_transaction_capacity,
        &transaction_length);
    if (protocol_result != DS5_PROTOCOL_OK) {
        ++failed_initialization_reports;
        printf("DS5 BT: initialization report build failed (result %d)\r\n",
               (int)protocol_result);
        return false;
    }

    send_result = ds5_l2cap_send(DS5_L2CAP_CHANNEL_INTERRUPT,
                                 bt_transaction, transaction_length);
    if (send_result < 0) {
        ++failed_initialization_reports;
        if (failed_initialization_reports <= 4U) {
            printf("DS5 BT: initialization report send failed (err %d)\r\n",
                   send_result);
        }
        return false;
    }

    printf("DS5 BT: original startup state report forwarded\r\n");
    return true;
}

static void ds5_bt_forward_audio(
    const uint8_t *haptics_data,
    bool microphone_enabled,
    const uint8_t *speaker_opus_data,
    size_t speaker_opus_data_length,
    uint8_t *bt_transaction,
    size_t bt_transaction_capacity)
{
    size_t transaction_length;
    ds5_protocol_result_t protocol_result;
    ds5_output_sequence_t previous_sequence = output_sequence;
    uint8_t previous_packet_counter = haptics_packet_counter;
    int send_result;

    protocol_result = ds5_build_bt_audio_transaction(
        &output_sequence, &haptics_packet_counter,
        haptics_data, DS5_HAPTICS_DATA_SIZE, microphone_enabled,
        headset_connected, speaker_opus_data, speaker_opus_data_length,
        bt_transaction, bt_transaction_capacity, &transaction_length);
    if (protocol_result != DS5_PROTOCOL_OK) {
        output_sequence = previous_sequence;
        haptics_packet_counter = previous_packet_counter;
        ++failed_haptics_reports;
        if (failed_haptics_reports <= 4U) {
            printf("DS5 BT: audio report rejected (result %d)\r\n",
                   (int)protocol_result);
        }
        return;
    }

    send_result = ds5_l2cap_send(DS5_L2CAP_CHANNEL_INTERRUPT,
                                 bt_transaction, transaction_length);
    if (send_result < 0) {
        output_sequence = previous_sequence;
        haptics_packet_counter = previous_packet_counter;
        ++failed_haptics_reports;
        if (failed_haptics_reports <= 4U) {
            printf("DS5 BT: audio send failed (err %d)\r\n",
                   send_result);
        }
        return;
    }

    ++transmitted_haptics_reports;
    if (transmitted_haptics_reports == 1U) {
        printf("DS5 BT: first native haptics report forwarded "
               "(%lu pre-ready block(s) discarded)\r\n",
               (unsigned long)discarded_haptics_not_ready);
    }

}

static bool ds5_bt_send_microphone_status(
    bool microphone_enabled, uint8_t *bt_transaction,
    size_t bt_transaction_capacity)
{
    size_t transaction_length;
    ds5_protocol_result_t protocol_result;
    ds5_output_sequence_t previous_sequence = output_sequence;
    int send_result;

    protocol_result = ds5_build_bt_microphone_status_transaction(
        &output_sequence, microphone_enabled, bt_transaction,
        bt_transaction_capacity, &transaction_length);
    if (protocol_result != DS5_PROTOCOL_OK) {
        output_sequence = previous_sequence;
        ++failed_microphone_status_reports;
        return false;
    }

    send_result = ds5_l2cap_send(DS5_L2CAP_CHANNEL_INTERRUPT,
                                 bt_transaction, transaction_length);
    if (send_result < 0) {
        output_sequence = previous_sequence;
        ++failed_microphone_status_reports;
        if (failed_microphone_status_reports <= 4U) {
            printf("DS5 BT: microphone state send failed (err %d)\r\n",
                   send_result);
        }
        return false;
    }

    ++transmitted_microphone_status_reports;
    printf("DS5 BT: microphone stream %s\r\n",
           microphone_enabled ? "enabled" : "disabled");
    return true;
}

static bool ds5_bt_send_feature_set(
    const ds5_feature_set_request_t *request,
    uint8_t *bt_transaction, size_t bt_transaction_capacity)
{
    size_t transaction_length;
    ds5_protocol_result_t protocol_result;
    int send_result;

    protocol_result = ds5_build_feature_set_transaction(
        request->report_id, request->payload, request->payload_length,
        bt_transaction, bt_transaction_capacity, &transaction_length);
    if (protocol_result != DS5_PROTOCOL_OK) {
        ++failed_feature_set_reports;
        printf("DS5 BT: Feature SET 0x%02x rejected (result %d)\r\n",
               (unsigned int)request->report_id, (int)protocol_result);
        return false;
    }

    send_result = ds5_l2cap_send(DS5_L2CAP_CHANNEL_CONTROL,
                                 bt_transaction, transaction_length);
    if (send_result < 0) {
        ++failed_feature_set_reports;
        if (failed_feature_set_reports <= 4U) {
            printf("DS5 BT: Feature SET 0x%02x send failed (err %d)\r\n",
                   (unsigned int)request->report_id, send_result);
        }
        return false;
    }

    ++transmitted_feature_set_reports;
    printf("DS5 BT: Feature SET 0x%02x forwarded, payload %u, total %lu\r\n",
           (unsigned int)request->report_id,
           (unsigned int)request->payload_length,
           (unsigned long)transmitted_feature_set_reports);
    return true;
}

static void ds5_bt_tx_worker(void *parameter)
{
    uint8_t usb_report[DS5_USB_OUTPUT_REPORT_SIZE];
    uint8_t haptics_data[DS5_HAPTICS_DATA_SIZE];
    uint8_t speaker_opus_data[DS5_BT_AUDIO_SPEAKER_DATA_SIZE];
    uint8_t bt_transaction[DS5_BT_HAPTICS_TRANSACTION_SIZE];
    ds5_feature_set_request_t feature_set_request;
    bool pending_usb_output = false;
    TickType_t pending_usb_output_since = 0U;
    bool pending_haptics = false;
    bool microphone_stream_enabled = false;
    bool pending_microphone_state = false;
    bool controller_microphone_muted = false;
    bool pending_microphone_mute = false;
    bool pending_feature_set = false;
    size_t speaker_frame_count = 0U;
    uint32_t observed_link_generation = link_generation;

    (void)parameter;

    while (1) {
        bool did_work = false;
        bool retry_pending = false;
        uint8_t newer_report[DS5_USB_OUTPUT_REPORT_SIZE];
        bool newest_microphone_state;

        if (observed_link_generation != link_generation) {
            observed_link_generation = link_generation;
            controller_microphone_muted = false;
            pending_microphone_mute = false;
            pending_microphone_state = true;
            pending_usb_output = false;
            pending_haptics = false;
            while (ds5_output_mailbox_try_receive_microphone_button_press()) {
            }
        }

        if (!pending_feature_set &&
            ds5_feature_set_mailbox_try_receive(&feature_set_request)) {
            pending_feature_set = true;
            did_work = true;
        }

        while (ds5_output_mailbox_try_receive(newer_report,
                                              sizeof(newer_report))) {
            if ((newer_report[DS5_USB_OUTPUT_VALID_FLAGS1_OFFSET] &
                 DS5_USB_OUTPUT_ALLOW_AUDIO_MUTE) != 0U) {
                controller_microphone_muted =
                    (newer_report[DS5_USB_OUTPUT_MUTE_CONTROL_OFFSET] &
                     DS5_USB_OUTPUT_MIC_MUTE) != 0U;

                /* Keep the yellow LED authoritative to the actual mute bit. */
                newer_report[DS5_USB_OUTPUT_VALID_FLAGS1_OFFSET] |=
                    DS5_USB_OUTPUT_ALLOW_MUTE_LIGHT;
                newer_report[DS5_USB_OUTPUT_MUTE_LIGHT_OFFSET] =
                    controller_microphone_muted ?
                        DS5_USB_OUTPUT_MUTE_LIGHT_ON : 0U;
                pending_microphone_mute = false;
            }
            memcpy(usb_report, newer_report, sizeof(usb_report));
            pending_usb_output = true;
            pending_usb_output_since = xTaskGetTickCount();
            did_work = true;
        }

        while (ds5_output_mailbox_try_receive_microphone_button_press()) {
            controller_microphone_muted = !controller_microphone_muted;
            pending_microphone_mute = true;
            did_work = true;
        }

        while (ds5_audio_mailbox_try_receive_microphone_stream_active(
            &newest_microphone_state)) {
            microphone_stream_enabled = newest_microphone_state;
            pending_microphone_state = true;
            did_work = true;
        }

        if (initialization_pending && ds5_bt_interrupt_ready()) {
            if (ds5_bt_send_initialization(bt_transaction,
                                           sizeof(bt_transaction))) {
                initialization_pending = false;
                did_work = true;
            } else {
                retry_pending = true;
            }
        }

        if (pending_microphone_state && ds5_bt_interrupt_ready()) {
            if (ds5_bt_send_microphone_status(microphone_stream_enabled,
                                               bt_transaction,
                                               sizeof(bt_transaction))) {
                pending_microphone_state = false;
                did_work = true;
            } else {
                retry_pending = true;
            }
        }

        if (pending_feature_set && control_channel_ready) {
            if (ds5_bt_send_feature_set(&feature_set_request,
                                        bt_transaction,
                                        sizeof(bt_transaction))) {
                pending_feature_set = false;
                did_work = true;
            } else {
                retry_pending = true;
            }
        }

        if (!pending_haptics &&
            ds5_haptics_mailbox_try_receive(haptics_data,
                                            sizeof(haptics_data))) {
            did_work = true;
            if (ds5_bt_interrupt_ready()) {
                pending_haptics = true;
            } else {
                ++discarded_haptics_not_ready;
            }
        }

        if (!ds5_audio_mailbox_speaker_stream_active()) {
            uint8_t discarded_speaker_frame[DS5_AUDIO_SPEAKER_OPUS_SIZE];

            speaker_frame_count = 0U;
            while (ds5_audio_mailbox_try_receive_speaker_opus(
                discarded_speaker_frame, sizeof(discarded_speaker_frame))) {
                did_work = true;
            }
        } else {
            while ((speaker_frame_count < DS5_BT_AUDIO_SPEAKER_FRAME_COUNT) &&
                   ds5_audio_mailbox_try_receive_speaker_opus(
                       &speaker_opus_data[speaker_frame_count *
                                          DS5_AUDIO_SPEAKER_OPUS_SIZE],
                       DS5_AUDIO_SPEAKER_OPUS_SIZE)) {
                ++speaker_frame_count;
                did_work = true;
            }
        }

        if (pending_haptics && ds5_bt_interrupt_ready() &&
            (!ds5_audio_mailbox_speaker_stream_active() ||
             (speaker_frame_count == DS5_BT_AUDIO_SPEAKER_FRAME_COUNT))) {
            ds5_bt_forward_audio(
                haptics_data, microphone_stream_enabled,
                ds5_audio_mailbox_speaker_stream_active() ? speaker_opus_data :
                                                            NULL,
                ds5_audio_mailbox_speaker_stream_active() ?
                    sizeof(speaker_opus_data) : 0U,
                bt_transaction, sizeof(bt_transaction));
            pending_haptics = false;
            speaker_frame_count = 0U;
            did_work = true;
        }

        if (pending_usb_output) {
            if (ds5_bt_interrupt_ready()) {
                if (ds5_bt_forward_usb_output(
                        usb_report, bt_transaction,
                        sizeof(bt_transaction))) {
                    pending_usb_output = false;
                    did_work = true;
                } else {
                    retry_pending = true;
                }
            } else if ((xTaskGetTickCount() - pending_usb_output_since) >=
                       pdMS_TO_TICKS(DS5_BT_OUTPUT_READY_TIMEOUT_MS)) {
                ++failed_output_reports;
                if (failed_output_reports <= 4U) {
                    printf("DS5 BT: USB output expired waiting for "
                           "controller\r\n");
                }
                pending_usb_output = false;
            }
        }

        /* A physical press is newer than any host report already queued. */
        if (!initialization_pending && !pending_usb_output &&
            pending_microphone_mute && ds5_bt_interrupt_ready()) {
            if (ds5_bt_send_microphone_mute(
                    controller_microphone_muted, newer_report,
                    sizeof(newer_report), bt_transaction,
                    sizeof(bt_transaction))) {
                pending_microphone_mute = false;
                did_work = true;
            } else {
                retry_pending = true;
            }
        }

        if (!did_work) {
            TickType_t wait_ticks;

            if (retry_pending) {
                wait_ticks = pdMS_TO_TICKS(DS5_BT_TX_RETRY_MS);
            } else if (pending_usb_output) {
                TickType_t timeout_ticks =
                    pdMS_TO_TICKS(DS5_BT_OUTPUT_READY_TIMEOUT_MS);
                TickType_t elapsed_ticks =
                    xTaskGetTickCount() - pending_usb_output_since;

                wait_ticks = elapsed_ticks >= timeout_ticks ?
                                 0U : timeout_ticks - elapsed_ticks;
            } else {
                wait_ticks = portMAX_DELAY;
            }

            (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
        }
    }
}

static bool ds5_bt_deadline_expired(TickType_t now, TickType_t deadline)
{
    return (deadline != 0U) && (deadline != portMAX_DELAY) &&
           ((int32_t)(now - deadline) >= 0);
}

static TickType_t ds5_bt_ticks_until(TickType_t now, TickType_t deadline)
{
    if ((deadline == 0U) || (deadline == portMAX_DELAY)) {
        return portMAX_DELAY;
    }

    return ds5_bt_deadline_expired(now, deadline) ? 0U : deadline - now;
}

static void ds5_bt_process_bond_recovery(TickType_t now)
{
    int err;

    if (!bond_recovery_pending || (active_connection != NULL) ||
        !ds5_bt_deadline_expired(now, bond_recovery_retry_at)) {
        return;
    }

    if (!bonded_peer_valid) {
        bond_recovery_pending = false;
        return;
    }

    printf("DS5 BT: saved bond rejected; clearing link key for fresh pairing\r\n");
    err = ds5_bt_forget_bonded_peer();
    if (err != 0) {
        printf("DS5 BT: failed to clear rejected bond (err %d)\r\n", err);
        bond_recovery_retry_at = now + pdMS_TO_TICKS(1000U);
        return;
    }

    bonded_peer_valid = false;
    bond_recovery_pending = false;
    bond_recovery_retry_at = 0U;
    memset(&candidate, 0, sizeof(candidate));
    ds5_bt_disable_page_scan();
    ds5_bt_set_state(DS5_BT_STATE_IDLE);
    ds5_bt_open_pairing_window();
}

static int ds5_bt_start_discovery_internal(void)
{
    int err;

    if ((bluetooth_state != DS5_BT_STATE_IDLE) || discovery_in_flight ||
        !discovery_allowed || (active_connection != NULL)) {
        return -EBUSY;
    }

    memset(&candidate, 0, sizeof(candidate));
    ++discovery_session;
    discovery_in_flight = true;
    discovery_stop_requested = false;
    ds5_bt_set_state(DS5_BT_STATE_DISCOVERING);
    err = bt_br_discovery_start(&discovery_param, discovery_results,
                                DS5_BT_DISCOVERY_RESULT_COUNT,
                                ds5_bt_discovery_complete);
    if (err != 0) {
        discovery_in_flight = false;
        printf("DS5 BT: discovery start failed (err %d)\r\n", err);
        ds5_bt_set_state(DS5_BT_STATE_IDLE);
        discovery_retry_at = xTaskGetTickCount() +
                             pdMS_TO_TICKS(DS5_BT_DISCOVERY_RETRY_MS);
        return err;
    }

    printf("DS5 BT: BR/EDR discovery started (session %lu)\r\n",
           (unsigned long)discovery_session);
    return 0;
}

static void ds5_bt_stop_discovery_if_needed(TickType_t now)
{
    int err;

    if (!discovery_in_flight || discovery_allowed ||
        discovery_stop_requested ||
        !ds5_bt_deadline_expired(now, discovery_retry_at)) {
        return;
    }

    discovery_stop_requested = true;
    err = bt_br_discovery_stop();
    if ((err == 0) || (err == -EALREADY)) {
        /* The pinned SDK does not invoke the completion callback on stop. */
        discovery_in_flight = false;
        discovery_stop_requested = false;
        discovery_retry_at = now + pdMS_TO_TICKS(DS5_BT_DISCOVERY_RETRY_MS);
        memset(&candidate, 0, sizeof(candidate));
        if (bluetooth_state == DS5_BT_STATE_DISCOVERING) {
            ds5_bt_set_state(DS5_BT_STATE_IDLE);
        }
        return;
    }

    discovery_stop_requested = false;
    discovery_retry_at = now + pdMS_TO_TICKS(100U);
    printf("DS5 BT: discovery stop failed (err %d)\r\n", err);
}

static void ds5_bt_policy_handle_timeout(TickType_t now)
{
    ds5_bt_state_t state = bluetooth_state;

    if (!ds5_bt_deadline_expired(now, link_deadline)) {
        return;
    }

    printf("DS5 BT: %s phase timed out\r\n", ds5_bt_state_name(state));
    if (active_connection != NULL) {
        if (state != DS5_BT_STATE_DISCONNECTING) {
            (void)ds5_bt_disconnect_active(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        } else {
            bool bonded_link = active_peer_is_bonded;

            /* Do not leave a dead ACL pointer blocking every future link. */
            (void)bt_conn_disconnect(active_connection,
                                     BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            ds5_bt_reset_link_state();
            ds5_bt_recover_after_link(bonded_link);
        }
    } else {
        ds5_bt_reset_link_state();
        ds5_bt_recover_after_link(false);
    }
}

static void ds5_bt_policy_step(void)
{
    TickType_t now = xTaskGetTickCount();

    ds5_bt_policy_handle_timeout(now);
    ds5_bt_process_bond_recovery(now);

    if (pairing_window_active &&
        ds5_bt_deadline_expired(now, pairing_window_deadline)) {
        pairing_window_active = false;
        discovery_allowed = false;
    }

    ds5_bt_stop_discovery_if_needed(now);

    if ((active_connection != NULL) ||
        (bluetooth_state == DS5_BT_STATE_SECURING) ||
        (bluetooth_state == DS5_BT_STATE_L2CAP_CONNECTING) ||
        (bluetooth_state == DS5_BT_STATE_READY) ||
        (bluetooth_state == DS5_BT_STATE_DISCONNECTING) ||
        (bluetooth_state == DS5_BT_STATE_ACL_CONNECTING)) {
        return;
    }

    if ((bluetooth_state == DS5_BT_STATE_CANDIDATE_READY) &&
        candidate.valid && ds5_bt_deadline_expired(now, candidate_retry_at)) {
        if (ds5_bt_connect_candidate() != 0) {
            candidate_retry_at = now + pdMS_TO_TICKS(1000U);
        }
        return;
    }

    if ((bluetooth_state == DS5_BT_STATE_IDLE) && discovery_allowed &&
        !discovery_in_flight &&
        ds5_bt_deadline_expired(now, discovery_retry_at)) {
        (void)ds5_bt_start_discovery_internal();
    }
}

static TickType_t ds5_bt_policy_wait_ticks(void)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t wait_ticks = portMAX_DELAY;
    TickType_t candidate_wait;
    TickType_t discovery_wait;
    TickType_t window_wait;
    TickType_t link_wait;
    TickType_t bond_wait;

    link_wait = ds5_bt_ticks_until(now, link_deadline);
    if (link_wait < wait_ticks) {
        wait_ticks = link_wait;
    }

    window_wait = pairing_window_active ?
        ds5_bt_ticks_until(now, pairing_window_deadline) : portMAX_DELAY;
    if (window_wait < wait_ticks) {
        wait_ticks = window_wait;
    }

    discovery_wait = ((bluetooth_state == DS5_BT_STATE_IDLE) &&
                      discovery_allowed && !discovery_in_flight) ?
        ds5_bt_ticks_until(now, discovery_retry_at) : portMAX_DELAY;
    if (discovery_in_flight && !discovery_allowed &&
        !discovery_stop_requested) {
        discovery_wait = ds5_bt_ticks_until(now, discovery_retry_at);
    }
    if (discovery_wait < wait_ticks) {
        wait_ticks = discovery_wait;
    }

    candidate_wait = (bluetooth_state == DS5_BT_STATE_CANDIDATE_READY) ?
        ds5_bt_ticks_until(now, candidate_retry_at) : portMAX_DELAY;
    if (candidate_wait < wait_ticks) {
        wait_ticks = candidate_wait;
    }

    bond_wait = bond_recovery_pending ?
        ds5_bt_ticks_until(now, bond_recovery_retry_at) : portMAX_DELAY;
    if (bond_wait < wait_ticks) {
        wait_ticks = bond_wait;
    }

    return wait_ticks;
}

static void ds5_bt_policy_worker(void *parameter)
{
    (void)parameter;

    while (1) {
        ds5_bt_policy_step();
        (void)ulTaskNotifyTake(pdTRUE, ds5_bt_policy_wait_ticks());
    }
}

static int ds5_bt_start_worker(void)
{
    if ((worker_task != NULL) && (tx_worker_task != NULL) &&
        (policy_task != NULL)) {
        return 0;
    }

    if (worker_task == NULL) {
        worker_task = xTaskCreateStatic(ds5_bt_worker, "ds5_bt_worker",
                                        DS5_BT_WORKER_STACK_DEPTH, NULL,
                                        configMAX_PRIORITIES - 5U,
                                        worker_task_stack,
                                        &worker_task_storage);
        if (worker_task == NULL) {
            return -ENOMEM;
        }
    }

    if (tx_worker_task == NULL) {
        tx_worker_task = xTaskCreateStatic(ds5_bt_tx_worker, "ds5_bt_tx",
                                           DS5_BT_TX_WORKER_STACK_DEPTH, NULL,
                                           configMAX_PRIORITIES - 5U,
                                           tx_worker_task_stack,
                                           &tx_worker_task_storage);
        if (tx_worker_task == NULL) {
            return -ENOMEM;
        }
    }

    if (policy_task == NULL) {
        policy_task = xTaskCreateStatic(
            ds5_bt_policy_worker, "ds5_bt_policy", DS5_BT_POLICY_STACK_DEPTH,
            NULL, configMAX_PRIORITIES - 5U, policy_task_stack,
            &policy_task_storage);
    }
    return policy_task != NULL ? 0 : -ENOMEM;
}

static void ds5_bt_ready(int err)
{
    size_t bond_count = 0U;

    if (err != 0) {
        printf("DS5 BT: host initialization failed (err %d)\r\n", err);
        ds5_bt_set_state(DS5_BT_STATE_OFF);
        return;
    }

    printf("DS5 BT: host ready\r\n");

    bt_conn_cb_register(&connection_callbacks);

    err = bt_conn_auth_cb_register(&authentication_callbacks);
    if (err != 0) {
        printf("DS5 BT: auth callback registration failed (err %d)\r\n", err);
        return;
    }

    err = ds5_l2cap_init();
    if (err != 0) {
        printf("DS5 BT: HID L2CAP registration failed (err %d)\r\n", err);
        return;
    }

    err = ds5_bt_start_worker();
    if (err != 0) {
        printf("DS5 BT: worker creation failed (err %d)\r\n", err);
        return;
    }

    printf("DS5 BT: HID L2CAP servers ready\r\n");

    /* bt_enable() has already restored BR link keys before this callback. */
    memset(&candidate, 0, sizeof(candidate));
    bonded_peer_valid = false;
    bond_recovery_pending = false;
    bond_recovery_retry_at = 0U;
    bt_br_foreach_bond(ds5_bt_restore_bond, &bond_count);
    if (bonded_peer_valid) {
        char address[BT_ADDR_STR_LEN];

        bt_addr_to_str(&bonded_peer_address, address, sizeof(address));
        printf("DS5 BT: restored saved BR/EDR peer %s "
               "(%u bond(s) stored)\r\n",
               address, (unsigned int)bond_count);
        if (bond_count > 1U) {
            printf("DS5 BT: multiple bonds present; using the first saved "
                   "peer\r\n");
        }

        ds5_bt_enable_bonded_page_scan();
    } else {
        printf("DS5 BT: no saved BR/EDR peer\r\n");
    }

    /*
     * Passive page scan for the bonded peer and active inquiry for a new
     * controller are independent procedures.  Start the pairing window
     * immediately; it must not delay reconnection of the bonded peer.
     */
    ds5_bt_set_state(DS5_BT_STATE_IDLE);
    ds5_bt_open_pairing_window();
}

int ds5_bt_init(void)
{
    int err;

    printf("DS5 BT: initializing controller\r\n");
    btble_controller_init(configMAX_PRIORITIES - 1U);
    printf("DS5 BT: controller initialized\r\n");

    printf("DS5 BT: initializing HCI driver\r\n");
    err = hci_driver_init();
    if (err != 0) {
        printf("DS5 BT: HCI driver initialization failed (err %d)\r\n", err);
        return err;
    }
    printf("DS5 BT: HCI driver initialized\r\n");

    printf("DS5 BT: enabling host\r\n");
    err = bt_enable(ds5_bt_ready);
    if (err != 0) {
        printf("DS5 BT: bt_enable failed (err %d)\r\n", err);
        return err;
    }
    printf("DS5 BT: host enable submitted\r\n");

    return 0;
}

int ds5_bt_start_discovery(void)
{
    if ((bluetooth_state == DS5_BT_STATE_OFF) ||
        (active_connection != NULL) ||
        (bluetooth_state != DS5_BT_STATE_IDLE)) {
        return -EBUSY;
    }

    ds5_bt_open_pairing_window();
    return ds5_bt_start_discovery_internal();
}

bool ds5_bt_candidate_available(void)
{
    return candidate.valid;
}

int ds5_bt_fallback_to_discovery(void)
{
    printf("DS5 BT: active pairing discovery requested\r\n");
    if (bluetooth_state == DS5_BT_STATE_OFF) {
        return -EAGAIN;
    }
    if (active_connection != NULL) {
        return -EBUSY;
    }
    if (bluetooth_state != DS5_BT_STATE_IDLE) {
        return -EBUSY;
    }
    return ds5_bt_start_discovery();
}

int ds5_bt_connect_candidate(void)
{
    static const struct bt_br_conn_param connection_param = {
        .allow_role_switch = true,
    };
    struct bt_conn *conn;

    if (!candidate.valid) {
        return -ENOENT;
    }

    if ((bluetooth_state != DS5_BT_STATE_CANDIDATE_READY) ||
        (active_connection != NULL)) {
        return -EBUSY;
    }

    bt_addr_copy(&outgoing_target_address, &candidate.address);
    outgoing_create_pending = true;
    ds5_bt_set_state(DS5_BT_STATE_ACL_CONNECTING);
    conn = bt_conn_create_br(&candidate.address, &connection_param);
    if (conn == NULL) {
        outgoing_create_pending = false;
        if (active_connection != NULL) {
            /* A saved peer may have arrived while create_br was pending. */
            ds5_bt_close_pairing_window();
            return 0;
        }

        memset(&candidate, 0, sizeof(candidate));
        ds5_bt_set_state(DS5_BT_STATE_IDLE);
        ds5_bt_open_pairing_window();
        return -EIO;
    }

    if ((active_connection != NULL) && (active_connection != conn)) {
        printf("DS5 BT: discarding outgoing ACL; another peer is active\r\n");
        outgoing_create_pending = false;
        (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return -EBUSY;
    }

    /* The callback can claim the same connection before create_br returns. */
    if (active_connection == conn) {
        outgoing_create_pending = false;
        return 0;
    }

    /* A terminal callback may have completed while create_br was returning.
     * Its recovery path already dealt with the connection object. */
    if (!outgoing_create_pending) {
        return -EIO;
    }

    if (!candidate.valid || (bluetooth_state != DS5_BT_STATE_ACL_CONNECTING)) {
        outgoing_create_pending = false;
        (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return -EIO;
    }

    if (!ds5_bt_claim_active_connection(
            conn, true, ds5_bt_connection_matches_saved_peer(conn), true)) {
        outgoing_create_pending = false;
        if (active_connection != NULL) {
            (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            return -EBUSY;
        }
        return -EIO;
    }

    outgoing_create_pending = false;
    memset(&candidate, 0, sizeof(candidate));
    ds5_bt_close_pairing_window();
    return 0;
}

int ds5_bt_disconnect(void)
{
    return ds5_bt_disconnect_active(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

int ds5_bt_clear_pairing(void)
{
    ds5_bt_candidate_t previous_candidate;
    int err;

    if (bluetooth_state == DS5_BT_STATE_OFF) {
        return -EAGAIN;
    }

    /* The discovery callback owns candidate selection until it completes. */
    if (bluetooth_state == DS5_BT_STATE_DISCOVERING) {
        return -EBUSY;
    }

    previous_candidate = candidate;
    memset(&candidate, 0, sizeof(candidate));
    err = bt_unpair(BT_ID_DEFAULT, NULL);
    if (err != 0) {
        candidate = previous_candidate;
        printf("DS5 BT: failed to clear pairing information (err %d)\r\n",
               err);
        return err;
    }

    bonded_peer_valid = false;
    bond_recovery_pending = false;
    bond_recovery_retry_at = 0U;

    ds5_feature_cache_clear();

    ds5_bt_disable_page_scan();
    ds5_bt_close_pairing_window();

    if (active_connection != NULL) {
        ds5_bt_set_state(DS5_BT_STATE_DISCONNECTING);
    } else {
        ds5_bt_set_state(DS5_BT_STATE_IDLE);
        ds5_bt_open_pairing_window();
    }

    printf("DS5 BT: all pairing information cleared\r\n");
    return 0;
}

ds5_bt_state_t ds5_bt_get_state(void)
{
    return bluetooth_state;
}

uint32_t ds5_bt_received_l2cap_packet_count(void)
{
    return received_l2cap_packets;
}

void ds5_bt_get_diagnostics(ds5_bt_diagnostics_t *diagnostics)
{
    if (diagnostics == NULL) {
        return;
    }

    diagnostics->state = bluetooth_state;
    diagnostics->control_channel_ready = control_channel_ready;
    diagnostics->interrupt_channel_ready = interrupt_channel_ready;
    diagnostics->bonded_peer_valid = bonded_peer_valid;
    diagnostics->discovery_active = discovery_in_flight;
    diagnostics->pairing_window_active = pairing_window_active;
    diagnostics->link_generation = link_generation;
    diagnostics->worker_task_stack_high_water_words =
        ds5_bt_stack_high_water_words(worker_task);
    diagnostics->tx_worker_task_stack_high_water_words =
        ds5_bt_stack_high_water_words(tx_worker_task);
}
