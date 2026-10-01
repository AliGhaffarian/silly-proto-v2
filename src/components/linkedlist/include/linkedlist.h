#pragma once

struct linked_list_node {
    struct linked_list_node *next;
    struct linked_list_node *prev;
    void *data;
    void (*free_data)(void *);
};

/**
 * @make append the node to the linked list, set as head if list is empty,
 * MOVE(data) if successful
 * TODO: @return node on success, NULL on failure
 */
void linked_list_append(
    struct linked_list_node **head, struct linked_list_node **to_append);
struct linked_list_node
    *

    /**
     * @make a node, MOVE(data) if successful
     * @return node on success, NULL on failure
     */
    linked_list_make_node(void **data, void (*free_data)(void *));
struct linked_list_node *linked_list_make_node_ref(void *data);

void linked_list_free(struct linked_list_node **);

int linked_list_replace_at(
    struct linked_list_node **, struct linked_list_node **, int);

struct linked_list_node *linked_list_get_at(struct linked_list_node *, int);

struct linked_list_node *linked_list_get_tail(struct linked_list_node *);

void linked_list_push_front(
    struct linked_list_node **, struct linked_list_node **);

/**
 * @brief remove the node from the linked list it is contained in
 */
void linked_list_remove_node(struct linked_list_node *_Nonnull *_Nonnull node);

void linked_list_push_front_ref(
    struct linked_list_node **head, struct linked_list_node **elem);

struct linked_list_node *linked_list_clone(
    struct linked_list_node *self, void *(*private_clone)(void *));

#define LINKED_LIST_NEXT_SAFE(current_node)                                    \
    ((current_node) != NULL ? (current_node->next) : NULL)

/**
 * @note do not change,remove,add links when using this, you can only use
 * linked_list_remove_node(&node) next_node is added to provide the ability to
 * remove nodes in the foreach block
 */
#define LINKED_LIST_FOREACH(current_node, next_node, head)                     \
    for((current_node) = (head),                                               \
    (next_node = LINKED_LIST_NEXT_SAFE(current_node));                         \
        (current_node) != NULL;                                                \
        (current_node) = next_node,                                            \
    next_node = LINKED_LIST_NEXT_SAFE(current_node))

#define __cleanup_linked_list_free__                                           \
    __attribute__((__cleanup__(linked_list_free)))
