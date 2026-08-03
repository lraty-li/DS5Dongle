#include "ds5_bt.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <FreeRTOS.h>

#include "bluetooth.h"
#include "btble_lib_api.h"
#include "conn.h"
#include "hci_driver.h"

#define DS5_BT_DISCOVERY_RESULT_COUNT 10U
#define DS5_BT_DISCOVERY_LENGTH       0x05U

#define DS5_BT_EIR_SHORT_NAME    0x08U
#define DS5_BT_EIR_COMPLETE_NAME 0x09U

static struct bt_br_discovery_result discovery_results[DS5_BT_DISCOVERY_RESULT_COUNT];

static void ds5_bt_connected(struct bt_conn *conn, u8_t err)
{
    (void)conn;

    if (err != 0U) {
        printf("DS5 BT: connection failed (err %u)\r\n", (unsigned int)err);
        return;
    }

    printf("DS5 BT: connection established\r\n");
}

static void ds5_bt_disconnected(struct bt_conn *conn, u8_t reason)
{
    (void)conn;
    printf("DS5 BT: disconnected (reason 0x%02x)\r\n", (unsigned int)reason);
}

static void ds5_bt_security_changed(struct bt_conn *conn, bt_security_t level,
                                    enum bt_security_err err)
{
    (void)conn;
    printf("DS5 BT: security changed (level %u, err %d)\r\n",
           (unsigned int)level, (int)err);
}

static struct bt_conn_cb connection_callbacks = {
    .connected = ds5_bt_connected,
    .disconnected = ds5_bt_disconnected,
    .security_changed = ds5_bt_security_changed,
};

static void ds5_bt_auth_cancel(struct bt_conn *conn)
{
    (void)conn;
    printf("DS5 BT: authentication cancelled\r\n");
}

static void ds5_bt_pairing_complete(struct bt_conn *conn, bool bonded)
{
    (void)conn;
    printf("DS5 BT: pairing complete (bonded %u)\r\n", bonded ? 1U : 0U);
}

static void ds5_bt_pairing_failed(struct bt_conn *conn,
                                  enum bt_security_err reason)
{
    (void)conn;
    printf("DS5 BT: pairing failed (reason %d)\r\n", (int)reason);
}

static const struct bt_conn_auth_cb authentication_callbacks = {
    .cancel = ds5_bt_auth_cancel,
    .pairing_complete = ds5_bt_pairing_complete,
    .pairing_failed = ds5_bt_pairing_failed,
};

static void ds5_bt_print_eir_name(const uint8_t *eir, size_t eir_length)
{
    size_t offset = 0U;

    while (offset < eir_length) {
        uint8_t field_length = eir[offset++];
        uint8_t field_type;
        size_t name_length;

        if (field_length == 0U) {
            return;
        }

        if ((size_t)field_length > (eir_length - offset)) {
            printf("  name: <malformed EIR>\r\n");
            return;
        }

        field_type = eir[offset];
        name_length = (size_t)field_length - 1U;

        if ((field_type == DS5_BT_EIR_SHORT_NAME) ||
            (field_type == DS5_BT_EIR_COMPLETE_NAME)) {
            printf("  name: %.*s\r\n", (int)name_length,
                   (const char *)&eir[offset + 1U]);
            return;
        }

        offset += (size_t)field_length;
    }
}

static void ds5_bt_discovery_complete(struct bt_br_discovery_result *results,
                                      size_t count)
{
    size_t index;

    printf("DS5 BT: discovery complete (%u result(s))\r\n",
           (unsigned int)count);

    if (results == NULL) {
        return;
    }

    for (index = 0U; index < count; ++index) {
        char address[BT_ADDR_STR_LEN];
        uint32_t device_class;

        device_class = (uint32_t)results[index].cod[0] |
                       ((uint32_t)results[index].cod[1] << 8U) |
                       ((uint32_t)results[index].cod[2] << 16U);
        bt_addr_to_str(&results[index].addr, address, sizeof(address));

        printf("  addr: %s, rssi: %d, class: 0x%06lx\r\n",
               address, (int)results[index].rssi,
               (unsigned long)device_class);
        ds5_bt_print_eir_name(results[index].eir,
                              sizeof(results[index].eir));
    }
}

static void ds5_bt_ready(int err)
{
    static const struct bt_br_discovery_param discovery_param = {
        .length = DS5_BT_DISCOVERY_LENGTH,
        .limited = false,
    };

    if (err != 0) {
        printf("DS5 BT: host initialization failed (err %d)\r\n", err);
        return;
    }

    printf("DS5 BT: host ready\r\n");

    bt_conn_cb_register(&connection_callbacks);

    err = bt_conn_auth_cb_register(&authentication_callbacks);
    if (err != 0) {
        printf("DS5 BT: auth callback registration failed (err %d)\r\n", err);
        return;
    }

    err = bt_br_discovery_start(&discovery_param, discovery_results,
                                DS5_BT_DISCOVERY_RESULT_COUNT,
                                ds5_bt_discovery_complete);
    if (err != 0) {
        printf("DS5 BT: discovery start failed (err %d)\r\n", err);
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
