#pragma once

#include "esp_task.h"
#include "esp_timer.h"
#include "hal/assert.h"
#include "silly_proto/master_state_machine.h"
#include "silly_proto/receiver.h"
#include "silly_proto/sender.h"
#include "silly_proto/silly_proto.h"
#include "silly_proto/slave_state_machine.h"
#include "silly_proto/token_passer.h"

struct sync_state_machine_token_passer;
struct silly_node_state_machine {
    struct silly_proto_shared_ctx *shared_ctx;

    struct silly_slave_state_machine *slave_state_machine;
    struct silly_master_state_machine *master_state_machine;
    struct sync_state_machine_token_passer *token_passer;

    int (*write_to_slave)(
        struct silly_node_state_machine *_Nonnull self,
        struct silly_tx_queue_elem *_Nonnull *_Nonnull tx_elem,
        TickType_t ticks_to_wait,
        silly_dev_t dst);

    int (*read_from_slave)(
        struct silly_node_state_machine *_Nonnull self,
        struct silly_rx_queue_elem **_Nonnull rx_elem,
        TickType_t ticks_to_wait,
        silly_dev_t *_Nonnull src);

    int (*write_to_master)(
        struct silly_node_state_machine *_Nonnull self,
        struct silly_tx_queue_elem *_Nonnull *_Nonnull tx_elem,
        TickType_t ticks_to_wait,
        silly_dev_t dst);

    int (*read_from_master)(
        struct silly_node_state_machine *_Nonnull self,
        struct silly_rx_queue_elem **_Nonnull rx_elem,
        TickType_t ticks_to_wait,
        silly_dev_t *_Nonnull src);

    struct receiver_task *receiver;
    struct sender_task *sender;
    struct emitter_task *emitter;
};

void assert_silly_node_state_machine(
    struct silly_node_state_machine *_Nonnull self)
{
    assert(self);
    assert(self->shared_ctx);
    assert_silly_slave_state_machine(self->slave_state_machine);
    assert_silly_master_state_machine(self->master_state_machine);
    assert(self->write_to_master);
    assert(self->read_from_master);
    assert(self->write_to_slave);
    assert(self->read_from_slave);
    assert_receiver_task(self->receiver);
    assert_sender_task(self->sender);
    assert_emitter_task(self->emitter);
}

// the following api is shared between master and slave,
// and to ensure synchronization of the shared state,
// we need to ensure they both call the same function.
// the shared function follow some ground rules:
// - if the function is meant to set a shared context,
//      and it is the master of the network, don't change anything
//      because the master side logic already has set the context.
//      for this to happen we need to require the caller to provide
//      the following arguments:
//        1. struct silly_node_state_machine *self
//        2. struct silly_slave_state_machine *caller
//      (the caller can infer the outer state machine by using CONTAINER_OF)
//      we can then verify if the caller is the master or the slave,
//      and if it is the master change the shared the state unconditionally

/**
 * @brief set the network topology to the one encoded in the pkt
 */
int silly_do_set_network_topology(
    struct silly_node_state_machine *_Nonnull self,
    void *_Nonnull caller,
    const struct silly_proto_header *_Nonnull hdr);
/**
 * @brief set the master to the one encoded in the pkt
 */
int silly_do_set_master_info(
    struct silly_node_state_machine *_Nonnull self,
    void *_Nonnull caller,
    const struct silly_proto_header *_Nonnull hdr);
/**
 * @brief respond to discovery query
 */
int silly_do_handle_discovery_query(
    struct silly_node_state_machine *_Nonnull self,
    const struct silly_proto_header *_Nonnull hdr);

#define SE_TOKEN_CANDIDATE_IS_ME -2
/**
 * @brief evaluate self->token_candidate
 * @return -1 on lock acquisition failure, `SE_TOKEN_CANDIDATE_IS_ME` on new candidate being self, 0
 * othersise
 */
int silly_do_reevaluate_token_candidate(
    struct silly_node_state_machine *_Nonnull self);

/**
 * @brief hand the sender task the data
 */
int silly_do_tx(
    struct silly_node_state_machine *_Nonnull self,
    void *_Nonnull *_Nonnull data,
    size_t len);

/**
 * the following are the functions pointers provided in silly_node_state_machine
 * TODO: reroute the following to a DRYed internal function
 */

int silly_state_machine_write_to_slave(
    struct silly_node_state_machine *_Nonnull self,
    struct silly_tx_queue_elem *_Nonnull *_Nonnull tx_elem,
    TickType_t ticks_to_wait,
    silly_dev_t dst);

int silly_state_machine_read_from_slave(
    struct silly_node_state_machine *_Nonnull self,
    struct silly_rx_queue_elem **_Nonnull rx_elem,
    TickType_t ticks_to_wait,
    silly_dev_t *_Nonnull src);

int silly_state_machine_write_to_master(
    struct silly_node_state_machine *_Nonnull self,
    struct silly_tx_queue_elem *_Nonnull *_Nonnull tx_elem,
    TickType_t ticks_to_wait,
    silly_dev_t dst);

int silly_state_machine_read_from_master(
    struct silly_node_state_machine *_Nonnull self,
    struct silly_rx_queue_elem **_Nonnull rx_elem,
    TickType_t ticks_to_wait,
    silly_dev_t *_Nonnull src);

/**
 * @brief the top level function of silly_proto, initializes everything
 * not a task itself, so the function name doesn't start with `task_*`
 *
 * TODO: leaks memory on failure
 * TODO: handle and log all errors
 *
 * init and start the following:
 * 1. locked port
 * 2. shared ctx
 * 3. receiver
 * 4. sender
 * 5. emitter
 * 6. slave
 * 7. master
 */
int start_silly_node_state_machine(
    uart_port_t uart_port,
    size_t slave_tx_q_size,
    size_t slave_rx_q_size,
    size_t master_tx_q_size,
    size_t master_rx_q_size,
    silly_dev_t dev,
    silly_master_caps_t caps,
    struct silly_node_state_machine **_Nonnull out_me);
