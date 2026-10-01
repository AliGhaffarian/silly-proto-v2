#include "silly_proto/emitter.h"
#include "silly_proto/receiver.h"
#include "silly_proto/sender.h"
#include "silly_proto/silly_proto.h"
#include "unity.h"

const enum EMITTER_EVENT_SOURCES receiver_event_source_enum =
    EMITTER_EVENT_SOURCES_RECV;

static void inline assert_emitter_deep(struct emitter_task *_Nonnull emitter)
{
    TEST_ASSERT(emitter);
    TEST_ASSERT(emitter->emitter_handle);
    TEST_ASSERT(emitter->event_notify_queue);
    for(int i = _EMITTER_HOOKPOINT_UNSPEC + 1; i < _EMITTER_HOOKPOINT_SIZE; i++)
        TEST_ASSERT(emitter->emitter_callbacks_mu[i]);

    for(int i = _EMITTER_EVENT_SOURCES_UNSPEC + 1;
        i < _EMITTER_EVENT_SOURCES_SIZE;
        i++) {
        TEST_ASSERT(emitter->event_source_arr[i].source_queue);
        TEST_ASSERT(emitter->event_source_arr[i].process_source_elem);
    }
}

void free_packet_refcounted_obj_callback(void *pkt, void *) { free(pkt); }

static struct refcounted_obj *
make_refcounted_packet_with_flag(silly_flags_t flag)
{
    struct silly_proto_header *hdr = calloc(1, sizeof(*hdr));
    struct refcounted_obj *ret = NULL;

    hdr->magic = SILLY_MAGIC;
    hdr->len = sizeof(struct silly_proto_header);
    hdr->flags = flag;

    refcounted_obj_new(
        MOVE(&hdr), free_packet_refcounted_obj_callback, NULL, NULL, &ret);

    return ret;
}

static struct emitter_task *emitter_simple_setup()
{
    uint32_t queue_len_arr[_EMITTER_EVENT_SOURCES_SIZE] = {
        [_EMITTER_EVENT_SOURCES_UNSPEC] = 0,
        [EMITTER_EVENT_SOURCES_RECV] = RECEIVER_DELIVER_QUEUE_SIZE,
        [EMITTER_EVENT_SOURCES_SENDER] = SENDER_DELIVER_QUEUE_SIZE,
        [EMITTER_EVENT_SOURCES_TIMER_EXPIRE] = SILLY_MAX_TIMERS};

    struct emitter_task *emitter_under_test = new_emitter_task(queue_len_arr);

    TEST_ASSERT(emitter_under_test);

    return emitter_under_test;
}

static struct emitter_subscriber *
emitter_sub_subbed_to_arg(enum SILLY_EVENT arg)
{
    struct emitter_subscriber *ret = calloc(1, sizeof(*ret));
    TEST_ASSERT(ret);
    memset(ret->subscribed_event_map, 0, _SILLY_EVENT_SIZE);
    ret->subscribed_event_map[arg] = 1;
    ret->sub_event_queue =
        xQueueCreate(1, sizeof(struct silly_emitter_emit_elem *));
    return ret;
}

TEST_CASE("start_emitter", "[emitter]")
{
    struct emitter_task *emitter_under_test = emitter_simple_setup();

    task_start_emitter(emitter_under_test);

    assert_emitter_deep(emitter_under_test);
}

TEST_CASE("add_subsriber_to_emitter", "[emitter]") {}

TEST_CASE("pass_packets_from_receiver", "[emitter]")
{
    // cases:
    // NOTE:we expect an RECV_ANY for all packets, and we expect
    // the pkt to be not null for packets that passed checksum
    //
    // 1. pass a token to 2 subs: one that subs to token, one sub that doesn't sub to token
    {
        struct emitter_task *emitter = emitter_simple_setup();
        task_start_emitter(emitter);
        struct emitter_subscriber *token_subber =
            emitter_sub_subbed_to_arg(SILLY_EVENT_SLAVE_PASSED_TOKEN);
        struct emitter_subscriber *token_not_subber =
            emitter_sub_subbed_to_arg(SILLY_EVENT_COLLISION);
        struct silly_emitter_emit_elem *emitter_elem = NULL;
        QueueHandle_t token_subber_q = token_subber->sub_event_queue;
        QueueHandle_t token_not_subber_q = token_not_subber->sub_event_queue;
        struct receiver_emit_elem *token_emit = calloc(1, sizeof(*token_emit));
        BaseType_t qsend_res = pdFALSE;
        BaseType_t qrecv_res = pdFALSE;

        token_emit->pkt = make_refcounted_packet_with_flag(SILLY_PASS_TOKEN);
        token_emit->recv_err = 0;

        add_subsriber_to_emitter(emitter, &token_subber);
        add_subsriber_to_emitter(emitter, &token_not_subber);

        qsend_res = xQueueSend(
            emitter->event_source_arr[EMITTER_EVENT_SOURCES_RECV].source_queue,
            &token_emit,
            0);
        TEST_ASSERT(qsend_res == pdTRUE);
        qsend_res = xQueueSend(
            emitter->event_notify_queue, &receiver_event_source_enum, 0);
        TEST_ASSERT(qsend_res == pdTRUE);

        qrecv_res = xQueueReceive(token_subber_q, &emitter_elem, 50);
        TEST_ASSERT(qrecv_res == pdTRUE);

        TEST_ASSERT(emitter_elem);
        TEST_ASSERT(emitter_elem->pkt);
        TEST_ASSERT(
            ((struct silly_proto_header *)emitter_elem->pkt->data)->flags ==
            SILLY_PASS_TOKEN);
        TEST_ASSERT(emitter_elem->event == SILLY_EVENT_SLAVE_PASSED_TOKEN);

        qrecv_res = xQueueReceive(token_not_subber_q, &emitter_elem, 0);
        TEST_ASSERT(qrecv_res != pdTRUE);
    }
    // 2. pass a token|master_discovery to 3 subs: one that subs to token, one sub that
    //      doesn't sub to token, one that subs to token and master_discovery
    // 3. for each flag, make a packet and pass that packet to emitter, and have subs for each flag
}

TEST_CASE("pass_collision_from_sender", "[emitter]") {}

TEST_CASE("pass_timeout_from_sender", "[emitter]") {}

TEST_CASE("callbacks", "[emitter]")
{
    // onrecv
}
