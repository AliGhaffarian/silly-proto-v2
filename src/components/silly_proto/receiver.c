#include "silly_proto/receiver.h"
#include "common_utils.h"
#include "errno.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "hal/assert.h"
#include "silly_proto/emitter.h"
#include "silly_proto/token_passer.h"
#include "uart_utils.h"

void free_packet_refcounted_obj_callback(void *pkt, void *) { free(pkt); }

// called on every token,
// rational: receiver doesn't have to know about internals of node_state_machine,
//      assigns this callback on all tokens for simplicity. we manually check if
//      this token was for us
void free_packet_and_pass_token_refcounted_obj_callback(
    void *pkt, void *node_state_machine)
{
    assert(node_state_machine);
    silly_dev_t token_owner = ((struct silly_proto_header *)pkt)->dst;

    free(pkt);

    if(token_owner == ((struct silly_node_state_machine *)node_state_machine)
                          ->shared_ctx->my_mac)
        sync_token_passer_mainloop(node_state_machine);
}

static void receiver_emit_elem(
    struct receiver_task *_Nonnull self,
    struct silly_proto_header *_Nonnull pkt,
    int recv_err)
{
    __cleanup_free__ struct silly_proto_header *cleanup_pkt_decl = pkt;
    __cleanup_receiver_emit_elem_freep__ struct receiver_emit_elem *emit_elem =
        NULL;
    __cleanup_release_refcounted_obj__ struct refcounted_obj
        *emit_elem_refcounted_pkt = NULL;
    BaseType_t notif_success = errQUEUE_FULL;
    int err = 0;
    void (*free_data_callback)(void *, void *) =
        free_packet_refcounted_obj_callback;

    emit_elem = calloc(1, sizeof(*emit_elem));
    if(!emit_elem) {
        ESP_LOGE(RECEIVER_TASK_NAME, "couldn't allocate memory for emit_elem");
        return;
    }

    if(pkt->flags & SILLY_PASS_TOKEN) {
        free_data_callback = free_packet_and_pass_token_refcounted_obj_callback;
    }

    // if there was an error while receiving, don't include the packet
    if(recv_err == 0) {
        // NOTE: don't MOVE(pkt) in case of refcounted_obj_new() failure
        err = refcounted_obj_new(
            pkt,
            free_data_callback,
            self->parent,
            NULL,
            &emit_elem_refcounted_pkt);
        if(err) {
            ESP_LOGE(
                RECEIVER_TASK_NAME,
                "couldn't allocate memory for emit_elem_refcounted_pkt");
            return;
        } else {
            cleanup_pkt_decl = NULL; // prevent __cleanup_free__
        }
    }

    emit_elem->pkt = MOVE(&emit_elem_refcounted_pkt);
    emit_elem->recv_err = recv_err;
    if(xQueueSend(self->out_queue, &emit_elem, 0) == pdTRUE) {
        emit_elem = NULL;
    } else {
        ESP_LOGW(RECEIVER_TASK_NAME, "pkt dropped because out queue was full");
        return;
    }

    notif_success = xQueueSend(
        self->deliver_notification_handle,
        &self->deliver_notification_value,
        0);
    if(notif_success != pdTRUE) {
        ESP_LOGW(
            RECEIVER_TASK_NAME,
            "pkt dropped because failed to send notification");
        xQueueReceive(self->out_queue, &emit_elem, 0);
    }
}

static void task_receiver_mainloop(void *_Nonnull vself)
{
    struct receiver_task *self = vself;
    int err = 0;
    bool sem_aqcuired = false;

    while(1) {
        __cleanup_free__ struct silly_proto_header *pkt = NULL;

        SCOPED_SEMAPHORE_TAKE(
            self->locked_port->port_mu, portMAX_DELAY, sem_aqcuired);

        if(!sem_aqcuired) {
            ESP_LOGW(RECEIVER_TASK_NAME, "failed to get the uart lock");
            continue;
        }

        pkt = calloc(1, sizeof(struct silly_proto_header));
        if(!pkt) {
            ESP_LOGE(RECEIVER_TASK_NAME, "couldn't allocate memory for pkt");
            continue;
        }

        err = silly_uart_recv_pkt(
            self->locked_port->port, &pkt, RECEIVER_RECV_TICKS);

        if(err == -ETIMEDOUT) {
            ESP_LOGE(
                RECEIVER_TASK_NAME,
                "timeout on a call that should've blocked forever");
            continue;
        }

        receiver_emit_elem(self, MOVE(&pkt), err);
    }
}

int task_start_receiver(struct receiver_task *_Nonnull self)
{
    xTaskCreate(
        task_receiver_mainloop,
        RECEIVER_TASK_NAME,
        RECEIVER_STACK_SIZE,
        self,
        RECEIVER_PRIORITY,
        &self->receiver_handle);
    if(self->receiver_handle == NULL)
        return -1;
    return 0;
}

struct receiver_task *new_receiver_task(
    QueueHandle_t deliver_notification_handle,
    emitter_notification_value_t notif_value,
    struct locked_uart_port *_Nonnull locked_port,
    QueueHandle_t out_queue)
{
    assert(locked_port);

    struct receiver_task *ret = calloc(1, sizeof(*ret));
    if(!ret)
        return NULL;

    ret->locked_port = locked_port;
    ret->deliver_notification_handle = deliver_notification_handle;
    ret->deliver_notification_value = notif_value;
    ret->out_queue = xQueueCreate(
        RECEIVER_DELIVER_QUEUE_SIZE, sizeof(struct receiver_emit_elem *));

    if(!ret->out_queue) {
        free(ret);
        return NULL;
    }

    return ret;
}

void assert_receiver_task(struct receiver_task *_Nonnull self)
{
    assert(self);
    assert(self->receiver_handle);
    assert(self->locked_port);
}
