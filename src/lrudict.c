#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include <assert.h>
#include "lrudict.h"
#include "lrudict_pq.h"
#include "lrudict_exctype.h"
#include "lrudict_statstype.h"
#ifdef __GNUC__
__attribute__((malloc))
extern PyObject * _PyObject_New(PyTypeObject *);

__attribute__((malloc))
extern void * PyMem_Malloc(size_t);
#endif

#include "tinyset.c"
static TinySet *lru_safe_types;


/*
 * This is an implementation of LRUDict that uses a Python dict and an
 * associated circular doubly linked list to keep track of recently
 * inserted/accessed items.
 *
 * Dict will store: key -> Node mapping, where Node is a linked list node.
 * The Node itself will contain the value as well as the key.
 *
 * A special node, self->root, is allocated with unique identity. It is never
 * deallocated throughout the lifetime of self, and it is never added to the
 * dict. Consequently, any node obtained from the dict cannot be the root node.
 * The linked list begins from the root node and wraps around to it. If there
 * are no additional nodes, the root node's prev and next pointers point to
 * itself. This enables mostly branchless code with linked-list operations by
 * eliminating a class of comparisons before dereferencing.
 */


static void
node_dealloc(Node *self)
{
    Py_DECREF(self->pl.key);
    Py_DECREF(self->pl.value);
    Py_TYPE(self)->tp_free((PyObject *)self);
}


static PyObject *
node_repr(Node *self)
{
    return PyObject_Repr(self->pl.value);
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


/* Return new ref to newly created node initialized with payload, or NULL
 * in case of failure to create node at all. */
static inline Node *
node_getnewfrom(const NodePayload *restrict payload)
{
    Node *n;

    if ((n = PyObject_New(Node, &NodeType)) != NULL) {
        Py_INCREF(payload->key);
        Py_INCREF(payload->value);
        /* Note: direct copy of struct via (indirect) assignment. */
        n->pl = *payload;
    }
    return n;
}


/* LRUDict internal critical section macros. These sections must be entered
 * with the Python GIL held. This is normally satisfied if the entrance/exit
 * sequence is only used in Python-facing methods and nowhere else */
#define LRU_ENTER_CRIT(self, failresult)    \
do {                                        \
    if ((self)->detect_conflict && (self)->internal_busy) \
    {  \
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

#define PURGE_MAYBE_FAIL(self)  \
    (unlikely(lru_purge_staging_impl((self), NO_FORCE_PURGE) == -2))


/* Linked-list data-structure implementations internal to LRUDict */
#define FIRST_NODE(s)        ((s)->root->next)
#define LAST_NODE(s)         ((s)->root->prev)
#define IS_VALID_NODE_IN(s, n)  ((n) != (s)->root)


/* Generic node-detach; node must already be a member and not root. After
 * detach the node's link pointers contain garabage. */
static inline void
lru_detach_node(const Node *restrict node)
{
    /* These two may point to the same thing but never "node" itself */
    Node *n_next = node->next;
    Node *n_prev = node->prev;
    assert(n_next != node);
    assert(n_prev != node);

    n_next->prev = n_prev;
    n_prev->next = n_next;
}


/* Generic attach (at head/first); node must be a non-aliased, well-formed Node
 * object that's not a member. */
static inline void
lru_attach_node_after(Node *restrict origin, Node *restrict node)
{
    Node *o_next = origin->next;  /* this thing may be the same as origin */
    assert(node != origin);
    assert(node != o_next);

    o_next->prev = node;
    node->next = o_next;
    node->prev = origin;
    origin->next = node;
}


/* Promote node to first. node must already be a member. (Semantically
 * equivalent to a detach followed by an attach; this uses a comparison to skip
 * it entirely if node is already first.) */
static inline void
lru_promote_node(const LRUDict *self, Node *node)
{
    if (node == FIRST_NODE(self)) {
        return;
    }
    lru_detach_node(node);
    lru_attach_node_after(self->root, node);
}


/* There's no way to compute whether an object is "DECREF-safe". We try to give
 * an conservative estimate: Some built-in, atom-like objects are safe because
 * they don't reference other Python objects and their deallocators are
 * in-built functions that don't manipulate other objects. This allows us to
 * bypass the push-to-staging and (X)DECREF them. */
static inline _Bool
lru_decref_unsafe(const PyObject *obj)
{
    return !ts_has_elem(lru_safe_types, (const void *)Py_TYPE(obj));
}


static inline int
lrupq_push(LRUDict_pq *q, Node *restrict n)
{
    if (PyList_Append(q->lst, (PyObject *restrict)n) == 0) {
        q->sinfo.tail++;
        return 0;
    }
    else {
        return -1;
    }
}


/* Can only be called while there's actually a node to delete (evict) */
static void
lru_delete_last_impl(LRUDict *self)
{
    Node *n = LAST_NODE(self);
    assert(IS_VALID_NODE_IN(self, n));

    /* Transfer the node to purge queue.
     * This transfer is not merely conditional on self->callback being set or
     * not. Not having a callback doesn't mean we can safely DECREF the node
     * here, because as we DECREF the last reference to the node, it's possible
     * to trigger arbitrary code in the Node's key or value's __del__.*/
    Py_INCREF(n);
    if (_PyDict_DelItem_KnownHash(self->dict, n->pl.key, n->pl.key_hash) == 0)
    {
        /* detach; n is never root because the only item cannot be evicted. */
        lru_detach_node(n);
        /* The list will increase the refcount to the node if successful */
        if (self->callback ||
            (lru_decref_unsafe(n->pl.key) | lru_decref_unsafe(n->pl.value)))
        {
            if (lrupq_push(self->purge_queue, n) == 0) {
                self->_pb = 1;
            }
        }
    }
    /* This DECREF in the case when the list append isn't succesful (a rare
     * condition) is the last resort, but in normal condition it simply mean
     * the reference is transfered to the list. */
    Py_DECREF(n);
}


/* Purging mechanism. */
static inline Py_ssize_t
lru_purge_staging_impl(LRUDict *self, purge_mode_t opt)
{
    Py_ssize_t res;

    if (self->purge_suspended && opt != FORCE_PURGE) {
        return 0;
    }

    if (self->_pb == 0) {
        return 0;
    }

    res = lrupq_purge(self->purge_queue, self->callback);
    if (res != 0) {
        self->_pb = 0;
    }

    return res;
}


/* Querying length information (Python __len__ or len() function) */
static inline Py_ssize_t
lru_length_impl(const LRUDict *self)
{
    return PyDict_GET_SIZE(self->dict);
}


static Py_ssize_t
LRU_length(LRUDict *self)
{
    return lru_length_impl(self);
}


/* Size (capacity) property access, validation, and setting (re-sizing) */
static PyObject *
LRU_size_getter(LRUDict *self, void *Py_UNUSED(closure))
{
    return PyLong_FromSsize_t(self->size);
}


static PyObject *
LRU_get_size_legacy(LRUDict *self, PyObject *Py_UNUSED(ignored))
{
    return LRU_size_getter(self, NULL);
}


static inline int
lru_set_size_impl(LRUDict *self, Py_ssize_t n)
{
    if (n > 0) {
        self->size = n;
        for (Py_ssize_t i = lru_length_impl(self) - n; i > 0; i--) {
            lru_delete_last_impl(self);
        }
        return 0;
    }
    else {
        PyErr_SetString(PyExc_ValueError, "size must be positive");
        return -1;
    }
}


/* Descriptor setter function for size */
static int
LRU_size_setter(LRUDict *self, PyObject *value, void *Py_UNUSED(closure))
{
    int status;
    Py_ssize_t newsize;

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

    if (PURGE_MAYBE_FAIL(self)) {
        return -1;
    }

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

    if (PURGE_MAYBE_FAIL(self)) {
        return NULL;
    }

    if (status == -1) {
        return NULL;
    }
    Py_RETURN_NONE;
}


/* Callback property: accessing, validation, and setting */
static PyObject *
LRU_callback_getter(LRUDict *self, void *Py_UNUSED(closure))
{
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
LRU_callback_setter(LRUDict *self, PyObject *value, void *Py_UNUSED(closure))
{
    int status;

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
/* Some building blocks below. lru_hit_impl always return new reference of
 * value (this saves some duplicate lines in the current implementation) */
static inline PyObject *
lru_hit_impl(LRUDict *self, Node *node)
{
    lru_promote_node(self, node);
    self->hits++;
    Py_INCREF(node->pl.value);
    return node->pl.value;
}


/* Optimized hash getter code-path that uses the memoized hash for strings. See
 * CPython: Objects/dictobject.c */
static inline Py_hash_t
get_hash(PyObject *k)
{
    Py_hash_t hash;

    if (!PyUnicode_CheckExact(k) || (hash = ((PyASCIIObject *)k)->hash) == -1)
    {
        hash = PyObject_Hash(k);
    }
    return hash;
}


static inline Py_ssize_t
direct_lookup(PyObject *restrict d, PyObject *restrict key, Py_hash_t kh,
              Node **restrict node_ref)
{
    PyDictObject *mp = (PyDictObject *)d;
#if PY_VERSION_HEX >= 0x03070000
    return (mp->ma_keys->dk_lookup)(mp, key, kh, (PyObject **)node_ref);
#else
    PyObject **vaddr;
    Py_ssize_t index;
    index = (mp->ma_keys->dk_lookup)(mp, key, kh, &vaddr, NULL);
    if (index >= 0 && vaddr) {
        *node_ref = (Node *)(*vaddr);
    }
    else {
        *node_ref = NULL;
    }
    return index;
#endif
}


/* Always write to output parameter "value" borrowed reference or NULL. */
static inline int
lru_subscript_impl(LRUDict *self, PyObject *key, PyObject **value)
{
    Node *n;
    Py_hash_t kh;
    Py_ssize_t index;

    if (unlikely((kh = get_hash(key)) == -1)) {
        goto fail;
    }

    index = direct_lookup(self->dict, key, kh, &n);

    if (unlikely(index == DKIX_ERROR)) {
        goto fail;
    }

    if (index < 0) {
        self->misses++;
        *value = NULL;
    }
    else {
        /* The "overt" dict is never a split table, hence index >= 0 implies
         * that n != NULL, hence can be dereferenced. */
        assert(n != NULL);
        *value = lru_hit_impl(self, n);
    }
    return 0;

fail:
    *value = NULL;
    return -1;
}


static PyObject *
LRU_subscript(LRUDict *self, PyObject *key)
{
    PyObject *value;
    int status;

    /* Subscripting changes the order of nodes, must protect. */
    LRU_ENTER_CRIT(self, NULL);
    status = lru_subscript_impl(self, key, &value);
    LRU_LEAVE_CRIT(self);

    if (status == 0 && value == NULL) {
        _PyErr_SetKeyError(key);
    }
    /* unified return: if error, value already NULL'ed. */
    return value;
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
lru_popnode_impl(LRUDict *self, PyObject *key, Py_hash_t kh, Node **node_ref)
{
    /* Identify the node to pop by borrowing a ref by key-keyhash. if not, set
     * exception. */
    int res;
    Py_ssize_t index;

    index = direct_lookup(self->dict, key, kh, node_ref);

    if (unlikely(index == DKIX_ERROR)) {
        return -1;
    }

    if (index < 0) {
        _PyErr_SetKeyError(key);
        return -1;
    }

    assert(node_ref != NULL);
    Py_INCREF(*node_ref);
    res = _PyDict_DelItem_KnownHash(self->dict, key, kh);
    if (res == 0) {
        /* If dict item-deletion succeed, detach from queue and keep this ref
         * for the output parameter. */
        lru_detach_node(*node_ref);
    }
    else {
        /* If dict item-deletion fail, rewind the INCREF so there's no net
         * refcount change to node_ref. Exception is already set. */
        Py_DECREF(*node_ref);
    }
    return res;
}


/* Insert a (well-formed, already-allocated, not-aliased-to-existing) Node
 * object. */
static inline int
lru_insert_new_node_impl(LRUDict *self, Node *restrict node)
{
    int res;

    res = _PyDict_SetItem_KnownHash(self->dict,
                                    node->pl.key,
                                    (PyObject *restrict)node,
                                    node->pl.key_hash);
    if (res == 0) {
        lru_attach_node_after(self->root, node);
    }

    if (lru_length_impl(self) > self->size) {
        lru_delete_last_impl(self);
    }

    return res;
}


/* Push key-value pair. Return error status.
 * If the error status != -1:
 *
 *  In the case of inserting new key, a new node is created, inserted, and
 *  pushed to the queue head. The output parameter oldvalue_ref is NULL.
 *
 *  In the case of replacing the value of old key, the node payload's value
 *  member is replaced and written to the output parameter oldvalue_ref (a
 *  borrowed ref, meaning that it's refcount is unchanged). The refcount of
 *  value is INCREF'ed (it is not "stolen").
 *
 * If th error status == -1:
 *
 *  The exception is set. The output parameter is ununsable. */
static inline int
lru_push_impl(LRUDict *self, const NodePayload *restrict payload,
              PyObject **oldvalue_ref)
{
    int res;
    Node *n;
    Py_ssize_t index;

    /* Try borrowing a ref from dict */
    index = direct_lookup(self->dict, payload->key, payload->key_hash, &n);

    if (n == NULL) {
        if (unlikely(index == DKIX_ERROR)) {
            return -1;
        }

        /* inserting new key */
        if (unlikely((n = node_getnewfrom(payload)) == NULL)) {
            return -1;
        }

        res = lru_insert_new_node_impl(self, n);
        if (res == 0) {
            *oldvalue_ref = NULL;
        }
        /* No matter the dict SetItem succeed or not, our ref is now useless.
         * Notice that the DECREF will not trigger deallocation of key or
         * value: it will only restore their refcounts to the state before this
         * function call. */
        Py_DECREF(n);
    }
    else {
        /* replacing old value of key -- no need to create new node, just
         * do the switcheroo for the node payload's value member. The former
         * value is NOT DECREF'ed at this point: its refcount stays the same
         * and it is now written to the output parameter oldvalue_ref. The
         * DECREF is expected to be done by the caller after leaving the
         * critical section. */
        Py_INCREF(payload->value);
        /* XXX: Is it worth it to skip replacing the identical value? */
        *oldvalue_ref = n->pl.value;
        n->pl.value = payload->value;
        /* Promote node to first. */
        lru_promote_node(self, n);
        res = 0;
    }
    return res;
}


static int
LRU_ass_sub(LRUDict *self, PyObject *key, PyObject *value)
{
    int res;
    Py_hash_t kh;

    if (unlikely((kh = get_hash(key)) == -1)) {
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
            assert(popped_node != NULL);
            Py_DECREF(popped_node);
        }
        return res;
    }
    else {
        /* insertion or replacement */
        PyObject *old_value;
        NodePayload pl = {key, value, kh};

        LRU_ENTER_CRIT(self, -1);
        res = lru_push_impl(self, &pl, &old_value);
        LRU_LEAVE_CRIT(self);
        if (res == 0) {
            if (old_value == NULL) {
                /* Inserted value */
                if (PURGE_MAYBE_FAIL(self)) {
                    res = -1;
                }
            }
            else {
                /* Replaced old_value */
                Py_DECREF(old_value);
            }
        }  /* test whether push result "meaningful"; fall through if not */
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
static PyObject *
collect(LRUDict *self, PyObject * (*getterfunc)(const Node *restrict))
{
    PyObject *v;
    const Node *curr;
    const Py_ssize_t len = lru_length_impl(self);

    v = PyList_New(len);
    if (v == NULL) {
        return NULL;
    }

    curr = FIRST_NODE(self);
    Py_ssize_t i = 0;
    while (IS_VALID_NODE_IN(self, curr)) {
        PyObject * obj;

        if ((obj = getterfunc(curr)) != NULL) {
            PyList_SET_ITEM(v, i++, obj);
            curr = curr->next;
        }
        else {
            goto fail;
        }
    }
    assert(i == len);
    return v;

fail:
    Py_DECREF(v);
    return NULL;
}


static PyObject *
get_key(const Node *restrict node)
{
    Py_INCREF(node->pl.key);
    return node->pl.key;
}


static PyObject *
LRU_keys(LRUDict *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *result;

    LRU_ENTER_CRIT(self, NULL);
    result = collect(self, get_key);
    LRU_LEAVE_CRIT(self);
    return result;
}


static PyObject *
get_value(const Node *restrict node)
{
    Py_INCREF(node->pl.value);
    return node->pl.value;
}


static PyObject *
LRU_values(LRUDict *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *result;

    LRU_ENTER_CRIT(self, NULL);
    result = collect(self, get_value);
    LRU_LEAVE_CRIT(self);
    return result;
}


static PyObject *
get_item(const Node *restrict node)
{
    PyObject *tuple = PyTuple_New(2);
    Py_INCREF(node->pl.key);
    Py_INCREF(node->pl.value);
    PyTuple_SET_ITEM(tuple, 0, node->pl.key);
    PyTuple_SET_ITEM(tuple, 1, node->pl.value);
    return tuple;
}


static PyObject *
LRU_items(LRUDict *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *result;

    LRU_ENTER_CRIT(self, NULL);
    result = collect(self, get_item);
    LRU_LEAVE_CRIT(self);
    return result;
}


/* Dict-like methods */
static PyObject *
LRU_get(LRUDict *self, PyObject *args)
{
    PyObject *key;
    PyObject *default_obj = Py_None;
    PyObject *result;
    int status;

    if (!PyArg_ParseTuple(args, "O|O:get", &key, &default_obj)) {
        return NULL;
    }
    assert(key != NULL);
    assert(default_obj != NULL);

    /* Subscripting changes the order of nodes, must protect. */
    LRU_ENTER_CRIT(self, NULL);
    status = lru_subscript_impl(self, key, &result);
    LRU_LEAVE_CRIT(self);

    if (status == 0) {
        return result ? result : (Py_INCREF(default_obj), default_obj);
    }
    else {
        /* error */
        assert(result == NULL);
        return result;
    }
}


typedef struct _LRUUpdateBuf {
    PyObject **const restrict buf;
    const size_t len;
    size_t n_written;
    Py_ssize_t pos;
} update_buf_t;


/* Fill at most one buffer of replaced values from self->dict as the src dict
 * is iterated over while updating self->dict.
 *
 * Return value:
 * 1: source not exhausted
 * 0: source exhausted
 * -1: error occurred */
static inline int
lru_update_fill_buffer(LRUDict *self, PyObject *src,
                       update_buf_t *restrict updbuf)
{
    PyObject *key;
    PyObject *value;
    Py_hash_t kh;
    size_t i;
    int ret_status;

    i = 0;
    ret_status = 1;
    while (i < updbuf->len) {
        int push_status;

        PyObject **restrict cur = updbuf->buf + i;

        if (PyDict_Next(src, &updbuf->pos, &key, &value)) {
            if (unlikely((kh = get_hash(key)) == -1)) {
                ret_status = -1;
                break;
            }

            NodePayload pl = {key, value, kh};
            push_status = lru_push_impl(self, &pl, cur);

            if (unlikely(push_status != 0)) {
                ret_status = -1;
                break;
            }

            if ((*cur) != NULL) {
                /* Only advance the position in buffer if the value written
                 * is not NULL */
                i++;
            }
        }
        else {
            /* src exhausted */
            ret_status = 0;
            break;
        }
    }
    updbuf->n_written = i;
    return ret_status;
}


/* Update self with other in batches of n at most.
 * Each batch pass into the critical section leaves the buffer filled with
 * updbuf->n_written old non-NULL values that are DECREF'ed outside the
 * critical section in one sweep. This goes on until other is exhausted, or a
 * failure.
 * Return value: whether the return is caused by a failure. */
static inline _Bool
lru_update_with(LRUDict *self, PyObject *other, update_buf_t *restrict updbuf)
{
    _Bool fail = 0;
    _Bool leave = 0;

    updbuf->n_written = 0;
    updbuf->pos = 0;
    do {
        int status;

        if (self->detect_conflict && self->internal_busy)
        {
            PyErr_SetString(LRUDictExc_BusyErr,
                            "attempted entry into LRUDict critical section"
                            " while busy");
            fail = 1;
            break;
        }

        self->internal_busy = 1;

        status = lru_update_fill_buffer(self, other, updbuf);

        if (status == 0) {
            leave = 1;
        } else if (status == -1) {
            fail = 1;
            leave= 1;
        }

        self->internal_busy = 0;

        /* Sweep the buffer for this pass. */
        for (size_t j = 0; j < updbuf->n_written; j++) {
            Py_DECREF(updbuf->buf[j]);
        }
    } while (!leave);

    return fail;
}


/* Like dict.update(): perform update of self.
 * This operation cannot be both safely and efficiently done in one single pass
 * if a) we require that all potentially __del__-triggering code be executed
 * outside the identified critical section, and b) we are big. Instead we make
 * multiple passes, each pass filling a buffer of replaced old values from
 * self. After each pass and before the next one (if necessary), the buffer's
 * contents are DECREF'ed.  The buffer doesn't have to be a large one: if we
 * ourselves have a large capacity, a single-pass operation would have required
 * as large a buffer, because each existing value could have been subject to
 * replacement.
 *
 * XXX: Idea for improvement with memory use: the purge (eviction) list could
 * grow as big as the difference (their_length - our_capacity). This will make
 * eviction more time-efficent but potentially very memory-consuming. We could
 * also purge as we leave each pass, but that won't help much because a huge
 * source will not fill the replacement buffer as it pushes a huge amount of
 * elements to the list.
 */
#define LRU_BATCH_MAX   64
static PyObject *
LRU_update(LRUDict *self, PyObject *args, PyObject *kwargs)
{
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

    update_buf_t updbuf = {
        .len = LRU_BATCH_MAX,
        .buf = PyMem_Malloc(LRU_BATCH_MAX * sizeof(PyObject *)),
    };
    if (unlikely(updbuf.buf == NULL)) {
        return PyErr_NoMemory();
    }

    if (other != NULL && PyDict_Check(other)) {
        fail = lru_update_with(self, other, &updbuf);
        if (fail) {
            res = NULL;
            goto cleanup;
        }
    }

    if (kwargs != NULL && PyDict_Check(kwargs)) {
        fail = lru_update_with(self, kwargs, &updbuf);
        if (fail) {
            res = NULL;
            goto cleanup;
        }
    }
    res = Py_None;
cleanup:
    PyMem_Free(updbuf.buf);
    if (PURGE_MAYBE_FAIL(self)) {
        res = NULL;
    }
    Py_XINCREF(res);
    return res;
}


/* Like dict.setdefault, this evaluates the hash function only once.
 * Should test: refcount of key and return value */
static PyObject *
LRU_setdefault(LRUDict *self, PyObject *args)
{
    /* args to be parsed */
    PyObject *key;
    PyObject *default_obj = Py_None;
    Node *ret_node;
    PyObject *res;
    Py_hash_t kh;
    Py_ssize_t index;

    if (!PyArg_ParseTuple(args, "O|O:setdefault", &key, &default_obj)) {
        return NULL;
    }
    assert(key != NULL);
    assert(default_obj != NULL);

    if (unlikely((kh = get_hash(key)) == -1)) {
        return NULL;
    }

    LRU_ENTER_CRIT(self, NULL);
    /* Try borrowing a ref by key */
    index = direct_lookup(self->dict, key, kh, &ret_node);
    if (ret_node == NULL) {
        /* Error or key not in */
        if (unlikely(index == DKIX_ERROR)) { /* GetItem internal error */
            LRU_LEAVE_CRIT(self);
            return NULL;
        }

        int status;
        NodePayload pl = {key, default_obj, kh};
        /* key not in, this is not a miss, pack default_obj and insert */
        if (unlikely((ret_node = node_getnewfrom(&pl)) == NULL)) {
            LRU_LEAVE_CRIT(self);
            return NULL;
        }

        status = lru_insert_new_node_impl(self, ret_node);
        if (status == 0) {
            /* Return new ref (this is in addition to the new ref owned by the
             * node payload. */
            Py_INCREF(default_obj);
            res = default_obj;
        }
        else {
            res = NULL;
        }
        /* Safe to DECREF; won't trigger further dealloc of key or value. */
        Py_DECREF(ret_node);
    }
    else {    /* not (ret_node == NULL) */
        /* key is in, this is a hit */
        res = lru_hit_impl(self, ret_node);
    }         /* end test if (ret_node == NULL) */
    LRU_LEAVE_CRIT(self);

    if (PURGE_MAYBE_FAIL(self)) {
        Py_XDECREF(res);
        res = NULL;
    }

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
        /* lru_hit_impl will do a promotion; don't use it. */
        lru_detach_node(ret_node);
        Py_INCREF(ret_node->pl.value);
        result = ret_node->pl.value;
        self->hits++;

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

    node = pop_least_recent ? LAST_NODE(self) : FIRST_NODE(self);
    /* item_to_pop is new reference if not NULL */
    item_to_pop = IS_VALID_NODE_IN(self, node) ? get_item(node) : NULL;
    if (item_to_pop == NULL) {
        PyErr_SetString(PyExc_KeyError, "popitem(): LRUDict is empty");
        LRU_LEAVE_CRIT(self);
        return NULL;
    }

    Py_INCREF(node);
    if (_PyDict_DelItem_KnownHash(self->dict,
                                  node->pl.key, node->pl.key_hash) == 0)
    {
        lru_detach_node(node);
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
     * the root's prev/next links and don't have to delink one by one. */
    assert(self->root != NULL);
    FIRST_NODE(self) = LAST_NODE(self) = self->root;
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
    Node *first = FIRST_NODE(self);
    if (IS_VALID_NODE_IN(self, first)) {
        result = get_item(first); /* New reference */
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
    Node *last = LAST_NODE(self);
    if (IS_VALID_NODE_IN(self, last)) {
        result = get_item(last);  /* New reference */
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


/* Copy to dict so that the source LRU->MRU order is the dst's key-insertion
 * order (hence iteration order) */
static PyObject *
LRU_to_dict(LRUDict *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *dst;
    int status = 0;
    const Node *n = LAST_NODE(self);

    if ((dst = PyDict_New()) == NULL) {
        return NULL;
    }

    LRU_ENTER_CRIT(self, NULL);
    while (IS_VALID_NODE_IN(self, n)) {
        status = _PyDict_SetItem_KnownHash(dst,
                                           n->pl.key,
                                           n->pl.value,
                                           n->pl.key_hash);
        if (unlikely(status == -1)) {
            break;
        }
        n = n->prev;
    }
    LRU_LEAVE_CRIT(self);

    if (unlikely(status == -1)) {
        Py_DECREF(dst);
        return NULL;
    }
    return dst;
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
    return Py_BuildValue("(kk)", self->hits, self->misses);
}
#endif /* LRUDICT_STRUCT_SEQUENCE_NOT_BROKEN */


/* "Manual" purge once */
static PyObject *
LRU_purge(LRUDict *self, PyObject *Py_UNUSED(ignored))
{
    self->_pb = 1;
    Py_ssize_t status = lru_purge_staging_impl(self, FORCE_PURGE);
    return unlikely(status == -2) ? NULL : PyLong_FromSsize_t(status);
}


#define MAP_BITFIELD(field, prop)                   \
static PyObject *                                   \
LRU_##prop##_getter(LRUDict *self, void *Py_UNUSED(closure))   \
{if (self->field) {Py_RETURN_TRUE;} else {Py_RETURN_FALSE;}}   \
static int                                          \
LRU_##prop##_setter(LRUDict *self, PyObject *value, void *Py_UNUSED(closure))\
{   int v;                                      \
    if (value == NULL) {                        \
        PyErr_SetString(PyExc_AttributeError, "can't delete "#prop);\
        return -1;                              \
    }                                           \
    if ((v = PyObject_IsTrue(value)) == -1) {   \
        PyErr_SetString(PyExc_ValueError,       \
                #prop" flag must evaluate to True or False");\
        return -1;                              \
    }                                           \
    self->field = v;                            \
    return 0;                                   \
}


/* debug property: disable purging */
MAP_BITFIELD(purge_suspended, _suspend_purge)
MAP_BITFIELD(detect_conflict, _detect_conflict)


static PyObject *
LRU__purge_queue_size_getter(LRUDict *self, void *Py_UNUSED(closure))
{
    Py_ssize_t len = 0;
    if (self->purge_queue) {
        len = self->purge_queue->sinfo.tail;
    }
    return PyLong_FromSsize_t(len);
}


static PyObject *
LRU__max_pending_callbacks_getter(LRUDict *self, void *Py_UNUSED(closure))
{
    if (self->purge_queue) {
        unsigned long ires = (unsigned long)(self->purge_queue->n_max);
        return PyLong_FromUnsignedLong(ires);
    }
    else {
        PyErr_SetString(PyExc_MemoryError, "purge queue pointer is NULL");
        return NULL;
    }
}


static int
LRU__max_pending_callbacks_setter(LRUDict *self, PyObject *value,
                                  void *Py_UNUSED(closure))
{
    if (value == NULL) {
        PyErr_SetString(PyExc_AttributeError,
                        "can't delete _max_pending_callbacks");
        return -1;
    }

    if (self->purge_queue == NULL) {
        PyErr_SetString(PyExc_MemoryError, "purge queue pointer is NULL");
        return -1;
    }

    Py_ssize_t ires = PyLong_AsSsize_t(value);

    if (PyErr_Occurred()) {
        return -1;
    }

    if (ires < 1 || ires > (Py_ssize_t)USHRT_MAX) {
        PyErr_Format(PyExc_ValueError,
                     "value must be between 1 and %u", USHRT_MAX);
        return -1;
    }
    else {
        self->purge_queue->n_max = (unsigned short)ires;
        return 0;
    }
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
        (PyCFunction)LRU_subscript, METH_O | METH_COEXIST,
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
    {"to_dict",
        (PyCFunction)LRU_to_dict, METH_NOARGS,
        PyDoc_STR("to_dict(self, /)\n--\n\n-> Dict\nReturn new dictionary as a shallow copy of self's entries. The dictionary's iteration order is the same as self's LRU-to-MRU order.")},
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
    {"_max_pending_callbacks",
        (getter)LRU__max_pending_callbacks_getter,
        (setter)LRU__max_pending_callbacks_setter,
        PyDoc_STR("Maximal number of callbacks allowed to be pending."),
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
    if (unlikely(self->dict == NULL)) {
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

    if ((self->purge_queue = lrupq_new()) == NULL) {
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

    /* Allocate a special node as root; The specific value doens't matter as
     * long as it is something that can be safely DECREF'ed (so that no special
     * case needs to go in node_dealloc. The hash doesn't matter, and -1 may be
     * useful to cause early fail in the case that the root got erroneously
     * inserted. */
    NodePayload rootpl = {Py_None, Py_None, -1};
    Node *root_node = node_getnewfrom(&rootpl);
    if (root_node == NULL) {
        self->root = NULL;
        return -1;
    }
    root_node->next = root_node->prev = root_node;
    self->root = root_node;

    self->hits = 0;
    self->misses = 0;
    self->purge_suspended = 0;
    self->detect_conflict = 1;
    self->_pb = 0;
    return 0;
}


/* Safe-finalization support. */
static void
LRU_fini(LRUDict *self)
{
    PyObject *exc_type, *exc_value, *traceback;
    PyErr_Fetch(&exc_type, &exc_value, &traceback);

    /* One last chance to honour any callback. */
    self->_pb = 1;
    if (lru_purge_staging_impl(self, FORCE_PURGE) == -2) {
        PyErr_WriteUnraisable((PyObject *)self);
    }

    PyErr_Restore(exc_type, exc_value, traceback);
    return;
}


/* NOTE: Argument names should not change because the function uses Py_VISIT
 * macro.
 *
 * The GC bypasses the Node object because there's no need to track a large
 * number of small Node objects. A Node can only be created by us and it can
 * only be found as values in self->dict or items in self->purge_queue->lst,
 * and it cannot be shared by different dicts. When doing the traverse we
 * already know where to visit, and we directly visit the Node's Python-object
 * members (which it owns). The visitations go through the node items (most
 * likely place to form cycles), node keys, then purge queue (usually short),
 * and finally the callback object. (NOTE: cycles are rare, and we fuse the
 * loops over node->pl.key and node->pl.value since the loops are likely to be
 * exhausted rather than early-returned. ) */
static int
LRU_traverse(LRUDict *self, visitproc visit, void *arg)
{
    Node *cur = LAST_NODE(self);

    while (IS_VALID_NODE_IN(self, cur)) {
        Py_VISIT(cur->pl.key);
        Py_VISIT(cur->pl.value);
        cur = cur->prev;
    }

    if (self->purge_queue && self->purge_queue->lst) {
        Py_ssize_t len = PyList_Size(self->purge_queue->lst);
        for (Py_ssize_t i = 0; i < len; i++) {
            cur = (Node *)PyList_GET_ITEM(self->purge_queue->lst, i);
            if (cur) {
                Py_VISIT(cur->pl.key);
                Py_VISIT(cur->pl.value);
            }
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
    /* Release storage (and all nodes in it) */
    if (self->dict) {
        self->internal_busy = 0;
        /* Will NOT call callback on any staging elems. */
        PyDict_Clear(self->dict);
        Py_CLEAR(self->dict);
    }

    if (self->purge_queue) {
        /* Release purge queue */
        if (lrupq_free(self->purge_queue) == -1) {
            /* This means the purge queue is still busy at the time of
             * teardown. This is a very abnormal situation, but otherwise the
             * refcount to the list is still held by anything operating on the
             * list. */
            PyObject *exc_type, *exc_value, *tb;
            PyErr_Fetch(&exc_type, &exc_value, &tb);
            PyErr_SetString(PyExc_RuntimeError,
                            "lru_ng.LRUDict: purge queue busy at teardown.");
            PyErr_WriteUnraisable(self->purge_queue->lst);
            PyErr_Restore(exc_type, exc_value, tb);
        }
        self->purge_queue = NULL;
    }

    /* Dispose of reference to callback if any. */
    Py_CLEAR(self->callback);
    return 0;
}


/* Deallocation, when the refcount to self reaches zero */
static void
LRU_dealloc(LRUDict *self)
{
    if (PyObject_CallFinalizerFromDealloc((PyObject *)self) < 0) {
        /* Resurrected */
        return;
    }

    PyObject_GC_UnTrack((PyObject *)self);

    LRU_tp_clear(self);
    Py_CLEAR(self->root);
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
    .tp_flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                 Py_TPFLAGS_HAVE_FINALIZE),
    .tp_doc = lru_doc,
    .tp_methods = LRU_methods,
    .tp_getset = LRU_descriptors,
    .tp_init = (initproc)LRU_init,
    .tp_new = PyType_GenericNew,
    .tp_finalize = (destructor)LRU_fini,
};


static void
lru_ng_module_free_safe_types(void *mself)
{
    if (mself == NULL) {
        return;
    }
    ts_destroy(lru_safe_types);
    return;
}


/* Module structure */
static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    .m_name = "lru_ng",
    .m_doc = lru_doc,
    .m_size = -1,
    .m_free = lru_ng_module_free_safe_types,
};


static PyObject *
moduleinit(void)
{
    PyObject *m;
    PyTypeObject *st_list[] = {&_PyNone_Type, &PyUnicode_Type, &PyLong_Type,
                               &PyBytes_Type, &PyByteArray_Type,
                               &PyBool_Type, &PyFloat_Type, &PyComplex_Type};
    lru_safe_types = ts_create((const void *const *)st_list,
                               sizeof(st_list) / sizeof(st_list[0]));
    if (lru_safe_types == NULL) {
        PyErr_SetString(PyExc_MemoryError,
                        "Failed to create or initialize safe-type set");
        return NULL;
    }

    /* Pull in the types */
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
    if (PyModule_AddObject(m, "LRUDict", (PyObject *)(&LRUDictType)) < 0 ||
        PyModule_AddObject(m, "LRUDictBusyError", LRUDictExc_BusyErr) < 0)
    {
        Py_DECREF(&LRUDictType);
        Py_DECREF(&NodeType);
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
