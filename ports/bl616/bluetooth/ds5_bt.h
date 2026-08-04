#ifndef DS5_BT_H
#define DS5_BT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DS5_BT_STATE_OFF = 0,
    DS5_BT_STATE_DISCOVERING,
    DS5_BT_STATE_IDLE,
    DS5_BT_STATE_CANDIDATE_READY,
    DS5_BT_STATE_RECONNECT_WAIT,
    DS5_BT_STATE_ACL_CONNECTING,
    DS5_BT_STATE_SECURING,
    DS5_BT_STATE_L2CAP_CONNECTING,
    DS5_BT_STATE_READY,
    DS5_BT_STATE_DISCONNECTING,
} ds5_bt_state_t;

int ds5_bt_init(void);
int ds5_bt_start_discovery(void);
bool ds5_bt_candidate_available(void);
int ds5_bt_connect_candidate(void);
int ds5_bt_disconnect(void);
/* Task-context API; clears every BR/EDR bond owned by this firmware. */
int ds5_bt_clear_pairing(void);
ds5_bt_state_t ds5_bt_get_state(void);
uint32_t ds5_bt_received_l2cap_packet_count(void);

#ifdef __cplusplus
}
#endif

#endif
