#pragma once

#include "freertos/FreeRTOS.h"
#include "hal/assert.h"
#include "silly_proto.h"
#include "silly_proto/emitter.h"
#include <stddef.h>

#define SENDER_DEFAULT_QUEUE_SIZE 10
#define SENDER_TASK_NAME          "sender"
#define SENDER_STASK_SIZE         (SILLY_DEFAULT_STACK_SIZE + 1)
#define SENDER_PRIORITY           (SILLY_DEFAULT_TASK_PRIORITY + 1)
#define SENDER_WAIT_TIME          30
#define SENDER_DELIVER_QUEUE_SIZE 8 /** emitter input queue */

struct sender_input_elem {
    void *data;
    size_t len;

    void (*free_data)(void *);
};
inline void freep_sender_input_elem(struct sender_input_elem **_Nonnull elemp)
{
    if(*elemp == NULL)
        return;
    if((*elemp)->free_data)
        ((*elemp)->free_data((*elemp)->data));
    free((*elemp));
    *elemp = NULL;
}
#define __cleanup_sender_input_elem_freep__                                    \
    __attribute__((__cleanup__(freep_sender_input_elem)))

struct sender_emit_elem {
    enum SILLY_EVENT event;
};

struct sender_task {
    TaskHandle_t self_handle;

    // elems: sender_input_elem (by value)
    QueueHandle_t input_queue;

    struct locked_uart_port *locked_port;

    // emitter related fields
    QueueHandle_t deliver_notification_handle;
    emitter_notification_value_t deliver_notification_value;

    QueueHandle_t emitter_out_event_q;
};

void assert_sender_task(struct sender_task *_Nonnull self);

static void sender_emit_collision(struct sender_task *_Nonnull self);

struct sender_task *new_sender_task(
    size_t input_queue_size,
    QueueHandle_t deliver_notification_handle,
    emitter_notification_value_t notif_value,
    struct locked_uart_port *_Nonnull locked_port,
    QueueHandle_t emitter_out_event_q);

static void send_all_in_queue(struct sender_task *_Nonnull self);
static void task_sender_mainloop(void *_Nonnull vself);

int task_start_sender(struct sender_task *_Nonnull self);
