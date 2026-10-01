#pragma once

#include "freertos/FreeRTOS.h"
#include <stddef.h>
#include <stdlib.h>

void freep(void *ptr);

TickType_t remaining_time(TickType_t deadline, TickType_t timeout);

#define __cleanup_free__      __attribute__((__cleanup__(freep)))
#define GET_NTH_BYTE(data, n) ((data & (0xffULL << ((n) * 8))) >> ((n) * 8))

#define MOVE(ptr_to_ptr)                                                       \
    ({                                                                         \
        void *__tmp_move_var = *ptr_to_ptr;                                    \
        *ptr_to_ptr = NULL;                                                    \
        __tmp_move_var;                                                        \
    })

int safe_realloc(void **_Nonnull ptr, size_t size);

struct refcounted_obj {
    void *data;
    void (*free_data)(void *data, void *free_data_arg);
    void *free_data_arg;
    uint16_t refcount;
    SemaphoreHandle_t refcount_mu; /** created using xSemaphoreCreateMutex() */
};

int refcounted_obj_new(
    void *data,
    void (*free_data)(void *, void *),
    void *free_data_arg,
    uint16_t *initial,
    struct refcounted_obj **_Nonnull out);

#pragma clang assume_nonnull begin
int refcounted_obj_acquire(struct refcounted_obj *obj);
int refcounted_obj_release(struct refcounted_obj **obj);

void inline refcounted_obj_release_p(struct refcounted_obj **obj)
{
    refcounted_obj_release(obj);
};
#pragma clang assume_nonnull end

#define __cleanup_release_refcounted_obj__                                     \
    __attribute__((__cleanup__(refcounted_obj_release_p)))

inline void semaphore_give(SemaphoreHandle_t *sem)
{
    if(!sem || !(*sem))
        return;

    xSemaphoreGive(*sem);
    *sem = NULL;
}
#define __cleanup_semaphore_give__ __attribute__((__cleanup__(semaphore_give)))

#define _PROTO_CONCAT_IMPL(a, b)  a##b
#define _PROTO_CONCAT(a, b)       _PROTO_CONCAT_IMPL(a, b)
#define _PROTO_UNIQUE_VAR(prefix) _PROTO_CONCAT(prefix, __COUNTER__)

/**
 * @brief Takes a semaphore for the current scope.
 * Automatically gives the semaphore back upon exiting the scope if acquired
 * successfully.
 *
 * @param bool acquired: is set to true if the semaphore is acquired
 * successfully
 */
#define SCOPED_SEMAPHORE_TAKE(sem, wait, acquired)                             \
    SemaphoreHandle_t __attribute__((cleanup(semaphore_give)))                 \
    __attribute__((unused))                                                    \
    _PROTO_UNIQUE_VAR(_sem_guard_) =                                           \
        (acquired = (xSemaphoreTake((sem), (wait)) == pdTRUE)) ? (sem) : NULL

// Source - https://stackoverflow.com/q/15832301
// Posted by jaeyong, modified by community. See post 'Timeline' for change
// history Retrieved 2026-08-10, License - CC BY-SA 4.0
#define CONTAINER_OF(ptr, type, member)                                        \
    ({                                                                         \
        const typeof(((type *)0)->member) *__mptr = (ptr);                     \
        (type *)((char *)__mptr - offsetof(type, member));                     \
    })
