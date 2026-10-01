#include "include/linkedlist.h"
#include "common_utils.h"
#include "hal/assert.h"
#include <stdlib.h>

void linked_list_append(
    struct linked_list_node **head, struct linked_list_node **to_append)
{
    struct linked_list_node **ptr_of_interest = head;
    struct linked_list_node *prev_node = NULL;

    while(*ptr_of_interest != (struct linked_list_node *)NULL) {
        prev_node = *ptr_of_interest;
        ptr_of_interest = &(*ptr_of_interest)->next;
    }

    *(ptr_of_interest) = *to_append;
    (*ptr_of_interest)->prev = prev_node;
    *to_append = NULL;
}

struct linked_list_node *
linked_list_make_node(void **data, void (*free_data)(void *))
{
    struct linked_list_node *tmp_node =
        calloc(1, sizeof(struct linked_list_node));
    if(!tmp_node)
        return (struct linked_list_node *)NULL;

    tmp_node->next = (struct linked_list_node *)NULL;
    tmp_node->prev = (struct linked_list_node *)NULL;
    if(data) {
        tmp_node->data = MOVE(data);
        if(free_data)
            tmp_node->free_data = free_data;
    } else
        tmp_node->data = NULL;

    return tmp_node;
}

struct linked_list_node *linked_list_make_node_ref(void *data)
{
    struct linked_list_node *tmp_node =
        calloc(1, sizeof(struct linked_list_node));
    if(!tmp_node)
        return (struct linked_list_node *)NULL;

    tmp_node->next = (struct linked_list_node *)NULL;
    tmp_node->prev = (struct linked_list_node *)NULL;
    if(data) {
        tmp_node->data = data;
    } else
        tmp_node->data = NULL;

    return tmp_node;
}

void linked_list_free(struct linked_list_node **head)
{
    struct linked_list_node *current = *head;
    struct linked_list_node *next = current->next;
    while(current != (struct linked_list_node *)NULL) {
        next = current->next;
        if(current->free_data)
            current->free_data(current->data);
        else
            free(current->data);
        free(current);
        current = next;
    }
    *head = NULL;
}

int linked_list_replace_at(
    struct linked_list_node **head, struct linked_list_node **to_insert, int at)
{
    int i = 0;
    struct linked_list_node *current_node = NULL;
    struct linked_list_node *node_before_at = NULL;

    if(at == 0) {
        if(*head)
            (*to_insert)->next = MOVE((void **)&(*head)->next);
        else
            (*to_insert)->next = NULL;

        free(*head);
        *head = MOVE((void **)to_insert);
        return 0;
    }

    if(*head == NULL)
        *head = linked_list_make_node(NULL, NULL);
    current_node = *head;
    while(i < at - 1) {
        if(current_node->next == NULL) {
            current_node->next = linked_list_make_node(NULL, NULL);

            if(current_node->next == NULL)
                return 1;

            current_node->next->prev = current_node;
        }

        current_node = current_node->next;

        i++;
    }
    node_before_at = current_node;

    if(node_before_at->next != NULL) {
        (*to_insert)->next = MOVE((void **)&node_before_at->next->next);
        free(node_before_at->next);
    }

    (*to_insert)->prev = node_before_at;
    node_before_at->next = MOVE((void **)to_insert);

    return 0;
}

struct linked_list_node *
linked_list_get_at(struct linked_list_node *head, int at)
{
    int cur = 0;
    struct linked_list_node *current_node = head;
    while(cur < at) {
        if(current_node->next == NULL)
            return NULL;
        current_node = current_node->next;
        cur++;
    }
    return current_node;
}

void linked_list_push_front(
    struct linked_list_node **head, struct linked_list_node **elem)
{
    (*elem)->next = MOVE((void **)head);

    *head = MOVE((void **)elem);
}

void linked_list_remove_node(struct linked_list_node *_Nonnull *_Nonnull node)
{
    assert(node);
    assert(*node);

    struct linked_list_node *prev = (*node)->prev;
    struct linked_list_node *next = (*node)->next;
    __cleanup_linked_list_free__ struct linked_list_node *node_backup = *node;
    if(prev)
        prev->next = next;
    if(next)
        next->prev = prev;
    // is head
    if(!prev && next)
        *node = (*node)->next;
    else
        *node = NULL;

    if(node_backup->free_data)
        node_backup->free_data(node_backup->data);
}

void linked_list_push_front_ref(
    struct linked_list_node **head, struct linked_list_node **elem)
{
    (*elem)->next = *head;

    *head = *elem;
}

struct linked_list_node *linked_list_get_tail(struct linked_list_node *head)
{
    if(!head)
        return NULL;
    if(!head->next)
        return head;

    struct linked_list_node *current_node = head;
    struct linked_list_node *next_node = head->next;

    while(next_node) {
        current_node = next_node;
        next_node = next_node->next;
    }
    return current_node;
}

struct linked_list_node *
linked_list_clone(struct linked_list_node *self, void *(*private_clone)(void *))
{
    struct linked_list_node *cloned_current_node = NULL;
    void *cloned_private_data = NULL;
    struct linked_list_node *clone_head = NULL;
    struct linked_list_node *self_current_node = self;

    while(self_current_node) {
        if(private_clone) {
            cloned_private_data = private_clone(self_current_node->data);
            if(!cloned_private_data)
                goto fail;
        } else
            cloned_private_data = self_current_node->data;

        cloned_current_node = linked_list_make_node(
            &cloned_private_data, cloned_current_node->free_data);
        if(!cloned_current_node)
            goto fail;

        linked_list_append(&clone_head, &cloned_current_node);
        self_current_node = self_current_node->next;
    }
    return clone_head;
fail:
    // TODO: free if error
    return NULL;
}
