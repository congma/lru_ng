#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include "lrudict_exctype.h"
#include "lrudict_statstype.h"
#include <stdlib.h>
#include <stddef.h>


/*
 * This is a simple implementation of LRUDict that uses a Python dict and an
 * associated doubly linked list to keep track of recently inserted/accessed
 * items.
 *
 * Dict will store: key -> Node mapping, where Node is a linked list node.
 * The Node itself will contain the value as well as the key.
 *
 * For eg:
 *
 * >>> l = LRUDict(2)
 * >>> l[0] = 'foo'
 * >>> l[1] = 'bar'
 *
 * can be visualised as:
 *
 *                 -+-hash(0)-+-   -+-hash(1)-+-
 * self->dict   ... |         | ... |         | ...
 *                 -+----|----+-   -+----|----+-
 *                       :               :
 *                +------v-----+   +-----v------+
 * self->last --->|<'foo'>, <0>|-->|<'bar'>, <1>|---> NULL
 *       NULL <---|            |<--|            |<--- self->first
 *                +------------+   +------------+
 *
 *  The invariant is to maintain the list to reflect the LRU order of items in
 *  the dict.  self->first will point to the MRU item and self->last to LRU
 *  item. Size of list will not grow beyond size of the dict.
 *
 */


#define GET_NODE(d, key) ((Node *)(Py_TYPE((d))->tp_as_mapping->mp_subscript((d), (key))))
#define PUT_NODE(d, key, node) (Py_TYPE((d))->tp_as_mapping->mp_ass_subscript((d), (key), ((PyObject *)node)))



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
    struct _Node * restrict prev;
    struct _Node * restrict next;
    PyObject *value;
    PyObject *key;
    Py_hash_t key_hash;
} Node;


static void
node_dealloc(Node *self)
{
    Py_DECREF(self->key);
    Py_DECREF(self->value);
    Py_TYPE(self)->tp_free((PyObject *)self);
}


static PyObject *
node_repr(Node *self)
{
    return PyObject_Repr(self->value);
}


static PyTypeObject NodeType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "lru_ng._Node",
    .tp_basicsize = sizeof(Node),
    .tp_itemsize = 0,
    .tp_dealloc = (destructor)node_dealloc,
    .tp_repr = (reprfunc)node_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "linked-list node for internal use",
    .tp_new = PyType_GenericNew,
};


#define NODE_INIT(_node, _k, _v, _kh)  \
do {                            \
    Py_INCREF((_k));            \
    Py_INCREF((_v));            \
    (_node)->value = (_v);      \
    (_node)->key = (_k);        \
    (_node)->key_hash = (_kh);  \
} while (0)


/* Implementation of LRUDict object */
/* Object structure */
typedef struct {
    PyObject_HEAD
    PyObject *dict;
    Node *first;
    Node *last;
    PyObject *callback;
    PyObject *staging_list;
    Py_ssize_t size;
    unsigned long hits;
    unsigned long misses;
    _Bool internal_busy:1;
    _Bool detect_conflict:1;
    _Bool should_purge;
    _Bool purge_busy:1;
    _Bool purge_suspended:1;
} LRUDict;


/* LRUDict internal critical section macros. These sections must be entered
 * with the Python GIL held. This is normally satisfied if the entrance/exit
 * sequence is only used in Python-facing methods and nowhere else */
#define LRU_ENTER_CRIT(self, failresult)    \
do {                                        \
    if ((self)->detect_conflict && (self)->internal_busy) {  \
        PyErr_SetString(LRUDictExc_BusyErr, \
                "attempted entry into LRUDict critical section while busy");\
        return (failresult);    \
    }                           \
    self->internal_busy = 1;    \
} while (0)


#define LRU_LEAVE_CRIT(self)    \
do {                            \
    (self)->internal_busy = 0;  \
} while (0)


/* Linked-list data-structure implementations internal to LRUDict */
static inline void
lru_remove_node_impl(LRUDict *self, Node * restrict node)
{
    if (self->first == node) {
        self->first = node->next;
    }
    if (self->last == node) {
        self->last = node->prev;
    }
    if (node->prev) {
        node->prev->next = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    }
    /* Optimization hack: let the detached node's link pointers dangle. */
    /* node->next = node->prev = NULL; */
}


static inline void
lru_add_node_at_head_impl(LRUDict *self, Node * restrict node)
{
    node->prev = NULL;
    if (!self->first) {
        self->first = self->last = node;
        node->next = NULL;
    }
    else {
        node->next = self->first;
        node->next->prev = node;
        self->first = node;
    }
}


static inline void
lru_delete_last_impl(LRUDict *self)
{
    Node *n = self->last;
    if (n == NULL) {
        return;
    }

    /* Transfer the node to staging.
     * This transfer is not merely conditional on self->callback being set or
     * not. Not having a callback doesn't mean we can safely DECREF the node
     * here, because as we DECREF the last reference to the node, it's possible
     * to trigger arbitrary code in the Node's key or value's __del__.*/
    Py_INCREF(n);
    if (_PyDict_DelItem_KnownHash(self->dict, n->key, n->key_hash) == 0) {
        lru_remove_node_impl(self, n);
        /* The list will increase the refcount to the node if successful */
        if (self->callback ||
            Py_REFCNT(n->key) == 1 || Py_REFCNT(n->value) == 1)
        {
            if (PyList_Append(self->staging_list, (PyObject *)n) != -1) {
                self->should_purge = 1;
            }
        }
    }
    /* This DECREF in the case when the list append isn't succesful (a rare
     * condition) is the last resort, but in normal condition it simply mean
     * the reference is transfered to the list. */
    Py_DECREF(n);
}


    /* Purging mechanism.
     *
     * NOTE: This is _the_ place where callback gets called. The function is
     * intended to be always called _outside_ the LRUDict-critical section. The
     * reason is that we cannot guarantee the callback function, which can do
     * anything, will play nice with the LRUDict object itself. If the callback
     * were executed within the critical section where/while the object
     * internal data is not consistent, the inconsistency can be left exposed.
     *
     * The staging area is just a Python list, and if a callback is set, it is
     * called along the list in ascending index order. Then the list elements
     * are wiped.
     *
     * The function must be called while the GIL is held. This is normally
     * true, but the callback may execute a code path that release the GIL. For
     * this reason a busy bit, self->purge_busy, is checked. While this bit is
     * set, another call into the section immediately returns (balks). The list
     * may grow during the callback action, and the recently appended items
     * will be purged later.
     */
static Py_ssize_t
lru_purge_staging_impl(LRUDict *self, purge_mode_t opt)
{
    Py_ssize_t i, len;
    PyObject *cb_tmp;

    if (opt != FORCE_PURGE && self->purge_suspended) {
        return 0;
    }
    if (!(self->should_purge) || self->internal_busy || self->purge_busy) {
        return 0;
    }

    /* Easy case: no callback */
    if (self->callback == NULL) {
        self->purge_busy = 1;
        self->should_purge = 0;
        len = PyList_Size(self->staging_list);
        if (PySequence_DelSlice(self->staging_list, 0, len) == -1) {
            PyErr_WriteUnraisable((PyObject *)self);
            PyErr_Clear();
        }
        self->purge_busy = 0;
        return len;
    }

    self->purge_busy = 1;
    self->should_purge = 0;
    /* Prevent the callback from being pulled from under us */
    cb_tmp = self->callback;
    Py_INCREF(cb_tmp);

    len = PyList_Size(self->staging_list);
    for (i = 0; i < len; i++) {
        Node *args;
        /* new reference */
        args = (Node *)PySequence_GetItem(self->staging_list, i);

        if (args != NULL) {
            PyObject *result;
            result = PyObject_CallFunctionObjArgs(cb_tmp,
                                                  args->key, args->value,
                                                  NULL);
            if (result == NULL) {
                if (PyErr_Occurred()) {
                    /* Callback indicates failure, Python exception may or may
                     * not be set. This becomes problematic as the exception
                     * may interfere with the return of the method that calls
                     * the purge. We shouldn't assume that the caller of the
                     * methods is expected to handle the exception caused by a
                     * totally unrelated reason.
                     */
                    PyErr_WriteUnraisable((PyObject *)self);
                    PyErr_Clear();
                }
            }
            else {
                /* Discard callback return value */
                Py_DECREF(result);
            }
        }
        else {  /* if (args), i.e., if we fail to get item from list */
            /* A rather bad situation where our idea of the list is not
             * consistent with the list, and we're not really ready to handle
             * that. */
            if (PyErr_Occurred()) {
                PyErr_WriteUnraisable((PyObject *)self);
                PyErr_Clear();
            }
        }       /* end of if (args) */
        /* dispose of new reference to list item */
        Py_XDECREF(args);
    }   /* end of loop over list items */
    if (PySequence_DelSlice(self->staging_list, 0, len) == -1) {
        if (PyErr_Occurred()) {
            PyErr_WriteUnraisable((PyObject *)self);
            PyErr_Clear();
        }
    }
    Py_DECREF(cb_tmp);
    self->purge_busy = 0;
    return len;
}


/* Querying length information (Python __len__ or len() function) */
static inline Py_ssize_t
lru_length_impl(LRUDict *self)
{
    return PyDict_Size(self->dict);
}


static Py_ssize_t
LRU_length(LRUDict *self)
{
    return lru_length_impl(self);
}


/* Size (capacity) property access, validation, and setting (re-sizing) */
static PyObject *
LRU_size_getter(LRUDict *self, void *closure)
{
    (void)closure;

    return PyLong_FromSsize_t(self->size);
}


static PyObject *
LRU_get_size_legacy(LRUDict *self, PyObject *Py_UNUSED(ignored))
{
    return LRU_size_getter(self, NULL);
}


static inline int
lru_set_size_impl(LRUDict *self, Py_ssize_t newsize)
{
    if (newsize <= 0) {
        PyErr_SetString(PyExc_ValueError, "size must be positive");
        return -1;
    }

    self->size = newsize;
    while (lru_length_impl(self) > newsize) {
        lru_delete_last_impl(self);
    }

    return 0;
}


/* Descriptor setter function for size */
static int
LRU_size_setter(LRUDict *self, PyObject *value, void *closure)
{
    int status;
    Py_ssize_t newsize;

    (void)closure;

    if (value == NULL) {
        PyErr_SetString(PyExc_AttributeError, "can't delete size");
        return -1;
    }
    if (!PyLong_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "size must be an integer");
        return -1;
    }

    newsize = PyLong_AsSsize_t(value);
    if (newsize == -1 && PyErr_Occurred()) {
        return -1;      /* this also checks for overflow */
    }

    /* Setting new size may trigger eviction, must protect */
    LRU_ENTER_CRIT(self, -1);
    status = lru_set_size_impl(self, newsize);
    LRU_LEAVE_CRIT(self);
    lru_purge_staging_impl(self, NO_FORCE_PURGE);

    return status;
}


static PyObject *
LRU_set_size_legacy(LRUDict *self, PyObject *args)
{
    Py_ssize_t newsize;
    int status;

    if (!PyArg_ParseTuple(args, "n:set_size", &newsize)) {
        return NULL;
    }
    /* Setting new size may trigger eviction, must protect */
    LRU_ENTER_CRIT(self, NULL);
    status = lru_set_size_impl(self, newsize);
    LRU_LEAVE_CRIT(self);
    lru_purge_staging_impl(self, NO_FORCE_PURGE);

    if (status == -1) {
        return NULL;
    }
    Py_RETURN_NONE;
}


/* Callback property: accessing, validation, and setting */
static PyObject *
LRU_callback_getter(LRUDict *self, void *closure)
{
    (void)closure;

    if (self->callback == NULL) {
        Py_RETURN_NONE;
    }
    /* Return new ref */
    Py_INCREF(self->callback);
    return self->callback;
}


static inline int
lru_set_callback_impl(LRUDict *self, PyObject *value_obj)
{
    if (value_obj == NULL) {
        /* Delete the attribute itself, not supported */
        PyErr_SetString(PyExc_AttributeError,
                "can't delete callback; set it to None to disable");
        return -1;
    }
    if (value_obj == Py_None) {
        /* Assigning self.callback = None in Python causes callback member to
         * be set NULL. The (PyObject *)Py_None is never set as the value of
         * self->callback. */
        Py_XDECREF(self->callback);
        self->callback = NULL;
        return 0;
    }
    if (!PyCallable_Check(value_obj)) {
        PyErr_SetString(PyExc_TypeError, "callback object must be callable");
        return -1;
    }
    Py_INCREF(value_obj);       /* Own a ref to the Python callable obj */
    Py_XDECREF(self->callback);
    self->callback = value_obj;
    return 0;
}


static int
LRU_callback_setter(LRUDict *self, PyObject *value, void *closure)
{
    int status;

    (void)closure;

    LRU_ENTER_CRIT(self, -1);
    status = lru_set_callback_impl(self, value);
    LRU_LEAVE_CRIT(self);

    return status;
}


static PyObject *
LRU_set_callback_legacy(LRUDict *self, PyObject *args)
{
    PyObject *value;
    int status;

    if (!PyArg_ParseTuple(args, "O:set_callback", &value)) {
        return NULL;
    }

    LRU_ENTER_CRIT(self, NULL);
    status = lru_set_callback_impl(self, value);
    LRU_LEAVE_CRIT(self);

    if (status == -1) {
        return NULL;
    }
    Py_RETURN_NONE;
}


/* Container support (Python __contains__ or the "in" keyword) */
static int
lru_contains_impl(LRUDict *self, PyObject *key)
{
    return PyDict_Contains(self->dict, key);
}


static PyObject *
LRU_contains(LRUDict *self, PyObject *key)
{
    int flag;

    flag = lru_contains_impl(self, key);

    if (flag == 1) {
        Py_RETURN_TRUE;
    }
    else if (flag == 0) {
        Py_RETURN_FALSE;
    }
    else {
        return NULL;
    }
}


static PyObject *
LRU_has_key_legacy(LRUDict *self, PyObject *args)
{
    PyObject *key;
    if (!PyArg_ParseTuple(args, "O:has_key", &key)) {
        return NULL;
    }
    return LRU_contains(self, key);
}


/* Mapping interface (__getitem__, __setitem__, __delitem__ will wrap around
 * them) */
static inline PyObject *
lru_subscript_impl(LRUDict *self, PyObject *key)
{
    Node *node = GET_NODE(self->dict, key);
    if (!node) {
        self->misses++;
        return NULL;
    }

    /* We don't need to move the node when it's already self->first. */
    if (node != self->first) {
        lru_remove_node_impl(self, node);
        lru_add_node_at_head_impl(self, node);
    }

    self->hits++;
    Py_INCREF(node->value);
    Py_DECREF(node);
    return node->value;
}


static PyObject *
LRU_subscript(LRUDict *self, PyObject *key)
{
    PyObject *result;

    /* Subscripting changes the order of nodes, must protect. */
    LRU_ENTER_CRIT(self, NULL);
    result = lru_subscript_impl(self, key);
    LRU_LEAVE_CRIT(self);

    return result;
}


/* Pop node by key and key hash kh. Return error status.
 *
 * In the case of success (return value != -1), the output parameter node_ref
 * is pointer to the popped node. The popped node is detached from the queue,
 * and its prev/next pointers are invalid. The popped node is a borrowed
 * reference and can now be unboxed.
 *
 * In the case of failure, (return value == -1), the output parameter is
 * unusable, the queue is not modified, and the exception is set. */
static inline int
lru_popnode_impl(LRUDict *self, PyObject *key, Py_hash_t kh,
                 Node ** restrict node_ref)
{
    /* identify the node to pop by borrowing a ref by key-keyhash. if not, set
     * exception. */
    int res;
    *node_ref = (Node *)_PyDict_GetItem_KnownHash(self->dict, key, kh);
    if (*node_ref == NULL) {
        if (!PyErr_Occurred()) {
            _PyErr_SetKeyError(key);
        }
        return -1;
    }
    Py_INCREF(*node_ref);
    res = _PyDict_DelItem_KnownHash(self->dict, key, kh);
    if (res == 0) {
        /* If dict item-deletion succeed, detach from queue and keep this ref
         * for the output parameter. */
        lru_remove_node_impl(self, *node_ref);
    }
    else {
        /* If dict item-deletion fail, rewind the INCREF so there's no net
         * refcount change to node_ref. Exception is already set. */
        Py_DECREF(*node_ref);
    }
    return res;
}


/* Push key-value pair. Return error status.
 * If the error status != -1:
 *
 *  In the case of inserting new key, a new node is created, inserted, and
 *  pushed to the queue head. The output parameter oldvalue_ref is NULL.
 *
 *  In the case of replacing the value of old key, the node's ->value member is
 *  replaced and written to the output parameter oldvalue_ref (a borrowed ref,
 *  meaning that it's refcount is unchanged)
 *
 * If th error status == -1:
 *
 *  The exception is set. The output parameter is ununsable. */
static inline int
lru_push_impl(LRUDict *self, PyObject *key, PyObject *value, Py_hash_t kh,
              PyObject **oldvalue_ref)
{
    int res;
    Node *node_ref;

    /* Try borrowing a ref from dict */
    node_ref = (Node *)_PyDict_GetItem_KnownHash(self->dict, key, kh);

    if (node_ref == NULL) {
        if (PyErr_Occurred()) {
            return -1;
        }

        /* inserting new key */
        Node *val_node = PyObject_New(Node, &NodeType);
        if (val_node == NULL) {
            return -1;
        }
        /* populate new node; this INCREF the key and value. */
        NODE_INIT(val_node, key, value, kh);
        res = _PyDict_SetItem_KnownHash(self->dict,
                key, (PyObject *)val_node, kh);
        if (res == 0) {
            lru_add_node_at_head_impl(self, val_node);
            *oldvalue_ref = NULL;
        }
        if (lru_length_impl(self) > self->size) {
            lru_delete_last_impl(self);
        }
        /* No matter the dict SetItem succeed or not, our ref is now useless.
         * Notice that the DECREF will not trigger deallocation of key or
         * value: it will only restore their refcounts to the state before this
         * function call. */
        Py_DECREF(val_node);
    }
    else {
        /* replacing old value of key -- no need to create new node, just
         * do the switcheroo for the node's ->value pointer. The former value
         * is NOT DECREF'ed: it's refcount stays the same and it is now written
         * to the output parameter oldvalue_ref. */
        Py_INCREF(value);
        *oldvalue_ref = node_ref->value;
        node_ref->value = value;
        /* Promote node to head */
        lru_remove_node_impl(self, node_ref);
        lru_add_node_at_head_impl(self, node_ref);
        res = 0;
    }
    return res;
}


static int
LRU_ass_sub(LRUDict *self, PyObject *key, PyObject *value)
{
    int res;
    Py_hash_t kh;

    if ((kh = PyObject_Hash(key)) == -1) {
        return -1;
    }

    /* Assignment (write) method, must protect */
    if (value == NULL) {
        /* deletion */
        Node *popped_node;

        LRU_ENTER_CRIT(self, -1);
        res = lru_popnode_impl(self, key, kh, &popped_node);
        LRU_LEAVE_CRIT(self);
        if (res == 0) {
            Py_DECREF(popped_node);
        }
        return res;
    }
    else {
        /* insertion or replacement */
        PyObject *old_value;

        LRU_ENTER_CRIT(self, -1);
        res = lru_push_impl(self, key, value, kh, &old_value);
        LRU_LEAVE_CRIT(self);
        if (res == 0) {
            if (old_value != NULL) {
                Py_DECREF(old_value);
            }
            else {
                lru_purge_staging_impl(self, NO_FORCE_PURGE);
            }
        }
        return res;
    }
}


/* Sequence-like methods structure, supporting "key in <LRUDict object>" */
static PySequenceMethods LRU_as_sequence = {
    .sq_contains = (objobjproc)lru_contains_impl,
};


/* Mapping methods structure */
static PyMappingMethods LRU_as_mapping = {
    (lenfunc)LRU_length,        /*mp_length*/
    (binaryfunc)LRU_subscript,  /*mp_subscript*/
    (objobjargproc)LRU_ass_sub, /*mp_ass_subscript*/
};


/* Create lists for keys, values, or key-value pairs */
/* XXX: Legacy behaviour */
static PyObject *
collect(LRUDict *self, PyObject * (*getterfunc)(Node *))
{
    PyObject *v;
    Node *curr;
    int i;

    v = PyList_New(lru_length_impl(self));
    if (v == NULL) {
        return NULL;
    }

    curr = self->first;
    i = 0;
    while (curr) {
        PyList_SET_ITEM(v, i++, getterfunc(curr));
        curr = curr->next;
    }
    assert(i == lru_length_impl(self));
    return v;
}


static PyObject *
get_key(Node *node)
{
    Py_INCREF(node->key);
    return node->key;
}


static PyObject *
LRU_keys(LRUDict *self, PyObject *Py_UNUSED(ignored)) {
    PyObject *result;

    /* Only increfs the keys, not critical per se. */
    result = collect(self, get_key);
    return result;
}


static PyObject *
get_value(Node *node)
{
    Py_INCREF(node->value);
    return node->value;
}


static PyObject *
LRU_values(LRUDict *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *result;

    /* Only increfs the values, not critical per se. */
    result = collect(self, get_value);
    return result;
}


static PyObject *
get_item(Node *node)
{
    PyObject *tuple = PyTuple_New(2);
    Py_INCREF(node->key);
    PyTuple_SET_ITEM(tuple, 0, node->key);
    Py_INCREF(node->value);
    PyTuple_SET_ITEM(tuple, 1, node->value);
    return tuple;
}


static PyObject *
LRU_items(LRUDict *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *result;

    /* Only create new tuples, not critical per se. */
    result = collect(self, get_item);
    return result;
}


/* Dict-like methods */
static PyObject *
LRU_get(LRUDict *self, PyObject *args)
{
    PyObject *key;
    PyObject *default_obj = Py_None;
    PyObject *result;

    if (!PyArg_ParseTuple(args, "O|O:get", &key, &default_obj)) {
        return NULL;
    }

    /* Subscripting changes the order of nodes, must protect. */
    LRU_ENTER_CRIT(self, NULL);
    result = lru_subscript_impl(self, key);
    LRU_LEAVE_CRIT(self);

    if (result) {
        return result;
    }

    PyErr_Clear();  /* GET_NODE sets an exception on miss. Shut it up. */

    assert(default_obj != NULL);
    Py_INCREF(default_obj);
    return default_obj;
}


/* Fill a buffer of old values as the src dict is iterated over.
 * Return value:
 * 1: source not exhausted
 * 0: source exhausted
 * -1: error occurred */
static inline int
lru_update_fill_buffer(LRUDict *self, PyObject *src, Py_ssize_t * restrict pos,
                       PyObject ** restrict buf, ptrdiff_t len,
                       ptrdiff_t * restrict end)
{
    PyObject *key;
    PyObject *value;
    Py_hash_t kh;
    ptrdiff_t i;
    int ret_status;

    i = 0;
    ret_status = 1;
    while (i < len) {
        int push_status;

        PyObject ** restrict cur = buf + i;

        if (PyDict_Next(src, pos, &key, &value)) {
            if ((kh = PyObject_Hash(key)) == -1) {
                ret_status = -1;
                break;
            }
            push_status = lru_push_impl(self, key, value, kh, cur);
            if (push_status != 0) {
                ret_status = -1;
                break;
            }
            if ((*cur) != NULL) {
                i++;
            }
        }
        else {
            ret_status = 0;
            break;
        }
    }
    *end = i;
    return ret_status;
}


/* Update self with other in batches of n at most.
 * Each batch pass into the critical section leaves the buf filled with
 * batch_end old non-NULL values that are DECREF'ed outside the critical
 * section in one sweep. This goes on until other is exhausted, or a failure.
 * Return value: whether the return is caused by a failure. */
static inline _Bool
lru_update_with(LRUDict *self, PyObject *other, PyObject ** restrict buf,
                ptrdiff_t n)
{
    Py_ssize_t pos = 0;
    ptrdiff_t batch_end = 0;
    _Bool fail = 0;
    _Bool leave = 0;

    do {
        int status;

        if (self->detect_conflict && self->internal_busy) {
            PyErr_SetString(LRUDictExc_BusyErr,
                    "attempted entry into LRUDict critical section while busy");
            fail = 1;
            break;
        }

        self->internal_busy = 1;

        status = lru_update_fill_buffer(self, other, &pos, buf, n, &batch_end);

        if (status == -1) {
            fail = 1;
            leave = 1;
        }
        else if (status == 0) {
            leave = 1;
        }

        self->internal_busy = 0;
        for (ptrdiff_t j = 0; j < batch_end; j++) {
            Py_DECREF(buf[j]);
        }
    } while (!leave);

    return fail;
}


#define LRU_BATCH_MAX   128
static PyObject *
LRU_update(LRUDict *self, PyObject *args, PyObject *kwargs)
{
    PyObject **buf;
    Py_ssize_t buf_len;
    PyObject *other = NULL;
    PyObject *res;
    _Bool fail;

    if (!PyArg_ParseTuple(args,
                          "|O;update() takes at most one positional-only"
                          " parameter",
                          &other))
    {
        return NULL;
    }

    buf_len = LRU_BATCH_MAX >= self->size ? self->size : LRU_BATCH_MAX;
    if ((buf = malloc((size_t)(buf_len * sizeof(PyObject *)))) == NULL) {
        return PyErr_NoMemory();
    }

    if (other != NULL && PyDict_Check(other)) {
        fail = lru_update_with(self, other, buf, buf_len);
        if (fail) {
            res = NULL;
            goto cleanup;
        }
    }

    if (kwargs != NULL && PyDict_Check(kwargs)) {
        fail = lru_update_with(self, kwargs, buf, buf_len);
        if (fail) {
            res = NULL;
            goto cleanup;
        }
    }
    res = Py_None;
cleanup:
    free(buf);
    lru_purge_staging_impl(self, NO_FORCE_PURGE);
    Py_XINCREF(res);
    return res;
}


/* Like dict.setdefault, this evaluates the hash function only once.
 * Should test: refcount of key and return value */
static PyObject *
LRU_setdefault(LRUDict *self, PyObject *args)
{
    /* args to be parsed */
    PyObject *key = NULL;
    PyObject *default_obj = Py_None;
    Node *ret_node;
    PyObject *res;
    Py_hash_t kh;

    if (!PyArg_ParseTuple(args, "O|O:setdefault", &key, &default_obj)) {
        return NULL;
    }
    assert(key != NULL);
    assert(default_obj != NULL);
    if ((kh = PyObject_Hash(key)) == -1) {
        return NULL;
    }

    LRU_ENTER_CRIT(self, NULL);
    /* Try borrowing a ref by key */
    ret_node = (Node *)_PyDict_GetItem_KnownHash(self->dict, key, kh);
    if (ret_node == NULL) {
        /* Error or key not in */
        if (PyErr_Occurred()) { /* GetItem internal error */
            LRU_LEAVE_CRIT(self);
            return NULL;
        }
        /* key not in, this is not a miss, pack default_obj and insert */
        Node *default_node = PyObject_New(Node, &NodeType);
        if (default_node == NULL) {
            LRU_LEAVE_CRIT(self);
            return NULL;
        }

        int status;

        NODE_INIT(default_node, key, default_obj, kh);
        status = _PyDict_SetItem_KnownHash(self->dict,
                key, (PyObject *)default_node, kh);
        if (status == 0) {
            lru_add_node_at_head_impl(self, default_node);
            if (lru_length_impl(self) > self->size) {
                lru_delete_last_impl(self);
            }
            Py_INCREF(default_obj);
            res = default_obj;
        }
        else {
            res = NULL;
        }
        Py_DECREF(default_node);
    }
    else {    /* not (ret_node == NULL) */
        /* key is in, this is a hit */
        lru_remove_node_impl(self, ret_node);
        lru_add_node_at_head_impl(self, ret_node);
        self->hits++;
        Py_INCREF(ret_node->value);
        res = ret_node->value;
    }           /* end test if (ret_node == NULL) */
    LRU_LEAVE_CRIT(self);
    lru_purge_staging_impl(self, NO_FORCE_PURGE);

    return res;
}


static PyObject *
LRU_pop(LRUDict *self, PyObject *args)
{
    PyObject *key;
    PyObject *default_obj = NULL;
    PyObject *result;
    Node *ret_node;

    if (!PyArg_ParseTuple(args, "O|O:pop", &key, &default_obj)) {
        return NULL;
    }

    /* Assignment method, must protect */
    LRU_ENTER_CRIT(self, NULL);
    /* Trying to access the item by key. */
    ret_node = (Node *)_PyDict_Pop(self->dict, key, NULL);

    if (ret_node) {
        /* ret_node != NULL, delete it, unbox, and return value */
        self->hits++;
        lru_remove_node_impl(self, ret_node);
        Py_INCREF(ret_node->value);

        result = ret_node->value;

        LRU_LEAVE_CRIT(self);

        Py_DECREF(ret_node);
    }
    else {    /* ret_node == NULL, i.e. key missing */
        self->misses++;
        if (default_obj != NULL) {      /* default_obj given */
            PyErr_Clear();
            Py_INCREF(default_obj);
        }
        /* Otherwise (key missing, and default_obj not given [i.e. == NULL]),
         * the appropriate KeyError has already been set. */
        result = default_obj;

        LRU_LEAVE_CRIT(self);
    }

    return result;
}


static PyObject *
LRU_popitem(LRUDict *self, PyObject *args)
{
    int pop_least_recent = 0;
    PyObject *item_to_pop;      /* Python tuple of (key, value) */
    Node *node;

    if (!PyArg_ParseTuple(args, "|p:popitem", &pop_least_recent)) {
        return NULL;
    }

    /* Assignment method, must protect */
    LRU_ENTER_CRIT(self, NULL);

    node = pop_least_recent ? self->last : self->first;
    /* item_to_pop is new reference if not NULL */
    item_to_pop = node ? get_item(node) : NULL;
    if (item_to_pop == NULL) {
        PyErr_SetString(PyExc_KeyError, "popitem(): LRUDict is empty");
        LRU_LEAVE_CRIT(self);
        return NULL;
    }

    Py_INCREF(node);
    if (_PyDict_DelItem_KnownHash(self->dict, node->key, node->key_hash) == 0)
    {
        lru_remove_node_impl(self, node);
    }
    else {
        /* Somehow fails to delete from dict, item_to_pop becomes useless */
        Py_CLEAR(item_to_pop);
    }
    LRU_LEAVE_CRIT(self);
    Py_DECREF(node);

    return item_to_pop;
}


static PyObject *
LRU_clear(LRUDict *self, PyObject *Py_UNUSED(ignored))
{
    /* Write into almost everything in self */
    LRU_ENTER_CRIT(self, NULL);
    /* Optimization hack: just let nodes go out of lifecycle by PyDict_Clear()
     * (out of critical section; for fear of triggering the __del__ of objects
     * referenced by nodes in turn), and let dealloc handle them. We can re-set
     * self->first, self->last and don't have to delink one by one. */
    self->first = self->last = NULL;
    self->hits = 0;
    self->misses = 0;
    LRU_LEAVE_CRIT(self);

    PyDict_Clear(self->dict);   /* no return value (void) */

    Py_RETURN_NONE;
}


/* Methods specific to LRUDict */
static PyObject *
LRU_peek_first_item(LRUDict *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *result;

    /* "peek" doesn't change dict content or node order */
    if (self->first != NULL) {
        result = get_item(self->first); /* New reference */
        if (result == NULL) {
            return result;
        }
    }
    else {
        result = NULL;
        PyErr_SetString(PyExc_KeyError, "peek_first_item(): LRUDict is empty");
    }

    return result;
}


static PyObject *
LRU_peek_last_item(LRUDict *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *result;

    /* "peek" doesn't change dict content or node order */
    if (self->last != NULL) {
        result = get_item(self->last);  /* New reference */
        if (result == NULL) {
            return result;
        }
    }
    else {
        result = NULL;
        PyErr_SetString(PyExc_KeyError, "peek_last_item(): LRUDict is empty");
    }

    return result;
}


/* Hit/miss information */
#ifdef LRUDICT_STRUCT_SEQUENCE_NOT_BROKEN
static PyObject *
LRU_get_stats(LRUDict *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *n;
    PyObject *res = PyStructSequence_New(LRUDictStatsType);
    if (res == NULL) {
        goto fail;
    }

    if ((n = PyLong_FromUnsignedLong(self->hits)) != NULL) {
        PyStructSequence_SetItem(res, 0, n);
    }
    else {
        goto fail;
    }

    if ((n = PyLong_FromUnsignedLong(self->misses)) != NULL) {
        PyStructSequence_SetItem(res, 1, n);
    }
    else {
        goto fail;
    }

    return res;

fail:
    Py_XDECREF(res);
    return NULL;
}
#else /* LRUDICT_STRUCT_SEQUENCE_NOT_BROKEN */
static PyObject *
LRU_get_stats(LRUDict *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *res = Py_BuildValue("(kk)", self->hits, self->misses);
    return res;
}
#endif /* LRUDICT_STRUCT_SEQUENCE_NOT_BROKEN */


/* "Manual" purge once */
static PyObject *
LRU_purge(LRUDict *self, PyObject *Py_UNUSED(ignored))
{
    self->should_purge = 1;
    return PyLong_FromSsize_t(lru_purge_staging_impl(self, FORCE_PURGE));
}


#define MAP_BITFIELD(field, property)                   \
static PyObject *                                       \
LRU_##property##_getter(LRUDict *self, void *closure)   \
{(void)closure; if (self->field) {Py_RETURN_TRUE;} else {Py_RETURN_FALSE;}}\
static int                                              \
LRU_##property##_setter(LRUDict *self, PyObject *value, void *closure)  \
{   int v; (void)closure;                       \
    if (value == NULL) {                        \
        PyErr_SetString(PyExc_AttributeError, "can't delete "#property);\
        return -1;                              \
    }                                           \
    if ((v = PyObject_IsTrue(value)) == -1) {   \
        PyErr_SetString(PyExc_ValueError,       \
                #property" flag must evaluate to True or False");\
        return -1;                              \
    }                                           \
    self->field = v;                            \
    return 0;                                   \
}


/* debug property: disable purging */
MAP_BITFIELD(purge_suspended, _suspend_purge)
MAP_BITFIELD(detect_conflict, _detect_conflict)


static PyObject *
LRU__purge_queue_size_getter(LRUDict *self, void *closure)
{
    Py_ssize_t len = 0;
    (void)closure;
    if (self->staging_list) {
        len = PyList_Size(self->staging_list);
    }
    return PyLong_FromSsize_t(len);
}


/* Array of methods
 * Notice that just like Python's dict, the __contains__ and __getitem__
 * methods are explicitly added with METH_COEXIST, which makes them faster when
 * used directly as such. The __setitem__ and __delitem__ methods are not yet
 * directly put into the method-def array (same with Python dict). See
 * discussion in https://bugs.python.org/issue34396 */
static PyMethodDef LRU_methods[] = {
    {"__contains__",
        (PyCFunction)LRU_contains, METH_O | METH_COEXIST,
        PyDoc_STR("__contains__(self, key, /)\n--\n\n-> Bool\nCheck if key is in the LRUDict.")},
    {"__getitem__",
        (PyCFunction)LRU_subscript, METH_O|  METH_COEXIST,
        PyDoc_STR("__getitem__(self, key, /)\n--\n\nReturn the value associated with key or raise KeyError if key is not found.")},
    {"keys",
        (PyCFunction)LRU_keys, METH_NOARGS,
        PyDoc_STR("keys(self, /)\n--\n\n-> List\nReturn a list of the keys in MRU order.")},
    {"values",
        (PyCFunction)LRU_values, METH_NOARGS,
        PyDoc_STR("values(self, /)\n--\n\n-> List\nReturn a list of values in MRU order.")},
    {"items",
        (PyCFunction)LRU_items, METH_NOARGS,
        PyDoc_STR("items(self, /)\n--\n\n-> List[Tuple[Object, Object]]\nReturn a list of (key, value) pairs in MRU order.")},
    {"has_key",
        (PyCFunction)LRU_has_key_legacy, METH_VARARGS,
        PyDoc_STR("has_key(self, key, /)\n--\n\n-> Bool\nCheck if key is in the LRUDict.\n*Deprecated:* Use the ``in`` operator instead.")},
    {"get",
        (PyCFunction)LRU_get, METH_VARARGS,
        PyDoc_STR("get(self, key, default=None, /)\n--\n\n-> Object\nReturn the value for key if key is in the LRUDict; otherwise return default.")},
    {"setdefault",
        (PyCFunction)LRU_setdefault, METH_VARARGS,
        PyDoc_STR("setdefault(self, key, default=None, /)\n--\n\n-> Object\nIf key is not in the LRUDict, insert key with the value default.\n\nReturn the value associated with key if key is in the LRUDict; otherwise return default.")},
    {"pop",
        (PyCFunction)LRU_pop, METH_VARARGS,
        PyDoc_STR("pop(self, key[, default]) -> Object\nRemove the specific key and return its value.\n\nIf key is not in the LRUDict, return default if it is present as an argument, but raise KeyError if default is not present.\n\nNotice that like Python dict.pop, the argument \"default\" is positional-only but optional.")},
    {"popitem",
        (PyCFunction)LRU_popitem, METH_VARARGS,
        PyDoc_STR("popitem(least_recent=False, /)\n--\n\n-> Tuple[Object, Object]\nRemove and return a (key, value) pair. The pair returned is the least-recently used if least_recent is True, or the most-recently used if least_recent is False. By default, remove and return the most-recently used item.")},
    {"set_size",
        (PyCFunction)LRU_set_size_legacy, METH_VARARGS,
        PyDoc_STR("set_size(self, size, /)\n--\n\n-> None\nSet the size (capacity) of the LRUDict.\n\nIf the new size is less than the current length (number of members), the least-recently used elements are removed.\n*Deprecated:* Assign value to the ``size`` attribute instead.")},
    {"get_size",
        (PyCFunction)LRU_get_size_legacy, METH_NOARGS,
        PyDoc_STR("get_size(self, /)\n--\n\n-> int\nReturn the size (capacity) of the LRUDict.\n*Deprecated:* Access the ``size`` attribute instead.")},
    {"clear",
        (PyCFunction)LRU_clear, METH_NOARGS,
        PyDoc_STR("clear(self, /)\n--\n\n-> None\nClear the contents in the LRUDict and reset its hit/miss counters. The callback will not be called.")},
    {"get_stats",
        (PyCFunction)LRU_get_stats, METH_NOARGS,
        PyDoc_STR("get_stats(self, /)\n--\n\n-> Tuple[int, int]\nReturn a tuple of (hits, misses).\nNotice that hits/misses are represented as C ``unsigned long`` internally and may overflow.")},
    {"peek_first_item",
        (PyCFunction)LRU_peek_first_item, METH_NOARGS,
        PyDoc_STR("peek_first_item(self, /)\n--\n\n-> Tuple[Object, Object]\nReturn the MRU item as tuple (key, value) without changing the key order.")},
    {"peek_last_item",
        (PyCFunction)LRU_peek_last_item, METH_NOARGS,
        PyDoc_STR("peek_last_item(self, /)\n--\n\n-> Tuple[Object, Object]\nReturn the LRU item as tuple (key, value) without changing the key order.")},
    {"update",
        (PyCFunction)(void(*)(void))LRU_update, METH_VARARGS | METH_KEYWORDS,
        PyDoc_STR("update(self, other={}, /, **kwargs)\n--\n\n-> None\nUpdate the LRUDict using the key-value pairs from the dictionary \"other\" and the optional keyword arguments.\nThe update is performed in the iteration order of other, and after that, the kwargs order as specified. This process may cause eviction from the LRUDict.")},
    {"set_callback",
        (PyCFunction)LRU_set_callback_legacy, METH_VARARGS,
        PyDoc_STR("set_callback(self, callback, /)\n--\n\n-> None\nSet a callback to call when an item is evicted.\nThe callaback has the type Callable[[Object, Object], Any], i.e.,\n    callaback(key, value)\nRaise TypeError if callback is not a callable object that is not None. Setting callback to None disables the callback mechanism.\n*Deprecated:* Assign to the ``callback`` property instead.")},
    {"purge",
        (PyCFunction)LRU_purge, METH_NOARGS,
        PyDoc_STR("purge(self, /)\n--\n\n-> int\nReturn the number of items purged.\nManually purge the evicted items in the eviction queue for once. During the purge, more items may have been added to the eviction queue by another thread.")},
    {NULL, NULL, 0, NULL},              /* sentinel */
};


/* Array of properties */
static PyGetSetDef LRU_descriptors[] = {
    {"size",
        (getter)LRU_size_getter,
        (setter)LRU_size_setter,
        PyDoc_STR("Size (capacity) of the LRUDict. Setting this property re-sizes the LRUDict and may trigger eviction if the new size is less than the current length."),
        NULL},
    {"callback",
        (getter)LRU_callback_getter,
        (setter)LRU_callback_setter,
        PyDoc_STR("Callback object with the signature\n    callback(key, value)\nIf set to a callable, the (key, value) pair will be passed to it after evicted from the LRUDict. If set to None, disable the callback mechanism. Setting it to a non-callable object that is not None raises TypeError."),
        NULL},
    {"_suspend_purge",
        (getter)LRU__suspend_purge_getter,
        (setter)LRU__suspend_purge_setter,
        PyDoc_STR("Boolean value indicating whether automatic purging should be suspended. Setting this flag to True does not automatically trigger purging even if the purge queue is not empty. For private use only."),
        NULL},
    {"_detect_conflict",
        (getter)LRU__detect_conflict_getter,
        (setter)LRU__detect_conflict_setter,
        PyDoc_STR("Boolean value indicating whether contented method call should be detected at runtime."),
        NULL},
    {"_purge_queue_size",
        (getter)LRU__purge_queue_size_getter,
        NULL,
        PyDoc_STR("Length of the internal purge-queue."),
        NULL},
    {NULL, NULL, NULL, NULL, NULL},     /* sentinel */
};


#define GETREPR_TRY_EXCEPT(namevar, callexpr, checkexpr, action_statement)  \
do {                              \
    (namevar) = (callexpr);       \
    if ((namevar) != NULL) {      \
        if ((checkexpr)) {        \
            action_statement;     \
        }                         \
    }                             \
    else if (PyErr_Occurred()) {  \
        PyErr_Clear();            \
    }                             \
} while (0)


static PyObject *
lru_cbname(PyObject *cb)
{
    PyObject *cb_name;

    GETREPR_TRY_EXCEPT(cb_name,
                       PyObject_GetAttrString(cb, "__name__"),
                       PyUnicode_Check(cb_name),
                       return cb_name);
    GETREPR_TRY_EXCEPT(cb_name,
                       PyUnicode_FromFormat("%R", cb),
                       1,
                       return cb_name);
    GETREPR_TRY_EXCEPT(cb_name,
                       PyUnicode_FromFormat("<object at %p>", cb),
                       1,
                       return cb_name);

    return cb_name;
}


static PyObject *
lru_cbrepr(PyObject *cb)
{
    PyObject *cb_name;
    PyObject *cb_repr;

    /* if no callback, return empty string without the ", callback=" text */
    if (cb == NULL) {
        return PyUnicode_FromString("");
    }

    /* otherwise the text reads ", callback=<something>" */
    cb_name = lru_cbname(cb);   /* new ref */
    GETREPR_TRY_EXCEPT(cb_repr,
                       PyUnicode_FromFormat(", callback=%V",
                                            cb_name, "<unknown>"),
                       1,
                       (void)0);
    Py_XDECREF(cb_name);
    return cb_repr;
}


/* __repr__ or repr() support */
static PyObject *
LRU_repr(LRUDict *self)
{
    PyObject *cb_repr;
    PyObject *dict_repr;
    PyObject *self_repr;
    /* repr of dict doesn't have to be very long, it's not like you can
     * literally eval the repr of self anyway */
    if (self->dict == NULL) {
        dict_repr = PyUnicode_FromString("<error>");
    }
    else {
        GETREPR_TRY_EXCEPT(dict_repr,
                           PyUnicode_FromFormat("%R", self->dict),
                           1,
                           (void)0);
    }
    /* Hard-coded length limit */
    if (dict_repr && PyUnicode_GetLength(dict_repr) > 128) {
        Py_DECREF(dict_repr);
        dict_repr = PyUnicode_FromString("{...}");
    }

    cb_repr = lru_cbrepr(self->callback);
    self_repr = PyUnicode_FromFormat(
            "<LRUDict(%zd%V) object with dict %V at %p>",
            self->size,
            cb_repr, ", callback=<error formatting repr>",
            dict_repr, "<error formatting repr>",
            self);
    Py_XDECREF(cb_repr);
    Py_XDECREF(dict_repr);
    return self_repr;
}


/* __init__ */
static int
LRU_init(LRUDict *self, PyObject *args, PyObject *kwds)
{
    Py_ssize_t initial_size = 0;
    static char *kwlist[] = {"size", "callback", NULL};
    PyObject *callback = Py_None;

    self->internal_busy = 0;

    /* Allocate resoures */
    if ((self->dict = PyDict_New()) == NULL) {
        PyErr_SetString(PyExc_MemoryError, "internal dict allocation failure");
        return -1;
    }

    if ((self->staging_list = PyList_New(0)) == NULL) {
        PyErr_SetString(PyExc_MemoryError,
                "internal staging list allocation failure");
        return -1;
    }

    /* Modify own structure member values */
    if (!PyArg_ParseTupleAndKeywords(args, kwds,
                                     "n|O:__init__",
                                     kwlist, &initial_size, &callback))
    {
        return -1;
    }

    if (lru_set_size_impl(self, initial_size) == -1) {
        return -1;
    }

    if (lru_set_callback_impl(self, callback) == -1) {
        return -1;
    }

    self->first = self->last = NULL;
    self->hits = 0;
    self->misses = 0;
    self->should_purge = 0;
    self->purge_busy = 0;
    self->purge_suspended = 0;
    self->detect_conflict = 1;
    return 0;
}


/* NOTE: Argument names should not change because the function uses Py_VISIT
 * macro.
 *
 * The GC bypasses the Node object because there's no need to track a large
 * number of small Node objects. A Node can only be created by us and it can
 * only be found as values in self->dict or items in self->staging_list. When
 * doing the traverse we already know where to visit, and we directly visit the
 * Node's Python-object members (which it owns). */
static int
LRU_traverse(LRUDict *self, visitproc visit, void *arg)
{
    Node *cur = self->last;

    while (cur) {
        Py_VISIT(cur->value);
        Py_VISIT(cur->key);
        cur = cur->prev;
    }

    if (self->staging_list) {
        Py_ssize_t i;
        Py_ssize_t len = PyList_Size(self->staging_list);
        for (i = 0; i < len; i++) {
            cur = (Node *)PyList_GET_ITEM(self->staging_list, i);
            Py_VISIT(cur->value);
            Py_VISIT(cur->key);
        }
    }

    if (self->callback) {
        Py_VISIT(self->callback);
    }
    return 0;
}


/* tp_clear slot function for breaking refcount cycles. NOT to be confused with
 * LRU_clear */
static int
LRU_tp_clear(LRUDict *self)
{
    self->internal_busy = 0;
    self->purge_busy = 0;
    /* Release storage (and all nodes in it) */
    if (self->dict) {
        /* Will NOT call callback on any staging elems. */
        LRU_clear(self, NULL);
        Py_CLEAR(self->dict);
    }
    /* Dispose of reference to callback */
    Py_CLEAR(self->callback);
    /* Set purge flag, trigger no-callback purge (decref all list elements) */
    if (self->staging_list) {
        self->should_purge = 1;
        lru_purge_staging_impl(self, FORCE_PURGE);
        /* Release purge staging list */
        Py_CLEAR(self->staging_list);
    }
    return 0;
}


/* Deallocation, when the refcount to self reaches zero */
static void
LRU_dealloc(LRUDict *self)
{
    PyObject_GC_UnTrack((PyObject *)self);

    LRU_tp_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}


PyDoc_STRVAR(lru_doc,
"LRUDict(size, callback=None) -> new LRUDict that can store up to ``size``\n"
"elements\n\n"
"An LRUDict behaves like a Python ``dict``, except that it stores only a\n"
"fixed number of key-value pairs. Once the number of stored elements goes\n"
"beyond the capacity, it evicts the least-recently used items. If a callback\n"
"is set, it will be called with the evicted key and item and they exit the\n"
"LRUDict.\n\n"
"Eg:\n"
">>> r = LRUDict(3)\n"
">>> for i in range(5):\n"
">>>     r[i] = str(i)\n"
">>> r.keys()\n"
"[4, 3, 2]\n\n"
"Note: An LRUDict(n) can be thought of as a dict that will have the most\n"
"recently accessed n items.\n");


/* Type structure */
static PyTypeObject LRUDictType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "lru_ng.LRUDict",
    .tp_basicsize = sizeof(LRUDict),
    .tp_dealloc = (destructor)LRU_dealloc,
    .tp_repr = (reprfunc)LRU_repr,
    .tp_traverse = (traverseproc)LRU_traverse,
    .tp_clear = (inquiry)LRU_tp_clear,
    .tp_as_sequence = &LRU_as_sequence,
    .tp_as_mapping = &LRU_as_mapping,
    .tp_hash = PyObject_HashNotImplemented,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_doc = lru_doc,
    .tp_methods = LRU_methods,
    .tp_getset = LRU_descriptors,
    .tp_init = (initproc)LRU_init,
    .tp_new = PyType_GenericNew,
};


/* Module structure */
static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    .m_name = "lru_ng",
    .m_doc = lru_doc,
    .m_size = -1,
};


static PyObject *
moduleinit(void)
{
    PyObject *m;

    /* Pull in the heap-allocated types */
    if (PyType_Ready(&NodeType) < 0) {
        return NULL;
    }
    if (PyType_Ready(&LRUDictType) < 0) {
        return NULL;
    }
    /* Create new exception */
    LRUDictExc_BusyErr = PyErr_NewExceptionWithDoc(
            "lru_ng.LRUDictBusyError",
            ("Exception indicating an LRUDict method cannot begin operation"
            " because another method has not finished"),
            PyExc_RuntimeError,
            NULL);
    if (LRUDictExc_BusyErr == NULL) {
        return NULL;
    }
    /* Create new namedtuple for stats information */
#ifdef LRUDICT_STRUCT_SEQUENCE_NOT_BROKEN
    LRUDictStatsType = PyStructSequence_NewType(&LRUDict_stats_desc);
    if (LRUDictStatsType == NULL) {
        return NULL;
    }
#endif

    /* Create module object */
    if ((m = PyModule_Create(&moduledef)) == NULL) {
        return NULL;
    }

    /* Make types available to module, or fail and cleanup */
    Py_INCREF(&NodeType);
    Py_INCREF(&LRUDictType);
#ifdef LRUDICT_STRUCT_SEQUENCE_NOT_BROKEN
    Py_INCREF(LRUDictStatsType);
#endif
    if (PyModule_AddObject(m, "LRUDict", (PyObject *)(&LRUDictType)) < 0 ||
        PyModule_AddObject(m, "LRUDictBusyError", LRUDictExc_BusyErr) < 0)
    {
        Py_DECREF(&LRUDictType);
        Py_DECREF(&NodeType);
#ifdef LRUDICT_STRUCT_SEQUENCE_NOT_BROKEN
        Py_DECREF(LRUDictStatsType);
#endif
        Py_DECREF(m);
        m = NULL;
    }

    return m;
}


PyMODINIT_FUNC
PyInit_lru_ng(void)
{
    return moduleinit();
}
