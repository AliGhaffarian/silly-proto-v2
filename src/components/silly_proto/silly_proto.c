#include "silly_proto/silly_proto.h"
#include "common_utils.h"
#include "driver/uart.h"
#include "errno.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "hal/assert.h"
#include "uart_utils.h"

const char *const enum_str_map_silly_event[_SILLY_EVENT_SIZE] = {
    [_SILLY_EVENT_UNSPEC] = "_SILLY_EVENT_UNSPEC",
    [SILLY_EVENT_NOEVENT] = "SILLY_EVENT_NOEVENT",
    [SILLY_EVENT_SLAVE_TOKEN_DEADLINE] = "SILLY_EVENT_SLAVE_TOKEN_DEADLINE",
    [SILLY_EVENT_NETWORK_STOP_TIMEOUT] = "SILLY_EVENT_NETWORK_STOP_TIMEOUT",
    [SILLY_EVENT_MASTER_DEATH_TIMEOUT] = "SILLY_EVENT_MASTER_DEATH_TIMEOUT",
    [SILLY_EVENT_COLLISION] = "SILLY_EVENT_COLLISION",
    [SILLY_EVENT_SLAVE_PASSED_TOKEN] = "SILLY_EVENT_SLAVE_PASSED_TOKEN",
    [SILLY_EVENT_MASTER_DISCOVERY_QUERY] = "SILLY_EVENT_MASTER_DISCOVERY_QUERY",
    [SILLY_EVENT_RECVED_NETWORK_TOPOLOGY] =
        "SILLY_EVENT_RECVED_NETWORK_TOPOLOGY",
    [SILLY_EVENT_RECVED_MASTER_INFO] = "SILLY_EVENT_RECVED_MASTER_INFO",
    [SILLY_EVENT_RECV_ANY] = "SILLY_EVENT_RECV_ANY",
};

static const char *TAG = "silly_proto";

int silly_verify_data_checksum(struct silly_proto_header *const hdr)
{
    if(hdr->data_checksum == silly_compute_data_checksum(hdr))
        return SILLY_CHECKSUM_VERIFY_SUCCESS;
    else
        return SILLY_CHECKSUM_VERIFY_FAILED;
}

int silly_verify_header_checksum(struct silly_proto_header *const hdr)
{
    if(hdr->header_checksum == silly_compute_header_checksum(hdr))
        return SILLY_CHECKSUM_VERIFY_SUCCESS;
    else
        return SILLY_CHECKSUM_VERIFY_FAILED;
}

int silly_verify_checksum(struct silly_proto_header *const hdr)
{
    int err = 0;

    err = silly_verify_header_checksum(hdr);
    if(err != SILLY_CHECKSUM_VERIFY_SUCCESS)
        return err;

    if(hdr->len > sizeof(struct silly_proto_header)) {
        err = silly_verify_data_checksum(hdr);
    }

    return err;
}

int silly_skip_until_magic(uart_port_t uart_port, TickType_t timeout)
{
    ESP_LOGI(TAG, "skipping until magic");
    size_t magic_arr_size = sizeof(((struct silly_proto_header *)NULL)->magic);
    uint8_t magic_byte;
    size_t i = 0;
    TimeOut_t vtask_timeout;
    TickType_t ticks_to_wait = timeout;
    vTaskSetTimeOutState(&vtask_timeout);

    while(i != magic_arr_size) {
        uart_read_bytes(uart_port, &magic_byte, 1, ticks_to_wait);

        if(xTaskCheckForTimeOut(&vtask_timeout, &ticks_to_wait) == pdTRUE)
            return -ETIMEDOUT;

        if(magic_byte == GET_NTH_BYTE(SILLY_MAGIC, i)) {
            i++;
        } else if(magic_byte == GET_NTH_BYTE(SILLY_MAGIC, 0)) {
            i = 1;
        } else {
            i = 0;
        }
    }

    ESP_LOGI(TAG, "successfully skipped until magic");
    return 0;
}

int silly_reset_header(
    struct silly_proto_shared_ctx *shared_ctx, struct silly_proto_header **hdr)
{
    assert(shared_ctx);

    int err = 0;
    bool lock_aquired = false;

    ESP_LOGD(TAG, "resetting the header");

    err = safe_realloc((void **)hdr, sizeof(struct silly_proto_header));

    if(err)
        return err;

    memset(*hdr, 0, sizeof(struct silly_proto_header));

    (*hdr)->magic = SILLY_MAGIC;
    (*hdr)->len = sizeof(struct silly_proto_header);
    (*hdr)->src = shared_ctx->my_mac;
    {
        SCOPED_SEMAPHORE_TAKE(
            shared_ctx->master_node_mu, portMAX_DELAY, lock_aquired);
        if(!lock_aquired)
            return -1;
        (*hdr)->master = shared_ctx->master_node;
    }

    return 0;
}

/**
 * @brief recv a silly packet, skip until magic
 *
 * @return 0 on success, -EBADMSG on corrupted packet, -ETIMEDOUT on timeout
 */
int silly_uart_recv_pkt(
    uart_port_t uart_port, struct silly_proto_header **hdr, TickType_t timeout)
{
    int err = 0;
    TimeOut_t vtask_timeout;
    TickType_t ticks_to_wait = timeout;
    vTaskSetTimeOutState(&vtask_timeout);

    ESP_LOGI(TAG, "trying to recv a packet");

    silly_skip_until_magic(uart_port, ticks_to_wait);

    if(xTaskCheckForTimeOut(&vtask_timeout, &ticks_to_wait) == pdTRUE)
        return -ETIMEDOUT;

    (*hdr)->magic = SILLY_MAGIC;

    uart_read_bytes(
        uart_port,
        ((void *)(*hdr)) + sizeof((*hdr)->magic),
        sizeof(struct silly_proto_header) - sizeof((*hdr)->magic),
        ticks_to_wait);

    err = silly_verify_header_checksum(*hdr);
    if(err != SILLY_CHECKSUM_VERIFY_SUCCESS) {
        ESP_LOGI(
            TAG,
            "corrupted header, in header:%d, computed:%d",
            (*hdr)->header_checksum,
            silly_compute_header_checksum(*hdr));
        return -EBADMSG;
    }

    __builtin_dump_struct(*hdr, printf);

    ESP_LOGI(TAG, "successfully read the header");
    if((*hdr)->len == sizeof(struct silly_proto_header)) {
        ESP_LOGI(TAG, "packet has no data, recv done");
        return 0;
    }

    ESP_LOGI(TAG, "trying to read the data");
    // if we haven't returned yet, we got data to process
    err = safe_realloc((void **)hdr, (*hdr)->len);
    if(err)
        return err;

    if(xTaskCheckForTimeOut(&vtask_timeout, &ticks_to_wait) == pdTRUE)
        return -ETIMEDOUT;

    uart_read_bytes(
        uart_port,
        (*hdr)->data,
        (*hdr)->len - sizeof(struct silly_proto_header),
        ticks_to_wait);

    err = silly_verify_data_checksum(*hdr);

    if(err != SILLY_CHECKSUM_VERIFY_SUCCESS) {
        ESP_LOGI(
            TAG,
            "corrupted data, in header:%d, computed:%d",
            (*hdr)->data_checksum,
            silly_compute_data_checksum(*hdr));
        return -EBADMSG;
    }
    ESP_LOGI(TAG, "packet recv done");
    return 0;
}

silly_checksum_t
silly_compute_data_checksum(struct silly_proto_header *const hdr)
{
    silly_checksum_t checksum = 0;

    for(int i = 0; i < hdr->len - sizeof(struct silly_proto_header); i++)
        checksum += hdr->data[i];

    return checksum;
}

silly_checksum_t
silly_compute_header_checksum(struct silly_proto_header *const hdr)
{
    silly_checksum_t checksum = 0;
    checksum += hdr->magic;
    checksum += hdr->len;
    checksum += hdr->flags;
    checksum += hdr->data_checksum;
    checksum += hdr->dst;
    checksum += hdr->src;
    checksum += hdr->master;
    checksum += hdr->elem_count;
    return checksum;
}

struct silly_proto_shared_ctx *
silly_new_proto_shared_ctx(silly_dev_t my_mac, QueueHandle_t tx_queue)
{
    __cleanup_free__ struct silly_proto_shared_ctx *shared_ctx =
        calloc(1, sizeof(*shared_ctx));
    if(!shared_ctx)
        return NULL;

    shared_ctx->tx_queue = tx_queue;
    shared_ctx->my_mac = my_mac;
    shared_ctx->master_node_mu = xSemaphoreCreateMutex();
    if(!(shared_ctx->master_node_mu))
        goto master_mu_failed;
    shared_ctx->talking_budget_mu = xSemaphoreCreateMutex();
    if(!(shared_ctx->talking_budget_mu))
        goto talking_budget_mu_failed;
    shared_ctx->topology_mu = xSemaphoreCreateMutex();
    if(!(shared_ctx->topology_mu))
        goto topology_mu_failed;

    return MOVE(&shared_ctx);

topology_mu_failed:
    vSemaphoreDelete(shared_ctx->talking_budget_mu);
talking_budget_mu_failed:
    vSemaphoreDelete(shared_ctx->master_node_mu);
master_mu_failed:
    return NULL;
}

struct silly_proto_header *silly_make_discovery_query_reply(
    struct silly_proto_shared_ctx *_Nonnull proto_shared_ctx)
{

    __cleanup_free__ struct silly_proto_header *ret = calloc(1, sizeof(*ret));
    int err = 0;
    if(!ret)
        return NULL;

    err = silly_reset_header(proto_shared_ctx, &ret);
    if(!ret)
        return NULL;

    return MOVE(&ret);
}

struct silly_proto_header *
silly_make_token(struct silly_proto_shared_ctx *_Nonnull proto_shared_ctx)
{
    assert(proto_shared_ctx);

    __cleanup_free__ struct silly_proto_header *ret = calloc(1, sizeof(*ret));
    int err = 0;
    if(!ret)
        return NULL;

    err = silly_reset_header(proto_shared_ctx, &ret);
    if(ret)
        return NULL;

    ret->flags |= SILLY_PASS_TOKEN;
    ret->dst = proto_shared_ctx->token_candidate;

    return MOVE(&ret);
}

int silly_encode_local_topology_into_hdr(
    const struct silly_network_topology *_Nonnull topology,
    struct silly_proto_header *_Nonnull *_Nonnull hdr)
{
    assert(topology);

    struct linked_list_node *current = NULL;
    struct linked_list_node *next = NULL;
    silly_dev_t current_dev = 0;
    silly_dev_t next_dev = 0;
    int err = 0;
    __cleanup_free__ struct silly_proto_topology_node_elem *node =
        calloc(1, sizeof(*node));

    if(!node)
        return -ENOMEM;

    node->type = SILLY_ELEM_TYPE_TOPOLOGY_NODE;

    LINKED_LIST_FOREACH(current, next, topology->topology_head)
    {
        current_dev = get_dev_from_topology_node(topology->topology_head);
        if(!next)
            next_dev = get_dev_from_topology_node(topology->topology_head);
        else
            next_dev = get_dev_from_topology_node(next);

        node->next = next_dev;
        node->current = current_dev;
        node->talking_budget = get_talking_budget_from_topology_node(current);

        err = silly_encode_dataref_into_header(
            node, sizeof(struct silly_proto_topology_node_elem), hdr);
        if(err) {
            return err;
        }
    }

    return 0;
}

int silly_decode_local_topology_from_hdr(
    struct silly_network_topology **_Nonnull topology,
    const silly_dev_t talking_budget_of_interest,
    uint8_t *talking_budget,
    const struct silly_proto_header *_Nonnull hdr)
{
    assert(topology);

    uint32_t current_header_offset = 0;
    silly_elem_count_t remaining_elems = 0;
    struct silly_proto_topology_node_elem *current_node = NULL;
    __cleanup_free__ struct silly_network_topology_node *local_node = NULL;
    int ret = 0;
    int err = 0;

    if(*topology)
        freep_network_topology(topology);
    *topology = calloc(1, sizeof(struct silly_network_topology));
    if(!(*topology))
        return -ENOMEM;

    do {
        current_node = next_silly_elem_of_type(
            &current_header_offset,
            &remaining_elems,
            SILLY_ELEM_TYPE_TOPOLOGY_NODE,
            hdr);

        local_node = silly_network_elem_node_to_local_node(current_node);
        if(!local_node)
            return -ENOMEM;

        err = silly_append_local_node_to_topology(*topology, &local_node);
        if(err)
            return err;

        ret++;
    } while(current_node != NULL);

    return ret;
}

struct linked_list_node *silly_next_of_ptr(
    struct linked_list_node *const _Nonnull node,
    const struct silly_network_topology *const _Nonnull topology)
{
    assert(topology);
    assert(node);

    if(node->next)
        return node->next;
    else
        return topology->topology_head;
}

silly_dev_t silly_next_of_dev(
    silly_dev_t dev,
    const struct silly_network_topology *const _Nonnull topology)
{
    assert(topology);

    struct linked_list_node *current = NULL;
    struct linked_list_node *next = NULL;
    struct linked_list_node *result = NULL;
    silly_dev_t current_dev = 0;
    LINKED_LIST_FOREACH(current, next, topology->topology_head)
    {
        current_dev = get_dev_from_topology_node(current);
        if(current_dev == dev) {
            result = next;
            break;
        }
    }

    if(result)
        return get_dev_from_topology_node(result);

    return 0;
}

static void *next_silly_elem(
    uint32_t *_Nonnull current_offset,
    silly_elem_count_t *_Nonnull remaining_elems,
    const struct silly_proto_header *_Nonnull hdr)
{
    assert(current_offset);
    assert(remaining_elems);
    assert(hdr);

    if(*remaining_elems == 0)
        return NULL;

    if(*current_offset == 0)
        *current_offset = sizeof(struct silly_proto_header);

    struct silly_elem *elem = ((void *)hdr) + (*current_offset);

    switch(elem->type) {
    case SILLY_ELEM_TYPE_MASTER_INFO: {
        (*current_offset) += sizeof(struct silly_master_info_elem);
        break;
    }
    case SILLY_ELEM_MASTER_DISCOVERY_QUERY: {
        (*current_offset) += sizeof(struct silly_master_discovery_query_elem);
        break;
    }
    case SILLY_ELEM_TYPE_TOPOLOGY_NODE: {
        (*current_offset) += sizeof(struct silly_proto_topology_node_elem);
        break;
    }
    case _SILLY_ELEM_TYPE_SIZE:
    case _SILLY_ELEM_TYPE_UNSPEC: {
        assert(0); // panic
    }
    }

    remaining_elems -= 1;
    return elem;
}

/**
 * @brief return pointer to start of next silly elem of the specified type
 * @return NULL on non found, a valid pointer into the header otherwise
 */
static void *next_silly_elem_of_type(
    uint32_t *_Nonnull current_offset,
    silly_elem_count_t *_Nonnull remaining_elems,
    enum SILLY_ELEM_TYPE type,
    const struct silly_proto_header *_Nonnull hdr)
{
    struct silly_elem *current =
        next_silly_elem(current_offset, remaining_elems, hdr);

    while(current && current->type != type)
        current = next_silly_elem(current_offset, remaining_elems, hdr);

    return current;
}

int silly_encode_data_into_header(
    void *_Nonnull *_Nonnull data,
    size_t len,
    struct silly_proto_header *_Nonnull *_Nonnull hdr)
{
    int err = silly_encode_dataref_into_header(*data, len, hdr);

    if(err)
        return err;

    freep(*data);
    *data = NULL;

    return 0;
}

int silly_encode_dataref_into_header(
    const void *_Nonnull data,
    size_t len,
    struct silly_proto_header *_Nonnull *_Nonnull hdr)
{
    assert(data);
    assert(hdr);
    assert(*hdr);

    uint32_t encode_start_offset = (*hdr)->len;
    int err = 0;

    err = safe_realloc((void **)hdr, (*hdr)->len + len);
    if(err) {
        return -ENOMEM;
    }

    memcpy(((void *)*hdr) + encode_start_offset, data, len);

    (*hdr)->len += len;

    return 0;
}

int silly_encode_master_info_into_hdr(
    struct silly_master_info_elem *_Nonnull *_Nonnull master_info,
    struct silly_proto_header *_Nonnull *_Nonnull hdr)
{
    return silly_encode_data_into_header(
        (void **)master_info, sizeof(struct silly_master_info_elem), hdr);
}

int silly_decode_master_info_from_hdr(
    struct silly_master_info_elem **_Nonnull master_info,
    const struct silly_proto_header *_Nonnull hdr)
{
    assert(master_info);

    struct silly_master_info_elem *master_info_in_header = NULL;
    uint32_t current_offset = 0;
    silly_elem_count_t remaining_elems = hdr->elem_count;

    master_info_in_header = next_silly_elem_of_type(
        &current_offset, &remaining_elems, SILLY_ELEM_TYPE_MASTER_INFO, hdr);

    if(!master_info_in_header)
        return -ENODATA;

    if(!(*master_info)) {
        *master_info = calloc(1, sizeof(struct silly_master_info_elem));
    }
    if(!(*master_info)) {
        return -ENOMEM;
    }

    memcpy(
        *master_info,
        master_info_in_header,
        sizeof(struct silly_master_info_elem));

    return 0;
}

int silly_encode_master_discovery_query_into_hdr(
    struct silly_master_discovery_query_elem *_Nonnull
        *_Nonnull discovery_query,
    struct silly_proto_header *_Nonnull *_Nonnull hdr)
{
    return silly_encode_data_into_header(
        (void **)discovery_query,
        sizeof(struct silly_master_discovery_query_elem),
        hdr);
}

int silly_dencode_master_discovery_query_into_hdr(
    struct silly_master_discovery_query_elem **_Nonnull discovery_query,
    const struct silly_proto_header *_Nonnull hdr)
{
    assert(discovery_query);

    struct silly_master_info_elem *master_info_in_header = NULL;
    uint32_t current_offset = 0;
    silly_elem_count_t remaining_elems = hdr->elem_count;

    master_info_in_header = next_silly_elem_of_type(
        &current_offset,
        &remaining_elems,
        SILLY_ELEM_MASTER_DISCOVERY_QUERY,
        hdr);

    if(!master_info_in_header)
        return -ENODATA;

    if(!(*discovery_query)) {
        *discovery_query =
            calloc(1, sizeof(struct silly_master_discovery_query_elem));
    }
    if(!(*discovery_query)) {
        return -ENOMEM;
    }

    memcpy(
        *discovery_query,
        master_info_in_header,
        sizeof(struct silly_master_discovery_query_elem));

    return 0;
}

silly_dev_t get_dev_from_topology_node(
    const struct linked_list_node *_Nonnull topology_node)
{
    assert(topology_node);
    assert(topology_node->data);

    struct silly_network_topology_node *node = topology_node->data;
    return node->current;
}

uint8_t get_talking_budget_from_topology_node(
    const struct linked_list_node *_Nonnull topology_node)
{
    assert(topology_node);
    assert(topology_node->data);

    struct silly_network_topology_node *node = topology_node->data;
    return node->talking_budget;
}

struct silly_network_topology_node *silly_network_elem_node_to_local_node(
    const struct silly_proto_topology_node_elem *_Nonnull node_elem)
{
    assert(node_elem);

    __cleanup_free__ struct silly_network_topology_node *ret =
        calloc(1, sizeof(*ret));

    if(!ret)
        return NULL;

    ret->current = node_elem->current;
    ret->talking_budget = node_elem->talking_budget;
    ret->next = node_elem->next;

    return MOVE(&ret);
}

int silly_append_local_node_to_topology(
    struct silly_network_topology *_Nonnull topology,
    struct silly_network_topology_node *_Nonnull *_Nonnull local_node)
{
    assert(topology);
    assert(local_node);
    assert(*local_node);
    // don't assert the topology's head, the linked list might be empty

    __cleanup_linked_list_free__ struct linked_list_node *to_append = NULL;
    to_append = linked_list_make_node((void **)local_node, free);

    if(!to_append)
        return -ENOMEM;

    linked_list_append(&topology->topology_head, &to_append);

    return 0;
}
