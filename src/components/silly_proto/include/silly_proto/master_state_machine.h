#pragma once

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hal/assert.h"
#include "silly_proto.h"

enum SILLY_MASTER_STATE {
    _SILLY_MASTER_STATE_UNSPEC,
    SILLY_MASTER_STATE_STAND_DOWN,
    SILLY_MASTER_STATE_OPERATE_TALKING,
    SILLY_MASTER_STATE_OPERATE_WATCH_NETWORK,
    _SILLY_MASTER_STATE_SIZE,
};
extern bool silly_master_state_machine_event_sub_map[_SILLY_EVENT_SIZE];

struct silly_master_state_machine {
    void *parent;
    TaskHandle_t self_handle;
    uint8_t talking_budget;

    enum SILLY_MASTER_STATE state;
    struct silly_proto_shared_ctx *shared_ctx;

    // constructed by self
    QueueHandle_t event_queue_handle;

    // queues exposed to the user
    // constructed by self
    QueueHandle_t tx_from_upper_queue;
    QueueHandle_t rx_to_upper_queue;

    /**
     * master_death_timeout
     * - initiate: entry of `MASTER_MODE::STAND_DOWN`
     * - reset: recv in the emitter task
     * - delete: exit of `MASTER_MODE::STAND_DOWN`
     */
    esp_timer_handle_t master_death_timeout;
    SemaphoreHandle_t master_death_timeout_mu;

    /**
     * network_stop_timeout
     * - initiate: entry of `MASTER_MODE::WATCH_NETWORK`
     * - reset: recv in the emitter task
     * - delete: exit of `MASTER_MODE::WATCH_NETWORK`
     */
    esp_timer_handle_t network_stop_timeout;
    SemaphoreHandle_t network_stop_timeout_mu;

    struct silly_emitter_emit_elem *token;
};
void assert_silly_master_state_machine(
    struct silly_master_state_machine *_Nonnull self)
{
    assert(self);
    assert(self->self_handle);
    assert(self->shared_ctx);
}

/**
 * 1. assign the shared ctx
 * 2. create tx and rx queues
 * 3. create event_queue_handle
 * 4. assign initial state
 * 5. assign the master capability
 */
struct silly_master_state_machine *new_silly_master_state_machine_task(
    struct silly_proto_shared_ctx *proto_shared_ctx,
    size_t master_tx_q_size,
    size_t master_rx_q_size,
    silly_master_caps_t caps);

int silly_task_master_dispatcher();

int silly_master_do_discovery(struct silly_master_state_machine *self, void *);

int silly_master_do_broadcast_topology(
    struct silly_master_state_machine *self, void *);

int silly_master_do_consume_msg(
    struct silly_master_state_machine *self, struct silly_proto_header *hdr);

int silly_master_do_master_negotiation(
    struct silly_master_state_machine *self, void *);

/**
 * @brief decrement the talking counter, send token to next if 0
 */
int silly_master_do_send_token_to_next(struct silly_master_state_machine *self);

int task_start_silly_master_state_machine(
    struct silly_master_state_machine *_Nonnull self);
