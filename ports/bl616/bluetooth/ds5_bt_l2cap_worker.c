#include <stdio.h>

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

static int ds5_bt_request_next_feature(void)
{
    uint8_t transaction[DS5_FEATURE_GET_TRANSACTION_SIZE];
    uint8_t report_id;
    size_t transaction_length;
    ds5_protocol_result_t protocol_result;
    int err;

    if (!control_channel_ready || feature_request_pending ||
        (feature_prefetch_index >=
         (sizeof(feature_prefetch_ids) / sizeof(feature_prefetch_ids[0])))) {
        return 0;
    }

    report_id = feature_prefetch_ids[feature_prefetch_index];
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
    printf("DS5 BT: Feature 0x%02x requested (%u/%u)\r\n",
           (unsigned int)report_id,
           (unsigned int)(feature_prefetch_index + 1U),
           (unsigned int)(sizeof(feature_prefetch_ids) /
                          sizeof(feature_prefetch_ids[0])));
    return 0;
}
static void ds5_bt_process_l2cap_event(const ds5_l2cap_event_t *event)
{
    ds5_protocol_result_t protocol_result;

    if (event == NULL) {
        return;
    }

    ds5_bt_lifecycle_lock();
    if ((event->conn != active_connection) ||
        (event->session != link_generation)) {
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
            int request_err = ds5_bt_request_next_feature();

            if (request_err != 0) {
                printf("DS5 BT: Feature prefetch request failed "
                       "(err %d)\r\n",
                       request_err);
            }
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
            uint8_t report_id;

            ++received_control_packets;
            if ((event->length >= 2U) &&
                (event->data[0] == DS5_FEATURE_DATA_HEADER)) {
                report_id = event->data[1];
                if (ds5_feature_cache_store(report_id, &event->data[2],
                                            event->length - 2U)) {
                    printf("DS5 BT: Feature 0x%02x cached (length %u)\r\n",
                           (unsigned int)report_id,
                           (unsigned int)(event->length - 2U));
                }

                if ((report_id == DS5_FEATURE_CALIBRATION_REPORT_ID) &&
                    !calibration_response_received) {
                    printf("DS5 BT: calibration Feature 0x05 response "
                           "received (length %u)\r\n",
                           (unsigned int)event->length);
                    calibration_response_received = true;
                }

                if (feature_request_pending &&
                    (feature_prefetch_index <
                     (sizeof(feature_prefetch_ids) /
                      sizeof(feature_prefetch_ids[0]))) &&
                    (report_id == feature_prefetch_ids[feature_prefetch_index])) {
                    int request_err;

                    feature_request_pending = false;
                    ++feature_prefetch_index;
                    request_err = ds5_bt_request_next_feature();
                    if (request_err != 0) {
                        printf("DS5 BT: Feature prefetch request failed "
                               "(err %d)\r\n",
                               request_err);
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
}

void ds5_bt_worker(void *parameter)
{
    ds5_l2cap_event_t event;

    (void)parameter;

    while (1) {
        if (ds5_l2cap_event_receive(&event)) {
            ds5_bt_process_l2cap_event(&event);
        }
    }
}
