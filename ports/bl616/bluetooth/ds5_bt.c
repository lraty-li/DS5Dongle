#include "ds5_bt.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include "task.h"

#include "bluetooth.h"
#include "btble_lib_api.h"
#include "conn.h"
#include "hci_driver.h"
#include "hci_err.h"

#include "ds5_bt_policy.h"
#include "ds5_l2cap.h"

#define DS5_BT_DISCOVERY_RESULT_COUNT 10U
#define DS5_BT_DISCOVERY_LENGTH       0x05U
#define DS5_BT_CANDIDATE_NAME_SIZE    64U
#define DS5_BT_WORKER_STACK_DEPTH     (configMINIMAL_STACK_SIZE * 4U)

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
 * BouffaloSDK v2.3.30 keeps the locally-created BR connection's sticky
 * reference until its failure/disconnect notification completes. This is a
 * borrowed pointer and must be cleared in those callbacks, not unreferenced by
 * the application (matching bredr_cli_cmds.c in this SDK).
 */
static struct bt_conn *active_connection;

static volatile ds5_bt_state_t bluetooth_state = DS5_BT_STATE_OFF;
static bool outgoing_acl;
static bool control_channel_ready;
static bool interrupt_channel_ready;
static volatile uint32_t received_l2cap_packets;

static StaticTask_t worker_task_storage;
static StackType_t worker_task_stack[DS5_BT_WORKER_STACK_DEPTH];
static TaskHandle_t worker_task;

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
    printf("DS5 BT: state %s -> %s\r\n",
           ds5_bt_state_name(previous), ds5_bt_state_name(state));
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

static void ds5_bt_reset_link_state(void)
{
    active_connection = NULL;
    outgoing_acl = false;
    control_channel_ready = false;
    interrupt_channel_ready = false;
}

static void ds5_bt_return_to_candidate(void)
{
    ds5_bt_set_state(candidate.valid ? DS5_BT_STATE_CANDIDATE_READY
                                     : DS5_BT_STATE_IDLE);
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
    int security_result;

    if (err != 0U) {
        if (conn == active_connection) {
            printf("DS5 BT: ACL connection failed (err %u)\r\n",
                   (unsigned int)err);
            ds5_bt_reset_link_state();
            ds5_bt_return_to_candidate();
        }
        return;
    }

    if (active_connection == NULL) {
        if (!ds5_bt_connection_matches_candidate(conn)) {
            ds5_bt_print_connection_address(conn,
                                            "rejecting unselected peer");
            (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            return;
        }

        active_connection = conn;
        outgoing_acl = false;
    } else if (active_connection != conn) {
        ds5_bt_print_connection_address(conn, "rejecting additional peer");
        (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return;
    }

    ds5_bt_print_connection_address(conn, "ACL connected to");
    ds5_bt_set_state(DS5_BT_STATE_SECURING);

    security_result = bt_conn_set_security(conn, BT_SECURITY_L2);
    if (security_result != 0) {
        printf("DS5 BT: security request failed (err %d)\r\n",
               security_result);
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
    if (conn != active_connection) {
        return;
    }

    printf("DS5 BT: ACL disconnected (reason 0x%02x)\r\n",
           (unsigned int)reason);
    ds5_bt_reset_link_state();
    ds5_bt_return_to_candidate();
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
        (void)ds5_bt_disconnect_active(BT_HCI_ERR_AUTH_FAIL);
        return;
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
        printf("DS5 BT: pairing complete (bonded %u)\r\n",
               bonded ? 1U : 0U);
    }
}

static void ds5_bt_pairing_failed(struct bt_conn *conn,
                                  enum bt_security_err reason)
{
    if (conn == active_connection) {
        printf("DS5 BT: pairing failed (reason %d)\r\n", (int)reason);
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
    uint16_t score = ds5_bt_policy_candidate_score(device_class, name, false);

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

static void ds5_bt_discovery_complete(struct bt_br_discovery_result *results,
                                      size_t count)
{
    size_t index;

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
}

static void ds5_bt_process_l2cap_event(const ds5_l2cap_event_t *event)
{
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
            ds5_bt_set_state(DS5_BT_STATE_READY);
        }
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
        break;
    case DS5_L2CAP_EVENT_DATA:
        ++received_l2cap_packets;
        break;
    case DS5_L2CAP_EVENT_ERROR:
        printf("DS5 BT: L2CAP channel %u failed (err %d)\r\n",
               (unsigned int)event->channel, event->status);
        if (active_connection != NULL) {
            (void)ds5_bt_disconnect_active(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
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

static int ds5_bt_start_worker(void)
{
    if (worker_task != NULL) {
        return 0;
    }

    worker_task = xTaskCreateStatic(ds5_bt_worker, "ds5_bt_worker",
                                    DS5_BT_WORKER_STACK_DEPTH, NULL,
                                    configMAX_PRIORITIES - 5U,
                                    worker_task_stack,
                                    &worker_task_storage);
    return worker_task != NULL ? 0 : -ENOMEM;
}

static void ds5_bt_ready(int err)
{
    static const struct bt_br_discovery_param discovery_param = {
        .length = DS5_BT_DISCOVERY_LENGTH,
        .limited = false,
    };

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
    ds5_bt_set_state(DS5_BT_STATE_DISCOVERING);

    err = bt_br_discovery_start(&discovery_param, discovery_results,
                                DS5_BT_DISCOVERY_RESULT_COUNT,
                                ds5_bt_discovery_complete);
    if (err != 0) {
        printf("DS5 BT: discovery start failed (err %d)\r\n", err);
        ds5_bt_set_state(DS5_BT_STATE_IDLE);
        return;
    }

    printf("DS5 BT: BR/EDR discovery started\r\n");
}

int ds5_bt_init(void)
{
    int err;

    printf("DS5 BT: initializing controller\r\n");
    btble_controller_init(configMAX_PRIORITIES - 1U);

    err = hci_driver_init();
    if (err != 0) {
        printf("DS5 BT: HCI driver initialization failed (err %d)\r\n", err);
        return err;
    }

    err = bt_enable(ds5_bt_ready);
    if (err != 0) {
        printf("DS5 BT: bt_enable failed (err %d)\r\n", err);
        return err;
    }

    return 0;
}

bool ds5_bt_candidate_available(void)
{
    return candidate.valid;
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

    ds5_bt_set_state(DS5_BT_STATE_ACL_CONNECTING);
    conn = bt_conn_create_br(&candidate.address, &connection_param);
    if (conn == NULL) {
        ds5_bt_return_to_candidate();
        return -EIO;
    }

    active_connection = conn;
    outgoing_acl = true;
    return 0;
}

int ds5_bt_disconnect(void)
{
    return ds5_bt_disconnect_active(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

ds5_bt_state_t ds5_bt_get_state(void)
{
    return bluetooth_state;
}

uint32_t ds5_bt_received_l2cap_packet_count(void)
{
    return received_l2cap_packets;
}
