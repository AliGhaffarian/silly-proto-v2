#pragma once

#include "emitter.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hal/assert.h"
#include "silly_proto.h"
#include "silly_proto/node_state_machine.h"

#define SLAVE_TASK_NAME  "slave"
#define SLAVE_STASK_SIZE (SILLY_DEFAULT_STACK_SIZE + 1)
#define SLAVE_PRIORITY   (SILLY_DEFAULT_TASK_PRIORITY + 1)

enum SILLY_SLAVE_STATE {
    _SILLY_SLAVE_STATE_UNSPEC,
    SILLY_SLAVE_STATE_OPERATE_WAIT_ON_TOKEN,
    SILLY_SLAVE_STATE_OPERATE_TALKING,
    _SILLY_SLAVE_STATE_SIZE,
};
#define SLAVE_INITIAL_STATE SILLY_SLAVE_STATE_OPERATE_WAIT_ON_TOKEN

extern bool silly_slave_state_machine_event_sub_map[_SILLY_EVENT_SIZE];

struct silly_slave_state_machine {
    void *parent;
    TaskHandle_t self_handle;
    uint8_t talking_budget;

    enum SILLY_SLAVE_STATE state;
    struct silly_proto_shared_ctx *shared_ctx;

    // constructed by self
    QueueHandle_t event_queue_handle;

    // queues exposed to the user
    // constructed by self
    // elem: silly_[rx|tx]_queue_elem (by pointer)
    QueueHandle_t tx_from_upper_queue;
    QueueHandle_t rx_to_upper_queue;

    struct silly_emitter_emit_elem *token;
};
void assert_silly_slave_state_machine(
    struct silly_slave_state_machine *_Nonnull self)
{
    assert(self);
    assert(self->self_handle);
    assert(self->shared_ctx);
}

struct slave_fsm_entry {
    enum SILLY_SLAVE_STATE (*action)(
        struct silly_slave_state_machine *_Nonnull self,
        const struct silly_emitter_emit_elem *_Nonnull input_elem);

    bool (*guard)(
        struct silly_slave_state_machine *_Nonnull self,
        const struct silly_emitter_emit_elem *_Nonnull input_elem);
};
/**
 * actions regarding an event in each state
 * if there is an action for [state][event]:
 *  0. call the guard, to see if we need to take action
 *      0.1 if false, discard this event
 *  1. call the current event's exit
 *  2. call the action
 *  3. call the next state's enter
 * @return the next state
 **/
extern struct slave_fsm_entry slave_fsm_table[_SILLY_SLAVE_STATE_SIZE]
                                             [_SILLY_EVENT_SIZE];

/**
 * reactions regarding an event in each state (no transitions)
 * naming convention:
 * if there is an action for [state][event]:
 *  1. call the action
 **/
extern void (
    *slave_fsm_reaction_table[_SILLY_EVENT_SIZE][_SILLY_SLAVE_STATE_SIZE])(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem);

extern void (*slave_state_enter[_SILLY_EVENT_SIZE])(
    struct silly_slave_state_machine *_Nonnull self);

extern void (*slave_state_exit[_SILLY_EVENT_SIZE])(
    struct silly_slave_state_machine *_Nonnull self);

/**
 * 1. assign the shared ctx
 * 2. create tx and rx queues
 * 3. create event_queue_handle
 * 4. assign initial state
 */
struct silly_slave_state_machine *new_silly_slave_state_machine_task(
    struct silly_proto_shared_ctx *shared_ctx,
    size_t tx_from_upper_queue_size,
    size_t rx_to_upper_queue_size);

/**
 * @brief deliver data to user queue
 * doesn't block for enqueue
 */
int silly_slave_do_deliver(
    struct silly_slave_state_machine *_Nonnull self,
    const void *_Nonnull data,
    size_t len);

void slave_do_action(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem);

void slave_do_reaction(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem);

bool exists_noevent_trueguard_action(
    struct silly_slave_state_machine *_Nonnull self);

/**
 * @brief state machine driver task
 * 1. waits on self->event_queue_handle
 * 2. passes the header inside the refcounted_obj dequeued from
 * self->event_queue_handle
 * 3. calls the approapriate actions (synchronously)
 * 4. adjusts self->state
 * 5. releases refcounted_obj dequeued from self->event_queue_handle
 */
int task_slave_mainloop(struct silly_slave_state_machine *_Nonnull self);

int task_start_silly_slave_state_machine(
    struct silly_slave_state_machine *_Nonnull self);

/**
 * fsm entry actions
 * naming conventions: s_sm_<state>_<event>()
 * this looses context on what will the state machine do, and
 *      makes us dependent on the protocols diagram. but i don't
 *      see a good way to encode the behavior into the name for
 *      each fsm entry either.
 */

static enum SILLY_SLAVE_STATE s_sm_operate_collision(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem);

static enum SILLY_SLAVE_STATE s_sm_operate_master_discovery_query(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem);

static enum SILLY_SLAVE_STATE s_sm_operate_recved_network_topology(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem);

static enum SILLY_SLAVE_STATE s_sm_operate_recved_master_info(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem);

static enum SILLY_SLAVE_STATE s_sm_operate_wait_on_token_recv_any(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem);

static enum SILLY_SLAVE_STATE s_sm_operate_wait_on_token_slave_passed_token(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem);

static enum SILLY_SLAVE_STATE s_sm_operate_talking_noevent(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem);

static enum SILLY_SLAVE_STATE s_sm_operate_talking_recv_any(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem);

/**
 * fsm transition guards
 * naming conventions is either of the following, first is preferred:
 *      - s_sm_guard_<condition>()
 *      - s_sm_guard_should_take_<state>_<event>()
 */

static bool s_sm_guard_is_dst_me(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem);

static bool s_sm_guard_tx_data_available_and_have_budget(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem);

/**
 * fsm state exit functions
 * naming conventions: s_sm_<state>_exit()
 */
static void
s_sm_operate_talking_exit(struct silly_slave_state_machine *_Nonnull self);
