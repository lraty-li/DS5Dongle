#include "ds5_l2cap.h"

#include <errno.h>
#include <string.h>

#include <FreeRTOS.h>
#include "queue.h"

#include "l2cap.h"
#include "net/buf.h"

/*
 * BouffaloSDK v2.3.30 enables BFLB_DYNAMIC_ALLOC_MEM and does not expose an
 * application TX net_buf pool. This local SDK entry allocates from the ACL TX
 * pool and applies the HCI headroom; the extra reserve below is for L2CAP.
 */
#include "conn_internal.h"

#define DS5_L2CAP_EVENT_QUEUE_LENGTH 8U

typedef enum {
    DS5_L2CAP_INIT_NONE = 0,
    DS5_L2CAP_INIT_QUEUE_READY,
    DS5_L2CAP_INIT_CONTROL_REGISTERED,
    DS5_L2CAP_INIT_READY,
} ds5_l2cap_init_state_t;

static struct bt_l2cap_br_chan control_channel;
static struct bt_l2cap_br_chan interrupt_channel;

static StaticQueue_t event_queue_storage;
static uint8_t event_queue_items[DS5_L2CAP_EVENT_QUEUE_LENGTH *
                                 sizeof(ds5_l2cap_event_t)];
static QueueHandle_t event_queue;

static ds5_l2cap_init_state_t init_state;
static bool outgoing_channel_sequence;
static volatile uint32_t dropped_event_count;

static ds5_l2cap_channel_t ds5_l2cap_channel_id(
    const struct bt_l2cap_chan *channel)
{
    if (channel == &control_channel.chan) {
        return DS5_L2CAP_CHANNEL_CONTROL;
    }

    return DS5_L2CAP_CHANNEL_INTERRUPT;
}

static struct bt_l2cap_br_chan *ds5_l2cap_get_channel(
    ds5_l2cap_channel_t channel)
{
    switch (channel) {
    case DS5_L2CAP_CHANNEL_CONTROL:
        return &control_channel;
    case DS5_L2CAP_CHANNEL_INTERRUPT:
        return &interrupt_channel;
    default:
        return NULL;
    }
}

static void ds5_l2cap_enqueue_event(ds5_l2cap_event_type_t type,
                                    ds5_l2cap_channel_t channel,
                                    int status,
                                    const uint8_t *data,
                                    size_t length)
{
    ds5_l2cap_event_t event = {
        .type = type,
        .channel = channel,
        .status = status,
        .length = length,
    };

    if ((event_queue == NULL) ||
        (length > DS5_L2CAP_MAX_EVENT_PAYLOAD) ||
        ((data == NULL) && (length != 0U))) {
        ++dropped_event_count;
        return;
    }

    if (length != 0U) {
        memcpy(event.data, data, length);
    }

    if (xQueueSend(event_queue, &event, 0U) != pdPASS) {
        ++dropped_event_count;
    }
}

static void ds5_l2cap_connected(struct bt_l2cap_chan *channel)
{
    ds5_l2cap_channel_t channel_id = ds5_l2cap_channel_id(channel);

    ds5_l2cap_enqueue_event(DS5_L2CAP_EVENT_CONNECTED, channel_id, 0,
                            NULL, 0U);

    if ((channel_id == DS5_L2CAP_CHANNEL_CONTROL) &&
        outgoing_channel_sequence &&
        (interrupt_channel.chan.conn == NULL)) {
        int err = bt_l2cap_chan_connect(channel->conn,
                                        &interrupt_channel.chan,
                                        DS5_HID_INTERRUPT_PSM);

        if (err != 0) {
            outgoing_channel_sequence = false;
            ds5_l2cap_enqueue_event(DS5_L2CAP_EVENT_ERROR,
                                    DS5_L2CAP_CHANNEL_INTERRUPT,
                                    err, NULL, 0U);
        }
    } else if (channel_id == DS5_L2CAP_CHANNEL_INTERRUPT) {
        outgoing_channel_sequence = false;
    }
}

static void ds5_l2cap_disconnected(struct bt_l2cap_chan *channel)
{
    ds5_l2cap_channel_t channel_id = ds5_l2cap_channel_id(channel);

    if (channel_id == DS5_L2CAP_CHANNEL_CONTROL) {
        outgoing_channel_sequence = false;
    }

    ds5_l2cap_enqueue_event(DS5_L2CAP_EVENT_DISCONNECTED, channel_id, 0,
                            NULL, 0U);
}

static int ds5_l2cap_recv(struct bt_l2cap_chan *channel,
                          struct net_buf *buffer)
{
    if (buffer == NULL) {
        return -EINVAL;
    }

    ds5_l2cap_enqueue_event(DS5_L2CAP_EVENT_DATA,
                            ds5_l2cap_channel_id(channel), 0,
                            buffer->data, buffer->len);

    /* The local BR/EDR receive path releases buffer after this callback. */
    return 0;
}

static const struct bt_l2cap_chan_ops channel_ops = {
    .connected = ds5_l2cap_connected,
    .disconnected = ds5_l2cap_disconnected,
    .recv = ds5_l2cap_recv,
};

static int ds5_l2cap_accept_control(struct bt_conn *conn,
                                    struct bt_l2cap_chan **channel)
{
    if ((conn == NULL) || (channel == NULL) ||
        (control_channel.chan.conn != NULL) ||
        ((interrupt_channel.chan.conn != NULL) &&
         (interrupt_channel.chan.conn != conn))) {
        return -ENOMEM;
    }

    outgoing_channel_sequence = false;
    *channel = &control_channel.chan;
    return 0;
}

static int ds5_l2cap_accept_interrupt(struct bt_conn *conn,
                                      struct bt_l2cap_chan **channel)
{
    if ((conn == NULL) || (channel == NULL) ||
        (interrupt_channel.chan.conn != NULL) ||
        ((control_channel.chan.conn != NULL) &&
         (control_channel.chan.conn != conn))) {
        return -ENOMEM;
    }

    *channel = &interrupt_channel.chan;
    return 0;
}

static struct bt_l2cap_server control_server = {
    .psm = DS5_HID_CONTROL_PSM,
    .sec_level = BT_SECURITY_L2,
    .accept = ds5_l2cap_accept_control,
};

static struct bt_l2cap_server interrupt_server = {
    .psm = DS5_HID_INTERRUPT_PSM,
    .sec_level = BT_SECURITY_L2,
    .accept = ds5_l2cap_accept_interrupt,
};

static void ds5_l2cap_prepare_channel(struct bt_l2cap_br_chan *channel)
{
    memset(channel, 0, sizeof(*channel));
    channel->chan.ops = &channel_ops;
    channel->chan.required_sec_level = BT_SECURITY_L2;
    channel->rx.mtu = DS5_L2CAP_MTU;
}

int ds5_l2cap_init(void)
{
    int err;

    if (init_state == DS5_L2CAP_INIT_READY) {
        return 0;
    }

    if (init_state == DS5_L2CAP_INIT_NONE) {
        event_queue = xQueueCreateStatic(DS5_L2CAP_EVENT_QUEUE_LENGTH,
                                         sizeof(ds5_l2cap_event_t),
                                         event_queue_items,
                                         &event_queue_storage);
        if (event_queue == NULL) {
            return -ENOMEM;
        }

        ds5_l2cap_prepare_channel(&control_channel);
        ds5_l2cap_prepare_channel(&interrupt_channel);
        init_state = DS5_L2CAP_INIT_QUEUE_READY;
    }

    if (init_state == DS5_L2CAP_INIT_QUEUE_READY) {
        err = bt_l2cap_br_server_register(&control_server);
        if (err != 0) {
            return err;
        }
        init_state = DS5_L2CAP_INIT_CONTROL_REGISTERED;
    }

    err = bt_l2cap_br_server_register(&interrupt_server);
    if (err != 0) {
        return err;
    }

    init_state = DS5_L2CAP_INIT_READY;
    return 0;
}

int ds5_l2cap_connect(struct bt_conn *conn)
{
    int err;

    if (conn == NULL) {
        return -EINVAL;
    }

    if (init_state != DS5_L2CAP_INIT_READY) {
        return -EAGAIN;
    }

    if ((control_channel.chan.conn != NULL) ||
        (interrupt_channel.chan.conn != NULL)) {
        return -EBUSY;
    }

    outgoing_channel_sequence = true;
    err = bt_l2cap_chan_connect(conn, &control_channel.chan,
                                DS5_HID_CONTROL_PSM);
    if (err != 0) {
        outgoing_channel_sequence = false;
    }

    return err;
}

int ds5_l2cap_disconnect(void)
{
    int first_error = 0;
    int err;

    outgoing_channel_sequence = false;

    if (interrupt_channel.chan.conn != NULL) {
        err = bt_l2cap_chan_disconnect(&interrupt_channel.chan);
        if (err != 0) {
            first_error = err;
        }
    }

    if (control_channel.chan.conn != NULL) {
        err = bt_l2cap_chan_disconnect(&control_channel.chan);
        if ((err != 0) && (first_error == 0)) {
            first_error = err;
        }
    }

    if ((interrupt_channel.chan.conn == NULL) &&
        (control_channel.chan.conn == NULL)) {
        return -ENOTCONN;
    }

    return first_error;
}

int ds5_l2cap_send(ds5_l2cap_channel_t channel, const uint8_t *data,
                   size_t length)
{
    struct bt_l2cap_br_chan *br_channel = ds5_l2cap_get_channel(channel);
    struct net_buf *buffer;
    int result;

    if ((br_channel == NULL) || (data == NULL) || (length == 0U)) {
        return -EINVAL;
    }

    if ((br_channel->chan.conn == NULL) ||
        (br_channel->chan.state != BT_L2CAP_CONNECTED)) {
        return -ENOTCONN;
    }

    if ((length > br_channel->tx.mtu) ||
        (length > CONFIG_BT_L2CAP_TX_MTU)) {
        return -EMSGSIZE;
    }

    buffer = bt_conn_create_pdu_timeout(NULL, BT_L2CAP_HDR_SIZE, K_NO_WAIT);
    if (buffer == NULL) {
        return -ENOBUFS;
    }

    if (net_buf_tailroom(buffer) < length) {
        net_buf_unref(buffer);
        return -EMSGSIZE;
    }

    net_buf_add_mem(buffer, data, length);
    result = bt_l2cap_chan_send(&br_channel->chan, buffer);
    if (result < 0) {
        /* BR/EDR rejects before queueing on every negative return path. */
        net_buf_unref(buffer);
    }

    return result;
}

bool ds5_l2cap_event_try_receive(ds5_l2cap_event_t *event)
{
    if ((event_queue == NULL) || (event == NULL)) {
        return false;
    }

    return xQueueReceive(event_queue, event, 0U) == pdPASS;
}

uint32_t ds5_l2cap_dropped_event_count(void)
{
    return dropped_event_count;
}
