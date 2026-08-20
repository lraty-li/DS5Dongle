#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include "portmacro.h"
#include "queue.h"

#include "conn.h"
#include "conn_internal.h"
#include "hci_host.h"
#include "misc/byteorder.h"
#include "net/buf.h"

#include "ds5_bt_internal.h"
#include "ds5_l2cap.h"
#include "ds5_log.h"

#define printf ds5_log_printf

#define DS5_BT_DISCONNECT_EVENT_QUEUE_LENGTH 4U
#define DS5_BT_HCI_OP_WRITE_LINK_SUPERVISION_TIMEOUT \
    BT_OP(BT_OGF_BASEBAND, 0x0037)

struct ds5_bt_hci_cp_write_link_supervision_timeout {
    u16_t handle;
    u16_t timeout;
} __packed;

typedef struct {
    struct bt_conn *conn;
    uint8_t reason;
} ds5_bt_disconnected_event_t;

static StaticQueue_t disconnected_event_queue_storage;
static uint8_t disconnected_event_queue_items[
    DS5_BT_DISCONNECT_EVENT_QUEUE_LENGTH *
    sizeof(ds5_bt_disconnected_event_t)];
static QueueHandle_t disconnected_event_queue;
static volatile bool disconnected_event_overflow;
static volatile uint8_t disconnected_event_overflow_reason;
static volatile uint32_t disconnected_event_drop_count;
static uint32_t supervision_configured_generation;
static uint32_t supervision_failed_generation;

int ds5_bt_liveness_init(void)
{
    if (disconnected_event_queue != NULL) {
        return 0;
    }

    disconnected_event_queue = xQueueCreateStatic(
        DS5_BT_DISCONNECT_EVENT_QUEUE_LENGTH,
        sizeof(ds5_bt_disconnected_event_t),
        disconnected_event_queue_items,
        &disconnected_event_queue_storage);
    return disconnected_event_queue != NULL ? 0 : -ENOMEM;
}

static void ds5_bt_arm_disconnect_overflow(uint8_t reason)
{
    disconnected_event_overflow_reason = reason;
    disconnected_event_overflow = true;
    ++disconnected_event_drop_count;
}

bool ds5_bt_enqueue_disconnected_event(struct bt_conn *conn,
                                       uint8_t reason)
{
    ds5_bt_disconnected_event_t event;
    BaseType_t result;

    if ((disconnected_event_queue == NULL) || (conn == NULL)) {
        ds5_bt_arm_disconnect_overflow(reason);
        ds5_bt_policy_wake();
        return false;
    }

    event.conn = bt_conn_ref(conn);
    event.reason = reason;

    if (xPortIsInsideInterrupt()) {
        BaseType_t task_woken = pdFALSE;

        result = xQueueSendFromISR(disconnected_event_queue, &event,
                                   &task_woken);
        portYIELD_FROM_ISR(task_woken);
    } else {
        result = xQueueSend(disconnected_event_queue, &event, 0U);
    }

    if (result != pdPASS) {
        bt_conn_unref(event.conn);
        ds5_bt_arm_disconnect_overflow(reason);
        ds5_bt_policy_wake();
        return false;
    }

    ds5_bt_policy_wake();
    return true;
}

static void ds5_bt_process_disconnected_event(
    const ds5_bt_disconnected_event_t *event)
{
    bool bonded_link;

    if ((event == NULL) || (event->conn == NULL)) {
        return;
    }

    if (event->conn != active_connection) {
        ds5_l2cap_abort_connection(event->conn);
        if (ds5_bt_connection_matches_outgoing_target(event->conn)) {
            outgoing_create_pending = false;
            if (active_connection == NULL) {
                memset(&candidate, 0, sizeof(candidate));
                ds5_bt_set_state(DS5_BT_STATE_IDLE);
                ds5_bt_open_pairing_window();
            }
        }

        /* The SDK clears page scan for every BR disconnection. */
        ds5_bt_enable_bonded_page_scan();
        return;
    }

    printf("DS5 BT: ACL disconnected (reason 0x%02x)\r\n",
           (unsigned int)event->reason);
    bonded_link = active_peer_is_bonded;
    ds5_bt_reset_link_state();
    ds5_bt_recover_after_link(bonded_link);
}

static bool ds5_bt_take_disconnect_overflow(uint8_t *reason)
{
    bool pending = disconnected_event_overflow;

    if (pending) {
        if (reason != NULL) {
            *reason = disconnected_event_overflow_reason;
        }
        disconnected_event_overflow = false;
    }

    return pending;
}

void ds5_bt_process_link_events(void)
{
    ds5_bt_disconnected_event_t event;
    uint8_t reason;

    while ((disconnected_event_queue != NULL) &&
           (xQueueReceive(disconnected_event_queue, &event, 0U) == pdPASS)) {
        ds5_bt_process_disconnected_event(&event);
        bt_conn_unref(event.conn);
    }

    if (!ds5_bt_take_disconnect_overflow(&reason)) {
        return;
    }

    printf("DS5 BT: disconnect event queue overflow (reason 0x%02x); "
           "recovering current link\r\n", (unsigned int)reason);
    if (active_connection != NULL) {
        bool bonded_link = active_peer_is_bonded;

        ds5_bt_reset_link_state();
        ds5_bt_recover_after_link(bonded_link);
    } else {
        ds5_bt_reset_link_state();
        ds5_bt_recover_after_link(false);
    }
}

void ds5_bt_check_link_liveness(void)
{
    bool bonded_link;

    if ((bluetooth_state != DS5_BT_STATE_READY) ||
        (active_connection == NULL) ||
        (active_connection->state == BT_CONN_CONNECTED)) {
        return;
    }

    printf("DS5 BT: host ACL state is no longer connected; recovering\r\n");
    bonded_link = active_peer_is_bonded;
    ds5_bt_reset_link_state();
    ds5_bt_recover_after_link(bonded_link);
}

static int ds5_bt_write_link_supervision_timeout(struct bt_conn *conn)
{
    struct ds5_bt_hci_cp_write_link_supervision_timeout *cp;
    struct net_buf *buf;
    u16_t handle;
    int err;

    err = bt_hci_get_conn_handle(conn, &handle);
    if (err != 0) {
        return err;
    }

    buf = bt_hci_cmd_create(
        DS5_BT_HCI_OP_WRITE_LINK_SUPERVISION_TIMEOUT, sizeof(*cp));
    if (buf == NULL) {
        return -ENOBUFS;
    }

    cp = net_buf_add(buf, sizeof(*cp));
    cp->handle = sys_cpu_to_le16(handle);
    cp->timeout = sys_cpu_to_le16(
        DS5_BT_ACL_SUPERVISION_TIMEOUT_SLOTS);

    return bt_hci_cmd_send_sync(
        DS5_BT_HCI_OP_WRITE_LINK_SUPERVISION_TIMEOUT, buf, NULL);
}

void ds5_bt_configure_link_supervision_timeout(void)
{
    int err;

    if ((bluetooth_state != DS5_BT_STATE_READY) ||
        (active_connection == NULL) ||
        (supervision_configured_generation == link_generation) ||
        (supervision_failed_generation == link_generation)) {
        return;
    }

    err = ds5_bt_write_link_supervision_timeout(active_connection);
    if (err == -ENOTCONN) {
        return;
    }

    if (err != 0) {
        supervision_failed_generation = link_generation;
        printf("DS5 BT: BR link supervision timeout setup failed (err %d); "
               "using controller default\r\n", err);
        return;
    }

    supervision_configured_generation = link_generation;
    printf("DS5 BT: BR link supervision timeout set to %u ms\r\n",
           (unsigned int)DS5_BT_ACL_SUPERVISION_TIMEOUT_MS);
}
