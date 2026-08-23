#include <errno.h>
#include <stdio.h>

#include "conn.h"
#include "conn_internal.h"
#include "ds5_bt_internal.h"
#include "ds5_audio_mailbox.h"
#include "ds5_feature_cache.h"
#include "ds5_feature_set_mailbox.h"
#include "ds5_input_mailbox.h"
#include "ds5_l2cap.h"
#include "ds5_output_mailbox.h"
#include "ds5_protocol.h"
#include "hci_err.h"
#include "ds5_log.h"

#define printf ds5_log_printf

static const uint8_t feature_prefetch_ids[] = {
    0x09U,
    0x20U,
    0x22U,
    0x05U,
};

#define DS5_BT_FEATURE_PREFETCH_COUNT \
    (sizeof(feature_prefetch_ids) / sizeof(feature_prefetch_ids[0]))

static bool ds5_bt_feature_deadline_expired(TickType_t now,
                                             TickType_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void ds5_bt_handle_feature_request_failure(uint8_t report_id,
                                                   int err)
{
    feature_request_pending = false;
    feature_request_deadline = 0U;

    if (feature_request_attempts < DS5_BT_FEATURE_MAX_ATTEMPTS) {
        ++feature_prefetch_retries;
        feature_request_retry_at = xTaskGetTickCount() +
            pdMS_TO_TICKS(DS5_BT_FEATURE_RETRY_MS);
        printf("DS5 BT: Feature 0x%02x attempt %u/%u failed "
               "(err %d); retrying\r\n",
               (unsigned int)report_id,
               (unsigned int)feature_request_attempts,
               (unsigned int)DS5_BT_FEATURE_MAX_ATTEMPTS, err);
        return;
    }

    ++feature_prefetch_failures;
    feature_prefetch_failed = true;
    printf("DS5 BT: Feature 0x%02x abandoned after %u attempt(s) "
           "(err %d)\r\n",
           (unsigned int)report_id,
           (unsigned int)feature_request_attempts, err);
    ++feature_prefetch_index;
    feature_request_attempts = 0U;
    feature_request_retry_at = 0U;
    if (feature_prefetch_index >= DS5_BT_FEATURE_PREFETCH_COUNT) {
        feature_prefetch_complete = true;
    }
}

static int ds5_bt_request_next_feature(void)
{
    uint8_t transaction[DS5_FEATURE_GET_TRANSACTION_SIZE];
    uint8_t report_id;
    size_t transaction_length;
    ds5_protocol_result_t protocol_result;
    int err;

    if (!control_channel_ready || feature_request_pending ||
        (feature_prefetch_index >= DS5_BT_FEATURE_PREFETCH_COUNT)) {
        return 0;
    }

    report_id = feature_prefetch_ids[feature_prefetch_index];
    ++feature_request_attempts;
    protocol_result = ds5_build_feature_get_transaction(
        report_id,
        transaction, sizeof(transaction), &transaction_length);
    if (protocol_result != DS5_PROTOCOL_OK) {
        return (int)protocol_result;
    }

    err = ds5_l2cap_send(DS5_L2CAP_CHANNEL_CONTROL,
                         transaction, transaction_length);
    if (err < 0) {
        return err;
    }

    feature_request_pending = true;
    feature_request_retry_at = 0U;
    feature_request_deadline = xTaskGetTickCount() +
        pdMS_TO_TICKS(DS5_BT_FEATURE_RESPONSE_TIMEOUT_MS);
    printf("DS5 BT: Feature 0x%02x requested (%u/%u, attempt %u/%u)\r\n",
           (unsigned int)report_id,
           (unsigned int)(feature_prefetch_index + 1U),
           (unsigned int)DS5_BT_FEATURE_PREFETCH_COUNT,
           (unsigned int)feature_request_attempts,
           (unsigned int)DS5_BT_FEATURE_MAX_ATTEMPTS);
    return 0;
}

static void ds5_bt_service_feature_prefetch(void)
{
    TickType_t now;
    uint8_t report_id;
    int err;

    if ((bluetooth_state != DS5_BT_STATE_READY) ||
        !control_channel_ready || !interrupt_channel_ready ||
        feature_prefetch_complete) {
        return;
    }
    if (feature_prefetch_index >= DS5_BT_FEATURE_PREFETCH_COUNT) {
        feature_prefetch_complete = true;
        return;
    }

    now = xTaskGetTickCount();
    if (feature_request_pending) {
        if (!ds5_bt_feature_deadline_expired(
                now, feature_request_deadline)) {
            return;
        }

        report_id = feature_prefetch_ids[feature_prefetch_index];
        ++feature_prefetch_timeouts;
        ds5_bt_handle_feature_request_failure(report_id, -ETIMEDOUT);
    }

    while ((bluetooth_state == DS5_BT_STATE_READY) &&
           control_channel_ready && interrupt_channel_ready &&
           !feature_prefetch_complete && !feature_request_pending) {
        if (feature_prefetch_index >= DS5_BT_FEATURE_PREFETCH_COUNT) {
            feature_prefetch_complete = true;
            return;
        }

        now = xTaskGetTickCount();
        if ((feature_request_attempts != 0U) &&
            !ds5_bt_feature_deadline_expired(
                now, feature_request_retry_at)) {
            return;
        }

        report_id = feature_prefetch_ids[feature_prefetch_index];
        err = ds5_bt_request_next_feature();
        if (err == 0) {
            return;
        }

        ds5_bt_handle_feature_request_failure(report_id, err);
    }
}

static bool ds5_bt_feature_prefetch_active(void)
{
    return (bluetooth_state == DS5_BT_STATE_READY) &&
           control_channel_ready && interrupt_channel_ready &&
           !feature_prefetch_complete;
}

static void ds5_bt_process_l2cap_event(const ds5_l2cap_event_t *event)
{
    ds5_protocol_result_t protocol_result;

    if (event == NULL) {
        return;
    }

    ds5_bt_lifecycle_lock();
    if ((event->conn != active_connection) ||
        (event->session != link_generation) ||
        (event->conn == NULL) ||
        (event->conn->state != BT_CONN_CONNECTED)) {
        goto out;
    }

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
            initialization_pending = true;
            ds5_bt_set_state(DS5_BT_STATE_READY);
        }
        ds5_bt_tx_wake();
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
        ds5_bt_tx_wake();
        break;
    case DS5_L2CAP_EVENT_DATA:
        ++received_l2cap_packets;
        if (event->channel == DS5_L2CAP_CHANNEL_CONTROL) {
            bool feature_cached;
            uint8_t report_id;

            ++received_control_packets;
            if ((event->length >= 2U) &&
                (event->data[0] == DS5_FEATURE_DATA_HEADER)) {
                report_id = event->data[1];
                feature_cached = ds5_feature_cache_store(
                    report_id, &event->data[2], event->length - 2U);
                if (feature_cached) {
                    printf("DS5 BT: Feature 0x%02x cached (length %u)\r\n",
                           (unsigned int)report_id,
                           (unsigned int)(event->length - 2U));
                }

                if (feature_cached &&
                    (report_id == DS5_FEATURE_CALIBRATION_REPORT_ID) &&
                    !calibration_response_received) {
                    printf("DS5 BT: calibration Feature 0x05 response "
                           "received (length %u)\r\n",
                           (unsigned int)event->length);
                    calibration_response_received = true;
                }

                if (feature_cached &&
                    (feature_prefetch_index <
                     DS5_BT_FEATURE_PREFETCH_COUNT) &&
                    (report_id ==
                     feature_prefetch_ids[feature_prefetch_index])) {
                    feature_request_pending = false;
                    feature_request_deadline = 0U;
                    feature_request_retry_at = 0U;
                    feature_request_attempts = 0U;
                    ++feature_prefetch_index;
                    if (feature_prefetch_index >=
                        DS5_BT_FEATURE_PREFETCH_COUNT) {
                        feature_prefetch_complete = true;
                        printf("DS5 BT: Feature prefetch complete\r\n");
                    }
                }
            } else if (received_control_packets <= 4U) {
                if (event->length >= 2U) {
                    printf("DS5 BT: unhandled HID Control packet "
                           "(length %u, prefix %02x %02x)\r\n",
                           (unsigned int)event->length,
                           (unsigned int)event->data[0],
                           (unsigned int)event->data[1]);
                } else {
                    printf("DS5 BT: short HID Control packet "
                           "(length %u)\r\n",
                           (unsigned int)event->length);
                }
            }
            break;
        }

        ++received_interrupt_packets;
        if (received_interrupt_packets == 1U) {
            if (event->length >= 3U) {
                printf("DS5 BT: first HID Interrupt packet length %u, "
                       "prefix %02x %02x %02x\r\n",
                       (unsigned int)event->length,
                       (unsigned int)event->data[0],
                       (unsigned int)event->data[1],
                       (unsigned int)event->data[2]);
            } else {
                printf("DS5 BT: first HID Interrupt packet is short "
                       "(length %u)\r\n",
                       (unsigned int)event->length);
            }
        }

        /*
         * The controller marks an Opus microphone payload in byte 2 of its
         * 0x31 Interrupt report.  It starts at byte 4, not at the normal USB
         * HID input payload offset.  Keep decode work out of the Bluetooth
         * worker by handing the fixed-size frame to the audio task.
         */
        if ((event->length >= 3U) &&
            (event->data[0] == DS5_BT_INPUT_TRANSACTION_HEADER) &&
            (event->data[1] == DS5_BT_INPUT_REPORT_ID) &&
            ((event->data[2] & 0x02U) != 0U)) {
            if (event->length < (4U + DS5_AUDIO_MIC_OPUS_SIZE)) {
                ++rejected_microphone_packets;
                if (rejected_microphone_packets <= 4U) {
                    printf("DS5 BT: short microphone packet (length %u)\r\n",
                           (unsigned int)event->length);
                }
            } else if (!ds5_audio_mailbox_publish_microphone_opus(
                           &event->data[4], DS5_AUDIO_MIC_OPUS_SIZE)) {
                ++rejected_microphone_packets;
                if (rejected_microphone_packets <= 4U) {
                    printf("DS5 BT: microphone audio mailbox full\r\n");
                }
            } else {
                ++received_microphone_packets;
                if (received_microphone_packets == 1U) {
                    printf("DS5 BT: first microphone Opus frame queued\r\n");
                }
            }
            break;
        }

        protocol_result = ds5_extract_usb_input_payload(
            event->data, event->length, latest_usb_input_payload,
            sizeof(latest_usb_input_payload));
        if (protocol_result == DS5_PROTOCOL_OK) {
            ++valid_input_reports;
            if (!ds5_input_mailbox_publish(latest_usb_input_payload,
                                           sizeof(latest_usb_input_payload))) {
                ++input_mailbox_publish_failures;
                if (input_mailbox_publish_failures <= 4U) {
                    printf("DS5 BT: input mailbox publish failed (%lu)\r\n",
                           (unsigned long)input_mailbox_publish_failures);
                }
            }
            if (valid_input_reports == 1U) {
                printf("DS5 BT: first valid 0x31 input report accepted "
                       "(length %u)\r\n",
                       (unsigned int)event->length);
            }

            /* Original src/main.cpp uses report byte 53 bit 0 for routing. */
            headset_connected =
                (latest_usb_input_payload[53U] & 0x01U) != 0U;

            /*
             * A DualSense reports the microphone button as a momentary HID
             * input.  The host must toggle both the hardware mute state and
             * its yellow LED on the rising edge; the controller does not do
             * that state machine for us while bridged over Bluetooth.
             */
            {
                bool pressed =
                    (latest_usb_input_payload[
                         DS5_USB_INPUT_BUTTONS2_OFFSET] &
                     DS5_USB_INPUT_MIC_BUTTON_MASK) != 0U;

                if (pressed && !microphone_button_pressed) {
                    if (ds5_output_mailbox_publish_microphone_button_press()) {
                        ++received_microphone_button_presses;
                        ds5_bt_tx_wake();
                    } else {
                        ++dropped_microphone_button_presses;
                        if (dropped_microphone_button_presses <= 4U) {
                            printf("DS5 BT: microphone button event queue "
                                   "full\r\n");
                        }
                    }
                }
                microphone_button_pressed = pressed;
            }

        } else {
            ++invalid_input_reports;
            if (invalid_input_reports <= 4U) {
                printf("DS5 BT: rejected HID Interrupt packet "
                       "(result %d, length %u)\r\n",
                       (int)protocol_result,
                       (unsigned int)event->length);
            }
        }
        break;
    case DS5_L2CAP_EVENT_ERROR:
        printf("DS5 BT: L2CAP channel %u failed (err %d)\r\n",
               (unsigned int)event->channel, event->status);
        if (active_connection != NULL) {
            (void)ds5_bt_disconnect_active(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
        ds5_bt_tx_wake();
        break;
    default:
        break;
    }

out:
    ds5_bt_lifecycle_unlock();
    if (event->conn != NULL) {
        bt_conn_unref(event->conn);
    }
}

void ds5_bt_worker(void *parameter)
{
    ds5_l2cap_event_t event;

    (void)parameter;

    while (1) {
        bool prefetch_active;
        bool event_received;

        ds5_bt_lifecycle_lock();
        prefetch_active = ds5_bt_feature_prefetch_active();
        ds5_bt_lifecycle_unlock();

        if (prefetch_active) {
            event_received = ds5_l2cap_event_receive_timeout(
                &event, DS5_BT_FEATURE_RETRY_MS);
        } else {
            event_received = ds5_l2cap_event_receive(&event);
        }

        if (event_received) {
            ds5_bt_process_l2cap_event(&event);
        }

        ds5_bt_lifecycle_lock();
        ds5_bt_service_feature_prefetch();
        ds5_bt_lifecycle_unlock();
    }
}
