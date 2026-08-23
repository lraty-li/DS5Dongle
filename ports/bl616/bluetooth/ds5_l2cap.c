#include "ds5_l2cap.h"

#include <errno.h>
#include <string.h>

#include <FreeRTOS.h>
#include "bflb_mtimer.h"
#include "queue.h"

#include "l2cap.h"
#include "net/buf.h"

/*
 * BouffaloSDK v2.3.30 enables BFLB_DYNAMIC_ALLOC_MEM and does not expose an
 * application TX net_buf pool. This local SDK entry allocates from the ACL TX
 * pool and applies the HCI headroom; the extra reserve below is for L2CAP.
 */
#include "conn_internal.h"
#include "hci_core.h"
#include "l2cap_internal.h"

#include "ds5_protocol.h"

#define DS5_L2CAP_EVENT_QUEUE_LENGTH 16U
#define DS5_L2CAP_RESERVED_LIFECYCLE_EVENTS 4U
#define DS5_L2CAP_COMPLETION_SAMPLE_INTERVAL 8U
#define DS5_L2CAP_COMPLETION_SAMPLE_SLOTS    4U

_Static_assert(DS5_L2CAP_MAX_EVENT_PAYLOAD <= DS5_L2CAP_MTU,
               "L2CAP event payload cannot exceed the channel MTU");
_Static_assert(DS5_L2CAP_MAX_EVENT_PAYLOAD >= DS5_BT_INPUT_MIN_SIZE,
               "L2CAP event payload must hold a DualSense input report");
_Static_assert(DS5_L2CAP_MAX_EVENT_PAYLOAD >=
                   (4U + DS5_AUDIO_MIC_OPUS_SIZE),
               "L2CAP event payload must hold microphone Opus data");
_Static_assert(DS5_L2CAP_MAX_EVENT_PAYLOAD >=
                   (2U + DS5_FEATURE_SET_MAX_PAYLOAD),
               "L2CAP event payload must hold the largest cached Feature");

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
static struct bt_conn *session_conn;
static uint32_t session_id;
static struct bt_conn *channel_connections[2];
static uint32_t channel_sessions[2];

typedef struct {
    volatile bool pending;
    uint64_t submitted_at_us;
} ds5_l2cap_completion_sample_t;

static ds5_l2cap_completion_sample_t
    completion_samples[DS5_L2CAP_COMPLETION_SAMPLE_SLOTS];
static volatile uint32_t audio_send_attempts;
static volatile uint32_t audio_send_accepted;
static volatile uint32_t audio_send_immediate_failures;
static volatile uint32_t audio_send_enobufs;
static volatile uint32_t max_audio_submit_interval_us;
static volatile uint32_t hci_zero_slot_observations;
static volatile uint32_t completion_samples_submitted;
static volatile uint32_t completion_samples_completed;
static volatile uint32_t last_completion_latency_us;
static volatile uint32_t max_completion_latency_us;
static volatile uint32_t average_completion_latency_us;
static volatile uint8_t hci_max_free_packets;
static volatile uint8_t hci_min_free_packets;
static volatile uint8_t last_hci_free_packets;
static volatile uint8_t last_connection_tx_queue_depth;
static volatile uint8_t max_connection_tx_queue_depth;
static uint64_t last_audio_submit_us;

static uint8_t ds5_l2cap_bounded_u8(unsigned int value)
{
    return value > UINT8_MAX ? UINT8_MAX : (uint8_t)value;
}

static uint32_t ds5_l2cap_bounded_u32(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static void ds5_l2cap_audio_completed(struct bt_conn *conn, void *user_data)
{
    ds5_l2cap_completion_sample_t *sample = user_data;
    uint32_t latency_us;
    uint32_t next_count;
    uint32_t current_average;

    (void)conn;

    if ((sample == NULL) || !sample->pending) {
        return;
    }

    latency_us = ds5_l2cap_bounded_u32(
        bflb_mtimer_get_time_us() - sample->submitted_at_us);
    last_completion_latency_us = latency_us;
    if (latency_us > max_completion_latency_us) {
        max_completion_latency_us = latency_us;
    }

    next_count = completion_samples_completed + 1U;
    current_average = average_completion_latency_us;
    if (next_count == 1U) {
        average_completion_latency_us = latency_us;
    } else if (latency_us >= current_average) {
        average_completion_latency_us = current_average +
            ((latency_us - current_average) / next_count);
    } else {
        average_completion_latency_us = current_average -
            ((current_average - latency_us) / next_count);
    }
    completion_samples_completed = next_count;
    __sync_synchronize();
    sample->pending = false;
}

static ds5_l2cap_completion_sample_t *
ds5_l2cap_claim_completion_sample(void)
{
    size_t index;

    for (index = 0U; index < DS5_L2CAP_COMPLETION_SAMPLE_SLOTS; ++index) {
        if (!completion_samples[index].pending) {
            completion_samples[index].submitted_at_us =
                bflb_mtimer_get_time_us();
            __sync_synchronize();
            completion_samples[index].pending = true;
            return &completion_samples[index];
        }
    }

    return NULL;
}

static void ds5_l2cap_observe_audio_submission(struct bt_conn *conn)
{
    uint64_t now_us = bflb_mtimer_get_time_us();
    uint8_t free_packets = ds5_l2cap_bounded_u8(
        k_sem_count_get(&bt_dev.br.pkts));
    uint8_t queue_depth = 0U;

    if (last_audio_submit_us != 0U) {
        uint32_t interval_us = ds5_l2cap_bounded_u32(
            now_us - last_audio_submit_us);

        if (interval_us > max_audio_submit_interval_us) {
            max_audio_submit_interval_us = interval_us;
        }
    }
    last_audio_submit_us = now_us;

    if (free_packets > hci_max_free_packets) {
        hci_max_free_packets = free_packets;
    }
    if (free_packets < hci_min_free_packets) {
        hci_min_free_packets = free_packets;
    }
    if (free_packets == 0U) {
        ++hci_zero_slot_observations;
    }
    last_hci_free_packets = free_packets;

    if (conn != NULL) {
        queue_depth = ds5_l2cap_bounded_u8(
            (unsigned int)k_queue_get_cnt(
                (struct k_queue *)&conn->tx_queue));
        if (queue_depth > max_connection_tx_queue_depth) {
            max_connection_tx_queue_depth = queue_depth;
        }
    }
    last_connection_tx_queue_depth = queue_depth;
}

static void ds5_l2cap_record_audio_failure(int error)
{
    ++audio_send_immediate_failures;
    if (error == -ENOBUFS) {
        ++audio_send_enobufs;
    }
}

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
                                    struct bt_conn *conn,
                                    uint32_t session,
                                    int status,
                                    const uint8_t *data,
                                    size_t length)
{
    ds5_l2cap_event_t event = {
        .type = type,
        .channel = channel,
        .conn = conn == NULL ? NULL : bt_conn_ref(conn),
        .session = session,
        .status = status,
        .length = length,
    };

    if ((event_queue == NULL) ||
        (length > DS5_L2CAP_MAX_EVENT_PAYLOAD) ||
        ((data == NULL) && (length != 0U))) {
        ++dropped_event_count;
        if (event.conn != NULL) {
            bt_conn_unref(event.conn);
        }
        return;
    }

    if (length != 0U) {
        memcpy(event.data, data, length);
    }

    /* Keep lifecycle events deliverable even while audio data is queued. */
    if ((type == DS5_L2CAP_EVENT_DATA) &&
         (uxQueueMessagesWaiting(event_queue) >=
          (DS5_L2CAP_EVENT_QUEUE_LENGTH -
           DS5_L2CAP_RESERVED_LIFECYCLE_EVENTS))) {
        ++dropped_event_count;
        if (event.conn != NULL) {
            bt_conn_unref(event.conn);
        }
        return;
    }

    if (((type == DS5_L2CAP_EVENT_DATA) ?
             xQueueSend(event_queue, &event, 0U) :
             xQueueSendToFront(event_queue, &event, 0U)) != pdPASS) {
        ++dropped_event_count;
        if (event.conn != NULL) {
            bt_conn_unref(event.conn);
        }
    }
}

static void ds5_l2cap_connected(struct bt_l2cap_chan *channel)
{
    ds5_l2cap_channel_t channel_id = ds5_l2cap_channel_id(channel);
    struct bt_conn *conn = channel_connections[channel_id];
    uint32_t channel_session = channel_sessions[channel_id];

    ds5_l2cap_enqueue_event(DS5_L2CAP_EVENT_CONNECTED, channel_id, conn,
                            channel_session, 0, NULL, 0U);

    if ((channel_id == DS5_L2CAP_CHANNEL_CONTROL) &&
        outgoing_channel_sequence &&
        (interrupt_channel.chan.conn == NULL)) {
        channel_connections[DS5_L2CAP_CHANNEL_INTERRUPT] = channel->conn;
        channel_sessions[DS5_L2CAP_CHANNEL_INTERRUPT] = channel_session;
        int err = bt_l2cap_chan_connect(channel->conn,
                                        &interrupt_channel.chan,
                                        DS5_HID_INTERRUPT_PSM);

        if (err != 0) {
            outgoing_channel_sequence = false;
            /* bt_l2cap_chan_connect() may fail before the stack installs the
             * channel, so no disconnected callback is guaranteed to clear
             * this metadata later. */
            channel_connections[DS5_L2CAP_CHANNEL_INTERRUPT] = NULL;
            channel_sessions[DS5_L2CAP_CHANNEL_INTERRUPT] = 0U;
            ds5_l2cap_enqueue_event(DS5_L2CAP_EVENT_ERROR,
                                    DS5_L2CAP_CHANNEL_INTERRUPT,
                                    channel->conn, channel_session,
                                    err, NULL, 0U);
        }
    } else if (channel_id == DS5_L2CAP_CHANNEL_INTERRUPT) {
        outgoing_channel_sequence = false;
        last_audio_submit_us = 0U;
    }
}

static void ds5_l2cap_disconnected(struct bt_l2cap_chan *channel)
{
    ds5_l2cap_channel_t channel_id = ds5_l2cap_channel_id(channel);
    struct bt_conn *conn = channel_connections[channel_id];
    uint32_t channel_session = channel_sessions[channel_id];

    if (channel_id == DS5_L2CAP_CHANNEL_CONTROL) {
        outgoing_channel_sequence = false;
    } else {
        last_audio_submit_us = 0U;
    }

    ds5_l2cap_enqueue_event(DS5_L2CAP_EVENT_DISCONNECTED, channel_id, conn,
                            channel_session, 0, NULL, 0U);
    channel_connections[channel_id] = NULL;
    channel_sessions[channel_id] = 0U;
}

static int ds5_l2cap_recv(struct bt_l2cap_chan *channel,
                          struct net_buf *buffer)
{
    ds5_l2cap_channel_t channel_id;

    if (buffer == NULL) {
        return -EINVAL;
    }

    channel_id = ds5_l2cap_channel_id(channel);
    ds5_l2cap_enqueue_event(DS5_L2CAP_EVENT_DATA,
                            channel_id, channel_connections[channel_id],
                            channel_sessions[channel_id], 0, buffer->data,
                            buffer->len);

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
    channel_connections[DS5_L2CAP_CHANNEL_CONTROL] = conn;
    channel_sessions[DS5_L2CAP_CHANNEL_CONTROL] =
        session_conn == conn ? session_id : 0U;
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

    channel_connections[DS5_L2CAP_CHANNEL_INTERRUPT] = conn;
    channel_sessions[DS5_L2CAP_CHANNEL_INTERRUPT] =
        session_conn == conn ? session_id : 0U;
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
        session_conn = NULL;
        session_id = 0U;
        memset(channel_connections, 0, sizeof(channel_connections));
        memset(channel_sessions, 0, sizeof(channel_sessions));
        memset(completion_samples, 0, sizeof(completion_samples));
        audio_send_attempts = 0U;
        audio_send_accepted = 0U;
        audio_send_immediate_failures = 0U;
        audio_send_enobufs = 0U;
        max_audio_submit_interval_us = 0U;
        hci_zero_slot_observations = 0U;
        completion_samples_submitted = 0U;
        completion_samples_completed = 0U;
        last_completion_latency_us = 0U;
        max_completion_latency_us = 0U;
        average_completion_latency_us = 0U;
        hci_max_free_packets = 0U;
        hci_min_free_packets = UINT8_MAX;
        last_hci_free_packets = 0U;
        last_connection_tx_queue_depth = 0U;
        max_connection_tx_queue_depth = 0U;
        last_audio_submit_us = 0U;
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

void ds5_l2cap_set_session(struct bt_conn *conn, uint32_t session)
{
    session_conn = conn;
    session_id = session;
}

void ds5_l2cap_clear_session(struct bt_conn *conn)
{
    if ((conn == NULL) || (session_conn == conn)) {
        session_conn = NULL;
        session_id = 0U;
    }
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

    channel_connections[DS5_L2CAP_CHANNEL_CONTROL] = conn;
    channel_sessions[DS5_L2CAP_CHANNEL_CONTROL] =
        session_conn == conn ? session_id : 0U;
    outgoing_channel_sequence = true;
    err = bt_l2cap_chan_connect(conn, &control_channel.chan,
                                DS5_HID_CONTROL_PSM);
    if (err != 0) {
        outgoing_channel_sequence = false;
        channel_connections[DS5_L2CAP_CHANNEL_CONTROL] = NULL;
        channel_sessions[DS5_L2CAP_CHANNEL_CONTROL] = 0U;
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

static bool ds5_l2cap_abort_channel(struct bt_l2cap_br_chan *channel,
                                    ds5_l2cap_channel_t channel_id,
                                    struct bt_conn *terminated_connection)
{
    struct bt_conn *channel_connection = channel->chan.conn;

    if (channel_connection != NULL) {
        if ((terminated_connection != NULL) &&
            (channel_connection != terminated_connection)) {
            /* Never let an old ACL callback delete a channel of a new ACL. */
            return false;
        }

        /* A known live object can still be removed from the SDK channel list. */
        bt_l2cap_chan_remove(channel_connection, &channel->chan);

        /* This invokes the application disconnected callback and clears conn. */
        bt_l2cap_chan_del(&channel->chan);
    } else if ((terminated_connection != NULL) &&
               (channel_connections[channel_id] != terminated_connection)) {
        /* The stack may have detached the channel before the ACL callback. */
        return false;
    }

    if ((terminated_connection == NULL) ||
        (channel_connections[channel_id] == terminated_connection)) {
        channel_connections[channel_id] = NULL;
        channel_sessions[channel_id] = 0U;
    }

    return true;
}

void ds5_l2cap_abort_connection(struct bt_conn *conn)
{
    bool control_aborted;
    bool interrupt_aborted;

    control_aborted = ds5_l2cap_abort_channel(
        &control_channel, DS5_L2CAP_CHANNEL_CONTROL, conn);
    interrupt_aborted = ds5_l2cap_abort_channel(
        &interrupt_channel, DS5_L2CAP_CHANNEL_INTERRUPT, conn);

    if (control_aborted || interrupt_aborted) {
        outgoing_channel_sequence = false;
    }
    if (interrupt_aborted) {
        last_audio_submit_us = 0U;
    }
}

int ds5_l2cap_send(ds5_l2cap_channel_t channel, const uint8_t *data,
                   size_t length)
{
    struct bt_l2cap_br_chan *br_channel = ds5_l2cap_get_channel(channel);
    struct net_buf *buffer;
    ds5_l2cap_completion_sample_t *completion_sample = NULL;
    bool is_audio = (channel == DS5_L2CAP_CHANNEL_INTERRUPT) &&
                    (length == DS5_BT_HAPTICS_TRANSACTION_SIZE);
    bool callback_send = false;
    int result;

    if (is_audio) {
        ++audio_send_attempts;
    }

    if ((br_channel == NULL) || (data == NULL) || (length == 0U)) {
        if (is_audio) {
            ds5_l2cap_record_audio_failure(-EINVAL);
        }
        return -EINVAL;
    }

    if ((br_channel->chan.conn == NULL) ||
        (br_channel->chan.state != BT_L2CAP_CONNECTED)) {
        if (is_audio) {
            ds5_l2cap_record_audio_failure(-ENOTCONN);
        }
        return -ENOTCONN;
    }

    if (is_audio) {
        ds5_l2cap_observe_audio_submission(br_channel->chan.conn);
    }

    if ((length > br_channel->tx.mtu) ||
        (length > CONFIG_BT_L2CAP_TX_MTU)) {
        if (is_audio) {
            ds5_l2cap_record_audio_failure(-EMSGSIZE);
        }
        return -EMSGSIZE;
    }

    buffer = bt_conn_create_pdu_timeout(NULL, BT_L2CAP_HDR_SIZE, K_NO_WAIT);
    if (buffer == NULL) {
        if (is_audio) {
            ds5_l2cap_record_audio_failure(-ENOBUFS);
        }
        return -ENOBUFS;
    }

    if (net_buf_tailroom(buffer) < length) {
        net_buf_unref(buffer);
        if (is_audio) {
            ds5_l2cap_record_audio_failure(-EMSGSIZE);
        }
        return -EMSGSIZE;
    }

    net_buf_add_mem(buffer, data, length);
    if (is_audio &&
        ((audio_send_attempts % DS5_L2CAP_COMPLETION_SAMPLE_INTERVAL) == 1U)) {
        completion_sample = ds5_l2cap_claim_completion_sample();
    }

    if (completion_sample != NULL) {
        callback_send = true;
        result = bt_l2cap_send_cb(br_channel->chan.conn,
                                  br_channel->tx.cid, buffer,
                                  ds5_l2cap_audio_completed,
                                  completion_sample);
        if (result >= 0) {
            ++completion_samples_submitted;
        } else {
            completion_sample->pending = false;
        }
    } else {
        result = bt_l2cap_chan_send(&br_channel->chan, buffer);
    }

    if ((result < 0) && !callback_send) {
        /* BR/EDR rejects before queueing on every negative return path. */
        net_buf_unref(buffer);
    }

    if (is_audio) {
        if (result < 0) {
            ds5_l2cap_record_audio_failure(result);
        } else {
            ++audio_send_accepted;
        }
    }

    return result;
}

void ds5_l2cap_get_diagnostics(ds5_l2cap_diagnostics_t *diagnostics)
{
    size_t index;
    uint8_t busy_slots = 0U;

    if (diagnostics == NULL) {
        return;
    }

    for (index = 0U; index < DS5_L2CAP_COMPLETION_SAMPLE_SLOTS; ++index) {
        if (completion_samples[index].pending) {
            ++busy_slots;
        }
    }

    diagnostics->hci_br_acl_mtu = bt_dev.br.mtu;
    diagnostics->hci_free_packets = last_hci_free_packets;
    diagnostics->hci_max_free_packets = hci_max_free_packets;
    diagnostics->hci_min_free_packets =
        hci_min_free_packets == UINT8_MAX ? last_hci_free_packets :
                                            hci_min_free_packets;
    diagnostics->connection_tx_queue_depth =
        last_connection_tx_queue_depth;
    diagnostics->max_connection_tx_queue_depth =
        max_connection_tx_queue_depth;
    diagnostics->completion_sample_slots_busy = busy_slots;
    diagnostics->audio_send_attempts = audio_send_attempts;
    diagnostics->audio_send_accepted = audio_send_accepted;
    diagnostics->audio_send_immediate_failures =
        audio_send_immediate_failures;
    diagnostics->audio_send_enobufs = audio_send_enobufs;
    diagnostics->max_audio_submit_interval_us =
        max_audio_submit_interval_us;
    diagnostics->hci_zero_slot_observations =
        hci_zero_slot_observations;
    diagnostics->completion_samples_submitted =
        completion_samples_submitted;
    diagnostics->completion_samples_completed =
        completion_samples_completed;
    diagnostics->last_completion_latency_us =
        last_completion_latency_us;
    diagnostics->max_completion_latency_us =
        max_completion_latency_us;
    diagnostics->average_completion_latency_us =
        average_completion_latency_us;
}

bool ds5_l2cap_event_try_receive(ds5_l2cap_event_t *event)
{
    if ((event_queue == NULL) || (event == NULL)) {
        return false;
    }

    return xQueueReceive(event_queue, event, 0U) == pdPASS;
}

bool ds5_l2cap_event_receive(ds5_l2cap_event_t *event)
{
    if ((event_queue == NULL) || (event == NULL)) {
        return false;
    }

    return xQueueReceive(event_queue, event, portMAX_DELAY) == pdPASS;
}

bool ds5_l2cap_event_receive_timeout(ds5_l2cap_event_t *event,
                                     uint32_t timeout_ms)
{
    if ((event_queue == NULL) || (event == NULL)) {
        return false;
    }

    return xQueueReceive(event_queue, event,
                         pdMS_TO_TICKS(timeout_ms)) == pdPASS;
}

uint32_t ds5_l2cap_dropped_event_count(void)
{
    return dropped_event_count;
}
