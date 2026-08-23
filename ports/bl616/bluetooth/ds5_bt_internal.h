#ifndef DS5_BT_INTERNAL_H
#define DS5_BT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <FreeRTOS.h>
#include "bluetooth.h"
#include "semphr.h"
#include "task.h"

#include "ds5_bt.h"
#include "ds5_protocol.h"

#define DS5_BT_DISCOVERY_RESULT_COUNT 10U
#define DS5_BT_DISCOVERY_LENGTH       0x05U
#define DS5_BT_CANDIDATE_NAME_SIZE    64U
#define DS5_BT_WORKER_STACK_DEPTH     (configMINIMAL_STACK_SIZE * 4U)
#define DS5_BT_TX_WORKER_STACK_DEPTH  (configMINIMAL_STACK_SIZE * 6U)
#define DS5_BT_POLICY_STACK_DEPTH     (configMINIMAL_STACK_SIZE * 3U)
#define DS5_BT_OUTPUT_READY_TIMEOUT_MS 5000U
#define DS5_BT_TX_RETRY_MS             1U
#define DS5_BT_FEATURE_RESPONSE_TIMEOUT_MS 500U
#define DS5_BT_FEATURE_RETRY_MS          20U
#define DS5_BT_FEATURE_MAX_ATTEMPTS       3U
#define DS5_BT_DEFAULT_MIC_SELECT      0U
#define DS5_BT_PAIRING_WINDOW_MS       20000U
#define DS5_BT_DISCOVERY_RETRY_MS      750U
#define DS5_BT_ACL_TIMEOUT_MS          10000U
#define DS5_BT_SECURITY_TIMEOUT_MS    5000U
#define DS5_BT_L2CAP_TIMEOUT_MS       5000U
#define DS5_BT_DISCONNECT_TIMEOUT_MS  5000U
#define DS5_BT_LINK_STATE_POLL_MS      500U
#define DS5_BT_ACL_SUPERVISION_TIMEOUT_MS 10000U
#define DS5_BT_ACL_SUPERVISION_TIMEOUT_SLOTS 16000U
#define DS5_BT_PAGE_SCAN_RETRY_MS     1000U

typedef struct {
    bool valid;
    bt_addr_t address;
    uint32_t device_class;
    int8_t rssi;
    uint16_t score;
    char name[DS5_BT_CANDIDATE_NAME_SIZE];
} ds5_bt_candidate_t;

extern ds5_bt_candidate_t candidate;
extern bt_addr_t bonded_peer_address;
extern volatile bool bonded_peer_valid;

extern struct bt_conn *active_connection;
extern bool active_peer_is_bonded;
extern bool active_peer_should_persist_bond;
extern bool outgoing_create_pending;
extern bt_addr_t outgoing_target_address;

extern volatile ds5_bt_state_t bluetooth_state;
extern bool outgoing_acl;
extern bool control_channel_ready;
extern bool interrupt_channel_ready;
extern bool calibration_response_received;
extern bool feature_request_pending;
extern bool feature_prefetch_complete;
extern bool feature_prefetch_failed;
extern size_t feature_prefetch_index;
extern uint8_t feature_request_attempts;
extern TickType_t feature_request_deadline;
extern TickType_t feature_request_retry_at;
extern uint32_t feature_prefetch_retries;
extern uint32_t feature_prefetch_timeouts;
extern uint32_t feature_prefetch_failures;
extern volatile uint32_t received_l2cap_packets;
extern uint32_t received_control_packets;
extern uint32_t received_interrupt_packets;
extern uint32_t valid_input_reports;
extern uint32_t invalid_input_reports;
extern uint32_t input_mailbox_publish_failures;
extern uint32_t transmitted_output_reports;
extern uint32_t failed_output_reports;
extern uint32_t transmitted_haptics_reports;
extern uint32_t failed_haptics_reports;
extern uint32_t discarded_haptics_not_ready;
extern uint32_t received_microphone_packets;
extern uint32_t rejected_microphone_packets;
extern uint32_t transmitted_microphone_status_reports;
extern uint32_t failed_microphone_status_reports;
extern uint32_t received_microphone_button_presses;
extern uint32_t dropped_microphone_button_presses;
extern uint32_t transmitted_microphone_mute_reports;
extern uint32_t failed_microphone_mute_reports;
extern uint32_t transmitted_feature_set_reports;
extern uint32_t failed_feature_set_reports;
extern uint8_t latest_usb_input_payload[DS5_USB_INPUT_PAYLOAD_SIZE];
extern uint32_t failed_initialization_reports;
extern bool initialization_pending;
extern bool headset_connected;
extern bool microphone_button_pressed;
extern uint32_t link_generation;

extern volatile bool discovery_in_flight;
extern volatile bool discovery_allowed;
extern volatile bool discovery_stop_requested;
extern volatile bool pairing_window_active;
extern TickType_t pairing_window_deadline;
extern TickType_t discovery_retry_at;
extern TickType_t candidate_retry_at;
extern TickType_t link_deadline;
extern TickType_t bond_recovery_retry_at;
extern TickType_t page_scan_retry_at;
extern uint32_t discovery_session;
extern volatile bool bond_recovery_pending;
extern bool page_scan_restore_pending;

extern StaticTask_t worker_task_storage;
extern StackType_t worker_task_stack[DS5_BT_WORKER_STACK_DEPTH];
extern TaskHandle_t worker_task;
extern StaticTask_t tx_worker_task_storage;
extern StackType_t tx_worker_task_stack[DS5_BT_TX_WORKER_STACK_DEPTH];
extern TaskHandle_t tx_worker_task;
extern StaticTask_t policy_task_storage;
extern StackType_t policy_task_stack[DS5_BT_POLICY_STACK_DEPTH];
extern TaskHandle_t policy_task;
extern StaticSemaphore_t lifecycle_mutex_storage;
extern SemaphoreHandle_t lifecycle_mutex;

void ds5_bt_lifecycle_lock(void);
void ds5_bt_lifecycle_unlock(void);
void ds5_bt_policy_wake(void);
void ds5_bt_tx_wake(void);
int ds5_bt_liveness_init(void);
bool ds5_bt_enqueue_disconnected_event(struct bt_conn *conn,
                                       uint8_t reason);
void ds5_bt_process_link_events(void);
void ds5_bt_check_link_liveness(void);
void ds5_bt_configure_link_supervision_timeout(void);
const char *ds5_bt_state_name(ds5_bt_state_t state);
void ds5_bt_set_state(ds5_bt_state_t state);

bool ds5_bt_connection_matches_saved_peer(const struct bt_conn *conn);
bool ds5_bt_connection_matches_outgoing_target(const struct bt_conn *conn);
bool ds5_bt_claim_active_connection(struct bt_conn *conn, bool outgoing,
                                    bool bonded, bool persist_bond);
void ds5_bt_enable_bonded_page_scan(void);
void ds5_bt_disable_page_scan(void);
int ds5_bt_forget_bonded_peer(void);
void ds5_bt_close_pairing_window(void);
void ds5_bt_open_pairing_window(void);
void ds5_bt_reset_link_state(void);
void ds5_bt_recover_after_link(bool bonded_link);
int ds5_bt_disconnect_active(uint8_t reason);
int ds5_bt_register_callbacks(void);

void ds5_bt_restore_bond(const struct bt_br_bond_info *info,
                         void *user_data);
int ds5_bt_start_discovery_internal(void);

void ds5_bt_worker(void *parameter);
void ds5_bt_tx_worker(void *parameter);
void ds5_bt_policy_worker(void *parameter);

#endif
