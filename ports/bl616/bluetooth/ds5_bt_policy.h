#ifndef DS5_BT_POLICY_H
#define DS5_BT_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS5_BT_COD_MAJOR_MASK          0x001F00U
#define DS5_BT_COD_MAJOR_PERIPHERAL    0x000500U
#define DS5_BT_COD_MINOR_TYPE_MASK     0x00003CU
#define DS5_BT_COD_MINOR_GAMEPAD       0x000008U

#define DS5_BT_CANDIDATE_SCORE_GAMEPAD 100U
#define DS5_BT_CANDIDATE_SCORE_NAME    100U
#define DS5_BT_CANDIDATE_SCORE_SAVED   1000U

#define DS5_BT_EIR_SHORT_NAME          0x08U
#define DS5_BT_EIR_COMPLETE_NAME       0x09U

typedef enum {
    DS5_BT_EIR_NAME_NOT_FOUND = 0,
    DS5_BT_EIR_NAME_FOUND,
    DS5_BT_EIR_NAME_MALFORMED,
} ds5_bt_eir_name_result_t;

bool ds5_bt_policy_is_gamepad(uint32_t device_class);
bool ds5_bt_policy_name_matches(const char *name);
uint16_t ds5_bt_policy_candidate_score(uint32_t device_class,
                                       const char *name,
                                       bool saved_address_match);

ds5_bt_eir_name_result_t ds5_bt_policy_extract_eir_name(
    const uint8_t *eir, size_t eir_length,
    char *name, size_t name_capacity);

#ifdef __cplusplus
}
#endif

#endif
