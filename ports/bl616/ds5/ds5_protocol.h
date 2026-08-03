#ifndef DS5_PROTOCOL_H
#define DS5_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS5_USB_INPUT_REPORT_ID            0x01U
#define DS5_USB_INPUT_PAYLOAD_SIZE         63U

#define DS5_BT_INPUT_REPORT_ID             0x31U
#define DS5_BT_INPUT_REPORT_ID_OFFSET      1U
#define DS5_BT_INPUT_PAYLOAD_OFFSET        3U
#define DS5_BT_INPUT_MIN_SIZE              66U

#define DS5_USB_OUTPUT_REPORT_ID           0x02U
#define DS5_USB_OUTPUT_REPORT_SIZE         64U
#define DS5_USB_OUTPUT_STATE_OFFSET        1U
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

#define DS5_FEATURE_GET_HEADER             0x43U
#define DS5_FEATURE_GET_TRANSACTION_SIZE   2U
#define DS5_FEATURE_SET_HEADER             0x53U
#define DS5_FEATURE_CRC_SIZE               4U
#define DS5_FEATURE_SET_OVERHEAD           6U

typedef enum {
    DS5_PROTOCOL_OK = 0,
    DS5_PROTOCOL_ERROR_ARGUMENT = -1,
    DS5_PROTOCOL_ERROR_LENGTH = -2,
    DS5_PROTOCOL_ERROR_REPORT_ID = -3,
    DS5_PROTOCOL_ERROR_CAPACITY = -4,
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
