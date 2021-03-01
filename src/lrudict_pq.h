#ifndef LRUDICT_PQ_H
#define LRUDICT_PQ_H
#include "Python.h"
/* Simple queue supporting pushing to the end and claim-and-work by multiple
 * agents from the head; see CPython's Modules/_queuemodule.c for reference. */


struct _pq_sinfo {
    Py_ssize_t head;
    Py_ssize_t tail;
};


typedef struct _LRUDict_pq {
    struct _pq_sinfo sinfo;
    PyObject *lst;
    unsigned int pending_requests;
} LRUDict_pq;


LRUDict_pq *
lrupq_new(void);

int
lrupq_free(LRUDict_pq *q);

Py_ssize_t
lru_purge(LRUDict_pq *q, PyObject *callback);

#endif
