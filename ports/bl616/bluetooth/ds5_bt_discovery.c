#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "conn.h"
#include "ds5_bt_internal.h"
#include "ds5_bt_policy.h"
#include "hci_err.h"
#include "ds5_log.h"

#define printf ds5_log_printf

static const struct bt_br_discovery_param discovery_param = {
    .length = DS5_BT_DISCOVERY_LENGTH,
    .limited = false,
};
static struct bt_br_discovery_result
    discovery_results[DS5_BT_DISCOVERY_RESULT_COUNT];

static uint32_t ds5_bt_result_device_class(
    const struct bt_br_discovery_result *result)
{
    return (uint32_t)result->cod[0] |
           ((uint32_t)result->cod[1] << 8U) |
           ((uint32_t)result->cod[2] << 16U);
}

static void ds5_bt_consider_candidate(
    const struct bt_br_discovery_result *result,
    uint32_t device_class, const char *name)
{
    bool saved_address_match = bonded_peer_valid &&
        (bt_addr_cmp(&result->addr, &bonded_peer_address) == 0);
    uint16_t score = ds5_bt_policy_candidate_score(
        device_class, name, saved_address_match);

    if (score == 0U) {
        return;
    }

    if (candidate.valid &&
        ((score < candidate.score) ||
         ((score == candidate.score) && (result->rssi <= candidate.rssi)))) {
        return;
    }

    candidate.valid = true;
    bt_addr_copy(&candidate.address, &result->addr);
    candidate.device_class = device_class;
    candidate.rssi = result->rssi;
    candidate.score = score;
    if (name != NULL) {
        strncpy(candidate.name, name, sizeof(candidate.name) - 1U);
        candidate.name[sizeof(candidate.name) - 1U] = '\0';
    } else {
        candidate.name[0] = '\0';
    }
}

void ds5_bt_restore_bond(const struct bt_br_bond_info *info,
                         void *user_data)
{
    size_t *bond_count = user_data;

    if ((info == NULL) || (info->addr == NULL) || (bond_count == NULL)) {
        return;
    }

    ++(*bond_count);
    if (!bonded_peer_valid) {
        bt_addr_copy(&bonded_peer_address, info->addr);
        bonded_peer_valid = true;
    }
}

static void ds5_bt_discovery_complete(struct bt_br_discovery_result *results,
                                      size_t count)
{
    size_t index;

    ds5_bt_lifecycle_lock();

    discovery_in_flight = false;
    discovery_stop_requested = false;
    discovery_retry_at = xTaskGetTickCount() +
                         pdMS_TO_TICKS(DS5_BT_DISCOVERY_RETRY_MS);

    /* A late inquiry completion must never overwrite an active link. */
    if ((active_connection != NULL) || !discovery_allowed ||
        (bluetooth_state != DS5_BT_STATE_DISCOVERING)) {
        memset(&candidate, 0, sizeof(candidate));
        printf("DS5 BT: discovery completion ignored for current link/policy\r\n");
        if (bluetooth_state == DS5_BT_STATE_DISCOVERING) {
            ds5_bt_set_state(DS5_BT_STATE_IDLE);
        }
        ds5_bt_policy_wake();
        goto out;
    }

    memset(&candidate, 0, sizeof(candidate));
    printf("DS5 BT: discovery complete (%u result(s))\r\n",
           (unsigned int)count);

    for (index = 0U; (results != NULL) && (index < count); ++index) {
        char address[BT_ADDR_STR_LEN];
        char name[DS5_BT_CANDIDATE_NAME_SIZE];
        uint32_t device_class = ds5_bt_result_device_class(&results[index]);
        ds5_bt_eir_name_result_t name_result;

        bt_addr_to_str(&results[index].addr, address, sizeof(address));
        name_result = ds5_bt_policy_extract_eir_name(
            results[index].eir, sizeof(results[index].eir),
            name, sizeof(name));

        printf("  addr: %s, rssi: %d, class: 0x%06lx",
               address, (int)results[index].rssi,
               (unsigned long)device_class);
        if (name_result == DS5_BT_EIR_NAME_FOUND) {
            printf(", name: %s", name);
        } else if (name_result == DS5_BT_EIR_NAME_MALFORMED) {
            printf(", name: <malformed EIR>");
        }
        printf("\r\n");

        ds5_bt_consider_candidate(&results[index], device_class,
                                  name_result == DS5_BT_EIR_NAME_FOUND
                                      ? name
                                      : NULL);
    }

    if (candidate.valid) {
        char address[BT_ADDR_STR_LEN];

        bt_addr_to_str(&candidate.address, address, sizeof(address));
        printf("DS5 BT: selected explicit-connect candidate %s "
               "(score %u, class 0x%06lx, name %s)\r\n",
               address, (unsigned int)candidate.score,
               (unsigned long)candidate.device_class,
               candidate.name[0] != '\0' ? candidate.name : "<unknown>");
        ds5_bt_set_state(DS5_BT_STATE_CANDIDATE_READY);
    } else {
        printf("DS5 BT: no gamepad-class candidate found\r\n");
        ds5_bt_set_state(DS5_BT_STATE_IDLE);
    }
    ds5_bt_policy_wake();

out:
    ds5_bt_lifecycle_unlock();
}

static bool ds5_bt_deadline_expired(TickType_t now, TickType_t deadline)
{
    return (deadline != 0U) && (deadline != portMAX_DELAY) &&
           ((int32_t)(now - deadline) >= 0);
}

static TickType_t ds5_bt_ticks_until(TickType_t now, TickType_t deadline)
{
    if ((deadline == 0U) || (deadline == portMAX_DELAY)) {
        return portMAX_DELAY;
    }

    return ds5_bt_deadline_expired(now, deadline) ? 0U : deadline - now;
}

static void ds5_bt_process_bond_recovery(TickType_t now)
{
    int err;

    if (!bond_recovery_pending || (active_connection != NULL) ||
        !ds5_bt_deadline_expired(now, bond_recovery_retry_at)) {
        return;
    }

    if (!bonded_peer_valid) {
        bond_recovery_pending = false;
        return;
    }

    printf("DS5 BT: saved bond rejected; clearing link key for fresh pairing\r\n");
    err = ds5_bt_forget_bonded_peer();
    if (err != 0) {
        printf("DS5 BT: failed to clear rejected bond (err %d)\r\n", err);
        bond_recovery_retry_at = now + pdMS_TO_TICKS(1000U);
        return;
    }

    bonded_peer_valid = false;
    bond_recovery_pending = false;
    bond_recovery_retry_at = 0U;
    memset(&candidate, 0, sizeof(candidate));
    ds5_bt_disable_page_scan();
    ds5_bt_set_state(DS5_BT_STATE_IDLE);
    ds5_bt_open_pairing_window();
}

static void ds5_bt_process_page_scan_retry(TickType_t now)
{
    if (!page_scan_restore_pending || !bonded_peer_valid ||
        (active_connection != NULL) ||
        !ds5_bt_deadline_expired(now, page_scan_retry_at)) {
        return;
    }

    ds5_bt_enable_bonded_page_scan();
}

int ds5_bt_start_discovery_internal(void)
{
    int err;

    if ((bluetooth_state != DS5_BT_STATE_IDLE) || discovery_in_flight ||
        !discovery_allowed || (active_connection != NULL)) {
        return -EBUSY;
    }

    memset(&candidate, 0, sizeof(candidate));
    ++discovery_session;
    discovery_in_flight = true;
    discovery_stop_requested = false;
    ds5_bt_set_state(DS5_BT_STATE_DISCOVERING);
    err = bt_br_discovery_start(&discovery_param, discovery_results,
                                DS5_BT_DISCOVERY_RESULT_COUNT,
                                ds5_bt_discovery_complete);
    if (err != 0) {
        discovery_in_flight = false;
        printf("DS5 BT: discovery start failed (err %d)\r\n", err);
        ds5_bt_set_state(DS5_BT_STATE_IDLE);
        discovery_retry_at = xTaskGetTickCount() +
                             pdMS_TO_TICKS(DS5_BT_DISCOVERY_RETRY_MS);
        return err;
    }

    printf("DS5 BT: BR/EDR discovery started (session %lu)\r\n",
           (unsigned long)discovery_session);
    return 0;
}

static void ds5_bt_stop_discovery_if_needed(TickType_t now)
{
    int err;

    if (!discovery_in_flight || discovery_allowed ||
        discovery_stop_requested ||
        !ds5_bt_deadline_expired(now, discovery_retry_at)) {
        return;
    }

    discovery_stop_requested = true;
    err = bt_br_discovery_stop();
    if ((err == 0) || (err == -EALREADY)) {
        /* The pinned SDK does not invoke the completion callback on stop. */
        discovery_in_flight = false;
        discovery_stop_requested = false;
        discovery_retry_at = now + pdMS_TO_TICKS(DS5_BT_DISCOVERY_RETRY_MS);
        memset(&candidate, 0, sizeof(candidate));
        if (bluetooth_state == DS5_BT_STATE_DISCOVERING) {
            ds5_bt_set_state(DS5_BT_STATE_IDLE);
        }
        return;
    }

    discovery_stop_requested = false;
    discovery_retry_at = now + pdMS_TO_TICKS(100U);
    printf("DS5 BT: discovery stop failed (err %d)\r\n", err);
}

static void ds5_bt_policy_handle_timeout(TickType_t now)
{
    ds5_bt_state_t state = bluetooth_state;

    if (!ds5_bt_deadline_expired(now, link_deadline)) {
        return;
    }

    printf("DS5 BT: %s phase timed out\r\n", ds5_bt_state_name(state));
    if (active_connection != NULL) {
        if (state != DS5_BT_STATE_DISCONNECTING) {
            (void)ds5_bt_disconnect_active(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        } else {
            bool bonded_link = active_peer_is_bonded;

            /* Do not leave a dead ACL pointer blocking every future link. */
            (void)bt_conn_disconnect(active_connection,
                                     BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            ds5_bt_reset_link_state();
            ds5_bt_recover_after_link(bonded_link);
        }
    } else {
        ds5_bt_reset_link_state();
        ds5_bt_recover_after_link(false);
    }
}

static void ds5_bt_policy_step(void)
{
    TickType_t now = xTaskGetTickCount();

    ds5_bt_lifecycle_lock();

    ds5_bt_process_link_events();
    ds5_bt_policy_handle_timeout(now);
    ds5_bt_check_link_liveness();
    ds5_bt_configure_link_supervision_timeout();
    ds5_bt_process_bond_recovery(now);
    ds5_bt_process_page_scan_retry(now);

    if (pairing_window_active &&
        ds5_bt_deadline_expired(now, pairing_window_deadline)) {
        pairing_window_active = false;
        discovery_allowed = false;
    }

    ds5_bt_stop_discovery_if_needed(now);

    if ((active_connection != NULL) ||
        (bluetooth_state == DS5_BT_STATE_SECURING) ||
        (bluetooth_state == DS5_BT_STATE_L2CAP_CONNECTING) ||
        (bluetooth_state == DS5_BT_STATE_READY) ||
        (bluetooth_state == DS5_BT_STATE_DISCONNECTING) ||
        (bluetooth_state == DS5_BT_STATE_ACL_CONNECTING)) {
        goto out;
    }

    if ((bluetooth_state == DS5_BT_STATE_CANDIDATE_READY) &&
        candidate.valid && ds5_bt_deadline_expired(now, candidate_retry_at)) {
        if (ds5_bt_connect_candidate() != 0) {
            candidate_retry_at = now + pdMS_TO_TICKS(1000U);
        }
        goto out;
    }

    if ((bluetooth_state == DS5_BT_STATE_IDLE) && discovery_allowed &&
        !discovery_in_flight &&
        ds5_bt_deadline_expired(now, discovery_retry_at)) {
        (void)ds5_bt_start_discovery_internal();
    }

out:
    ds5_bt_lifecycle_unlock();
}

static TickType_t ds5_bt_policy_wait_ticks(void)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t wait_ticks = portMAX_DELAY;
    TickType_t candidate_wait;
    TickType_t discovery_wait;
    TickType_t window_wait;
    TickType_t link_wait;
    TickType_t bond_wait;
    TickType_t page_scan_wait;
    TickType_t link_state_wait;

    ds5_bt_lifecycle_lock();

    link_wait = ds5_bt_ticks_until(now, link_deadline);
    if (link_wait < wait_ticks) {
        wait_ticks = link_wait;
    }

    window_wait = pairing_window_active ?
        ds5_bt_ticks_until(now, pairing_window_deadline) : portMAX_DELAY;
    if (window_wait < wait_ticks) {
        wait_ticks = window_wait;
    }

    discovery_wait = ((bluetooth_state == DS5_BT_STATE_IDLE) &&
                      discovery_allowed && !discovery_in_flight) ?
        ds5_bt_ticks_until(now, discovery_retry_at) : portMAX_DELAY;
    if (discovery_in_flight && !discovery_allowed &&
        !discovery_stop_requested) {
        discovery_wait = ds5_bt_ticks_until(now, discovery_retry_at);
    }
    if (discovery_wait < wait_ticks) {
        wait_ticks = discovery_wait;
    }

    candidate_wait = (bluetooth_state == DS5_BT_STATE_CANDIDATE_READY) ?
        ds5_bt_ticks_until(now, candidate_retry_at) : portMAX_DELAY;
    if (candidate_wait < wait_ticks) {
        wait_ticks = candidate_wait;
    }

    bond_wait = bond_recovery_pending ?
        ds5_bt_ticks_until(now, bond_recovery_retry_at) : portMAX_DELAY;
    if (bond_wait < wait_ticks) {
        wait_ticks = bond_wait;
    }

    page_scan_wait = (page_scan_restore_pending &&
                      (active_connection == NULL)) ?
        ds5_bt_ticks_until(now, page_scan_retry_at) : portMAX_DELAY;
    if (page_scan_wait < wait_ticks) {
        wait_ticks = page_scan_wait;
    }

    link_state_wait = ((bluetooth_state == DS5_BT_STATE_READY) &&
                       (active_connection != NULL)) ?
        pdMS_TO_TICKS(DS5_BT_LINK_STATE_POLL_MS) : portMAX_DELAY;
    if (link_state_wait < wait_ticks) {
        wait_ticks = link_state_wait;
    }

    ds5_bt_lifecycle_unlock();
    return wait_ticks;
}

void ds5_bt_policy_worker(void *parameter)
{
    (void)parameter;

    while (1) {
        ds5_bt_policy_step();
        (void)ulTaskNotifyTake(pdTRUE, ds5_bt_policy_wait_ticks());
    }
}
