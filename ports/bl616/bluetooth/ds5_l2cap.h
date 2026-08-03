#ifndef DS5_L2CAP_H
#define DS5_L2CAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS5_HID_CONTROL_PSM          0x0011U
#define DS5_HID_INTERRUPT_PSM        0x0013U
#define DS5_L2CAP_MTU                672U
#define DS5_L2CAP_MAX_EVENT_PAYLOAD  80U

struct bt_conn;

typedef enum {
    DS5_L2CAP_CHANNEL_CONTROL = 0,
    DS5_L2CAP_CHANNEL_INTERRUPT,
} ds5_l2cap_channel_t;

typedef enum {
    DS5_L2CAP_EVENT_CONNECTED = 0,
    DS5_L2CAP_EVENT_DISCONNECTED,
    DS5_L2CAP_EVENT_DATA,
    DS5_L2CAP_EVENT_ERROR,
} ds5_l2cap_event_type_t;

typedef struct {
    ds5_l2cap_event_type_t type;
    ds5_l2cap_channel_t channel;
    int status;
    size_t length;
    uint8_t data[DS5_L2CAP_MAX_EVENT_PAYLOAD];
} ds5_l2cap_event_t;

/* Register the BR/EDR HID servers. Call only after bt_enable is ready. */
int ds5_l2cap_init(void);

/* Start the Control channel; Interrupt follows after Control is connected. */
int ds5_l2cap_connect(struct bt_conn *conn);
int ds5_l2cap_disconnect(void);

/*
 * Allocate and send from task context. The SDK takes ownership of its net_buf
 * only after bt_l2cap_chan_send accepts the packet.
 */
int ds5_l2cap_send(ds5_l2cap_channel_t channel, const uint8_t *data,
                   size_t length);

/* Non-blocking handoff for a future Bluetooth worker task. */
bool ds5_l2cap_event_try_receive(ds5_l2cap_event_t *event);
uint32_t ds5_l2cap_dropped_event_count(void);

#ifdef __cplusplus
}
#endif

#endif
