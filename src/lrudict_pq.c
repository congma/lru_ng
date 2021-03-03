#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include "lrudict_pq.h"
#include "lrudict.h" /* For Node */
#include <limits.h>


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


/* Check for the four horsepersons...
 * - RecursionError: Prevent "Fatal Python error: Cannot recover from stack
 *   overflow"; should fail early and let Python know.
 * - SystemError: Should be bubbled up all the way back to Python.
 * - MemoryError: Same as above.
 * - SystemExit: Always honour explicit process-level exit.
 */
static inline _Bool
lrupq_err_bad(PyObject *exc)
{
    return (PyErr_GivenExceptionMatches(exc, PyExc_RecursionError) ||
            PyErr_GivenExceptionMatches(exc, PyExc_SystemError) ||
            PyErr_GivenExceptionMatches(exc, PyExc_MemoryError) ||
            PyErr_GivenExceptionMatches(exc, PyExc_SystemExit));
}


/* Execute the purge with callback (optional, can be NULL).
 * Return the number of items actually dislodged from the head of the queue,
 * or -1 in the case of "swallowed" error, or -2 in the case of unrecoverable
 * error that should request the attention of Python (thinking of this as an
 * escape hatch). */
Py_ssize_t
lrupq_purge(LRUDict_pq *q, PyObject *callback)
{
    Py_ssize_t res;
    struct _pq_sinfo batch;

    /* Load status quo */
    batch = q->sinfo;
    /* Skip if there's nothing to do. Also, we don't need to check for list
     * storage-freeing opportunity when given an empty slice, as long as we
     * assume that the callback always returns eventually, and that the list
     * slice-deletion always succeeds. The former is usually true (if not, we
     * have bigger problems), and the latter seems to hinge on "an assumption
     * that the system realloc() never fails when passed a number of bytes <=
     * the number of bytes last allocated" (see CPython Objects/listobject.c).
     */
    if (batch.tail == batch.head) {
        return 0;
    }

    /* Skip if too many pending.
     * Notice that a larger limit increases the possibility of hitting
     * recursion limit with a misbehaving callback, while a lower value
     * makes the queue more likely to have stuck items (by not being purged as
     * aggressively). */
    if (q->pending_requests == UINT_MAX) {
        return 0;
    }

    /* Claim up to current tail. */
    q->sinfo.head = batch.tail;

    if (callback != NULL) {
        _Bool fail = 0;
        q->pending_requests++;
        Py_INCREF(callback);

        for (Py_ssize_t i = batch.head; i < batch.tail; i++) {
            Node *n;
            PyObject *cres;
            /* Borrow reference from list. */
            n = (Node *)PyList_GetItem(q->lst, i);

            if (n == NULL) {
                lrupurge_unraise(q->lst);
                continue;
            }

            cres = PyObject_CallFunctionObjArgs(callback, n->key, n->value,
                                                NULL);
            if (cres != NULL) {
                /* Discard return value of callback. */
                Py_DECREF(cres);
                continue;
            }

            /* This block is executed if callback returns NULL. A sufficiently
             * bad callback may fail to set (or swallow) exception, so we check
             * explicitly. */
            {
                PyObject *exc = PyErr_Occurred();
                if (exc) {
                    if (lrupq_err_bad(exc)) {
                        /* External exception that needs the attention of
                         * interpreter: do not suppress. Instead, abandon,
                         * leave loop, and signal our intent to go all the way
                         * back to Python. */
                        fail = 1;
                        break;
                    }
                    else {
                        PyErr_WriteUnraisable(callback);
                        PyErr_Clear();
                    }
                }
            }
        }  /* end of "for item in batch" loop */

        Py_DECREF(callback);
        q->pending_requests--;
        if (fail) {
            return -2;
        }
    }

    /* Reclaim storage space from garbage items before head. Only do this while
     * no one else's working on the list.
     * The last one to leave the building turns off the lights (on behalf of
     * everyone). */
    if (q->pending_requests == 0) {
        q->pending_requests++;

        /* Re-load current status. */
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
        q->pending_requests--;
    }
    else {
        res = 0;
    }

    return res;
}
