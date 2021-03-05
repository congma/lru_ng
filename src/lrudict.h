#ifndef LRUDICT_H
#define LRUDICT_H

#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include "lrudict_pq.h"

#if (defined __GNUC__) || (defined __clang__)
#define likely(p)     __builtin_expect(!!(p), 1)
#define unlikely(p)   __builtin_expect(!!(p), 0)
#else
#define likely(p)     (p)
#define unlikely(p)   (p)
#endif

/* Programming support for manual/forced purging control */
typedef enum {
    NO_FORCE_PURGE = 0,
    FORCE_PURGE = 1,
} purge_mode_t;


/* 
 * Node object and type, lightweight Python object type as stored values in
 * Python dict
 */
typedef struct _Node {
    PyObject_HEAD
    PyObject *key;
    PyObject *value;
    Py_hash_t key_hash;
    struct _Node *prev;
    struct _Node *next;
} Node;


/* Implementation of LRUDict object */
/* Object structure */
typedef struct _LRUDict {
    PyObject_HEAD
    LRUDict_pq *purge_queue;
    PyObject *callback;
    Node *first;
    Node *last;
    PyObject *dict;
    Py_ssize_t size;
    unsigned long hits;
    unsigned long misses;
    _Bool internal_busy:1;
    _Bool detect_conflict:1;
    _Bool purge_suspended:1;
} LRUDict;


#endif
