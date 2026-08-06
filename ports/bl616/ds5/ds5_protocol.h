#ifndef DS5_PROTOCOL_H
#define DS5_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS5_USB_INPUT_REPORT_ID            0x01U
#define DS5_USB_INPUT_PAYLOAD_SIZE         63U

#define DS5_BT_INPUT_TRANSACTION_HEADER    0xA1U
#define DS5_BT_INPUT_REPORT_ID             0x31U
#define DS5_BT_INPUT_REPORT_ID_OFFSET      1U
#define DS5_BT_INPUT_PAYLOAD_OFFSET        3U
#define DS5_BT_INPUT_MIN_SIZE              66U

#define DS5_USB_OUTPUT_REPORT_ID           0x02U
#define DS5_USB_OUTPUT_REPORT_SIZE         48U
#define DS5_USB_OUTPUT_STATE_OFFSET        1U
#define DS5_USB_OUTPUT_STATE_SIZE          47U
#define DS5_SET_STATE_SIZE                 63U

#define DS5_BT_OUTPUT_TRANSACTION_HEADER   0xA2U
#define DS5_BT_OUTPUT_TRANSACTION_SIZE     79U
#define DS5_BT_OUTPUT_REPORT_OFFSET        1U
#define DS5_BT_OUTPUT_REPORT_SIZE          78U
#define DS5_BT_OUTPUT_REPORT_ID            0x31U
#define DS5_BT_OUTPUT_SEQUENCE_OFFSET      1U
#define DS5_BT_OUTPUT_TAG_OFFSET           2U
#define DS5_BT_OUTPUT_TAG                  0x10U
#define DS5_BT_OUTPUT_STATE_OFFSET         3U
#define DS5_BT_OUTPUT_CRC_OFFSET           74U

/* Original src/bt.cpp update_state() report, including its BT CRC. */
#define DS5_BT_INITIALIZATION_TRANSACTION_SIZE 143U
#define DS5_BT_INITIALIZATION_REPORT_SIZE  142U
#define DS5_BT_INITIALIZATION_REPORT_ID    0x32U
#define DS5_BT_INITIALIZATION_TAG          0x10U
#define DS5_BT_INITIALIZATION_FLAGS        0x90U
#define DS5_BT_INITIALIZATION_MODE         0x3FU
#define DS5_BT_INITIALIZATION_STATE_OFFSET 4U
#define DS5_BT_INITIALIZATION_CRC_OFFSET   138U

#define DS5_HAPTICS_DATA_SIZE              128U
#define DS5_BT_HAPTICS_TRANSACTION_SIZE    548U
#define DS5_BT_HAPTICS_REPORT_SIZE         547U
#define DS5_BT_HAPTICS_REPORT_ID           0x39U
#define DS5_BT_HAPTICS_SEQUENCE_OFFSET     1U
#define DS5_BT_HAPTICS_STREAM_FLAGS        0x91U
#define DS5_BT_HAPTICS_HEADER_LENGTH       0x06U
#define DS5_BT_HAPTICS_ROUTING             0x7EU
#define DS5_BT_HAPTICS_BUFFER_LENGTH       64U
#define DS5_BT_HAPTICS_BLOCK_FLAGS         0xD2U
#define DS5_BT_HAPTICS_BLOCK_LENGTH        64U
#define DS5_BT_HAPTICS_DATA_OFFSET         12U
#define DS5_BT_HAPTICS_CRC_OFFSET          543U

/* Native DualSense Bluetooth audio transport, mirrored from src/audio.cpp. */
#define DS5_AUDIO_SAMPLE_RATE               48000U
#define DS5_AUDIO_FRAME_SAMPLES             480U
#define DS5_AUDIO_SPEAKER_CHANNELS          2U
#define DS5_AUDIO_MIC_CHANNELS              1U
#define DS5_AUDIO_SPEAKER_OPUS_SIZE         200U
#define DS5_AUDIO_MIC_OPUS_SIZE             71U
#define DS5_BT_AUDIO_SPEAKER_FRAME_COUNT    2U
#define DS5_BT_AUDIO_SPEAKER_DATA_SIZE      \
    (DS5_AUDIO_SPEAKER_OPUS_SIZE * DS5_BT_AUDIO_SPEAKER_FRAME_COUNT)
#define DS5_BT_AUDIO_SPEAKER_FLAGS_OFFSET   140U
#define DS5_BT_AUDIO_SPEAKER_LENGTH_OFFSET  141U
#define DS5_BT_AUDIO_SPEAKER_DATA_OFFSET    142U
#define DS5_BT_AUDIO_SPEAKER_FLAGS          0xD3U
#define DS5_BT_AUDIO_HEADPHONE_FLAGS        0xD6U
#define DS5_BT_AUDIO_MIC_ENABLED_ROUTING    0x7FU

/* src/audio.cpp update_mic_status() uses a 0x32 state report. */
#define DS5_BT_MIC_STATUS_TRANSACTION_SIZE  DS5_BT_INITIALIZATION_TRANSACTION_SIZE
#define DS5_BT_MIC_STATUS_REPORT_SIZE       DS5_BT_INITIALIZATION_REPORT_SIZE
#define DS5_BT_MIC_STATUS_REPORT_ID         0x32U
#define DS5_BT_MIC_STATUS_FLAGS             0x91U
#define DS5_BT_MIC_STATUS_LENGTH            0x01U
#define DS5_BT_MIC_STATUS_ENABLED            0x03U
#define DS5_BT_MIC_STATUS_DISABLED           0x02U

#define DS5_FEATURE_GET_HEADER             0x43U
#define DS5_FEATURE_GET_TRANSACTION_SIZE   2U
#define DS5_FEATURE_DATA_HEADER            0xA3U
#define DS5_FEATURE_CALIBRATION_REPORT_ID  0x05U
#define DS5_FEATURE_SET_HEADER             0x53U
#define DS5_FEATURE_CRC_SIZE               4U
#define DS5_FEATURE_SET_OVERHEAD           6U
#define DS5_FEATURE_SET_MAX_PAYLOAD        63U

typedef enum {
    DS5_PROTOCOL_OK = 0,
    DS5_PROTOCOL_ERROR_ARGUMENT = -1,
    DS5_PROTOCOL_ERROR_LENGTH = -2,
    DS5_PROTOCOL_ERROR_REPORT_ID = -3,
    DS5_PROTOCOL_ERROR_CAPACITY = -4,
    DS5_PROTOCOL_ERROR_TRANSACTION_HEADER = -5,
} ds5_protocol_result_t;

typedef struct {
    uint8_t next_value;
} ds5_output_sequence_t;

void ds5_output_sequence_reset(ds5_output_sequence_t *sequence,
                               uint8_t next_value);

ds5_protocol_result_t ds5_extract_usb_input_payload(
    const uint8_t *bt_transaction,
    size_t bt_transaction_length,
    uint8_t *usb_payload,
    size_t usb_payload_capacity);

ds5_protocol_result_t ds5_build_bt_output_transaction(
    ds5_output_sequence_t *sequence,
    const uint8_t *usb_report,
    size_t usb_report_length,
    uint8_t *bt_transaction,
    size_t bt_transaction_capacity,
    size_t *bt_transaction_length);

/*
 * Build the startup state report emitted by original src/bt.cpp after the
 * HID Interrupt channel opens. mic_select is the original two-bit setting.
 */
ds5_protocol_result_t ds5_build_bt_initialization_transaction(
    uint8_t mic_select,
    uint8_t *bt_transaction,
    size_t bt_transaction_capacity,
    size_t *bt_transaction_length);

ds5_protocol_result_t ds5_build_bt_haptics_transaction(
    ds5_output_sequence_t *sequence,
    uint8_t *packet_counter,
    const uint8_t *haptics_data,
    size_t haptics_data_length,
    uint8_t *bt_transaction,
    size_t bt_transaction_capacity,
    size_t *bt_transaction_length);

/*
 * Build the original 0x39 Bluetooth audio report.  speaker_opus_data is
 * either absent or exactly two fixed 200-byte Opus frames.  Both audio and
 * ordinary HID output reports advance the same Bluetooth sequence counter.
 */
ds5_protocol_result_t ds5_build_bt_audio_transaction(
    ds5_output_sequence_t *sequence,
    uint8_t *packet_counter,
    const uint8_t *haptics_data,
    size_t haptics_data_length,
    bool microphone_enabled,
    bool route_to_headphones,
    const uint8_t *speaker_opus_data,
    size_t speaker_opus_data_length,
    uint8_t *bt_transaction,
    size_t bt_transaction_capacity,
    size_t *bt_transaction_length);

ds5_protocol_result_t ds5_build_bt_microphone_status_transaction(
    ds5_output_sequence_t *sequence,
    bool microphone_enabled,
    uint8_t *bt_transaction,
    size_t bt_transaction_capacity,
    size_t *bt_transaction_length);

ds5_protocol_result_t ds5_build_feature_get_transaction(
    uint8_t report_id,
    uint8_t *control_transaction,
    size_t control_transaction_capacity,
    size_t *control_transaction_length);

/* payload excludes the report ID and the four-byte Feature CRC. */
ds5_protocol_result_t ds5_build_feature_set_transaction(
    uint8_t report_id,
    const uint8_t *payload,
    size_t payload_length,
    uint8_t *control_transaction,
    size_t control_transaction_capacity,
    size_t *control_transaction_length);

#ifdef __cplusplus
}
#endif

#endif
