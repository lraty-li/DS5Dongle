#include "ds5_bt.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include "portmacro.h"
#include "semphr.h"
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
#include "ds5_bt_internal.h"

#define printf ds5_log_printf

ds5_bt_candidate_t candidate;
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
bt_addr_t bonded_peer_address;
volatile bool bonded_peer_valid;

/*
 * BouffaloSDK v2.3.30 keeps the locally-created BR connection's sticky
 * reference until its failure/disconnect notification completes. This is a
 * borrowed pointer and must be cleared in those callbacks, not unreferenced by
 * the application (matching bredr_cli_cmds.c in this SDK).
 */
struct bt_conn *active_connection;
bool active_peer_is_bonded;
bool active_peer_should_persist_bond;
bool outgoing_create_pending;
bt_addr_t outgoing_target_address;

volatile ds5_bt_state_t bluetooth_state = DS5_BT_STATE_OFF;
bool outgoing_acl;
bool control_channel_ready;
bool interrupt_channel_ready;
bool calibration_response_received;
bool feature_request_pending;
bool feature_prefetch_complete;
bool feature_prefetch_failed;
size_t feature_prefetch_index;
uint8_t feature_request_attempts;
TickType_t feature_request_deadline;
TickType_t feature_request_retry_at;
uint32_t feature_prefetch_retries;
uint32_t feature_prefetch_timeouts;
uint32_t feature_prefetch_failures;
volatile uint32_t received_l2cap_packets;
uint32_t received_control_packets;
uint32_t received_interrupt_packets;
uint32_t valid_input_reports;
uint32_t invalid_input_reports;
uint32_t input_mailbox_publish_failures;
uint32_t transmitted_output_reports;
uint32_t failed_output_reports;
uint32_t transmitted_haptics_reports;
uint32_t failed_haptics_reports;
uint32_t discarded_haptics_not_ready;
uint32_t received_microphone_packets;
uint32_t rejected_microphone_packets;
uint32_t transmitted_microphone_status_reports;
uint32_t failed_microphone_status_reports;
uint32_t received_microphone_button_presses;
uint32_t dropped_microphone_button_presses;
uint32_t transmitted_microphone_mute_reports;
uint32_t failed_microphone_mute_reports;
uint32_t transmitted_feature_set_reports;
uint32_t failed_feature_set_reports;
uint8_t latest_usb_input_payload[DS5_USB_INPUT_PAYLOAD_SIZE];
uint32_t failed_initialization_reports;
bool initialization_pending;
bool headset_connected;
bool microphone_button_pressed;
uint32_t link_generation;
volatile bool discovery_in_flight;
volatile bool discovery_allowed;
volatile bool discovery_stop_requested;
volatile bool pairing_window_active;
TickType_t pairing_window_deadline;
TickType_t discovery_retry_at;
TickType_t candidate_retry_at;
TickType_t link_deadline;
TickType_t bond_recovery_retry_at;
TickType_t page_scan_retry_at;
uint32_t discovery_session;
volatile bool bond_recovery_pending;
bool page_scan_restore_pending;

StaticTask_t worker_task_storage;
StackType_t worker_task_stack[DS5_BT_WORKER_STACK_DEPTH];
TaskHandle_t worker_task;
StaticTask_t tx_worker_task_storage;
StackType_t tx_worker_task_stack[DS5_BT_TX_WORKER_STACK_DEPTH];
TaskHandle_t tx_worker_task;
StaticTask_t policy_task_storage;
StackType_t policy_task_stack[DS5_BT_POLICY_STACK_DEPTH];
TaskHandle_t policy_task;
StaticSemaphore_t lifecycle_mutex_storage;
SemaphoreHandle_t lifecycle_mutex;

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

void ds5_bt_policy_wake(void)
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

void ds5_bt_lifecycle_lock(void)
{
    if (lifecycle_mutex != NULL) {
        (void)xSemaphoreTakeRecursive(lifecycle_mutex, portMAX_DELAY);
    }
}

void ds5_bt_lifecycle_unlock(void)
{
    if (lifecycle_mutex != NULL) {
        (void)xSemaphoreGiveRecursive(lifecycle_mutex);
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

const char *ds5_bt_state_name(ds5_bt_state_t state)
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

void ds5_bt_set_state(ds5_bt_state_t state)
{
    ds5_bt_state_t previous;

    ds5_bt_lifecycle_lock();
    previous = bluetooth_state;

    if (previous == state) {
        ds5_bt_lifecycle_unlock();
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
    ds5_bt_lifecycle_unlock();
    printf("DS5 BT: state %s -> %s\r\n",
           ds5_bt_state_name(previous), ds5_bt_state_name(state));
    ds5_bt_policy_wake();
}

int ds5_bt_start_discovery(void)
{
    int result;

    ds5_bt_lifecycle_lock();
    if ((bluetooth_state == DS5_BT_STATE_OFF) ||
        (active_connection != NULL) ||
        (bluetooth_state != DS5_BT_STATE_IDLE)) {
        result = -EBUSY;
        goto out;
    }

    ds5_bt_open_pairing_window();
    result = ds5_bt_start_discovery_internal();

out:
    ds5_bt_lifecycle_unlock();
    return result;
}

bool ds5_bt_candidate_available(void)
{
    bool available;

    ds5_bt_lifecycle_lock();
    available = candidate.valid;
    ds5_bt_lifecycle_unlock();
    return available;
}

int ds5_bt_fallback_to_discovery(void)
{
    int result;

    ds5_bt_lifecycle_lock();
    printf("DS5 BT: active pairing discovery requested\r\n");
    if (bluetooth_state == DS5_BT_STATE_OFF) {
        result = -EAGAIN;
        goto out;
    }
    if (active_connection != NULL) {
        result = -EBUSY;
        goto out;
    }
    if (bluetooth_state != DS5_BT_STATE_IDLE) {
        result = -EBUSY;
        goto out;
    }
    result = ds5_bt_start_discovery();

out:
    ds5_bt_lifecycle_unlock();
    return result;
}

int ds5_bt_connect_candidate(void)
{
    static const struct bt_br_conn_param connection_param = {
        .allow_role_switch = true,
    };
    struct bt_conn *conn;
    int result;

    ds5_bt_lifecycle_lock();

    if (!candidate.valid) {
        result = -ENOENT;
        goto out;
    }

    if ((bluetooth_state != DS5_BT_STATE_CANDIDATE_READY) ||
        (active_connection != NULL)) {
        result = -EBUSY;
        goto out;
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
            result = 0;
            goto out;
        }

        memset(&candidate, 0, sizeof(candidate));
        ds5_bt_set_state(DS5_BT_STATE_IDLE);
        ds5_bt_open_pairing_window();
        result = -EIO;
        goto out;
    }

    if ((active_connection != NULL) && (active_connection != conn)) {
        printf("DS5 BT: discarding outgoing ACL; another peer is active\r\n");
        outgoing_create_pending = false;
        (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        result = -EBUSY;
        goto out;
    }

    /* The callback can claim the same connection before create_br returns. */
    if (active_connection == conn) {
        outgoing_create_pending = false;
        result = 0;
        goto out;
    }

    /* A terminal callback may have completed while create_br was returning.
     * Its recovery path already dealt with the connection object. */
    if (!outgoing_create_pending) {
        result = -EIO;
        goto out;
    }

    if (!candidate.valid || (bluetooth_state != DS5_BT_STATE_ACL_CONNECTING)) {
        outgoing_create_pending = false;
        (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        result = -EIO;
        goto out;
    }

    if (!ds5_bt_claim_active_connection(
            conn, true, ds5_bt_connection_matches_saved_peer(conn), true)) {
        outgoing_create_pending = false;
        if (active_connection != NULL) {
            (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            result = -EBUSY;
            goto out;
        }
        result = -EIO;
        goto out;
    }

    outgoing_create_pending = false;
    memset(&candidate, 0, sizeof(candidate));
    ds5_bt_close_pairing_window();
    result = 0;

out:
    ds5_bt_lifecycle_unlock();
    return result;
}

int ds5_bt_disconnect(void)
{
    return ds5_bt_disconnect_active(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

int ds5_bt_clear_pairing(void)
{
    ds5_bt_candidate_t previous_candidate;
    int err;
    int result;

    ds5_bt_lifecycle_lock();

    if (bluetooth_state == DS5_BT_STATE_OFF) {
        result = -EAGAIN;
        goto out;
    }

    /* The discovery callback owns candidate selection until it completes. */
    if (bluetooth_state == DS5_BT_STATE_DISCOVERING) {
        result = -EBUSY;
        goto out;
    }

    previous_candidate = candidate;
    memset(&candidate, 0, sizeof(candidate));
    err = bt_unpair(BT_ID_DEFAULT, NULL);
    if (err != 0) {
        candidate = previous_candidate;
        printf("DS5 BT: failed to clear pairing information (err %d)\r\n",
               err);
        result = err;
        goto out;
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
    result = 0;

out:
    ds5_bt_lifecycle_unlock();
    return result;
}

ds5_bt_state_t ds5_bt_get_state(void)
{
    ds5_bt_state_t state;

    ds5_bt_lifecycle_lock();
    state = bluetooth_state;
    ds5_bt_lifecycle_unlock();
    return state;
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

    ds5_bt_lifecycle_lock();
    diagnostics->state = bluetooth_state;
    diagnostics->control_channel_ready = control_channel_ready;
    diagnostics->interrupt_channel_ready = interrupt_channel_ready;
    diagnostics->bonded_peer_valid = bonded_peer_valid;
    diagnostics->discovery_active = discovery_in_flight;
    diagnostics->pairing_window_active = pairing_window_active;
    diagnostics->feature_prefetch_complete = feature_prefetch_complete;
    diagnostics->feature_request_pending = feature_request_pending;
    diagnostics->feature_prefetch_failed = feature_prefetch_failed;
    diagnostics->link_generation = link_generation;
    diagnostics->feature_prefetch_retries = feature_prefetch_retries;
    diagnostics->feature_prefetch_timeouts = feature_prefetch_timeouts;
    diagnostics->feature_prefetch_failures = feature_prefetch_failures;
    diagnostics->worker_task_stack_high_water_words =
        ds5_bt_stack_high_water_words(worker_task);
    diagnostics->tx_worker_task_stack_high_water_words =
        ds5_bt_stack_high_water_words(tx_worker_task);
    ds5_bt_lifecycle_unlock();
}
