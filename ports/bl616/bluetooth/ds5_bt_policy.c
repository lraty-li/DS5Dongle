#include "ds5_bt_policy.h"

#include <string.h>

bool ds5_bt_policy_is_gamepad(uint32_t device_class)
{
    return ((device_class & DS5_BT_COD_MAJOR_MASK) ==
            DS5_BT_COD_MAJOR_PERIPHERAL) &&
           ((device_class & DS5_BT_COD_MINOR_TYPE_MASK) ==
            DS5_BT_COD_MINOR_GAMEPAD);
}

bool ds5_bt_policy_name_matches(const char *name)
{
    static const char *const known_names[] = {
        "Wireless Controller",
        "DualSense Wireless Controller",
        "DualSense Edge Wireless Controller",
    };
    size_t index;

    if (name == NULL) {
        return false;
    }

    for (index = 0U; index < (sizeof(known_names) / sizeof(known_names[0]));
         ++index) {
        if (strcmp(name, known_names[index]) == 0) {
            return true;
        }
    }

    return false;
}

uint16_t ds5_bt_policy_candidate_score(uint32_t device_class,
                                       const char *name,
                                       bool saved_address_match)
{
    uint16_t score = 0U;

    if (saved_address_match) {
        score = DS5_BT_CANDIDATE_SCORE_SAVED;
    } else if (!ds5_bt_policy_is_gamepad(device_class)) {
        return 0U;
    } else {
        score = DS5_BT_CANDIDATE_SCORE_GAMEPAD;
    }

    if (ds5_bt_policy_name_matches(name)) {
        score = (uint16_t)(score + DS5_BT_CANDIDATE_SCORE_NAME);
    }

    return score;
}

ds5_bt_eir_name_result_t ds5_bt_policy_extract_eir_name(
    const uint8_t *eir, size_t eir_length,
    char *name, size_t name_capacity)
{
    size_t offset = 0U;

    if ((eir == NULL) || (name == NULL) || (name_capacity == 0U)) {
        return DS5_BT_EIR_NAME_MALFORMED;
    }

    name[0] = '\0';

    while (offset < eir_length) {
        uint8_t field_length = eir[offset++];
        uint8_t field_type;
        size_t source_length;
        size_t copy_length;

        if (field_length == 0U) {
            return DS5_BT_EIR_NAME_NOT_FOUND;
        }

        if ((size_t)field_length > (eir_length - offset)) {
            return DS5_BT_EIR_NAME_MALFORMED;
        }

        field_type = eir[offset];
        source_length = (size_t)field_length - 1U;

        if ((field_type == DS5_BT_EIR_SHORT_NAME) ||
            (field_type == DS5_BT_EIR_COMPLETE_NAME)) {
            copy_length = source_length;
            if (copy_length >= name_capacity) {
                copy_length = name_capacity - 1U;
            }

            if (copy_length != 0U) {
                memcpy(name, &eir[offset + 1U], copy_length);
            }
            name[copy_length] = '\0';
            return DS5_BT_EIR_NAME_FOUND;
        }

        offset += (size_t)field_length;
    }

    return DS5_BT_EIR_NAME_NOT_FOUND;
}
