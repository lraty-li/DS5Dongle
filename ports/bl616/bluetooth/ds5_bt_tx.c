#include <stdio.h>
#include <string.h>

#include "ds5_bt_internal.h"
#include "ds5_audio_mailbox.h"
#include "ds5_feature_set_mailbox.h"
#include "ds5_haptics_mailbox.h"
#include "ds5_l2cap.h"
#include "ds5_output_mailbox.h"
#include "ds5_protocol.h"
#include "ds5_log.h"

#define printf ds5_log_printf

typedef struct {
    bool control_ready;
    bool interrupt_ready;
    bool initialization_pending;
    bool headset_connected;
    uint32_t link_generation;
} ds5_bt_tx_link_state_t;

static void ds5_bt_get_tx_link_state(ds5_bt_tx_link_state_t *state)
{
    if (state == NULL) {
        return;
    }

    ds5_bt_lifecycle_lock();
    state->control_ready = control_channel_ready;
    state->interrupt_ready =
        (bluetooth_state == DS5_BT_STATE_READY) &&
        interrupt_channel_ready;
    state->initialization_pending = initialization_pending;
    state->headset_connected = headset_connected;
    state->link_generation = link_generation;
    ds5_bt_lifecycle_unlock();
}
static bool ds5_bt_complete_initialization(uint32_t expected_generation)
{
    bool completed = false;

    ds5_bt_lifecycle_lock();
    if ((link_generation == expected_generation) &&
        initialization_pending) {
        initialization_pending = false;
        completed = true;
    }
    ds5_bt_lifecycle_unlock();

    return completed;
}

static bool ds5_bt_forward_usb_output(
    ds5_output_sequence_t *sequence,
    const uint8_t *usb_report,
    uint8_t *bt_transaction,
    size_t bt_transaction_capacity)
{
    size_t transaction_length;
    ds5_protocol_result_t protocol_result;
    ds5_output_sequence_t previous_sequence = *sequence;
    int send_result;

    if (usb_report == NULL) {
        ++failed_output_reports;
        return false;
    }

    protocol_result = ds5_build_bt_output_transaction(
        sequence, usb_report, DS5_USB_OUTPUT_REPORT_SIZE,
        bt_transaction, bt_transaction_capacity, &transaction_length);
    if (protocol_result != DS5_PROTOCOL_OK) {
        ++failed_output_reports;
        if (failed_output_reports <= 4U) {
            printf("DS5 BT: USB output report rejected (result %d)\r\n",
                   (int)protocol_result);
        }
        return false;
    }

    send_result = ds5_l2cap_send(DS5_L2CAP_CHANNEL_INTERRUPT,
                                 bt_transaction, transaction_length);
    if (send_result < 0) {
        *sequence = previous_sequence;
        ++failed_output_reports;
        if (failed_output_reports <= 4U) {
            printf("DS5 BT: HID output send failed (err %d)\r\n",
                   send_result);
        }
        return false;
    }

    ++transmitted_output_reports;
    if (transmitted_output_reports == 1U) {
        printf("DS5 BT: first USB output report forwarded\r\n");
    }

    return true;
}

static bool ds5_bt_send_microphone_mute(
    ds5_output_sequence_t *sequence,
    bool muted,
    uint8_t *usb_report,
    size_t usb_report_capacity,
    uint8_t *bt_transaction,
    size_t bt_transaction_capacity)
{
    ds5_protocol_result_t protocol_result;

    protocol_result = ds5_build_usb_microphone_mute_report(
        muted, usb_report, usb_report_capacity);
    if (protocol_result != DS5_PROTOCOL_OK) {
        ++failed_microphone_mute_reports;
        return false;
    }

    if (!ds5_bt_forward_usb_output(sequence, usb_report, bt_transaction,
                                   bt_transaction_capacity)) {
        ++failed_microphone_mute_reports;
        return false;
    }

    ++transmitted_microphone_mute_reports;
    return true;
}

static bool ds5_bt_send_initialization(uint8_t *bt_transaction,
                                       size_t bt_transaction_capacity)
{
    size_t transaction_length;
    ds5_protocol_result_t protocol_result;
    int send_result;

    protocol_result = ds5_build_bt_initialization_transaction(
        DS5_BT_DEFAULT_MIC_SELECT, bt_transaction, bt_transaction_capacity,
        &transaction_length);
    if (protocol_result != DS5_PROTOCOL_OK) {
        ++failed_initialization_reports;
        printf("DS5 BT: initialization report build failed (result %d)\r\n",
               (int)protocol_result);
        return false;
    }

    send_result = ds5_l2cap_send(DS5_L2CAP_CHANNEL_INTERRUPT,
                                 bt_transaction, transaction_length);
    if (send_result < 0) {
        ++failed_initialization_reports;
        if (failed_initialization_reports <= 4U) {
            printf("DS5 BT: initialization report send failed (err %d)\r\n",
                   send_result);
        }
        return false;
    }

    printf("DS5 BT: original startup state report forwarded\r\n");
    return true;
}

static bool ds5_bt_forward_audio(
    ds5_output_sequence_t *sequence,
    uint8_t *packet_counter,
    const uint8_t *haptics_data,
    bool microphone_enabled,
    bool route_to_headset,
    const uint8_t *speaker_opus_data,
    size_t speaker_opus_data_length,
    uint8_t *bt_transaction,
    size_t bt_transaction_capacity)
{
    size_t transaction_length;
    ds5_protocol_result_t protocol_result;
    ds5_output_sequence_t previous_sequence = *sequence;
    uint8_t previous_packet_counter = *packet_counter;
    int send_result;

    protocol_result = ds5_build_bt_audio_transaction(
        sequence, packet_counter,
        haptics_data, DS5_HAPTICS_DATA_SIZE, microphone_enabled,
        route_to_headset, speaker_opus_data, speaker_opus_data_length,
        bt_transaction, bt_transaction_capacity, &transaction_length);
    if (protocol_result != DS5_PROTOCOL_OK) {
        *sequence = previous_sequence;
        *packet_counter = previous_packet_counter;
        ++failed_haptics_reports;
        if (failed_haptics_reports <= 4U) {
            printf("DS5 BT: audio report rejected (result %d)\r\n",
                   (int)protocol_result);
        }
        return false;
    }

    send_result = ds5_l2cap_send(DS5_L2CAP_CHANNEL_INTERRUPT,
                                 bt_transaction, transaction_length);
    if (send_result < 0) {
        *sequence = previous_sequence;
        *packet_counter = previous_packet_counter;
        ++failed_haptics_reports;
        if (failed_haptics_reports <= 4U) {
            printf("DS5 BT: audio send failed (err %d)\r\n",
                   send_result);
        }
        return false;
    }

    ++transmitted_haptics_reports;
    if (transmitted_haptics_reports == 1U) {
        printf("DS5 BT: first native haptics report forwarded "
               "(%lu pre-ready block(s) discarded)\r\n",
               (unsigned long)discarded_haptics_not_ready);
    }

    return true;
}

static bool ds5_bt_send_microphone_status(
    ds5_output_sequence_t *sequence,
    bool microphone_enabled, uint8_t *bt_transaction,
    size_t bt_transaction_capacity)
{
    size_t transaction_length;
    ds5_protocol_result_t protocol_result;
    ds5_output_sequence_t previous_sequence = *sequence;
    int send_result;

    protocol_result = ds5_build_bt_microphone_status_transaction(
        sequence, microphone_enabled, bt_transaction,
        bt_transaction_capacity, &transaction_length);
    if (protocol_result != DS5_PROTOCOL_OK) {
        *sequence = previous_sequence;
        ++failed_microphone_status_reports;
        return false;
    }

    send_result = ds5_l2cap_send(DS5_L2CAP_CHANNEL_INTERRUPT,
                                 bt_transaction, transaction_length);
    if (send_result < 0) {
        *sequence = previous_sequence;
        ++failed_microphone_status_reports;
        if (failed_microphone_status_reports <= 4U) {
            printf("DS5 BT: microphone state send failed (err %d)\r\n",
                   send_result);
        }
        return false;
    }

    ++transmitted_microphone_status_reports;
    printf("DS5 BT: microphone stream %s\r\n",
           microphone_enabled ? "enabled" : "disabled");
    return true;
}

static bool ds5_bt_send_feature_set(
    const ds5_feature_set_request_t *request,
    uint8_t *bt_transaction, size_t bt_transaction_capacity)
{
    size_t transaction_length;
    ds5_protocol_result_t protocol_result;
    int send_result;

    protocol_result = ds5_build_feature_set_transaction(
        request->report_id, request->payload, request->payload_length,
        bt_transaction, bt_transaction_capacity, &transaction_length);
    if (protocol_result != DS5_PROTOCOL_OK) {
        ++failed_feature_set_reports;
        printf("DS5 BT: Feature SET 0x%02x rejected (result %d)\r\n",
               (unsigned int)request->report_id, (int)protocol_result);
        return false;
    }

    send_result = ds5_l2cap_send(DS5_L2CAP_CHANNEL_CONTROL,
                                 bt_transaction, transaction_length);
    if (send_result < 0) {
        ++failed_feature_set_reports;
        if (failed_feature_set_reports <= 4U) {
            printf("DS5 BT: Feature SET 0x%02x send failed (err %d)\r\n",
                   (unsigned int)request->report_id, send_result);
        }
        return false;
    }

    ++transmitted_feature_set_reports;
    printf("DS5 BT: Feature SET 0x%02x forwarded, payload %u, total %lu\r\n",
           (unsigned int)request->report_id,
           (unsigned int)request->payload_length,
           (unsigned long)transmitted_feature_set_reports);
    return true;
}

void ds5_bt_tx_worker(void *parameter)
{
    uint8_t usb_report[DS5_USB_OUTPUT_REPORT_SIZE];
    uint8_t haptics_data[DS5_HAPTICS_DATA_SIZE];
    uint8_t speaker_opus_data[DS5_BT_AUDIO_SPEAKER_DATA_SIZE];
    uint8_t discarded_speaker_frame[DS5_AUDIO_SPEAKER_OPUS_SIZE];
    uint8_t bt_transaction[DS5_BT_HAPTICS_TRANSACTION_SIZE];
    ds5_feature_set_request_t feature_set_request;
    bool pending_usb_output = false;
    TickType_t pending_usb_output_since = 0U;
    bool pending_haptics = false;
    bool microphone_stream_enabled = false;
    bool pending_microphone_state = false;
    bool controller_microphone_muted = false;
    bool pending_microphone_mute = false;
    bool pending_feature_set = false;
    size_t speaker_frame_count = 0U;
    ds5_output_sequence_t output_sequence;
    uint8_t haptics_packet_counter = 0U;
    uint32_t observed_link_generation = 0U;

    (void)parameter;
    ds5_output_sequence_reset(&output_sequence, 0U);

    while (1) {
        bool did_work = false;
        bool retry_pending = false;
        uint8_t newer_report[DS5_USB_OUTPUT_REPORT_SIZE];
        bool newest_microphone_state;
        ds5_bt_tx_link_state_t link_state;

        ds5_bt_get_tx_link_state(&link_state);
        if (observed_link_generation != link_state.link_generation) {
            observed_link_generation = link_state.link_generation;
            /* Feature SET belongs to the USB/ACL session that queued it. */
            pending_feature_set = false;
            ds5_feature_set_mailbox_clear();
            controller_microphone_muted = false;
            pending_microphone_mute = false;
            pending_microphone_state = true;
            pending_usb_output = false;
            pending_haptics = false;
            /* Never carry real-time audio across an ACL session boundary. */
            speaker_frame_count = 0U;
            while (ds5_audio_mailbox_try_receive_speaker_opus(
                discarded_speaker_frame,
                sizeof(discarded_speaker_frame))) {
                did_work = true;
            }
            ds5_output_sequence_reset(&output_sequence, 0U);
            haptics_packet_counter = 0U;
            while (ds5_output_mailbox_try_receive_microphone_button_press()) {
            }
        }

        if (!pending_feature_set &&
            ds5_feature_set_mailbox_try_receive(&feature_set_request)) {
            pending_feature_set = true;
            did_work = true;
        }

        while (ds5_output_mailbox_try_receive(newer_report,
                                              sizeof(newer_report))) {
            if ((newer_report[DS5_USB_OUTPUT_VALID_FLAGS1_OFFSET] &
                 DS5_USB_OUTPUT_ALLOW_AUDIO_MUTE) != 0U) {
                controller_microphone_muted =
                    (newer_report[DS5_USB_OUTPUT_MUTE_CONTROL_OFFSET] &
                     DS5_USB_OUTPUT_MIC_MUTE) != 0U;

                /* Keep the yellow LED authoritative to the actual mute bit. */
                newer_report[DS5_USB_OUTPUT_VALID_FLAGS1_OFFSET] |=
                    DS5_USB_OUTPUT_ALLOW_MUTE_LIGHT;
                newer_report[DS5_USB_OUTPUT_MUTE_LIGHT_OFFSET] =
                    controller_microphone_muted ?
                        DS5_USB_OUTPUT_MUTE_LIGHT_ON : 0U;
                pending_microphone_mute = false;
            }
            memcpy(usb_report, newer_report, sizeof(usb_report));
            pending_usb_output = true;
            pending_usb_output_since = xTaskGetTickCount();
            did_work = true;
        }

        while (ds5_output_mailbox_try_receive_microphone_button_press()) {
            controller_microphone_muted = !controller_microphone_muted;
            pending_microphone_mute = true;
            did_work = true;
        }

        while (ds5_audio_mailbox_try_receive_microphone_stream_active(
            &newest_microphone_state)) {
            microphone_stream_enabled = newest_microphone_state;
            pending_microphone_state = true;
            did_work = true;
        }

        /* Serialize each bounded send transaction with link teardown. */
        ds5_bt_lifecycle_lock();
        if (link_generation == link_state.link_generation) {
            if (link_state.initialization_pending &&
                link_state.interrupt_ready) {
                if (ds5_bt_send_initialization(bt_transaction,
                                               sizeof(bt_transaction))) {
                    (void)ds5_bt_complete_initialization(
                        link_state.link_generation);
                    did_work = true;
                } else {
                    retry_pending = true;
                }
            }

            if (pending_microphone_state && link_state.interrupt_ready) {
                if (ds5_bt_send_microphone_status(
                        &output_sequence, microphone_stream_enabled,
                        bt_transaction, sizeof(bt_transaction))) {
                    pending_microphone_state = false;
                    did_work = true;
                } else {
                    retry_pending = true;
                }
            }

            if (pending_feature_set && link_state.control_ready) {
                if (ds5_bt_send_feature_set(&feature_set_request,
                                            bt_transaction,
                                            sizeof(bt_transaction))) {
                    pending_feature_set = false;
                    did_work = true;
                } else {
                    retry_pending = true;
                }
            }
        }
        ds5_bt_lifecycle_unlock();

        if (!pending_haptics &&
            ds5_haptics_mailbox_try_receive(haptics_data,
                                            sizeof(haptics_data))) {
            did_work = true;
            if (link_state.interrupt_ready) {
                pending_haptics = true;
            } else {
                ++discarded_haptics_not_ready;
            }
        }

        if (!ds5_audio_mailbox_speaker_stream_active()) {
            speaker_frame_count = 0U;
            while (ds5_audio_mailbox_try_receive_speaker_opus(
                discarded_speaker_frame, sizeof(discarded_speaker_frame))) {
                did_work = true;
            }
        } else {
            while ((speaker_frame_count < DS5_BT_AUDIO_SPEAKER_FRAME_COUNT) &&
                   ds5_audio_mailbox_try_receive_speaker_opus(
                       &speaker_opus_data[speaker_frame_count *
                                          DS5_AUDIO_SPEAKER_OPUS_SIZE],
                       DS5_AUDIO_SPEAKER_OPUS_SIZE)) {
                ++speaker_frame_count;
                did_work = true;
            }
        }

        ds5_bt_lifecycle_lock();
        if (link_generation == link_state.link_generation) {
            if (pending_haptics && link_state.interrupt_ready &&
                (!ds5_audio_mailbox_speaker_stream_active() ||
                 (speaker_frame_count == DS5_BT_AUDIO_SPEAKER_FRAME_COUNT))) {
                if (ds5_bt_forward_audio(
                    &output_sequence, &haptics_packet_counter,
                    haptics_data, microphone_stream_enabled,
                    link_state.headset_connected,
                    ds5_audio_mailbox_speaker_stream_active() ?
                        speaker_opus_data : NULL,
                    ds5_audio_mailbox_speaker_stream_active() ?
                        sizeof(speaker_opus_data) : 0U,
                    bt_transaction, sizeof(bt_transaction))) {
                    pending_haptics = false;
                    speaker_frame_count = 0U;
                    did_work = true;
                } else {
                    retry_pending = true;
                }
            }

            if (pending_usb_output) {
                if (link_state.interrupt_ready) {
                    if (ds5_bt_forward_usb_output(
                            &output_sequence,
                            usb_report, bt_transaction,
                            sizeof(bt_transaction))) {
                        pending_usb_output = false;
                        did_work = true;
                    } else {
                        retry_pending = true;
                    }
                } else if ((xTaskGetTickCount() - pending_usb_output_since) >=
                           pdMS_TO_TICKS(DS5_BT_OUTPUT_READY_TIMEOUT_MS)) {
                    ++failed_output_reports;
                    if (failed_output_reports <= 4U) {
                        printf("DS5 BT: USB output expired waiting for "
                               "controller\r\n");
                    }
                    pending_usb_output = false;
                }
            }

            /* A physical press is newer than any host report already queued. */
            if (!link_state.initialization_pending && !pending_usb_output &&
                pending_microphone_mute && link_state.interrupt_ready) {
                if (ds5_bt_send_microphone_mute(
                        &output_sequence,
                        controller_microphone_muted, newer_report,
                        sizeof(newer_report), bt_transaction,
                        sizeof(bt_transaction))) {
                    pending_microphone_mute = false;
                    did_work = true;
                } else {
                    retry_pending = true;
                }
            }
        }
        ds5_bt_lifecycle_unlock();

        if (!did_work) {
            TickType_t wait_ticks;

            if (retry_pending) {
                wait_ticks = pdMS_TO_TICKS(DS5_BT_TX_RETRY_MS);
            } else if (pending_usb_output) {
                TickType_t timeout_ticks =
                    pdMS_TO_TICKS(DS5_BT_OUTPUT_READY_TIMEOUT_MS);
                TickType_t elapsed_ticks =
                    xTaskGetTickCount() - pending_usb_output_since;

                wait_ticks = elapsed_ticks >= timeout_ticks ?
                                 0U : timeout_ticks - elapsed_ticks;
            } else {
                wait_ticks = portMAX_DELAY;
            }

            (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
        }
    }
}
