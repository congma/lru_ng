#ifndef LRUDICT_STATSTYPE_H
#define LRUDICT_STATSTYPE_H
#include "Python.h"


/* namedtuple type representing the hits/misses information. Compatible with
 * the old behaviour (tuple), but also with names for convenience. */


#if ((PY_MAJOR_VERSION) >= 3 && (PY_MINOR_VERSION >= 8))
#ifndef LRUDICT_STRUCT_SEQUENCE_NOT_BROKEN
#define LRUDICT_STRUCT_SEQUENCE_NOT_BROKEN
#endif
static PyStructSequence_Field LRUDict_stats_fields[] = {
    {"hits", PyDoc_STR("Number of hits")},
    {"misses", PyDoc_STR("Number of misses")},
    {NULL, NULL},
};


static PyStructSequence_Desc LRUDict_stats_desc = {
    .name = "lru_ng.LRUDictStats",
    .doc = PyDoc_STR("Hit/miss information for LRUDict object"),
    .fields = LRUDict_stats_fields,
    .n_in_sequence = 2,
};


static PyTypeObject *LRUDictStatsType;
#else	/* version check */
#ifdef LRUDICT_STRUCT_SEQUENCE_NOT_BROKEN
#undef LRUDICT_STRUCT_SEQUENCE_NOT_BROKEN
#endif
#endif	/* version check */


#endif /* LRUDICT_STATSTYPE_H */
