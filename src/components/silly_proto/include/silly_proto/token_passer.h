#pragma once

#include "freertos/FreeRTOS.h"
#include "silly_proto/node_state_machine.h"

#define TOKEN_PASSER_DEFAULT_QUEUE_SIZE        10
#define TOKEN_PASSER_INPUT_QUEUE_FLUSH_TIMEOUT 50

struct sync_state_machine_token_passer {
    void *parent;
    QueueHandle_t event_queue_handle;
    QueueHandle_t tx_handle;

    /**
     * TODO: implement
     * slave_token_deadline
     * - initiate: entry of `SLAVE_MODE::OPERATE::PASS_TOKEN`
     * - reset: None
     * - delete: exit of `SLAVE_MODE::OPERATE::PASS_TOKEN`
     * NOTE: doesn't need a mutex as it's used by the transitions only
     */
    esp_timer_handle_t slave_token_deadline;
};

// all zero, to make emitter not emit anything to us while inactive
extern bool token_passer_state_machine_event_sub_map[_SILLY_EVENT_SIZE];

// copy this to the emitter's sub map when we are active
extern bool active_token_passer_state_machine_event_sub_map[_SILLY_EVENT_SIZE];

struct
    silly_node_state_machine; // node_state_machine.h and token_passer.h include each other, so we need this forward declaration

/**
 * @brief pass token to next
 * 1. initialize the sub map in the emitter
 * 2. send the token to the token candidate
 * 3. wait on event_handle_t
 * 4. on slave_token_deadline:
 *      4.1. reevaluate the token candidate
 *      4.2. send the token to candidate
 * 5. on recv_any, return
 * 6. return when done passing the token
 */
void sync_token_passer_mainloop(
    struct silly_node_state_machine *_Nonnull parent);

static void
activate_our_emitter_sub_map(struct silly_node_state_machine *_Nonnull parent);

static void deactivate_our_emitter_sub_map(
    struct silly_node_state_machine *_Nonnull parent);

static int send_token_to_current_candidate(
    struct silly_node_state_machine *_Nonnull parent);

static void
xqrecv_until_timeout(struct sync_state_machine_token_passer *_Nonnull self);

int new_token_passer(
    struct sync_state_machine_token_passer *_Nonnull *_Nonnull self);

/**
 * @brief emit timer expire elem to emitter
 * 1. emit the timer expiration event to
 * self->event_source_arr[EMITTER_EVENT_SOURCES_TIMER_EXPIRE]
 * 2. emit the event notification to self->event_notify_queue
 * 3. delete the timer
 * 4. return EMITTER_CALLBACK_ACTION_DELETE_CALLBACK
 * dispatch method: interrupt
 */
void isr_expire_slave_token_deadline(void *_Nonnull vself);

static void create_timer_slave_token_deadline(
    struct sync_state_machine_token_passer *_Nonnull self);
