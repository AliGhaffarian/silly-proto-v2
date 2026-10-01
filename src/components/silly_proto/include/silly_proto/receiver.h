#pragma once

#include "common_utils.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "silly_proto.h"
#include "silly_proto/emitter.h"

static const char *const RECEIVER_TASK_NAME = "receiver_task";
static const uint32_t RECEIVER_STACK_SIZE = SILLY_DEFAULT_STACK_SIZE;
static const BaseType_t RECEIVER_PRIORITY = SILLY_DEFAULT_TASK_PRIORITY + 1;
#define RECEIVER_DELIVER_QUEUE_SIZE 8
#define RECEIVER_RECV_TICKS         300

struct receiver_task {
    void *parent;
    TaskHandle_t receiver_handle;

    // resolved in init time by the node_state_machine
    QueueHandle_t deliver_notification_handle;
    emitter_notification_value_t deliver_notification_value;

    struct locked_uart_port *locked_port;

    QueueHandle_t out_queue;
};
void assert_receiver_task(struct receiver_task *_Nonnull self);

/**
 * @brief init a receiver_task
 * @param deliver_notification_handle the queue to which
 *      notif_value will be sent upon reception of a packet,
 *      no notification will happen if is set to NULL
 * @param out_queue
 *      queue to which the a pointer to a receiver_emit_elem will be sent,
 *      a queue will be created if set to NULL
 *
 * 1. assign emitter notification and input queue
 * 2. assign locked_port
 * 3. assign out_queue
 */
struct receiver_task *new_receiver_task(
    QueueHandle_t deliver_notification_handle,
    emitter_notification_value_t notif_value,
    struct locked_uart_port *_Nonnull locked_port,
    QueueHandle_t out_queue);

struct receiver_emit_elem {
    struct refcounted_obj *pkt;
    int recv_err;
};

inline void receiver_emit_elem_freep(struct receiver_emit_elem **_Nonnull elem)
{
    if(!(*elem))
        return;

    if(&(*elem)->pkt)
        refcounted_obj_release(&(*elem)->pkt);

    free(*elem);
    *elem = NULL;
}
#define __cleanup_receiver_emit_elem_freep__                                   \
    __attribute__((__cleanup__(receiver_emit_elem_freep)))

static void task_receiver_mainloop(void *_Nonnull self);
int task_start_receiver(struct receiver_task *_Nonnull self);
