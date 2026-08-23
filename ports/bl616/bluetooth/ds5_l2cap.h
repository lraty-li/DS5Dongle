#ifndef DS5_L2CAP_H
#define DS5_L2CAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t hci_br_acl_mtu;
    uint8_t hci_free_packets;
    uint8_t hci_max_free_packets;
    uint8_t hci_min_free_packets;
    uint8_t connection_tx_queue_depth;
    uint8_t max_connection_tx_queue_depth;
    uint8_t completion_sample_slots_busy;
    uint32_t audio_send_attempts;
    uint32_t audio_send_accepted;
    uint32_t audio_send_immediate_failures;
    uint32_t audio_send_enobufs;
    uint32_t max_audio_submit_interval_us;
    uint32_t hci_zero_slot_observations;
    uint32_t completion_samples_submitted;
    uint32_t completion_samples_completed;
    uint32_t last_completion_latency_us;
    uint32_t max_completion_latency_us;
    uint32_t average_completion_latency_us;
} ds5_l2cap_diagnostics_t;

#define DS5_HID_CONTROL_PSM          0x0011U
#define DS5_HID_INTERRUPT_PSM        0x0013U
#define DS5_L2CAP_MTU                672U
/* The 0x39 DualSense audio report is 547 bytes before its L2CAP header. */
#define DS5_L2CAP_MAX_EVENT_PAYLOAD  DS5_L2CAP_MTU

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
    struct bt_conn *conn;
    uint32_t session;
    int status;
    size_t length;
    uint8_t data[DS5_L2CAP_MAX_EVENT_PAYLOAD];
} ds5_l2cap_event_t;

/* Register the BR/EDR HID servers. Call only after bt_enable is ready. */
int ds5_l2cap_init(void);

/* Associate future channel callbacks with the current ACL session. */
void ds5_l2cap_set_session(struct bt_conn *conn, uint32_t session);
void ds5_l2cap_clear_session(struct bt_conn *conn);

/* Start the Control channel; Interrupt follows after Control is connected. */
int ds5_l2cap_connect(struct bt_conn *conn);
int ds5_l2cap_disconnect(void);

/*
 * Force local channel teardown after a BR/EDR ACL has already terminated.
 * A non-NULL conn limits cleanup to that ACL; NULL explicitly means all.
 */
void ds5_l2cap_abort_connection(struct bt_conn *conn);

/*
 * Allocate and send from task context. The SDK takes ownership of its net_buf
 * only after bt_l2cap_chan_send accepts the packet. Returns a non-negative
 * byte count on success, matching the pinned SDK, or a negative error code.
 */
int ds5_l2cap_send(ds5_l2cap_channel_t channel, const uint8_t *data,
                   size_t length);

/* Non-blocking handoff for a future Bluetooth worker task. */
bool ds5_l2cap_event_try_receive(ds5_l2cap_event_t *event);
bool ds5_l2cap_event_receive(ds5_l2cap_event_t *event);
bool ds5_l2cap_event_receive_timeout(ds5_l2cap_event_t *event,
                                     uint32_t timeout_ms);
uint32_t ds5_l2cap_dropped_event_count(void);
void ds5_l2cap_get_diagnostics(ds5_l2cap_diagnostics_t *diagnostics);

#ifdef __cplusplus
}
#endif

#endif
