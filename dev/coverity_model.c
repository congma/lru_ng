/* Model file for Coverity Scan.
 * Currently this file is chiefly used to describe the behaviour of external
 * functions.
 */


void *
PyMem_Malloc(size_t size)
{
    if (size == 0) {
        size++;
    }
    return __coverity_alloc__(size);
}


void
PyMem_Free(void *ptr)
{
    __coverity_free__(ptr);
}
