#include "ds5_protocol.h"

#include <string.h>

#define DS5_CRC32_POLYNOMIAL  0xEDB88320U
#define DS5_OUTPUT_CRC32_SEED 0xEADA2D49U
#define DS5_FEATURE_CRC32_SEED 0x2060EFC3U

_Static_assert(DS5_BT_INPUT_PAYLOAD_OFFSET + DS5_USB_INPUT_PAYLOAD_SIZE ==
                   DS5_BT_INPUT_MIN_SIZE,
               "Bluetooth input report size mismatch");
_Static_assert(DS5_USB_OUTPUT_STATE_OFFSET + DS5_USB_OUTPUT_STATE_SIZE ==
                   DS5_USB_OUTPUT_REPORT_SIZE,
               "USB output report size mismatch");
_Static_assert(DS5_BT_OUTPUT_REPORT_OFFSET + DS5_BT_OUTPUT_REPORT_SIZE ==
                   DS5_BT_OUTPUT_TRANSACTION_SIZE,
               "Bluetooth output transaction size mismatch");
_Static_assert(DS5_BT_OUTPUT_CRC_OFFSET + 4U == DS5_BT_OUTPUT_REPORT_SIZE,
               "Bluetooth output CRC offset mismatch");
_Static_assert(DS5_BT_OUTPUT_STATE_OFFSET + DS5_SET_STATE_SIZE <=
                   DS5_BT_OUTPUT_CRC_OFFSET,
               "Bluetooth output state overlaps CRC");
_Static_assert(DS5_BT_INITIALIZATION_REPORT_SIZE + 1U ==
                   DS5_BT_INITIALIZATION_TRANSACTION_SIZE,
               "Bluetooth initialization transaction size mismatch");
_Static_assert(DS5_BT_INITIALIZATION_STATE_OFFSET + DS5_SET_STATE_SIZE <=
                   DS5_BT_INITIALIZATION_CRC_OFFSET,
               "Bluetooth initialization state overlaps CRC");
_Static_assert(DS5_BT_INITIALIZATION_CRC_OFFSET + 4U ==
                   DS5_BT_INITIALIZATION_REPORT_SIZE,
               "Bluetooth initialization CRC offset mismatch");
_Static_assert(DS5_BT_HAPTICS_REPORT_SIZE + 1U ==
                   DS5_BT_HAPTICS_TRANSACTION_SIZE,
               "Bluetooth haptics transaction size mismatch");
_Static_assert(DS5_BT_HAPTICS_CRC_OFFSET + 4U ==
                   DS5_BT_HAPTICS_REPORT_SIZE,
               "Bluetooth haptics CRC offset mismatch");
_Static_assert(DS5_BT_HAPTICS_DATA_OFFSET + DS5_HAPTICS_DATA_SIZE <=
                   DS5_BT_HAPTICS_CRC_OFFSET,
               "Bluetooth haptics data overlaps CRC");

static uint32_t ds5_crc32_seeded(const uint8_t *data, size_t length,
                                 uint32_t seed)
{
    uint32_t crc = ~seed;
    size_t index;

    for (index = 0U; index < length; ++index) {
        uint32_t bit;

        crc ^= data[index];
        for (bit = 0U; bit < 8U; ++bit) {
            uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (DS5_CRC32_POLYNOMIAL & mask);
        }
    }

    return ~crc;
}

static void ds5_write_u32_le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 0U);
    destination[1] = (uint8_t)(value >> 8U);
    destination[2] = (uint8_t)(value >> 16U);
    destination[3] = (uint8_t)(value >> 24U);
}

void ds5_output_sequence_reset(ds5_output_sequence_t *sequence,
                               uint8_t next_value)
{
    if (sequence != NULL) {
        sequence->next_value = next_value & 0x0FU;
    }
}

ds5_protocol_result_t ds5_extract_usb_input_payload(
    const uint8_t *bt_transaction,
    size_t bt_transaction_length,
    uint8_t *usb_payload,
    size_t usb_payload_capacity)
{
    if ((bt_transaction == NULL) || (usb_payload == NULL)) {
        return DS5_PROTOCOL_ERROR_ARGUMENT;
    }

    if (bt_transaction_length < DS5_BT_INPUT_MIN_SIZE) {
        return DS5_PROTOCOL_ERROR_LENGTH;
    }

    if (usb_payload_capacity < DS5_USB_INPUT_PAYLOAD_SIZE) {
        return DS5_PROTOCOL_ERROR_CAPACITY;
    }

    if (bt_transaction[0] != DS5_BT_INPUT_TRANSACTION_HEADER) {
        return DS5_PROTOCOL_ERROR_TRANSACTION_HEADER;
    }

    if (bt_transaction[DS5_BT_INPUT_REPORT_ID_OFFSET] !=
        DS5_BT_INPUT_REPORT_ID) {
        return DS5_PROTOCOL_ERROR_REPORT_ID;
    }

    memcpy(usb_payload,
           &bt_transaction[DS5_BT_INPUT_PAYLOAD_OFFSET],
           DS5_USB_INPUT_PAYLOAD_SIZE);
    return DS5_PROTOCOL_OK;
}

ds5_protocol_result_t ds5_build_bt_output_transaction(
    ds5_output_sequence_t *sequence,
    const uint8_t *usb_report,
    size_t usb_report_length,
    uint8_t *bt_transaction,
    size_t bt_transaction_capacity,
    size_t *bt_transaction_length)
{
    uint8_t *bt_report;
    uint8_t sequence_value;
    uint32_t crc;

    if (bt_transaction_length != NULL) {
        *bt_transaction_length = 0U;
    }

    if ((sequence == NULL) || (usb_report == NULL) ||
        (bt_transaction == NULL) || (bt_transaction_length == NULL)) {
        return DS5_PROTOCOL_ERROR_ARGUMENT;
    }

    if (usb_report_length != DS5_USB_OUTPUT_REPORT_SIZE) {
        return DS5_PROTOCOL_ERROR_LENGTH;
    }

    if (bt_transaction_capacity < DS5_BT_OUTPUT_TRANSACTION_SIZE) {
        return DS5_PROTOCOL_ERROR_CAPACITY;
    }

    if (usb_report[0] != DS5_USB_OUTPUT_REPORT_ID) {
        return DS5_PROTOCOL_ERROR_REPORT_ID;
    }

    memset(bt_transaction, 0, DS5_BT_OUTPUT_TRANSACTION_SIZE);
    bt_transaction[0] = DS5_BT_OUTPUT_TRANSACTION_HEADER;
    bt_report = &bt_transaction[DS5_BT_OUTPUT_REPORT_OFFSET];

    sequence_value = sequence->next_value & 0x0FU;
    bt_report[0] = DS5_BT_OUTPUT_REPORT_ID;
    bt_report[DS5_BT_OUTPUT_SEQUENCE_OFFSET] =
        (uint8_t)(sequence_value << 4U);
    bt_report[DS5_BT_OUTPUT_TAG_OFFSET] = DS5_BT_OUTPUT_TAG;
    memcpy(&bt_report[DS5_BT_OUTPUT_STATE_OFFSET],
           &usb_report[DS5_USB_OUTPUT_STATE_OFFSET],
           DS5_USB_OUTPUT_STATE_SIZE);

    crc = ds5_crc32_seeded(bt_report, DS5_BT_OUTPUT_CRC_OFFSET,
                           DS5_OUTPUT_CRC32_SEED);
    ds5_write_u32_le(&bt_report[DS5_BT_OUTPUT_CRC_OFFSET], crc);

    sequence->next_value = (uint8_t)((sequence_value + 1U) & 0x0FU);
    *bt_transaction_length = DS5_BT_OUTPUT_TRANSACTION_SIZE;
    return DS5_PROTOCOL_OK;
}

ds5_protocol_result_t ds5_build_bt_initialization_transaction(
    uint8_t mic_select,
    uint8_t *bt_transaction,
    size_t bt_transaction_capacity,
    size_t *bt_transaction_length)
{
    uint8_t *report;
    uint8_t *state;
    uint32_t crc;

    if (bt_transaction_length != NULL) {
        *bt_transaction_length = 0U;
    }

    if ((bt_transaction == NULL) || (bt_transaction_length == NULL) ||
        (mic_select > 3U)) {
        return DS5_PROTOCOL_ERROR_ARGUMENT;
    }

    if (bt_transaction_capacity < DS5_BT_INITIALIZATION_TRANSACTION_SIZE) {
        return DS5_PROTOCOL_ERROR_CAPACITY;
    }

    memset(bt_transaction, 0, DS5_BT_INITIALIZATION_TRANSACTION_SIZE);
    bt_transaction[0] = DS5_BT_OUTPUT_TRANSACTION_HEADER;
    report = &bt_transaction[1];
    report[0] = DS5_BT_INITIALIZATION_REPORT_ID;
    report[1] = DS5_BT_INITIALIZATION_TAG;
    report[2] = DS5_BT_INITIALIZATION_FLAGS;
    report[3] = DS5_BT_INITIALIZATION_MODE;

    /* Exact SetStateData fields initialized by original src/bt.cpp. */
    state = &report[DS5_BT_INITIALIZATION_STATE_OFFSET];
    state[0] = 0x80U; /* AllowAudioControl */
    state[1] = 0x04U; /* AllowLedColor */
    state[7] = mic_select; /* MicSelect */
    state[38] = 0x03U; /* Light brightness and fade-animation controls */
    state[41] = 0x02U; /* LightFadeAnimation::FadeOut */
    state[42] = 0x00U; /* LightBrightness::Bright */
    state[44] = 0xFFU;
    state[45] = 0xD7U;
    state[46] = 0x00U;

    crc = ds5_crc32_seeded(report, DS5_BT_INITIALIZATION_CRC_OFFSET,
                           DS5_OUTPUT_CRC32_SEED);
    ds5_write_u32_le(&report[DS5_BT_INITIALIZATION_CRC_OFFSET], crc);
    *bt_transaction_length = DS5_BT_INITIALIZATION_TRANSACTION_SIZE;
    return DS5_PROTOCOL_OK;
}

ds5_protocol_result_t ds5_build_bt_haptics_transaction(
    ds5_output_sequence_t *sequence,
    uint8_t *packet_counter,
    const uint8_t *haptics_data,
    size_t haptics_data_length,
    uint8_t *bt_transaction,
    size_t bt_transaction_capacity,
    size_t *bt_transaction_length)
{
    uint8_t *bt_report;
    uint8_t sequence_value;
    uint32_t crc;

    if (bt_transaction_length != NULL) {
        *bt_transaction_length = 0U;
    }

    if ((sequence == NULL) || (packet_counter == NULL) ||
        (haptics_data == NULL) || (bt_transaction == NULL) ||
        (bt_transaction_length == NULL)) {
        return DS5_PROTOCOL_ERROR_ARGUMENT;
    }

    if (haptics_data_length != DS5_HAPTICS_DATA_SIZE) {
        return DS5_PROTOCOL_ERROR_LENGTH;
    }

    if (bt_transaction_capacity < DS5_BT_HAPTICS_TRANSACTION_SIZE) {
        return DS5_PROTOCOL_ERROR_CAPACITY;
    }

    memset(bt_transaction, 0, DS5_BT_HAPTICS_TRANSACTION_SIZE);
    bt_transaction[0] = DS5_BT_OUTPUT_TRANSACTION_HEADER;
    bt_report = &bt_transaction[1];

    sequence_value = sequence->next_value & 0x0FU;
    bt_report[0] = DS5_BT_HAPTICS_REPORT_ID;
    bt_report[DS5_BT_HAPTICS_SEQUENCE_OFFSET] =
        (uint8_t)(sequence_value << 4U);
    bt_report[2] = DS5_BT_HAPTICS_STREAM_FLAGS;
    bt_report[3] = DS5_BT_HAPTICS_HEADER_LENGTH;
    bt_report[4] = DS5_BT_HAPTICS_ROUTING;
    bt_report[5] = DS5_BT_HAPTICS_BUFFER_LENGTH;
    bt_report[6] = DS5_BT_HAPTICS_BUFFER_LENGTH;
    bt_report[7] = DS5_BT_HAPTICS_BUFFER_LENGTH;
    bt_report[8] = DS5_BT_HAPTICS_BUFFER_LENGTH;
    *packet_counter = (uint8_t)(*packet_counter + 2U);
    bt_report[9] = *packet_counter;
    bt_report[10] = DS5_BT_HAPTICS_BLOCK_FLAGS;
    bt_report[11] = DS5_BT_HAPTICS_BLOCK_LENGTH;
    memcpy(&bt_report[DS5_BT_HAPTICS_DATA_OFFSET], haptics_data,
           DS5_HAPTICS_DATA_SIZE);

    crc = ds5_crc32_seeded(bt_report, DS5_BT_HAPTICS_CRC_OFFSET,
                           DS5_OUTPUT_CRC32_SEED);
    ds5_write_u32_le(&bt_report[DS5_BT_HAPTICS_CRC_OFFSET], crc);

    sequence->next_value = (uint8_t)((sequence_value + 1U) & 0x0FU);
    *bt_transaction_length = DS5_BT_HAPTICS_TRANSACTION_SIZE;
    return DS5_PROTOCOL_OK;
}

ds5_protocol_result_t ds5_build_feature_get_transaction(
    uint8_t report_id,
    uint8_t *control_transaction,
    size_t control_transaction_capacity,
    size_t *control_transaction_length)
{
    if (control_transaction_length != NULL) {
        *control_transaction_length = 0U;
    }

    if ((control_transaction == NULL) ||
        (control_transaction_length == NULL)) {
        return DS5_PROTOCOL_ERROR_ARGUMENT;
    }

    if (control_transaction_capacity < DS5_FEATURE_GET_TRANSACTION_SIZE) {
        return DS5_PROTOCOL_ERROR_CAPACITY;
    }

    control_transaction[0] = DS5_FEATURE_GET_HEADER;
    control_transaction[1] = report_id;
    *control_transaction_length = DS5_FEATURE_GET_TRANSACTION_SIZE;
    return DS5_PROTOCOL_OK;
}

ds5_protocol_result_t ds5_build_feature_set_transaction(
    uint8_t report_id,
    const uint8_t *payload,
    size_t payload_length,
    uint8_t *control_transaction,
    size_t control_transaction_capacity,
    size_t *control_transaction_length)
{
    size_t required_length;
    uint32_t crc;

    if (control_transaction_length != NULL) {
        *control_transaction_length = 0U;
    }

    if ((control_transaction == NULL) ||
        (control_transaction_length == NULL) ||
        ((payload == NULL) && (payload_length != 0U))) {
        return DS5_PROTOCOL_ERROR_ARGUMENT;
    }

    if (payload_length > (SIZE_MAX - DS5_FEATURE_SET_OVERHEAD)) {
        return DS5_PROTOCOL_ERROR_LENGTH;
    }

    required_length = payload_length + DS5_FEATURE_SET_OVERHEAD;
    if (control_transaction_capacity < required_length) {
        return DS5_PROTOCOL_ERROR_CAPACITY;
    }

    control_transaction[0] = DS5_FEATURE_SET_HEADER;
    control_transaction[1] = report_id;
    if (payload_length != 0U) {
        memcpy(&control_transaction[2], payload, payload_length);
    }

    crc = ds5_crc32_seeded(&control_transaction[1], payload_length + 1U,
                           DS5_FEATURE_CRC32_SEED);
    ds5_write_u32_le(&control_transaction[2U + payload_length], crc);

    *control_transaction_length = required_length;
    return DS5_PROTOCOL_OK;
}
