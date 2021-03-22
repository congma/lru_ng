#ifndef LRUDICT_H
#define LRUDICT_H

#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include "lrudict_pq.h"

#if (defined __GNUC__) || (defined __clang__) || (defined __INTEL_COMPILER)
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
 * Python dict. The NodePayload struct is mostly just a (hopefully) convenient
 * way to "pack" values (as non-boxed PoD values) on the stack, and passed
 * around by pointer, so as to reduce the number of arguments to internal
 * functions/macros. It cannot "own" (in the Python reference-counting sense)
 * the key and value: the real "owner" is the Node object. The payload struct
 * is just something that can be copied to the pl member of Node.
 */
typedef struct _NodePayload {
    PyObject *key;
    PyObject *value;
    Py_hash_t key_hash;
} NodePayload;


typedef struct _Node {
    PyObject_HEAD
    struct _Node *next;
    struct _Node *prev;
    NodePayload pl;
} Node;


/* Implementation of LRUDict object */
/* Object structure */
typedef struct _LRUDict {
    PyObject_HEAD
    PyObject *dict;
    Py_ssize_t size;
    Node *root;
    unsigned long hits;
    unsigned long misses;
    LRUDict_pq *purge_queue;
    PyObject *callback;
    _Bool _pb;
    _Bool detect_conflict:1;
    _Bool internal_busy:1;
    _Bool purge_suspended:1;
} LRUDict;


/* Forward declarations */
#if PY_VERSION_HEX >= 0x03070000
typedef Py_ssize_t (*dict_lookup_func)
(PyDictObject *mp, PyObject *key, Py_hash_t hash, PyObject **value_addr);
#else
typedef Py_ssize_t (*dict_lookup_func)
(PyDictObject *mp, PyObject *key, Py_hash_t hash, PyObject ***value_addr,
 Py_ssize_t *hashpos);
#endif


struct _dictkeysobject {
    Py_ssize_t dk_refcnt;
    Py_ssize_t dk_size;
    dict_lookup_func dk_lookup;
    Py_ssize_t dk_usable;
    Py_ssize_t dk_nentries;
    char dk_indices[];
};


#ifndef DKIX_ERROR
#define DKIX_ERROR ((Py_ssize_t)(-3))
#endif


#ifndef PyDict_GET_SIZE
#define PyDict_GET_SIZE(d)  (((PyDictObject *)(d))->ma_used)
#endif


#endif
