#pragma once

#include "common_utils.h"
#include "errno.h"
#include "linkedlist.h"
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <inttypes.h>

#define ESP_TASK_EVENT_PRIO 20

#define SILLY_PROTO_MAX_NODES       32
#define SILLY_DEFAULT_STACK_SIZE    2048
#define SILLY_DEFAULT_TASK_PRIORITY ESP_TASK_EVENT_PRIO

#define TIMER_SILLY_SLAVE_TOKEN_DEADLINE 10

enum SILLY_PROTO_FLAGS {
    _SILLY_PROTO_FLAGS_UNSPEC = 0,
    SILLY_PASS_TOKEN = 1 << 0,
    SILLY_MASTER_DISCOVERY_QUERY = 1 << 1,
    SILLY_CONTAINS_MASTER_INFO = 1 << 2,
    SILLY_CONTAINS_NETWORK_TOPOLOGY = 1 << 3,
    _SILLY_PROTO_FLAGS_SIZE = 1 << 4,
};

// master capabilities
enum SILLY_MASTER_CAPS {
    _SILLY_MASTER_CAPS_UNSPEC = 0,
    SILLY_MASTER_CAPS_DRIVER_NETWORK = 1 << 0,
    SILLY_MASTER_CAPS_DO_MASTER_STUFF = 1 << 1,
    _SILLY_MASTER_CAPS_SIZE = 1 << 2,
};

// types of elements (objects) that can be present in the header
enum SILLY_ELEM_TYPE {
    _SILLY_ELEM_TYPE_UNSPEC = 0,
    SILLY_ELEM_TYPE_TOPOLOGY_NODE,
    SILLY_ELEM_MASTER_DISCOVERY_QUERY,
    SILLY_ELEM_TYPE_MASTER_INFO,
    _SILLY_ELEM_TYPE_SIZE,
};

#define SILLY_MAGIC 0x22aa

typedef uint16_t silly_magic_t;
typedef uint32_t silly_len_t;
typedef uint32_t silly_flags_t;
typedef uint32_t silly_checksum_t;
typedef uint32_t silly_elem_count_t;
typedef uint64_t silly_dev_t;
silly_dev_t silly_dev_fetch_from_chip();
bool silly_dev_is_equal(silly_dev_t first, silly_dev_t second);

struct silly_proto_header {
    silly_magic_t magic;
    silly_len_t len;     /** len of the header+data */
    silly_flags_t flags; /** flags, of SILLY_PROTO_FLAGS */
    silly_checksum_t header_checksum;
    silly_checksum_t data_checksum;

    silly_dev_t src;
    silly_dev_t dst; /** optional when not passing the token */
    silly_dev_t master;

    silly_elem_count_t elem_count; /** number of silly elements inside data */
    char data[];
} __attribute__((packed));

int silly_uart_recv_pkt(
    uart_port_t uart_port, struct silly_proto_header **hdr, TickType_t timeout);

#define SILLY_CHECKSUM_VERIFY_SUCCESS 0
#define SILLY_CHECKSUM_VERIFY_FAILED  -1

int silly_verify_data_checksum(struct silly_proto_header *const _Nonnull hdr);
silly_checksum_t
silly_compute_data_checksum(struct silly_proto_header *const _Nonnull hdr);

int silly_verify_header_checksum(struct silly_proto_header *const _Nonnull hdr);
silly_checksum_t
silly_compute_header_checksum(struct silly_proto_header *const _Nonnull hdr);

int silly_verify_checksum(struct silly_proto_header *const _Nonnull hdr);

struct silly_elem {
    enum SILLY_ELEM_TYPE type;

    char data[];
} __attribute__((packed));

typedef uint8_t silly_master_caps_t;
struct silly_master_info_elem {
    enum SILLY_ELEM_TYPE type;

    silly_dev_t dev;
    silly_master_caps_t caps;
} __attribute__((packed));

struct silly_proto_topology_node_elem {
    enum SILLY_ELEM_TYPE type;

    silly_dev_t current;
    uint8_t talking_budget;
    silly_dev_t next;
} __attribute__((packed));

struct silly_master_discovery_query_elem {
    enum SILLY_ELEM_TYPE type;

    silly_dev_t prefix;
    silly_dev_t mask;
} __attribute__((packed));

struct silly_network_topology_node {
    silly_dev_t current;
    uint8_t talking_budget;
    silly_dev_t next;
};

/**
 * each node is a silly_network_topology_node, needs to be in the heap and a pointer to it must
 * be assigned to the data member of linked_list_node
 */
struct silly_network_topology {
    struct linked_list_node *topology_head;
    struct linked_list_node *topology_me;
};
inline void
freep_network_topology(struct silly_network_topology **_Nonnull topologyp)
{
    linked_list_free(&(*topologyp)->topology_head);
    free(*topologyp);
    *topologyp = NULL;
}
#define __cleanup_free_network_topology__                                      \
    __attribute__((__cleanup__(freep_network_topology)))

/**
 * @return `current` field of the embedded `silly_network_topology_node`
 */
silly_dev_t get_dev_from_topology_node(
    const struct linked_list_node *_Nonnull topology_node);

/**
 * @return `talking_budget` field of the embedded `silly_network_topology_node`
 */
uint8_t get_talking_budget_from_topology_node(
    const struct linked_list_node *_Nonnull topology_node);

struct silly_proto_shared_ctx {
    // written in initialization, readonly in runtime
    silly_dev_t my_mac;

    silly_dev_t master_node;
    SemaphoreHandle_t master_node_mu;

    struct silly_network_topology *topology;
    SemaphoreHandle_t topology_mu;

    // talking budget, declared by master, decremented on each message enqueued
    // to sender task
    uint8_t talking_budget;
    SemaphoreHandle_t talking_budget_mu;

    // isn't protected by a lock, because we guarantee one reader/writer by
    // guaranteeing one state machine to execute the token passing sequence,
    // and other state machines to just ignore and carry on to the next state
    //      init: when broadcasting/receiving a topolgy (inside silly_do_set_network_topology())
    //      reevaluate: action of SILLY_EVENT_SLAVE_TOKEN_DEADLINE
    // because only one state machine stays in SILLY_PASS_TOKEN state, we don't need a lock
    silly_dev_t token_candidate;

    // queue that is used to send to sender, when the token is ours
    QueueHandle_t tx_queue;
};

/**
 * @brief init with default value, truncate the data
 *
 * 1. realloc the header to `sizeof(struct silly_proto_header)`
 * 2. assign magic
 * 3. assign me as src
 * 4. assign master
 * 5. assign `sizeof(struct silly_proto_header)` as len
 *
 * @return 0 on success,
 *      -1 on master lock acquisition failure,
 *      err of `safe_realloc()` otherwise
 */
int silly_reset_header(
    struct silly_proto_shared_ctx *_Nonnull shared_ctx,
    struct silly_proto_header **_Nonnull hdr);

/**
 * 1. construct the locked_port object
 * 2. init my mac
 * 3. init talking counter's mutex
 * 4. init talking budget's mutex
 * 5. init topology's mutex
 */
struct silly_proto_shared_ctx *
silly_new_proto_shared_ctx(silly_dev_t my_mac, QueueHandle_t tx_queue);

struct silly_proto_header *silly_make_discovery_query_reply(
    struct silly_proto_shared_ctx *_Nonnull proto_shared_ctx);

struct silly_proto_header *
silly_make_token(struct silly_proto_shared_ctx *_Nonnull proto_shared_ctx);

/**
 * @brief encode the entire topology into header
 * @return 0 on success,
 *      -ENOMEM on memory allocation failure
 */
int silly_encode_local_topology_into_hdr(
    const struct silly_network_topology *_Nonnull topology,
    struct silly_proto_header *_Nonnull *_Nonnull hdr);

/**
 * @brief extract node objects from the header
 *
 * if `me` is non zero:
 *      - set `*talking_budget` to the value
 *          declared by the topology in `hdr` for the node `me`
 *      - set `topology->topology-me` to the node with matching `silly_dev_t`
 *
 * @return negative on error, number of extracted nodes otherwise
 */
int silly_decode_local_topology_from_hdr(
    struct silly_network_topology **_Nonnull topology,
    const silly_dev_t me,
    uint8_t *talking_budget,
    const struct silly_proto_header *_Nonnull hdr);

/**
 * @brief translate `silly_proto_topology_node_elem` to `silly_network_topology_node`
 * @return NULL on memory allocation failure, pointer to a `silly_network_topology_node` otherwise
 */
struct silly_network_topology_node *silly_network_elem_node_to_local_node(
    const struct silly_proto_topology_node_elem *_Nonnull node_elem);

/**
 * @return 0 on success
 *      -ENOMEM on memory allocation failure
 */
int silly_append_local_node_to_topology(
    struct silly_network_topology *_Nonnull topology,
    struct silly_network_topology_node *_Nonnull *_Nonnull local_node);

struct linked_list_node *silly_next_of_ptr(
    struct linked_list_node *const _Nonnull node,
    const struct silly_network_topology *const _Nonnull topology);

silly_dev_t silly_next_of_dev(
    silly_dev_t dev,
    const struct silly_network_topology *const _Nonnull topology);

// TODO:
silly_dev_t silly_is_first_after_second(
    silly_dev_t first,
    silly_dev_t second,
    const struct silly_network_topology *_Nonnull topology);

// TODO:
silly_dev_t silly_remove_between_first_and_second(
    silly_dev_t first,
    silly_dev_t second,
    struct silly_network_topology *_Nonnull topology);

/**
 * @brief return pointer to start of next silly elem
 * @param current_offset offset of the next elem to be returned, initialize with zero to reset
 * won't check anything, just moves the current_offset and returns the new elem pointer
 * @return NULL on elem exhuastion, a valid pointer otherwise
 */
static void *next_silly_elem(
    uint32_t *_Nonnull current_offset,
    silly_elem_count_t *_Nonnull remaining_elems,
    const struct silly_proto_header *_Nonnull hdr);

/**
 * @brief return pointer to start of next silly elem of the specified type
 * @return NULL on non found, a valid pointer into the header otherwise
 */
static void *next_silly_elem_of_type(
    uint32_t *_Nonnull current_offset,
    silly_elem_count_t *_Nonnull remaining_elems,
    enum SILLY_ELEM_TYPE type,
    const struct silly_proto_header *_Nonnull hdr);

/**
 * @brief encode data info into header
 *
 * frees data on success
 *
 * @return 0 on success, -ENOMEM on allocation failure
 */
int silly_encode_data_into_header(
    void *_Nonnull *_Nonnull data,
    size_t len,
    struct silly_proto_header *_Nonnull *_Nonnull hdr);

/**
 * @brief encode data info into header
 *
 * does not free data on success
 *
 * @return 0 on success, -ENOMEM on allocation failure
 */
int silly_encode_dataref_into_header(
    const void *_Nonnull data,
    size_t len,
    struct silly_proto_header *_Nonnull *_Nonnull hdr);

/**
 * @brief encode master info into header
 * directly calls silly_encode_data_into_header
 */
int silly_encode_master_info_into_hdr(
    struct silly_master_info_elem *_Nonnull *_Nonnull master_info,
    struct silly_proto_header *_Nonnull *_Nonnull hdr);

/**
 * @brief decode master info from header
 * @return 0 on success,
 *      -ENODATA on no master info found,
 *      -ENOMEM on memory allocation failure
 */
int silly_decode_master_info_from_hdr(
    struct silly_master_info_elem **_Nonnull master_info,
    const struct silly_proto_header *_Nonnull hdr);

/**
 * @brief encode master discovery query into header
 * directly calls silly_encode_data_into_header
 */
int silly_encode_master_discovery_query_into_hdr(
    struct silly_master_discovery_query_elem *_Nonnull
        *_Nonnull discovery_query,
    struct silly_proto_header *_Nonnull *_Nonnull hdr);

/**
 * @brief decode master discoveryquery from header
 * @return 0 on success,
 *      -ENODATA on no master info found,
 *      -ENOMEM on memory allocation failure
 */
int silly_dencode_master_discovery_query_into_hdr(
    struct silly_master_discovery_query_elem **_Nonnull discovery_query,
    const struct silly_proto_header *_Nonnull hdr);

#define SILLY_MAX_TIMERS 3 /** token deadline, network stop, master death */

enum SILLY_EVENT {
    _SILLY_EVENT_UNSPEC,
    SILLY_EVENT_NOEVENT,
    SILLY_EVENT_SLAVE_TOKEN_DEADLINE,
    SILLY_EVENT_NETWORK_STOP_TIMEOUT,
    SILLY_EVENT_MASTER_DEATH_TIMEOUT,
    SILLY_EVENT_COLLISION,
    SILLY_EVENT_SLAVE_PASSED_TOKEN,
    SILLY_EVENT_MASTER_DISCOVERY_QUERY,
    SILLY_EVENT_RECVED_NETWORK_TOPOLOGY,
    SILLY_EVENT_RECVED_MASTER_INFO,
    SILLY_EVENT_RECV_ANY,
    _SILLY_EVENT_SIZE,
};
extern const char *const enum_str_map_silly_event[_SILLY_EVENT_SIZE];

/**
 * @brief data elements delivered to the user application
 */
struct silly_rx_queue_elem {
    silly_len_t len;
    silly_dev_t src;
    char data[];
};

/**
 * @brief data elements delivered from the user application
 */
struct silly_tx_queue_elem {
    silly_len_t len;
    silly_dev_t dst;
    char data[];
};

//TODO:
int silly_get_payload(
    const struct silly_proto_header *_Nonnull pkt,
    void **_Nonnull payload,
    size_t *_Nonnull payload_len);
