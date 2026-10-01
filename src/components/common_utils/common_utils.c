#include "include/common_utils.h"
#include "esp_log.h"
#include "hal/assert.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>

void freep(void *ptr)
{
    free(*(void **)ptr);
    *(void **)ptr = NULL;
}

int safe_realloc(void **_Nonnull ptr, size_t size)
{
    void *tmp;

    assert(ptr);

    if(size == 0) {
        return 0;
    }
    tmp = realloc(*ptr, size);
    if(tmp) {
        *ptr = MOVE(&tmp);
        return 0;
    }
    return ENOMEM;
}

int refcounted_obj_new(
    void *data,
    void (*free_data)(void *, void *),
    void *free_data_arg,
    uint16_t *initial,
    struct refcounted_obj **_Nonnull out)
{
    BaseType_t mutex_acquire_result = pdFALSE;

    assert(out);

    if(*out) {
        mutex_acquire_result =
            xSemaphoreTake((*out)->refcount_mu, portMAX_DELAY);
        if(mutex_acquire_result == pdFALSE)
            return -ETIME;
        if((*out)->refcount != 1) {
            xSemaphoreGive((*out)->refcount_mu);
            return -ETOOMANYREFS;
        }

        // freeing the data
        if((*out)->free_data)
            (*out)->free_data((*out)->data, (*out)->free_data_arg);
        else
            free((*out)->data);

        xSemaphoreGive((*out)->refcount_mu);
        goto assign_data;
    }

    *out = calloc(1, sizeof(struct refcounted_obj));

    if(!(*out))
        goto out_alloc_fail;

    (*out)->refcount_mu = xSemaphoreCreateMutex();
    if(!(*out)->refcount_mu)
        goto mu_create_fail;

    (*out)->refcount = initial ? *initial : 1;

assign_data:
    (*out)->free_data = free_data;
    (*out)->free_data_arg = free_data_arg;
    (*out)->data = data;
    return 0;

mu_create_fail:
    free(*out);
    *out = NULL;
out_alloc_fail:
    return -ENOMEM;
}

int refcounted_obj_acquire(struct refcounted_obj *obj)
{

    assert(obj);

    BaseType_t mutex_acquire_result =
        xSemaphoreTake(obj->refcount_mu, portMAX_DELAY);
    int err = 0;

    if(mutex_acquire_result == pdFALSE)
        return -ETIME;

    if(!((obj->refcount) > 0)) {
        err = -EIDRM;
        goto finish;
    }

    obj->refcount++;

finish:
    xSemaphoreGive(obj->refcount_mu);
    return err;
}
int refcounted_obj_release(struct refcounted_obj **obj)
{

    assert(obj);
    assert(*obj);

    BaseType_t mutex_acquire_result =
        xSemaphoreTake((*obj)->refcount_mu, portMAX_DELAY);
    int err = 0;

    if(mutex_acquire_result == pdFALSE)
        return -ETIME;

    if(!((*obj)->refcount > 0)) {
        err = -EIDRM;
        goto finish;
    }

    (*obj)->refcount--;

    if((*obj)->refcount == 0) {
        if((*obj)->free_data)
            (*obj)->free_data((*obj)->data, (*obj)->free_data);
        else
            free((*obj)->data);

        (*obj)->data = NULL;
        (*obj)->free_data = NULL;

        xSemaphoreGive((*obj)->refcount_mu);
        vSemaphoreDelete((*obj)->refcount_mu);

        free(*obj);
        *obj = NULL;
    }

finish:
    if(*obj)
        xSemaphoreGive((*obj)->refcount_mu);
    return err;
}
