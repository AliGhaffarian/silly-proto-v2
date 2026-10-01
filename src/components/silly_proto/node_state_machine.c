#include "silly_proto/node_state_machine.h"
#include "errno.h"
#include "esp_log.h"
#include "hal/assert.h"
#include "hal/uart_types.h"
#include "silly_proto/emitter.h"
//#include "silly_proto/master_state_machine.h"
#include "silly_proto/receiver.h"
#include "silly_proto/sender.h"
#include "silly_proto/silly_proto.h"
#include "silly_proto/slave_state_machine.h"
#include "uart_utils.h"

/**
 * @brief set the network topology to the one encoded in the pkt
 */
int silly_do_set_network_topology(
    struct silly_node_state_machine *_Nonnull self,
    void *_Nonnull caller,
    const struct silly_proto_header *_Nonnull hdr)
{
    assert(self);
    assert(hdr);
    assert(caller);
    assert(self->slave_state_machine);
    assert(self->shared_ctx);

    bool lock_acquired = false;
    int err = 0;
    __cleanup_free_network_topology__ struct silly_network_topology
        *new_topology = NULL;
    uint8_t my_talking_budget = 0;

    {
        SCOPED_SEMAPHORE_TAKE(
            self->shared_ctx->master_node_mu, portMAX_DELAY, lock_acquired);
        if(!lock_acquired) {
            ESP_LOGE(__func__, "failed to acquire the lock self->master_node");
            return -1;
        }

        // if this node is the master, do nothing (master already called this)
        if(caller == self->slave_state_machine &&
           self->shared_ctx->master_node == self->shared_ctx->my_mac) {
            return 0;
        }
    }

    err = silly_decode_local_topology_from_hdr(
        &new_topology, self->shared_ctx->my_mac, &my_talking_budget, hdr);

    if(err < 0) {
        ESP_LOGE(__func__, "failed to decode the topology from header");
        return err;
    }
    {
        SCOPED_SEMAPHORE_TAKE(
            self->shared_ctx->topology_mu, portMAX_DELAY, lock_acquired);
        if(!lock_acquired) {
            ESP_LOGE(__func__, "failed to acquire the lock self->topology");
            return -1;
        }
        self->shared_ctx->topology = MOVE(&new_topology);
        self->shared_ctx->token_candidate = silly_next_of_dev(
            self->shared_ctx->my_mac, self->shared_ctx->topology);
    }

    {
        SCOPED_SEMAPHORE_TAKE(
            self->shared_ctx->talking_budget_mu, portMAX_DELAY, lock_acquired);
        if(!lock_acquired) {
            ESP_LOGE(
                __func__, "failed to acquire the lock self->talking_budget_mu");
            return -1;
        }
        self->shared_ctx->talking_budget = my_talking_budget;
    }

    return 0;
}

/**
 * @brief set the master to the one encoded in the pkt
 */
int silly_do_set_master_info(
    struct silly_node_state_machine *_Nonnull self,
    void *_Nonnull caller,
    const struct silly_proto_header *_Nonnull hdr)
{
    assert(self);
    assert(hdr);
    assert(caller);
    assert(self->slave_state_machine);
    assert(self->shared_ctx);

    int err = 0;
    __cleanup_free__ struct silly_master_info_elem *master_info = NULL;
    bool lock_acquired = false;
    {
        SCOPED_SEMAPHORE_TAKE(
            self->shared_ctx->master_node_mu, portMAX_DELAY, lock_acquired);
        if(!lock_acquired) {
            ESP_LOGE(__func__, "failed to acquire the lock self->master_node");
            return -1;
        }

        // if this node is the master, do nothing (master already called this)
        if(caller == self->slave_state_machine &&
           self->shared_ctx->master_node == self->shared_ctx->my_mac) {
            return 0;
        }
    }

    err = silly_decode_master_info_from_hdr(&master_info, hdr);
    if(err) {
        ESP_LOGE(__func__, "failed to decode the topology maser info from hdr");
        return err;
    }

    {
        SCOPED_SEMAPHORE_TAKE(
            self->shared_ctx->master_node_mu, portMAX_DELAY, lock_acquired);
        if(!lock_acquired) {
            ESP_LOGE(
                __func__, "failed to acquire the lock self->master_node_mu");
            return -1;
        }
        self->shared_ctx->master_node = master_info->dev;
    }

    return 0;
}

/**
 * @brief respond to discovery query
 */
int silly_do_handle_discovery_query(
    struct silly_node_state_machine *_Nonnull self,
    const struct silly_proto_header *_Nonnull hdr)
{
    assert(self);
    assert(hdr);
    assert(self->shared_ctx);

    int err = 0;
    bool should_answer = false;
    __cleanup_free__ struct silly_master_discovery_query_elem *discovery_query =
        NULL;
    __cleanup_free__ struct silly_proto_header *discovery_query_reply = NULL;

    err = silly_dencode_master_discovery_query_into_hdr(&discovery_query, hdr);
    if(err) {
        ESP_LOGE(
            __func__, "failed to decode the maser discovery query from hdr");
        return err;
    }

    should_answer =
        ((self->shared_ctx->my_mac & discovery_query->mask) ==
         discovery_query->prefix);
    if(!should_answer)
        return 0;

    discovery_query_reply = silly_make_discovery_query_reply(self->shared_ctx);
    if(!discovery_query_reply) {
        ESP_LOGE(__func__, "failed to make a discovery query reply");
    }

    silly_do_tx(
        self, (void **)&discovery_query_reply, discovery_query_reply->len);

    return 0;
}

int silly_do_reevaluate_token_candidate(
    struct silly_node_state_machine *_Nonnull self)
{
    assert(self);
    assert(self->shared_ctx);
    assert(self->shared_ctx->topology);

    bool lock_acquired = false;
    silly_dev_t new_candidate = 0;

    {
        SCOPED_SEMAPHORE_TAKE(
            self->shared_ctx->topology_mu, portMAX_DELAY, lock_acquired);
        if(!lock_acquired) {
            ESP_LOGE(__func__, "failed to acquire the lock self->topology_mu");
            return -1;
        }
        new_candidate = silly_next_of_dev(
            self->shared_ctx->token_candidate, self->shared_ctx->topology);
    }

    if(new_candidate == self->shared_ctx->my_mac) {
        return SE_TOKEN_CANDIDATE_IS_ME;
    }

    self->shared_ctx->token_candidate = new_candidate;
    return 0;
}

int silly_do_tx(
    struct silly_node_state_machine *_Nonnull self,
    void *_Nonnull *_Nonnull data,
    size_t len)
{

    assert(self);
    assert(self->master_state_machine);
    assert(self->slave_state_machine);
    assert(self->sender);
    assert(data);
    assert(*data);

    BaseType_t queue_send_result = errQUEUE_FULL;
    struct sender_input_elem emit_elem = {
        .free_data = free, .data = MOVE(data), .len = len};

    queue_send_result = xQueueSend(self->shared_ctx->tx_queue, &emit_elem, 0);
    if(queue_send_result != pdTRUE) {
        ESP_LOGE(__func__, "failed to enqueue a message");
        if(emit_elem.free_data)
            emit_elem.free_data(emit_elem.data);
        return -1;
    }

    return 0;
}

int silly_state_machine_write_to_slave(
    struct silly_node_state_machine *_Nonnull self,
    struct silly_tx_queue_elem *_Nonnull *_Nonnull tx_elem,
    TickType_t ticks_to_wait,
    silly_dev_t dst)
{
    assert_silly_node_state_machine(self);
    assert(tx_elem);
    assert(*tx_elem);

    int err = 0;

    err = xQueueSend(
        self->slave_state_machine->tx_from_upper_queue, tx_elem, ticks_to_wait);

    if(err == pdTRUE) {
        *tx_elem = NULL;
        return 0;
    } else {
        if((*tx_elem)->data)
            free((*tx_elem)->data);
        *tx_elem = NULL;
        return -1;
    }
}

int silly_state_machine_read_from_slave(
    struct silly_node_state_machine *_Nonnull self,
    struct silly_rx_queue_elem **_Nonnull rx_elem,
    TickType_t ticks_to_wait,
    silly_dev_t *_Nonnull src)
{
    assert_silly_node_state_machine(self);
    assert(rx_elem);

    int err = 0;

    err = xQueueReceive(
        self->slave_state_machine->rx_to_upper_queue, rx_elem, ticks_to_wait);

    if(err == pdTRUE)
        return 0;
    else
        return -1;
}

int silly_state_machine_write_to_master(
    struct silly_node_state_machine *_Nonnull self,
    struct silly_tx_queue_elem *_Nonnull *_Nonnull tx_elem,
    TickType_t ticks_to_wait,
    silly_dev_t dst)
{
    assert_silly_node_state_machine(self);
    assert(tx_elem);
    assert(*tx_elem);

    int err = 0;

    err = xQueueSend(
        self->master_state_machine->tx_from_upper_queue,
        tx_elem,
        ticks_to_wait);

    if(err == pdTRUE) {
        *tx_elem = NULL;
        return 0;
    } else {
        if((*tx_elem)->data)
            free((*tx_elem)->data);
        *tx_elem = NULL;
        return -1;
    }
}

int silly_state_machine_read_from_master(
    struct silly_node_state_machine *_Nonnull self,
    struct silly_rx_queue_elem **_Nonnull rx_elem,
    TickType_t ticks_to_wait,
    silly_dev_t *_Nonnull src)
{
    assert_silly_node_state_machine(self);
    assert(rx_elem);

    int err = 0;

    err = xQueueReceive(
        self->master_state_machine->rx_to_upper_queue, rx_elem, ticks_to_wait);

    if(err == pdTRUE)
        return 0;
    else
        return -1;
}

int start_silly_node_state_machine(
    uart_port_t uart_port,
    size_t slave_tx_q_size,
    size_t slave_rx_q_size,
    size_t master_tx_q_size,
    size_t master_rx_q_size,
    silly_dev_t dev,
    silly_master_caps_t caps,
    struct silly_node_state_machine **_Nonnull out_me)
{
    assert(out_me);

    struct silly_proto_shared_ctx *proto_shared_ctx = NULL;
    struct receiver_task *receiver_task = NULL;
    struct silly_node_state_machine *me = calloc(1, sizeof(*me));
    struct emitter_task *emitter = NULL;
    struct locked_uart_port *locked_port = calloc(1, sizeof(*locked_port));
    struct sender_task *sender = NULL;
    struct silly_slave_state_machine *slave_machine = NULL;
    struct silly_master_state_machine *master_machine = NULL;
    struct emitter_subscriber *slave_emitter_subscriber =
        calloc(1, sizeof(*slave_emitter_subscriber));
    struct emitter_subscriber *master_emitter_subscriber =
        calloc(1, sizeof(*master_emitter_subscriber));
    struct emitter_subscriber *token_passer_emitter_subscriber =
        calloc(1, sizeof(*token_passer_emitter_subscriber));

    int err = 0;

    uint32_t queue_len_arr[_EMITTER_EVENT_SOURCES_SIZE] = {
        [_EMITTER_EVENT_SOURCES_UNSPEC] = 0,
        [EMITTER_EVENT_SOURCES_RECV] = RECEIVER_DELIVER_QUEUE_SIZE,
        [EMITTER_EVENT_SOURCES_SENDER] = SENDER_DELIVER_QUEUE_SIZE,
        [EMITTER_EVENT_SOURCES_TIMER_EXPIRE] = SILLY_MAX_TIMERS};

    if(!(me && locked_port && slave_emitter_subscriber &&
         master_emitter_subscriber && token_passer_emitter_subscriber)) {
        ESP_LOGE(__func__, "out of memory");
        return -ENOMEM;
    }

    // ### init the structures, don't start anything yet ###

    // locked_port
    locked_port->port = uart_port;
    locked_port->port_mu = xSemaphoreCreateMutex();
    if(!locked_port->port_mu) {
        ESP_LOGE(__func__, "out of memory");
        return -ENOMEM;
    }

    // init the emitter
    emitter = new_emitter_task(queue_len_arr);

    // sender
    sender = new_sender_task(
        SENDER_DEFAULT_QUEUE_SIZE,
        emitter->event_notify_queue,
        EMITTER_EVENT_SOURCES_SENDER,
        locked_port,
        emitter->event_source_arr[EMITTER_EVENT_SOURCES_SENDER].source_queue);

    // proto_shared_ctx
    proto_shared_ctx = silly_new_proto_shared_ctx(dev, sender->input_queue);
    if(!proto_shared_ctx) {
        ESP_LOGE(__func__, "failed to initialize shared context");
        return -1;
    }

    // receiver task
    receiver_task = new_receiver_task(
        emitter->event_notify_queue,
        EMITTER_EVENT_SOURCES_RECV,
        locked_port,
        emitter->event_source_arr[EMITTER_EVENT_SOURCES_RECV].source_queue);
    receiver_task->parent = me;

    // slave, init and add self as a subscriber
    slave_machine = new_silly_slave_state_machine_task(
        proto_shared_ctx, slave_tx_q_size, slave_rx_q_size);
    slave_machine->parent = me;

    memcpy(
        slave_emitter_subscriber->subscribed_event_map,
        silly_slave_state_machine_event_sub_map,
        _SILLY_EVENT_SIZE);
    slave_emitter_subscriber->sub_event_queue =
        slave_machine->event_queue_handle;

    add_subsriber_to_emitter(emitter, &slave_emitter_subscriber);

    //TODO:
    // master, init and add self as a subscriber
    // master_machine = new_silly_master_state_machine_task(
    //     proto_shared_ctx, master_tx_q_size, master_rx_q_size, caps);
    // master_machine->parent = me;

    // memcpy(
    //     master_emitter_subscriber->subscribed_event_map,
    //     silly_master_state_machine_event_sub_map,
    //     _SILLY_EVENT_SIZE);
    // master_emitter_subscriber->sub_event_queue =
    //     master_machine->event_queue_handle;

    //add_subsriber_to_emitter(emitter, &master_emitter_subscriber);

    // token passer, init and add self as a subscriber
    err = new_token_passer(&me->token_passer);
    me->token_passer->parent = me;
    if(err) {
        ESP_LOGE(__func__, "failed to create token passer");
        return err;
    }
    memcpy(
        token_passer_emitter_subscriber->subscribed_event_map,
        token_passer_state_machine_event_sub_map,
        _SILLY_EVENT_SIZE);
    token_passer_emitter_subscriber->sub_event_queue =
        me->token_passer->event_queue_handle;

    // init me
    // TODO:
    me->receiver = MOVE(&receiver_task);
    me->shared_ctx = MOVE(&proto_shared_ctx);
    me->sender = MOVE(&sender);
    //me->master_state_machine = MOVE(&master_machine);
    me->slave_state_machine = MOVE(&slave_machine);
    me->emitter = MOVE(&emitter);
    //me->read_from_master = silly_state_machine_read_from_master;
    //me->write_to_master = silly_state_machine_write_to_master;
    me->read_from_slave = silly_state_machine_read_from_slave;
    me->write_to_slave = silly_state_machine_write_to_slave;

    // ### now we start everything ###

    err = task_start_receiver(me->receiver);
    if(err) {
        ESP_LOGE(__func__, "failed to start receiver");
        return err;
    }

    err = task_start_sender(sender);
    if(err) {
        ESP_LOGE(__func__, "failed to start sender");
        return err;
    }

    err = task_start_emitter(emitter);
    if(err) {
        ESP_LOGE(__func__, "failed to start emitter");
        return err;
    }

    err = task_start_silly_slave_state_machine(slave_machine);
    if(err) {
        ESP_LOGE(__func__, "failed to start slave");
        return err;
    }
    //TODO:
    // err = task_start_silly_master_state_machine(master_machine);
    // if(err) {
    //     ESP_LOGE(__func__, "failed to start master");
    //     return err;
    // }

    *out_me = me;
    assert_silly_node_state_machine(*out_me);
    return 0;
}
