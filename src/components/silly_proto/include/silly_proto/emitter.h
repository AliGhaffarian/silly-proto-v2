#pragma once

#include "freertos/FreeRTOS.h"
#include "hal/assert.h"
#include "linkedlist.h"
#include "silly_proto.h"

static const char *const EMITTER_TASK_NAME = "emitter_task";
static const uint32_t EMITTER_STACK_SIZE = SILLY_DEFAULT_STACK_SIZE + 2048;
static const BaseType_t EMITTER_PRIORITY = SILLY_DEFAULT_TASK_PRIORITY + 1;

enum EMITTER_HOOKPOINT {
    _EMITTER_HOOKPOINT_UNSPEC,
    EMITTER_HOOKPOINT_ON_RECV,
    _EMITTER_HOOKPOINT_SIZE,
};

enum EMITTER_EVENT_SOURCES {
    _EMITTER_EVENT_SOURCES_UNSPEC,
    EMITTER_EVENT_SOURCES_RECV, /** elems: `struct receiver_emit_elem *` */
    EMITTER_EVENT_SOURCES_SENDER, /** elems: `struct sender_emit_elem *` */
    EMITTER_EVENT_SOURCES_TIMER_EXPIRE, /** elems: `struct timeout_expire_emit_elem *` */
    _EMITTER_EVENT_SOURCES_SIZE,
};

/**
 * @brief derive notif queue len from the input queues
 */
uint32_t
emitter_compute_notif_queue_len(int len_arr[_EMITTER_EVENT_SOURCES_SIZE]);

/**
 * @brief action that needs to be taken by the emitter, returned by callbacks
 */
enum EMITTER_CALLBACK_ACTION {
    _EMITTER_CALLBACK_ACTION_UNSPEC,
    EMITTER_CALLBACK_ACTION_NOOP,
    EMITTER_CALLBACK_ACTION_DELETE_CALLBACK, /** unregistered this callback */
    _EMITTER_CALLBACK_ACTION_SIZE,
};

struct emitter_subscriber {
    bool subscribed_event_map[_SILLY_EVENT_SIZE];
    QueueHandle_t sub_event_queue;
};

struct emitter_task; // forward declaration to satisfy event_source_elem and
// *_process functions
struct event_source_elem {
    // constructed by self for each event source
    QueueHandle_t source_queue;
    /**
     * @brief process the received elem, and emit the needed events
     * must trigger hook points that relate to the events, like
     *  EMITTER_HOOKPOINT_ON_RECV
     * might need to emit multiple events
     * MUST steal the passed elem
     */
    int (*process_source_elem)(struct emitter_task *_Nonnull self, void **);
};

int emit_eventid_to_subs(
    struct emitter_task *_Nonnull self, enum SILLY_EVENT event);

/**
 * input: struct receiver_emit_elem **
 * 0. trigger EMITTER_HOOKPOINT_ON_RECV
 * 1. break down the receiver elem to the events the packet is declaring
 * 2. for each event
 *  2.1 allocate an emitter elem
 *  2.2 emit_event_to_subs(self, &elem)
 */
int receiver_elem_process(
    struct emitter_task *_Nonnull self,
    void *_Nonnull *_Nonnull vreciever_elem);

/**
 * input: struct sender_emit_elem **
 *
 * calls emit_eventid_to_subs
 */
int sender_elem_process(
    struct emitter_task *_Nonnull self, void *_Nonnull *_Nonnull vsender_elem);

/**
 * input: struct timeout_expire_emit_elem **
 *
 * calls emit_eventid_to_subs
 */
int timeout_elem_process(
    struct emitter_task *_Nonnull self, void **vtimeout_elem);

typedef uint8_t emitter_notification_value_t;

/**
 * @brief emitter_task context
 * @note all fields except for emitter_callback are read only once the emitter
 *   is running
 *   exception: the subscribers may change the sub map but they would need to
 *      handle the synchronization with the emitter on their own
 */
struct emitter_task {
    TaskHandle_t emitter_handle;

    /**
    * emitter waits on this queue to be notified of a enqueud event on either
    * of the event_source_arr queue elem: uint32_t: event source
    *
    * to add an event source:
    *  1. add the source to enum EMITTER_EVENT_SOURCES
    *  2. modify new_emitter_task() to assign the correct process_source_elem callback
    *  3. the event source needs to use the queue constructed in emitter_task->event_source_arr[src_enum].source_queue
    *      to emit the events
    *       3.1 implement the correct process_source_elem as a function in the emitter's source file
    *  4. event source needs to include the following fields to be able to emit an event:
    *      4.1 QueueHandle_t deliver_notification_handle
    *      4.2 emitter_notification_value_t deliver_notification_value
    *      4.3 QueueHandle_t out_q (no particular naming convention for this)
    *  emitting an event:
    *      1. emit the event into out_q
    *      2. notify the emitter via deliver_notification_handle and deliver_notification_value
    */
    QueueHandle_t event_notify_queue;
    struct event_source_elem event_source_arr[_EMITTER_EVENT_SOURCES_SIZE];

    struct linked_list_node *subscribers;

    struct linked_list_node *emitter_callbacks[_EMITTER_HOOKPOINT_SIZE];
    SemaphoreHandle_t emitter_callbacks_mu[_EMITTER_HOOKPOINT_SIZE];
};

/**
 * 1. init mutex of each hook point
 * 2. init event sources
 *      2.1. construct queue for each event source
 *      2.2. assign the process callback of each event source
 * 3. construct notification queue
 * @param event_source_queue_size_arary array of size
 *      _EMITTER_EVENT_SOURCES_SIZE, with each entry indicating the corresponding
 *      source queue size
 */
struct emitter_task *new_emitter_task(uint32_t *event_source_queue_size_arary);

void assert_emitter_task(struct emitter_task *self);

struct emitter_callback {
    enum EMITTER_CALLBACK_ACTION (*callback)(
        struct emitter_task *_Nonnull self, void *);
    void *argument;
    void (*free_arguement)(void *);
};
/**
 * @brief free emitter_callback, acquire lock of the linked list before calling
 * provided as linked_list_node->free_data, since this is a node in
 * emitter_task->emitter_callbacks
 */
inline void free_emitter_callbackp(void *velem)
{
    struct emitter_callback *elem = velem;
    if(elem->free_arguement)
        elem->free_arguement(MOVE(&elem->argument));
    free(elem);
}

/**
 * @brief add the subscriber to emitter task
 * @note emitter must not be running
 */
int add_subsriber_to_emitter(
    struct emitter_task *_Nonnull self,
    struct emitter_subscriber *_Nonnull *_Nonnull subscriber);

/**
 * @brief add the callback to emitter task
 * @note can block
 * can be used by other tasks at emitter's run time
 */
int add_callback_to_emitter(
    struct emitter_task *_Nonnull self,
    struct emitter_callback *_Nonnull *_Nonnull callback,
    enum EMITTER_HOOKPOINT hookpoint);

/**
 * @brief add the event_source to emitter task
 * @note emitter must not be running
 * @note if an event_source with source_id is already present, it will be
 * overwritten
 */
void add_event_source_to_emitter(
    struct emitter_task *_Nonnull self,
    struct event_source_elem *_Nonnull *_Nonnull event_source,
    enum EMITTER_EVENT_SOURCES source_id);

/**
 * @brief emitted event objects into the state machines queue
 * @note must be allocated from the heap
 */
struct silly_emitter_emit_elem {
    enum SILLY_EVENT event;
    struct refcounted_obj *pkt;
};

/**
 * @brief allocate a silly_emitter_emit_elem, and do a refcounted_obj_acquire on
 * elem->pkt
 */
static struct silly_emitter_emit_elem *
silly_emitter_emit_elem_clone(struct silly_emitter_emit_elem *_Nonnull elem);

void inline silly_emitter_emit_elem_release_p(
    struct silly_emitter_emit_elem **_Nonnull elem)
{
    if(!(*elem))
        return;
    if((*elem)->pkt)
        refcounted_obj_release(&(*elem)->pkt);
    free(*elem);
    *elem = NULL;
};
#define __cleanup_release_silly_emitter_emit_elem__                            \
    __attribute__((__cleanup__(silly_emitter_emit_elem_release_p)))

/**
 * @brief for each entry in the self->emitter_callback[hook_point], call the
 * .callback[.argument]
 */
static void trigger_hook_point(
    struct emitter_task *_Nonnull self, enum EMITTER_HOOKPOINT hook_point);
/**
 * @brief clone the elem for each subscriber and emit the cloned elem
 */
static void emit_event_to_subs(
    struct emitter_task *_Nonnull self,
    struct silly_emitter_emit_elem *_Nonnull *_Nonnull elem);

/**
 * 1. wait on self->event_notify_queue
 * 2. fetch event from self->event_source_arr[event].source_queue
 * 3. translate the event to a silly_emitter_emit_elem
 *  3.0. there's a callback for translation:
 * self->event_source_arr[event].translate_to_emitter_elem(received_elem) 3.1.
 * trigger hook points that relate to the event, like on_recv
 * 4. emit_event_to_subs(elem)
 */
static void task_emitter_mainloop(void *_Nonnull vself);

int task_start_emitter(struct emitter_task *_Nonnull self);

struct emitter_subscriber *lookup_emitter_subscriber_by_sub_event_queue(
    const struct emitter_task *_Nonnull self, QueueHandle_t event_q);
