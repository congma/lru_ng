#ifndef TINYSET_C
#define TINYSET_C

/* Tiny frozen-set-like lookup table for fixed-size, small collection of
 * pointers. This file is intended as a simple in-source include file with
 * static-declared functions.
 *
 * Supported operations:
 *
 * - Creation from a non-repeating array of non-null pointers.
 * - Lookup by pointer value, i.e. determining whether the input pointer is in
 *   the set.
 *
 * Pros and cons:
 *
 * - Constant lookup time, with exactly one hashing, one array indexing, and
 *   one comparison (or xor) per lookup.
 * - OK-ish small table size if input collection is small.
 * - Very fast hash function (in the words of K&R, originally for a simple
 *   string hash, "[t]his is not the best possible hash function, but it is
 *   short and effective.")
 *
 *  vs.
 *
 * - Set-up time is rather high (especially if input collection is large) and
 *   the operations quite naive (brute-forcing a parameter in the hash function
 *   to make it perfect for the input collection).
 * - Very likely not minimal, wasting storage.
 * - Looking up the null pointer always results in a "in", unless we're lucky
 *   to stumble upon a minimal hash function by chance.
 *
 * Assertions are used liberally in the code, even on critical paths. All they
 * do is to "state the obvious" (invariants) and should be compiled out with
 * -DNDEBUG in release/production code.
 */

#ifndef PY_SSIZE_T_CLEAN
#define PY_SSIZE_T_CLEAN
#endif

#include "Python.h"  /* Brings in config macros */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <assert.h>

#define TS_WIDTH_BITS  (SIZEOF_VOID_P * CHAR_BIT)
#define ROT_MASK ((unsigned int)(TS_WIDTH_BITS - 1U))

#if SIZEOF_VOID_P == 4    /* 32-bit */

    #define LOG_WIDTH 5U  /* log(32) */
    #define FIB_FACTOR    UINT32_C(0x61c88647)

#elif SIZEOF_VOID_P == 8  /* 64-bit */

    #define LOG_WIDTH 6U  /* log(64) */
    #define FIB_FACTOR    UINT64_C(0x61c8864680b583eb)

#else                     /* unknown-bit */
    #error pointer byte size must be 4 or 8
#endif


struct ts_param {
    unsigned int shift_offset;    /* Number of bits to shift circularly. */
    unsigned int log_size_compl;  /* TS_WIDTH_BITS - (log of array size). */
};


typedef struct ts_set {
    struct ts_param hash_context;
    const void *const *array;
} TinySet;


/* Rotate unsigned representation right ("counterclockwise") by b bits. b and
 * its modular-negation is modular-reduced to be within the width of i so that
 * the behaviour is defined at the C level. This should compile nicely into one
 * rotation instruction. */
static inline uintptr_t
ror(uintptr_t i, unsigned int b)
{
    b &= ROT_MASK;
    return (i >> b) | (i << (-b & ROT_MASK));
}


/* Return the exponent of two rounded up. Example: 0 -> 0; 1 -> 0; 2 -> 1;
 * {5, 6, 7} -> 3; 8 -> 3, {9, 10, 11, 12} -> 4. */
static inline unsigned int
next_exponent(size_t i)
{
    unsigned int l = 0;
    unsigned int t = i & 1;
    while (i >>= 1) {
        l++;
        t += i & 1;
    }
    l += t > 1 ? 1 : 0;
    return l;
}


/* Evaluate the index for input x in context pointed to by ctx. This is based
 * on a simple combination of rotation (i.e. circular shift, see Python's
 * pointer hash) parametrised by the context's shift_offset field, and an
 * (implied) modular-multiplication by (1 - phi), followed by the extraction of
 * the most-significant bits. Notice that rotation in itself doesn't have good
 * distributing ability; it's chiefly to avoid fixed patterns, constants, and
 * strides caused by alignment and memory-allocator behaviour. This also tends
 * to work better than one simple shift-and-xor for pointers. The rotation
 * tends to complement the modular multiplication by shifting the low-entropy
 * bits in the source to the low-sensitivity places of the latter. */
static unsigned int
ts_get_index(const void *x, const struct ts_param *restrict ctx)
{
    uintptr_t h;
    h = ror((uintptr_t)x, ctx->shift_offset);
    h *= FIB_FACTOR;
    h >>= ctx->log_size_compl;
    return (unsigned int)h;
}


/* Brute-force solve or "crack" the offset constant in the index function given
 * array of pointers src, its length slen, and a target table-size specified as
 * the base-2 logarithm table_log. If found, return the offset (positive and
 * between 0 and the bit-width of uintptr_t) as int, to be cast to unsigned
 * int; if not, return -1. */
static int
ts_crack_offset(const void *const *restrict src, size_t slen,
                unsigned int target_log)
{
    int offset, res;
    /* Create "bucket" as array of bitmasks to detect collision for the input
     * sources. Each index in the array corresponds to a quotient, and each
     * bit-position in that index's item corresponds to a remainder. */
    size_t nbuckets = (1U << target_log) / TS_WIDTH_BITS;
    if (nbuckets == 0) {
        nbuckets++;
    }
    /* There are enough bucket slots for everyone. */
    assert(nbuckets * TS_WIDTH_BITS >= (1U << target_log));

    size_t bucket_bytes = nbuckets * sizeof(uintptr_t);
    uintptr_t *const bucket_array = PyMem_Malloc(bucket_bytes);
    if (!bucket_array) {
        return -1;
    }

    struct ts_param trial_param;

    /* Target size cannot exceed the maximum allowed by uintprt_t */
    assert(TS_WIDTH_BITS > target_log);
    trial_param.log_size_compl = TS_WIDTH_BITS - target_log;

    for (offset = 0; offset < TS_WIDTH_BITS; offset++) {
        _Bool no_collision = 1;
        memset(bucket_array, 0, bucket_bytes);

        for (size_t i = 0; i < slen; i++) {
            trial_param.shift_offset = offset;
            unsigned int ix = ts_get_index(src[i], &trial_param);
            /* Put into bucket by division with platform pointer-width. */
            unsigned int bi = ix >> LOG_WIDTH;              /* quotient */
            unsigned int bpos = ix & (TS_WIDTH_BITS - 1);   /* remainder */
            /* Shift by bpos is always safe */
            assert(bpos < TS_WIDTH_BITS);
            uintptr_t flag = 1 << bpos;
            if (!(bucket_array[bi] & flag)) {
                bucket_array[bi] |= flag;
            }
            else {
                /* Same index seen. */
                no_collision = 0;
                break;
            }
        }
        if (no_collision) {
            res = offset;
            /* "no_collision" means it. */
            assert(res >= 0);
            goto cleanup;
        }
    }
    /* not found */
    res = -1;
cleanup:
    PyMem_Free(bucket_array);
    return res;
}


/* Create TinySet table. Return the pointer to the newly allocated tinyset on
 * success, or NULL on failure. */
#define TS_MAX_ARR_SIZE_LOG 9U    /* log(512) */
static TinySet *
ts_create(const void *const *restrict src, size_t slen)
{
    assert(slen != 0);
    int k = -1;         /* Trial value of offset. */
    unsigned int tlog;  /* Trial value of logarithm of table-size. */
    /* Augment table size, starting from the power of two not smaller than the
     * source length for sparsity and ease of computation of remainder, and
     * crack for collision-free hash given the input array. */
    for (tlog = next_exponent(slen); tlog < TS_MAX_ARR_SIZE_LOG; tlog++) {
        k = ts_crack_offset(src, slen, tlog);
        if (k != -1) {
            break;
        }
    }
    if (k == -1) {
        /* not found */
        return NULL;
    }

    /* It is always OK to shift by tlog or its complement (TS_WIDTH_BITS -
     * tlog); the latter is used in the hash function body. */
    assert(tlog < TS_WIDTH_BITS);
    assert(tlog > 0);
    size_t tlen = 1U << tlog;  /* True sparse-table length. */
    struct ts_param param_solution = {(unsigned int)k, TS_WIDTH_BITS - tlog};

    /* The length we found, if at all, is always good for allocation and
     * indexing. */
    assert(tlen >= slen);
    const void **arr = PyMem_Malloc(tlen * sizeof(const void *));
    if (!arr) {
        return NULL;
    }

    TinySet *ts = PyMem_Malloc(sizeof(TinySet));
    if (!ts) {
        PyMem_Free(arr);
        return NULL;
    }

    for (size_t i = 0; i < tlen; i++) {
        arr[i] = NULL;
    }
    /* Populate sparse table with the sources at their indices. This does some
     * re-computation but only once. */
    for (size_t i = 0; i < slen; i++) {
        unsigned int index = ts_get_index(src[i], &param_solution);
        /* May not be obvious: the index returned is always within bounds. */
        assert(index < tlen);
        arr[index] = src[i];
    }
    ts->hash_context = param_solution;
    ts->array = arr;
    return ts;
}


static void
ts_destroy(TinySet *ts)
{
    PyMem_Free((void *)(ts->array));
    PyMem_Free(ts);
}


static _Bool
ts_has_elem(const TinySet *restrict s, const void *restrict elem)
{
    unsigned int index = ts_get_index(elem, &(s->hash_context));
    /* Maybe not obvious: the index is always within array bounds, no matter if
     * elem is in or not. */
    assert(index < (1U << (TS_WIDTH_BITS - s->hash_context.log_size_compl)));
    return !((uintptr_t)(s->array[index]) ^ (uintptr_t)elem);
}
#endif /* TINYSET_C */
