#include "silly_proto/slave_state_machine.h"
#include "errno.h"
#include "freertos/FreeRTOS.h"
#include "silly_proto/emitter.h"
#include "silly_proto/node_state_machine.h"
#include "silly_proto/timeout_expire_emit_elem.h"

bool silly_slave_state_machine_event_sub_map[_SILLY_EVENT_SIZE] = {
    [_SILLY_EVENT_UNSPEC] = 0,
    [SILLY_EVENT_NOEVENT] = 0,
    [SILLY_EVENT_SLAVE_TOKEN_DEADLINE] = 0,
    [SILLY_EVENT_NETWORK_STOP_TIMEOUT] = 0,
    [SILLY_EVENT_MASTER_DEATH_TIMEOUT] = 0,
    [SILLY_EVENT_COLLISION] = 1,
    [SILLY_EVENT_SLAVE_PASSED_TOKEN] = 1,
    [SILLY_EVENT_MASTER_DISCOVERY_QUERY] = 1,
    [SILLY_EVENT_RECVED_NETWORK_TOPOLOGY] = 1,
    [SILLY_EVENT_RECVED_MASTER_INFO] = 1,
    [SILLY_EVENT_RECV_ANY] = 1,
};

struct slave_fsm_entry
    slave_fsm_table[_SILLY_SLAVE_STATE_SIZE][_SILLY_EVENT_SIZE] = {
        // wait on token
        [SILLY_SLAVE_STATE_OPERATE_WAIT_ON_TOKEN][SILLY_EVENT_NOEVENT] =
            {
                .action = NULL,
                .guard = NULL,
            },
        [SILLY_SLAVE_STATE_OPERATE_WAIT_ON_TOKEN][SILLY_EVENT_COLLISION] =
            {
                .action = s_sm_operate_collision,
                .guard = NULL,
            },
        [SILLY_SLAVE_STATE_OPERATE_WAIT_ON_TOKEN]
            [SILLY_EVENT_SLAVE_PASSED_TOKEN] =
                {
                    .action = s_sm_operate_wait_on_token_slave_passed_token,
                    .guard = s_sm_guard_is_dst_me,
                },
        [SILLY_SLAVE_STATE_OPERATE_WAIT_ON_TOKEN]
            [SILLY_EVENT_MASTER_DISCOVERY_QUERY] =
                {
                    .action = s_sm_operate_master_discovery_query,
                    .guard = NULL,
                },
        [SILLY_SLAVE_STATE_OPERATE_WAIT_ON_TOKEN]
            [SILLY_EVENT_RECVED_NETWORK_TOPOLOGY] =
                {
                    .action = s_sm_operate_recved_network_topology,
                    .guard = NULL,
                },
        [SILLY_SLAVE_STATE_OPERATE_WAIT_ON_TOKEN]
            [SILLY_EVENT_RECVED_MASTER_INFO] =
                {
                    .action = s_sm_operate_recved_master_info,
                    .guard = NULL,
                },
        [SILLY_SLAVE_STATE_OPERATE_WAIT_ON_TOKEN][SILLY_EVENT_RECV_ANY] =
            {
                .action = s_sm_operate_wait_on_token_recv_any,
                .guard = NULL,
            },

        // talk
        [SILLY_SLAVE_STATE_OPERATE_TALKING][SILLY_EVENT_NOEVENT] =
            {
                .action = s_sm_operate_talking_noevent,
                .guard = s_sm_guard_tx_data_available_and_have_budget,
            },
        [SILLY_SLAVE_STATE_OPERATE_TALKING][SILLY_EVENT_COLLISION] =
            {
                .action = s_sm_operate_collision,
                .guard = NULL,
            },
        [SILLY_SLAVE_STATE_OPERATE_TALKING][SILLY_EVENT_SLAVE_PASSED_TOKEN] =
            {
                .action = NULL,
                .guard = NULL,
            },
        [SILLY_SLAVE_STATE_OPERATE_TALKING]
            [SILLY_EVENT_MASTER_DISCOVERY_QUERY] =
                {
                    .action = s_sm_operate_master_discovery_query,
                    .guard = NULL,
                },
        [SILLY_SLAVE_STATE_OPERATE_TALKING]
            [SILLY_EVENT_RECVED_NETWORK_TOPOLOGY] =
                {
                    .action = s_sm_operate_recved_network_topology,
                    .guard = NULL,
                },
        [SILLY_SLAVE_STATE_OPERATE_TALKING][SILLY_EVENT_RECVED_MASTER_INFO] =
            {
                .action = s_sm_operate_recved_master_info,
                .guard = NULL,
            },
        [SILLY_SLAVE_STATE_OPERATE_TALKING][SILLY_EVENT_RECV_ANY] =
            {
                .action = s_sm_operate_talking_recv_any,
                .guard = NULL,
            },
};

void (*slave_fsm_reaction_table[_SILLY_EVENT_SIZE][_SILLY_SLAVE_STATE_SIZE])(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem) = {0};

void (*slave_state_enter[_SILLY_EVENT_SIZE])(
    struct silly_slave_state_machine *_Nonnull self) = {0};

void (*slave_state_exit[_SILLY_EVENT_SIZE])(
    struct silly_slave_state_machine *_Nonnull self) = {0};

void slave_do_action(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem)
{
    assert(self);
    assert(input_elem);

    enum SILLY_SLAVE_STATE next_state;

    enum SILLY_SLAVE_STATE (*action)(
        struct silly_slave_state_machine *_Nonnull self,
        const struct silly_emitter_emit_elem *_Nonnull input_elem) =
        slave_fsm_table[self->state][input_elem->event].action;

    bool (*guard)(
        struct silly_slave_state_machine *_Nonnull self,
        const struct silly_emitter_emit_elem *_Nonnull input_elem) =
        slave_fsm_table[self->state][input_elem->event].guard;

    if(!action)
        return;

    if(guard && guard(self, input_elem) == false)
        return;

    if(slave_state_exit[self->state])
        slave_state_exit[self->state](self);

    next_state = action(self, input_elem);

    if(slave_state_enter[self->state])
        slave_state_enter[next_state](self);

    self->state = next_state;
}

int silly_slave_do_deliver(
    struct silly_slave_state_machine *_Nonnull self,
    const void *_Nonnull data,
    size_t len)
{
    struct silly_rx_queue_elem *rx_elem = calloc(sizeof(*rx_elem) + len, 1);
    BaseType_t deliver_result = pdFALSE;
    if(!rx_elem)
        return -ENOMEM;

    memcpy(rx_elem->data, data, len);

    deliver_result = xQueueSend(self->rx_to_upper_queue, &rx_elem, 0);

    if(deliver_result == pdTRUE)
        return 0;
    else
        return -1;
}

void slave_do_reaction(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem)
{
    if(slave_fsm_reaction_table[self->state][input_elem->event])
        slave_fsm_reaction_table[self->state][input_elem->event](
            self, input_elem);
}

bool exists_noevent_trueguard_action(
    struct silly_slave_state_machine *_Nonnull self)
{
    assert(self);
    struct slave_fsm_entry noevent_entry =
        slave_fsm_table[self->state][SILLY_EVENT_NOEVENT];
    struct silly_emitter_emit_elem noevent = {
        .event = SILLY_EVENT_NOEVENT, .pkt = NULL};
    if(noevent_entry.action == NULL)
        return false;
    if(!noevent_entry.guard(self, &noevent)) {
        return false;
    }
    return true;
}

int task_slave_mainloop(struct silly_slave_state_machine *_Nonnull self)
{
    BaseType_t q_receive_result = pdFALSE;
    while(1) {
        __cleanup_release_silly_emitter_emit_elem__ struct
            silly_emitter_emit_elem *input_elem = NULL;

        if(exists_noevent_trueguard_action(self)) {
            input_elem = calloc(1, sizeof(*input_elem));
            if(!input_elem) {
                ESP_LOGE(
                    SLAVE_TASK_NAME, "failed to allocate for noevent elem");
                continue;
            }
            input_elem->event = SILLY_EVENT_NOEVENT;
            input_elem->pkt = NULL;
        } else {
            q_receive_result = xQueueReceive(
                self->event_queue_handle, &input_elem, portMAX_DELAY);
            if(q_receive_result != pdTRUE) {
                ESP_LOGE(SLAVE_TASK_NAME, "failed to fetch an emitter elem");
                continue;
            }
        }

        // hold onto one instance of the packet containing the token flag,
        //      since it's teardown's callback is passing the token.
        //      we need to set the self->token before taking any action,
        //      in case the actions cause us to exit the talking state
        if(input_elem->event == SILLY_EVENT_SLAVE_PASSED_TOKEN)
            self->token = input_elem;

        // we assume only one takes action
        // state is changed inside slave_do_action
        slave_do_action(self, input_elem);
        slave_do_reaction(self, input_elem);

        input_elem = NULL;
    }
}

int task_start_silly_slave_state_machine(
    struct silly_slave_state_machine *_Nonnull self)
{
    xTaskCreate(
        task_slave_mainloop,
        SLAVE_TASK_NAME,
        SLAVE_STASK_SIZE,
        self,
        SLAVE_PRIORITY,
        &self->self_handle);

    if(self->self_handle)
        return 0;
    else
        return -1;
}

static enum SILLY_SLAVE_STATE s_sm_operate_collision(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem)
{
    return SILLY_SLAVE_STATE_OPERATE_WAIT_ON_TOKEN;
}

static enum SILLY_SLAVE_STATE s_sm_operate_master_discovery_query(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem)
{
    assert(input_elem);
    assert(input_elem->pkt);

    // TODO: handle error
    silly_do_handle_discovery_query(self->parent, input_elem->pkt->data);
    return SILLY_SLAVE_STATE_OPERATE_WAIT_ON_TOKEN;
}

static enum SILLY_SLAVE_STATE s_sm_operate_recved_network_topology(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem)
{
    assert(input_elem);
    assert(input_elem->pkt);

    // TODO: handle error
    silly_do_set_network_topology(self->parent, self, input_elem->pkt->data);
    return SILLY_SLAVE_STATE_OPERATE_WAIT_ON_TOKEN;
}

static enum SILLY_SLAVE_STATE s_sm_operate_recved_master_info(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem)
{
    assert(input_elem);
    assert(input_elem->pkt);

    // TODO: handle error
    silly_do_set_master_info(self->parent, self, input_elem->pkt->data);
    return SILLY_SLAVE_STATE_OPERATE_WAIT_ON_TOKEN;
}

static enum SILLY_SLAVE_STATE s_sm_operate_wait_on_token_recv_any(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem)
{
    assert(input_elem);
    assert(input_elem->pkt);

    struct silly_proto_header *pkt = input_elem->pkt->data;
    __cleanup_free__ void *payload = NULL;
    size_t payload_len = 0;
    int err = silly_get_payload(pkt, &payload, &payload_len);
    if(err) {
        ESP_LOGE(__func__, "failed to get payload of pkt");
        goto done;
    }
    if(pkt->dst == self->shared_ctx->my_mac) {
        silly_slave_do_deliver(self, payload, payload_len);
    }

done:
    return SILLY_SLAVE_STATE_OPERATE_WAIT_ON_TOKEN;
}

static enum SILLY_SLAVE_STATE s_sm_operate_wait_on_token_slave_passed_token(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem)
{
    return SILLY_SLAVE_STATE_OPERATE_TALKING;
}

static enum SILLY_SLAVE_STATE s_sm_operate_talking_noevent(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem)
{
    assert(self);

    __cleanup_free__ struct silly_tx_queue_elem *elem = NULL;
    __cleanup_free__ struct silly_proto_header *tx = calloc(1, sizeof(*tx));
    int err = 0;

    if(!tx)
        goto stop_talking;

    if(xQueueReceive(self->tx_from_upper_queue, &elem, 0) != pdTRUE)
        goto stop_talking; // NOTE: unexpected, we ensured this in guard

    err = silly_reset_header(self->shared_ctx, &tx);
    if(err)
        goto stop_talking;

    err = silly_encode_dataref_into_header(elem->data, elem->len, &tx);
    if(err)
        goto stop_talking;

    err = silly_do_tx(self->parent, (void **)&tx, tx->len);
    if(err)
        return SILLY_SLAVE_STATE_OPERATE_TALKING; // NOTE: shouldn't happen, we need to make sure sender's queue has space for both master and slave

    self->talking_budget--;

    return SILLY_SLAVE_STATE_OPERATE_TALKING;

stop_talking:
    silly_emitter_emit_elem_release_p(&self->token);
    return SILLY_SLAVE_STATE_OPERATE_WAIT_ON_TOKEN;
}

static enum SILLY_SLAVE_STATE s_sm_operate_talking_recv_any(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem)
{
    silly_emitter_emit_elem_release_p(&self->token);
    return SILLY_SLAVE_STATE_OPERATE_WAIT_ON_TOKEN;
}

static bool s_sm_guard_is_dst_me(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem)
{
    assert(input_elem);
    assert(input_elem->pkt);
    assert(input_elem->pkt->data);

    struct silly_proto_header *pkt = input_elem->pkt->data;

    return silly_dev_is_equal(pkt->dst, self->shared_ctx->my_mac);
}

static bool s_sm_guard_tx_data_available_and_have_budget(
    struct silly_slave_state_machine *_Nonnull self,
    const struct silly_emitter_emit_elem *_Nonnull input_elem)
{
    struct silly_tx_queue_elem *peek = NULL;

    if(self->talking_budget == 0)
        return false;

    return (xQueuePeek(self->tx_from_upper_queue, &peek, 0) == pdTRUE);
}

struct silly_slave_state_machine *new_silly_slave_state_machine_task(
    struct silly_proto_shared_ctx *shared_ctx,
    size_t tx_from_upper_queue_size,
    size_t rx_to_upper_queue_size)
{
    struct silly_slave_state_machine *ret = calloc(1, sizeof(*ret));
    if(!ret)
        return NULL;

    ret->shared_ctx = shared_ctx;

    ret->rx_to_upper_queue = xQueueCreate(
        rx_to_upper_queue_size, sizeof(struct silly_rx_queue_elem));
    if(!ret->rx_to_upper_queue)
        return NULL;

    ret->rx_to_upper_queue = xQueueCreate(
        tx_from_upper_queue_size, sizeof(struct silly_tx_queue_elem));
    if(!ret->tx_from_upper_queue)
        return NULL;

    ret->state = SLAVE_INITIAL_STATE;

    return ret;
}
