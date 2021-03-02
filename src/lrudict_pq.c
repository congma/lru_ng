#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include "lrudict_pq.h"
#include "lrudict.h" /* For Node */
#include <limits.h>
#if (defined __STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
    #ifndef __STDC_NO_ATOMICS__
        #include <stdatomic.h>
    #else
        #define atomic_thread_fence(order) do { /* nothing */ } while (0)
    #endif
#endif


/* Internal function: check exception, write a message to unraisable hook, and
 * suppress the exception. */
static inline void
lrupurge_unraise(PyObject *obj)
{
    if (PyErr_Occurred()) {
        PyErr_WriteUnraisable(obj);
        PyErr_Clear();
    }
}


/* Return newly allocated and initialized purge-queue struct or NULL in case of
 * failure. */
LRUDict_pq *
lrupq_new(void)
{
    LRUDict_pq *q;
    PyObject *new_list;

    if ((q = PyMem_Malloc(sizeof(LRUDict_pq))) == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    if ((new_list = PyList_New(0)) == NULL) {
        PyMem_Free(q);
        return NULL;
    }

    q->lst = new_list;
    q->sinfo.head = q->sinfo.tail = 0;
    q->pending_requests = 0;
    return q;
}


/* DECREF the underlying list and free the memory space of the purge-queue
 * struct. Return 0 on success or -1 on error (typically because somehow a
 * callback cannot leave the purging procedure). */
int
lrupq_free(LRUDict_pq *q)
{
    if (q->pending_requests) {
        return -1;
    }
    else {
        Py_CLEAR(q->lst);
        PyMem_Free(q);
        return 0;
    }
}


/* Execute the purge with callback (optional, can be NULL).
 * Return the number of items actually dislodged from the head of the queue,
 * or -1 in the case of error. */
Py_ssize_t
lrupq_purge(LRUDict_pq *q, PyObject *callback)
{
    Py_ssize_t res;
    struct _pq_sinfo batch;

    /* Load status quo */
    atomic_thread_fence(memory_order_acquire);
    batch = q->sinfo;

    /* Claim up to current tail. */
    q->sinfo.head = batch.tail;
    atomic_thread_fence(memory_order_release);

    /* No need to check for empty batch: the for loop below does it. */

    /* Skip if too many pending. */
    if (q->pending_requests == UINT_MAX) {
        return 0;
    }

    if (callback != NULL) {
        q->pending_requests++;
        Py_INCREF(callback);

        for (Py_ssize_t i = batch.head; i < batch.tail; i++) {
            Node *n;
            PyObject *cres;
            /* Borrow reference from list. */
            n = (Node *)PyList_GetItem(q->lst, i);

            if (n != NULL) {
                cres = PyObject_CallFunctionObjArgs(callback,
                                                    n->key, n->value, NULL);
                if (cres == NULL) {
                    lrupurge_unraise(callback);
                }
                else {
                    /* Discard return value of callback. */
                    Py_DECREF(cres);
                }
            }
            else {
                lrupurge_unraise(q->lst);
            }
        }

        Py_DECREF(callback);
        q->pending_requests--;
    }

    /* Reclaim storage space from garbage items before head. Only do this while
     * no one else's working on the list.
     * The last one to leave the building turns off the lights (on behalf of
     * everyone). */
    if (q->pending_requests == 0) {
        q->pending_requests++;

        /* Re-load current status. */
        atomic_thread_fence(memory_order_acquire);
        batch = q->sinfo;

        if (PyList_SetSlice(q->lst, 0, batch.head, NULL) == -1) {
            lrupurge_unraise(q->lst);
            res = -1;
        }
        else {
            /* Re-sync index information. */
            res = batch.head;
            q->sinfo.head -= res;
            q->sinfo.tail -= res;
        }
        atomic_thread_fence(memory_order_release);

        q->pending_requests--;
    }
    else {
        res = 0;
    }

    return res;
}
