#include "silly_proto/token_passer.h"
#include "esp_timer.h"
#include "silly_proto/emitter.h"
#include "silly_proto/node_state_machine.h"
#include "silly_proto/timeout_expire_emit_elem.h"

bool token_passer_state_machine_event_sub_map[_SILLY_EVENT_SIZE] = {0};

bool active_token_passer_state_machine_event_sub_map[_SILLY_EVENT_SIZE] = {
    [_SILLY_EVENT_UNSPEC] = 0,
    [SILLY_EVENT_NOEVENT] = 0,
    [SILLY_EVENT_SLAVE_TOKEN_DEADLINE] = 1,
    [SILLY_EVENT_NETWORK_STOP_TIMEOUT] = 1,
    [SILLY_EVENT_MASTER_DEATH_TIMEOUT] = 1,
    [SILLY_EVENT_COLLISION] = 1,
    [SILLY_EVENT_SLAVE_PASSED_TOKEN] = 1,
    [SILLY_EVENT_MASTER_DISCOVERY_QUERY] = 1,
    [SILLY_EVENT_RECVED_NETWORK_TOPOLOGY] = 1,
    [SILLY_EVENT_RECVED_MASTER_INFO] = 1,
    [SILLY_EVENT_RECV_ANY] = 1,
};

static void
activate_our_emitter_sub_map(struct silly_node_state_machine *_Nonnull parent)
{
    struct emitter_subscriber *our_sub_obj =
        lookup_emitter_subscriber_by_sub_event_queue(
            parent->emitter, parent->token_passer->event_queue_handle);

    if(!our_sub_obj) {
        ESP_LOGE("__func__", "failed to lookup token passer's sub obj");
        assert(0); // panic
    }

    memcpy(
        our_sub_obj->subscribed_event_map,
        active_token_passer_state_machine_event_sub_map,
        _SILLY_EVENT_SIZE);
}

static void
deactivate_our_emitter_sub_map(struct silly_node_state_machine *_Nonnull parent)
{
    struct emitter_subscriber *our_sub_obj =
        lookup_emitter_subscriber_by_sub_event_queue(
            parent->emitter, parent->token_passer->event_queue_handle);

    if(!our_sub_obj) {
        ESP_LOGE("__func__", "failed to lookup token passer's sub obj");
        assert(0); // panic
    }

    memcpy(
        our_sub_obj->subscribed_event_map,
        token_passer_state_machine_event_sub_map,
        _SILLY_EVENT_SIZE);
}

int new_token_passer(
    struct sync_state_machine_token_passer *_Nonnull *_Nonnull self)
{
    assert(self);
    *self = calloc(1, sizeof(struct sync_state_machine_token_passer));
    if(!(*self))
        return -ENOMEM;

    (*self)->event_queue_handle = xQueueCreate(
        TOKEN_PASSER_DEFAULT_QUEUE_SIZE,
        sizeof(struct silly_emitter_emit_elem *));
    if(!(*self)->event_queue_handle)
        return -ENOMEM;

    return 0;
}

static void create_timer_slave_token_deadline(
    struct sync_state_machine_token_passer *_Nonnull self)
{
    assert(self);
    assert(self->parent);
    esp_timer_create_args_t timer_args = {
        .callback = isr_expire_slave_token_deadline,
        .dispatch_method = ESP_TIMER_ISR,
        .arg = ((struct silly_node_state_machine *)self->parent)->emitter,
        .name = "my_isr_timer",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &self->slave_token_deadline));
}

static int send_token_to_current_candidate(
    struct silly_node_state_machine *_Nonnull parent)
{
    assert(parent);
    struct silly_proto_header *token = silly_make_token(parent->shared_ctx);

    if(!token) {
        //WARN: this will make the whole network to reset,
        //  because we are dropping the token
        ESP_LOGE(__func__, "failed to make token");
        return -ENOMEM;
    }

    esp_timer_stop(parent->token_passer->slave_token_deadline);
    esp_timer_start_once(
        parent->token_passer->slave_token_deadline,
        TIMER_SILLY_SLAVE_TOKEN_DEADLINE);

    return silly_do_tx(parent, (void **)&token, token->len);
}

static void
xqrecv_until_timeout(struct sync_state_machine_token_passer *_Nonnull self)
{
    BaseType_t q_receive_result = pdFALSE;
    while(1) {
        __cleanup_release_silly_emitter_emit_elem__ struct
            silly_emitter_emit_elem *input_elem = NULL;
        q_receive_result = xQueueReceive(
            self->event_queue_handle,
            &input_elem,
            TOKEN_PASSER_INPUT_QUEUE_FLUSH_TIMEOUT);

        if(q_receive_result != pdTRUE) {
            break;
        }
    }
}

void sync_token_passer_mainloop(
    struct silly_node_state_machine *_Nonnull parent)
{
    assert(parent);

    activate_our_emitter_sub_map(parent);

    BaseType_t q_receive_result = pdFALSE;
    struct sync_state_machine_token_passer *self = parent->token_passer;

    send_token_to_current_candidate(parent);

    while(1) {
        __cleanup_release_silly_emitter_emit_elem__ struct
            silly_emitter_emit_elem *input_elem = NULL;

        q_receive_result =
            xQueueReceive(self->event_queue_handle, &input_elem, portMAX_DELAY);
        if(q_receive_result != pdTRUE) {
            ESP_LOGE(__func__, "failed to get an event");
            continue;
        }

        if(input_elem->event == SILLY_EVENT_SLAVE_TOKEN_DEADLINE) {
            silly_do_reevaluate_token_candidate(parent);
            send_token_to_current_candidate(parent);
        } else
            goto done;
    }

done:
    esp_timer_stop(parent->token_passer->slave_token_deadline);
    deactivate_our_emitter_sub_map(parent);
    xqrecv_until_timeout(self);
}

void isr_expire_slave_token_deadline(void *_Nonnull vself)
{
    assert_emitter_task(vself);
    struct emitter_task *_Nonnull self = vself;
    BaseType_t queue_send_result;
    __cleanup_free__ struct timeout_expire_emit_elem *timeout_elem =
        calloc(1, sizeof(*timeout_elem));
    if(!timeout_elem)
        return; // TODO: fatal?

    timeout_elem->event = SILLY_EVENT_SLAVE_TOKEN_DEADLINE;

    emitter_notification_value_t notif_value =
        EMITTER_EVENT_SOURCES_TIMER_EXPIRE;

    queue_send_result = xQueueSend(
        self->event_source_arr[EMITTER_EVENT_SOURCES_TIMER_EXPIRE].source_queue,
        &timeout_elem,
        0);

    assert(
        queue_send_result ==
        pdTRUE); // TODO:fatal error? we can make the queue size

    // as long as we have timers, so this should be a fatal error

    queue_send_result = xQueueSend(self->event_notify_queue, &notif_value, 0);
    assert(queue_send_result == pdTRUE); // TODO:fatal error?

    timeout_elem = NULL;

    return;
}
