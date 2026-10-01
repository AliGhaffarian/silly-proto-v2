#include "silly_proto/emitter.h"
#include "assert.h"
#include "common_utils.h"
#include "errno.h"
#include "esp_log.h"
#include "hal/assert.h"
#include "linkedlist.h"
#include "silly_proto/receiver.h"
#include "silly_proto/sender.h"
#include "silly_proto/silly_proto.h"
#include "silly_proto/timeout_expire_emit_elem.h"

void debug_dump_sub_map(const bool *const sub_map)
{
    ESP_LOGD(__func__, "dumping sub map");
    for(int i = _SILLY_EVENT_UNSPEC; i < _SILLY_EVENT_SIZE; i++) {
        ESP_LOGD(__func__, "%s = %d", enum_str_map_silly_event[i], sub_map[i]);
    }
    ESP_LOGD(__func__, "finished dumping sub map");
}

void debug_emiter_dump_subscribers(const struct emitter_task *_Nonnull self)
{

    if(esp_log_get_default_level() < ESP_LOG_DEBUG)
        return;

    ESP_LOGD(__func__, "dumping subscribers");
    const struct linked_list_node *current = NULL;
    const struct linked_list_node *next = NULL;

    if(self->subscribers) {
        ESP_LOGD(__func__, "head:");
        //__builtin_dump_struct(self->subscribers, printf);
    } else
        ESP_LOGD(__func__, "head is NULL");

    LINKED_LIST_FOREACH(current, next, self->subscribers)
    {
        const struct emitter_subscriber *current_sub = current->data;

        //__builtin_dump_struct(current, printf);
        //__builtin_dump_struct(current_sub, printf);
        debug_dump_sub_map(current_sub->subscribed_event_map);
    }
    ESP_LOGD(__func__, "finished dumping subscribers");
}

static void emit_recv_any(struct emitter_task *_Nonnull self)
{
    assert_emitter_task(self);

    ESP_LOGD(EMITTER_TASK_NAME, "emitting recv_any");

    __cleanup_release_silly_emitter_emit_elem__ struct silly_emitter_emit_elem
        *elem = NULL;

    elem = calloc(1, sizeof(*elem));

    if(!elem) {
        ESP_LOGE(
            EMITTER_TASK_NAME, "failed to allocate memory for %s", __func__);
        return;
    }

    elem->event = SILLY_EVENT_RECV_ANY;

    emit_event_to_subs(self, &elem);
};

static void emitter_emit_collision(struct emitter_task *_Nonnull self)
{
    assert_emitter_task(self);

    ESP_LOGD(EMITTER_TASK_NAME, "emitting collision");

    __cleanup_release_silly_emitter_emit_elem__ struct silly_emitter_emit_elem
        *elem = NULL;

    elem = calloc(1, sizeof(*elem));

    if(!elem) {
        ESP_LOGE(
            EMITTER_TASK_NAME, "failed to allocate memory for %s", __func__);
        return;
    }

    elem->event = SILLY_EVENT_COLLISION;

    emit_event_to_subs(self, &elem);
}

/**
 * @brief construct a silly_emitter_emit_elem for the given flag
 * acquires the receiver_elem->pkt for the new silly_emitter_emit_elem
 */
struct silly_emitter_emit_elem *receiver_elem_next_emitter_elem(
    struct receiver_emit_elem *_Nonnull receiver_elem, silly_flags_t flag)
{

    assert(receiver_elem);

    __cleanup_release_silly_emitter_emit_elem__ struct silly_emitter_emit_elem
        *elem = NULL;
    elem = calloc(1, sizeof(*elem));
    if(!elem)
        return NULL;

    // couldn't use a flag-to-event map, because the flags increment by
    // shifting, not actually incrementing by 1, so it would be exponentially
    // wasteful if we had a simple map we could change the argument to bit_pos
    // of the flag, and then have a map based on the bit pos
    switch(flag) {
    case SILLY_PASS_TOKEN: {
        elem->event = SILLY_EVENT_SLAVE_PASSED_TOKEN;
        break;
    }
    case SILLY_MASTER_DISCOVERY_QUERY: {
        elem->event = SILLY_EVENT_MASTER_DISCOVERY_QUERY;
        break;
    }
    case SILLY_CONTAINS_MASTER_INFO: {
        elem->event = SILLY_EVENT_RECVED_MASTER_INFO;
        break;
    }
    case SILLY_CONTAINS_NETWORK_TOPOLOGY: {
        elem->event = SILLY_EVENT_RECVED_NETWORK_TOPOLOGY;
        break;
    }
    default: {
        return NULL;
    }
    }
    if(refcounted_obj_acquire(receiver_elem->pkt) != 0) {
        ESP_LOGE(
            EMITTER_TASK_NAME,
            "failed to acquire pkt for the new emitter elem");
        return NULL;
    }
    elem->pkt = receiver_elem->pkt;

    return MOVE(&elem);
}

int receiver_elem_process(
    struct emitter_task *_Nonnull self, void *_Nonnull *_Nonnull vreciever_elem)
{
    assert_emitter_task(self);
    assert(vreciever_elem);
    assert(*vreciever_elem);

    trigger_hook_point(self, EMITTER_HOOKPOINT_ON_RECV);

    __cleanup_receiver_emit_elem_freep__ struct receiver_emit_elem
        *receiver_elem = MOVE(vreciever_elem);
    const struct silly_proto_header *pkt = receiver_elem->pkt->data;

    ESP_LOGD(EMITTER_TASK_NAME, "received receiver_elem:");
    //__builtin_dump_struct(receiver_elem, printf);

    emit_recv_any(self);

    if(receiver_elem->recv_err != 0) {
        ESP_LOGI(
            "DBG",
            "elem ptr: %p, err ptr: %p, val: %d",
            receiver_elem,
            (void *)&receiver_elem->recv_err,
            (int)receiver_elem->recv_err);

        emitter_emit_collision(self);
        return 0;
    }

    for(silly_flags_t bit_mask = _SILLY_PROTO_FLAGS_UNSPEC << 1;
        bit_mask < _SILLY_PROTO_FLAGS_SIZE;
        bit_mask = bit_mask << 1) {
        __cleanup_release_silly_emitter_emit_elem__ struct
            silly_emitter_emit_elem *emitter_elem = NULL;

        if(!(pkt->flags & bit_mask)) {
            continue;
        }
        ESP_LOGD(
            EMITTER_TASK_NAME,
            "constructing emit elem with flag: %x",
            bit_mask);
        emitter_elem = receiver_elem_next_emitter_elem(
            receiver_elem, pkt->flags & bit_mask);

        if(!emitter_elem) {
            ESP_LOGE(
                EMITTER_TASK_NAME, "failed to receiver_elem_next_emitter_elem");
            return -1;
        }

        ESP_LOGD(EMITTER_TASK_NAME, "emitting event with flag: %x", bit_mask);
        emit_event_to_subs(self, &emitter_elem);
    }

    return 0;
}

int emit_eventid_to_subs(
    struct emitter_task *_Nonnull self, enum SILLY_EVENT event)
{
    struct silly_emitter_emit_elem *emit = calloc(1, sizeof(*emit));
    if(!emit)
        return -ENOMEM;

    emit->event = event;
    emit_event_to_subs(self, &emit);
    return 0;
}

int sender_elem_process(
    struct emitter_task *_Nonnull self, void *_Nonnull *_Nonnull vsender_elem)
{
    assert(vsender_elem);
    assert(*vsender_elem);

    __cleanup_free__ struct sender_emit_elem *input_elem = MOVE(vsender_elem);

    return emit_eventid_to_subs(self, input_elem->event);
}

int timeout_elem_process(
    struct emitter_task *_Nonnull self, void *_Nonnull *_Nonnull vtimeout_elem)
{
    assert(vtimeout_elem);
    assert(*vtimeout_elem);

    __cleanup_free__ struct timeout_expire_emit_elem *input_elem =
        MOVE(vtimeout_elem);

    return emit_eventid_to_subs(self, input_elem->event);
}

static struct silly_emitter_emit_elem *
silly_emitter_emit_elem_clone(struct silly_emitter_emit_elem *_Nonnull elem)
{

    // NOTE: some events don't carry a pkt with them, so we don't assert it,
    // we could conditionally assert the pkt, but that's overkill for now
    assert(elem);

    __cleanup_release_silly_emitter_emit_elem__ struct silly_emitter_emit_elem
        *clone = calloc(1, sizeof(*clone));
    int err = 0;

    if(!clone)
        return NULL;

    clone->event = elem->event;
    if(elem->pkt)
        err = refcounted_obj_acquire(elem->pkt);
    if(err) {
        ESP_LOGE(
            EMITTER_TASK_NAME, "refcounted_obj_acquire failed, err:%d", err);
        return NULL;
    }
    clone->pkt = elem->pkt;

    return MOVE(&clone);
}

static void trigger_hook_point(
    struct emitter_task *_Nonnull self, enum EMITTER_HOOKPOINT hook_point)
{
    assert_emitter_task(self);

    struct linked_list_node *current_node = NULL;
    struct linked_list_node *next_node = NULL;
    struct emitter_callback *current_callback = NULL;
    enum EMITTER_CALLBACK_ACTION callback_action =
        _EMITTER_CALLBACK_ACTION_UNSPEC;
    bool lock_acquired = false;

    SCOPED_SEMAPHORE_TAKE(
        self->emitter_callbacks_mu[hook_point], portMAX_DELAY, lock_acquired);
    if(!lock_acquired) {
        ESP_LOGE(
            EMITTER_TASK_NAME,
            "failed to acquire lock of hookpoint callback:%d",
            hook_point);
    }
    LINKED_LIST_FOREACH(
        current_node, next_node, self->emitter_callbacks[hook_point])
    {
        current_callback = current_node->data;
        callback_action =
            current_callback->callback(self, current_callback->argument);

        switch(callback_action) {
        case EMITTER_CALLBACK_ACTION_DELETE_CALLBACK: {
            linked_list_remove_node(&current_node);
            break;
        }
        case EMITTER_CALLBACK_ACTION_NOOP: {
            break;
        }
        default: {
            ESP_LOGE(EMITTER_TASK_NAME, "invalid emitter action returned");
            assert(0); // panic
            break;
        }
        }
    }
}

static void emit_event_to_subs(
    struct emitter_task *_Nonnull self,
    struct silly_emitter_emit_elem *_Nonnull *_Nonnull elem)
{
    assert_emitter_task(self);
    assert(elem);
    assert(*elem);

    const struct linked_list_node *current_node = NULL;
    const struct linked_list_node *next_node = NULL;
    const struct emitter_subscriber *current_subscriber = NULL;

    ESP_LOGD(
        __func__,
        "i'm called with event: %s",
        enum_str_map_silly_event[(*elem)->event]);

    int i = -1;
    debug_emiter_dump_subscribers(self);
    LINKED_LIST_FOREACH(current_node, next_node, self->subscribers)
    {
        i++;
        current_subscriber = current_node->data;

        ESP_LOGD(EMITTER_TASK_NAME, "current sub to check: %d", i);

        // not subscribed to this event
        if(!current_subscriber->subscribed_event_map[(*elem)->event]) {
            ESP_LOGD(
                EMITTER_TASK_NAME,
                "subscriber: %d is not subbed to event %s, skipping",
                i,
                enum_str_map_silly_event[(*elem)->event]);
            continue;
        } else {
            ESP_LOGD(EMITTER_TASK_NAME, "emitting to subscriber: %d", i);
        }

        __cleanup_release_silly_emitter_emit_elem__ struct
            silly_emitter_emit_elem *to_emit = NULL;
        to_emit = silly_emitter_emit_elem_clone(*elem);
        if(!to_emit) {
            ESP_LOGE(EMITTER_TASK_NAME, "failed to clone event to emit");
            continue;
        }

        if(xQueueSend(current_subscriber->sub_event_queue, &to_emit, 0) ==
           pdTRUE) {
            to_emit = NULL;
            ESP_LOGD(EMITTER_TASK_NAME, "emission success");
        } else {
            ESP_LOGE(EMITTER_TASK_NAME, "failed to emit elem to sub: %d", i);
            continue;
        }
    }

    silly_emitter_emit_elem_release_p(elem);
}

static void task_emitter_mainloop(void *_Nonnull vself)
{

    struct emitter_task *self = vself;
    BaseType_t queue_recv_result;
    void *received_event = NULL;
    int err = 0;

    assert_emitter_task(self);

    enum EMITTER_EVENT_SOURCES event_source = _EMITTER_EVENT_SOURCES_UNSPEC;
    while(1) {
        event_source = _EMITTER_EVENT_SOURCES_UNSPEC;

        queue_recv_result = xQueueReceive(
            self->event_notify_queue, &event_source, portMAX_DELAY);
        if(queue_recv_result != pdTRUE)
            continue;

        ESP_LOGD(
            EMITTER_TASK_NAME,
            "received notification from source: %d",
            event_source);

        queue_recv_result = xQueueReceive(
            self->event_source_arr[event_source].source_queue,
            &received_event,
            0);

        if(queue_recv_result != pdTRUE) {
            ESP_LOGE(
                EMITTER_TASK_NAME,
                "unexpected queue recv fail from source: %d",
                event_source);
            continue;
        }

        ESP_LOGD(
            EMITTER_TASK_NAME, "received event from source: %d", event_source);

        err = self->event_source_arr[event_source].process_source_elem(
            self, &received_event);

        if(err) {
            ESP_LOGE(
                EMITTER_TASK_NAME,
                "failed to process event from source:%d",
                event_source);
            continue;
        }
    }
}

int add_subsriber_to_emitter(
    struct emitter_task *_Nonnull self,
    struct emitter_subscriber *_Nonnull *_Nonnull subscriber)
{

    assert_emitter_task(self);
    assert(subscriber);
    assert(*subscriber);

    __cleanup_linked_list_free__ struct linked_list_node *subscriber_node =
        NULL;

    subscriber_node = linked_list_make_node((void **)subscriber, free);
    if(!subscriber_node) {
        ESP_LOGE(
            __func__, "failed to allocate a linked_list_node for subscriber");
        return -1;
    }

    linked_list_append(&self->subscribers, &subscriber_node);
    subscriber_node = NULL;

    return 0;
}

int add_callback_to_emitter(
    struct emitter_task *_Nonnull self,
    struct emitter_callback *_Nonnull *_Nonnull callback,
    enum EMITTER_HOOKPOINT hookpoint)
{
    assert_emitter_task(self);
    assert(callback);
    assert(*callback);

    __cleanup_linked_list_free__ struct linked_list_node *callback_node = NULL;
    bool lock_acquired = false;

    callback_node = linked_list_make_node((void **)callback, free);
    if(!callback_node) {
        ESP_LOGE(
            __func__, "failed to allocate a linked_list_node for callback");
        return -1;
    }

    SCOPED_SEMAPHORE_TAKE(
        self->emitter_callbacks_mu[hookpoint], portMAX_DELAY, lock_acquired);
    if(!lock_acquired) {
        ESP_LOGE(
            __func__, "failed to acquire callback list lock to add a callback");
        *callback = MOVE(
            &callback_node->data); // revert the MOVE inside linked_list_node
        return -1;
    }

    linked_list_append(
        &self->emitter_callbacks[hookpoint], MOVE(&callback_node));

    return 0;
}

void add_event_source_to_emitter(
    struct emitter_task *_Nonnull self,
    struct event_source_elem *_Nonnull *_Nonnull event_source,
    enum EMITTER_EVENT_SOURCES source_id)
{
    assert_emitter_task(self);
    assert(event_source);
    assert(*event_source);

    self->event_source_arr[source_id].process_source_elem =
        (*event_source)->process_source_elem;
    self->event_source_arr[source_id].source_queue =
        (*event_source)->source_queue;

    freep(event_source);
}

/**
 * @brief start the emitter_task
 *
 * self->emitter_handle will be overwritten, the called must provide all the
 * other fields
 */
int task_start_emitter(struct emitter_task *_Nonnull self)
{
    assert(self);

    xTaskCreate(
        task_emitter_mainloop,
        EMITTER_TASK_NAME,
        EMITTER_STACK_SIZE,
        self,
        EMITTER_PRIORITY,
        &self->emitter_handle);

    if(self->emitter_handle)
        return 0;
    else
        return -1;
}

uint32_t
emitter_compute_notif_queue_len(int len_arr[_EMITTER_EVENT_SOURCES_SIZE])
{
    uint32_t result = 0;
    for(int i = 0; i < _EMITTER_EVENT_SOURCES_SIZE; i++) {
        result += len_arr[i];
    }
    return result;
}

struct emitter_subscriber *lookup_emitter_subscriber_by_sub_event_queue(
    const struct emitter_task *_Nonnull self, QueueHandle_t event_q)
{
    assert(self);

    struct linked_list_node *current = NULL;
    struct linked_list_node *next_node = NULL;
    struct emitter_subscriber *current_sub = NULL;

    LINKED_LIST_FOREACH(current, next_node, self->subscribers)
    {
        current_sub = current->data;
        if(current_sub->sub_event_queue == event_q)
            return current_sub;
    }

    return NULL;
}

struct emitter_task *new_emitter_task(uint32_t *event_source_queue_size_arary)
{

    struct emitter_task *self = calloc(1, sizeof(*self));
    uint32_t notification_queue_len = 0;
    if(!self) {
        ESP_LOGE(__func__, "failed to allocate memory for self");
        return NULL;
    }

    for(int i = 0; i < _EMITTER_HOOKPOINT_SIZE; i++) {
        self->emitter_callbacks_mu[i] = xSemaphoreCreateMutex();
        if(!self->emitter_callbacks_mu[i]) {
            ESP_LOGE(__func__, "failed to allocate memory for callback mutex");
            return NULL;
        }
    }

    for(int i = _EMITTER_EVENT_SOURCES_UNSPEC + 1;
        i < _EMITTER_EVENT_SOURCES_SIZE;
        i++) {
        ESP_LOGD(
            __func__,
            "creating event source queue with length of %d",
            event_source_queue_size_arary[i]);
        self->event_source_arr[i].source_queue =
            xQueueCreate(event_source_queue_size_arary[i], sizeof(void *));
        if(!self->event_source_arr[i].source_queue) {
            ESP_LOGE(
                __func__, "failed to allocate memory for event source queue");
            return NULL;
        }
        notification_queue_len += event_source_queue_size_arary[i];
    }

    self->event_source_arr[EMITTER_EVENT_SOURCES_RECV].process_source_elem =
        receiver_elem_process;
    self->event_source_arr[EMITTER_EVENT_SOURCES_SENDER].process_source_elem =
        sender_elem_process;
    self->event_source_arr[EMITTER_EVENT_SOURCES_TIMER_EXPIRE]
        .process_source_elem = timeout_elem_process;

    self->event_notify_queue = xQueueCreate(
        notification_queue_len, sizeof(enum EMITTER_EVENT_SOURCES));
    if(!self->event_notify_queue) {
        ESP_LOGE(
            __func__, "failed to allocate memory for event notification queue");
        return NULL;
    }

    return self;
}

void assert_emitter_task(struct emitter_task *self)
{
    assert(self);
    assert(self->emitter_handle);
    /**
     * no need to assert the following:
     *  - subscribers: might be empty, the emitter will be useless but it's not
     * memory corruption
     *  - emitter_callback, event_source_arr: won't ever be NULL, they are pre
     * allocated arrays
     */
}
