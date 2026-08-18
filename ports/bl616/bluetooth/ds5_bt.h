#ifndef DS5_BT_H
#define DS5_BT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DS5_BT_STATE_OFF = 0,
    DS5_BT_STATE_DISCOVERING = 1,
    DS5_BT_STATE_IDLE = 2,
    DS5_BT_STATE_CANDIDATE_READY = 3,
    /* Value 4 was RECONNECT_WAIT in diagnostic protocol version 1. */
    DS5_BT_STATE_ACL_CONNECTING = 5,
    DS5_BT_STATE_SECURING = 6,
    DS5_BT_STATE_L2CAP_CONNECTING = 7,
    DS5_BT_STATE_READY = 8,
    DS5_BT_STATE_DISCONNECTING = 9,
} ds5_bt_state_t;

typedef struct {
    ds5_bt_state_t state;
    bool control_channel_ready;
    bool interrupt_channel_ready;
    bool bonded_peer_valid;
    bool discovery_active;
    bool pairing_window_active;
    uint32_t link_generation;
    uint16_t worker_task_stack_high_water_words;
    uint16_t tx_worker_task_stack_high_water_words;
} ds5_bt_diagnostics_t;

int ds5_bt_init(void);
/* Wake the Bluetooth TX worker after a mailbox or link-state change. */
void ds5_bt_tx_wake(void);
int ds5_bt_start_discovery(void);
bool ds5_bt_candidate_available(void);
int ds5_bt_connect_candidate(void);
int ds5_bt_disconnect(void);
/* Task-context API; clears every BR/EDR bond owned by this firmware. */
int ds5_bt_clear_pairing(void);
ds5_bt_state_t ds5_bt_get_state(void);
uint32_t ds5_bt_received_l2cap_packet_count(void);
void ds5_bt_get_diagnostics(ds5_bt_diagnostics_t *diagnostics);

/* Start an active BR/EDR pairing-discovery window.  A bonded peer remains
 * passively connectable while this window is running. */
int ds5_bt_fallback_to_discovery(void);

#ifdef __cplusplus
}
#endif

#endif
