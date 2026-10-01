#include "silly_proto/sender.h"
#include "common_utils.h"
#include "uart_utils.h"

struct sender_task *new_sender_task(
    size_t input_queue_size,
    QueueHandle_t deliver_notification_handle,
    emitter_notification_value_t notif_value,
    struct locked_uart_port *_Nonnull locked_port,
    QueueHandle_t emitter_out_event_q)
{
    assert(locked_port);

    struct sender_task *ret = calloc(1, sizeof(*ret));
    if(!ret)
        return NULL;

    ret->locked_port = locked_port;
    ret->input_queue =
        xQueueCreate(input_queue_size, sizeof(struct sender_input_elem));

    if(!ret->input_queue) {
        free(ret);
        return NULL;
    }

    ret->deliver_notification_handle = deliver_notification_handle;
    ret->deliver_notification_value = notif_value;
    ret->emitter_out_event_q = emitter_out_event_q;

    return ret;
}

static void send_all_in_queue(struct sender_task *_Nonnull self)
{
    bool lock_acquired = false;
    struct sender_input_elem input_elem;
    bool collision_flag = false;
    esp_err_t err = ESP_OK;

    {
        SCOPED_SEMAPHORE_TAKE(
            self->locked_port->port_mu, portMAX_DELAY, lock_acquired);

        if(!lock_acquired) {
            ESP_LOGE(
                SENDER_TASK_NAME, "failed to acquire lock_acquired->port_mu");
            return;
        }

        while(xQueueReceive(self->input_queue, &input_elem, SENDER_WAIT_TIME) ==
              pdTRUE) {

            uart_write_bytes(
                self->locked_port->port, input_elem.data, input_elem.len);

            input_elem.free_data(input_elem.data);

            err = uart_wait_tx_done(self->locked_port->port, portMAX_DELAY);
            if(err != ESP_OK) {
                ESP_LOGE(
                    SENDER_TASK_NAME,
                    "unexpected error doing uart_wait_tx_done");
                continue;
            }

            err = uart_get_collision_flag(
                self->locked_port->port, &collision_flag);
            if(err != ESP_OK) {
                ESP_LOGE(
                    SENDER_TASK_NAME,
                    "unexpected error doing uart_get_collision_flag");
                continue;
            }

            if(collision_flag)
                sender_emit_collision(self);
        }
    }
}

static void task_sender_mainloop(void *_Nonnull vself)
{
    struct sender_task *_Nonnull self = vself;
    void *peek = NULL;
    BaseType_t peek_result;
    while(1) {
        peek_result = xQueuePeek(self->input_queue, &peek, portMAX_DELAY);
        if(peek_result != pdTRUE)
            continue;
        send_all_in_queue(self);
    }
}

int task_start_sender(struct sender_task *_Nonnull self)
{
    assert_sender_task(self);

    xTaskCreate(
        task_sender_mainloop,
        SENDER_TASK_NAME,
        SENDER_STASK_SIZE,
        self,
        SENDER_PRIORITY,
        &self->self_handle);

    if(self->self_handle)
        return 0;
    else
        return -1;
}

static void sender_emit_collision(struct sender_task *_Nonnull self)
{
    assert(self);

    __cleanup_free__ struct sender_emit_elem *to_emit =
        calloc(1, sizeof(*to_emit));
    BaseType_t qsend_result = pdFALSE;

    if(!to_emit) {
        ESP_LOGE(SENDER_TASK_NAME, "failed to allocate an emit elem");
        return;
    }

    to_emit->event = SILLY_EVENT_COLLISION;

    qsend_result = xQueueSend(self->emitter_out_event_q, &to_emit, 0);
    if(qsend_result != pdTRUE) {
        ESP_LOGE(SENDER_TASK_NAME, "failed to emit collision");
        return;
    }

    qsend_result = xQueueSend(
        self->deliver_notification_handle,
        &self->deliver_notification_value,
        0);
    if(qsend_result != pdTRUE) {
        ESP_LOGE(SENDER_TASK_NAME, "failed to emit collision notification");
        xQueueReceive(self->emitter_out_event_q, &to_emit, 0);
        return;
    }

    to_emit = NULL;
}

void assert_sender_task(struct sender_task *_Nonnull self)
{
    assert(self);
    assert(self->locked_port);
}
