/**
 * @file   tm.c
 * @author Paolo Celada <paolo.celada@epfl.ch>
 *
 * @section LICENSE
 *
 * Copyright © 2018-2019 Paolo Celada.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * any later version. Please see https://gnu.org/licenses/gpl.html
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * @section DESCRIPTION
 *
 * Implementation of a Dual-versioned transactional memory
**/

// Requested features
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#ifdef __STDC_NO_ATOMICS__
#error Current C11 compiler does not support atomic operations
#endif

// External headers
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// Internal headers
#include <tm.h>

// -------------------------------------------------------------------------- //

/** Define a proposition as likely true.
 * @param prop Proposition
**/
#undef likely
#ifdef __GNUC__
#define likely(prop) \
    __builtin_expect((prop) ? 1 : 0, 1)
#else
#define likely(prop) \
    (prop)
#endif

/** Define a proposition as likely false.
 * @param prop Proposition
**/
#undef unlikely
#ifdef __GNUC__
#define unlikely(prop) \
    __builtin_expect((prop) ? 1 : 0, 0)
#else
#define unlikely(prop) \
    (prop)
#endif

/** Define one or several attributes.
 * @param type... Attribute names
**/
#undef as
#ifdef __GNUC__
#define as(type...) \
    __attribute__((type))
#else
#define as(type...)
#warning This compiler has no support for GCC attributes
#endif



// -------------------------------------------------------------------------- //

/** Mutex lock functions (with simple abstraction)
**/

typedef struct lock_s {
    pthread_mutex_t mutex;
} lock_t;

/** Initialize the given lock.
 * @param lock Lock to initialize
 * @return Whether the operation is a success
**/
static bool lock_init(lock_t* lock) {
    return pthread_mutex_init(&(lock->mutex), NULL) == 0;
}

/** Clean the given lock up.
 * @param lock Lock to clean up
**/
static void lock_cleanup(lock_t* lock) {
    pthread_mutex_destroy(&(lock->mutex));
}

/** Wait and acquire the given lock.
 * @param lock Lock to acquire
 * @return Whether the operation is a success
**/
static bool lock_acquire(lock_t* lock) {
    return pthread_mutex_lock(&(lock->mutex)) == 0;
}

/** Release the given lock.
 * @param lock Lock to release
**/
static void lock_release(lock_t* lock) {
    pthread_mutex_unlock(&(lock->mutex));
}

// -------------------------------------------------------------------------- //
/** Batcher functions
 * The goal of the batcher is to create artificial points in time when no transaction runs. It perform job similar
 * to the mutex one, but differently it allows multiple threads (transactions) enter the critical section every time.
 * Data needed:
 *  - counter (int) --> keep track of the current epoch through a counter
 *  - remaining (int) --> count number of threads in CS
 *  - blocked_count (int) --> count number of waiting threads (substitute blocked)
 *  - lock (mutex) --> guarantee mutual exclusion on remaining variable
 *  - cond_var (CV) --> guarantee sleeping of multiple threads waiting for a condition
 * Functions to be implemented:
 *  - get_epoch()
 *  - enter()
 *  - leave()
**/

typedef struct batcher_s {
    int counter;
    int remaining;
    struct lock_t lock;
    pthread_cond_t cond_var;
    int blocked_count;
} batcher_t;

/** Simple getter function for getting the current epoch number
 * @param batcher batcher instance
 * @return number of current epoch (int)
**/
int get_epoch(batcher_t *batcher)
{
    return batcher.counter;
}

/** Enter in the critical section, or wait until woken up
 * @param batcher batcher instance
**/
void enter(batcher_t *batcher)
{
    lock_acquire(&batcher->lock);
    if(batcher->remaining  == 0) {
        batcher->remaining = 1;
    } else {
        blocked_count++;
        pthread_cond_wait(&batcher->cond_var, &batcher->lock);
    }
    lock_release(&batcher->lock);
    return;
}

/** Leave critical section, and if you are the last thread wake up waiting threads
 * @param batcher batcher instance
**/
void leave(batcher_t *batcher)
{
    lock_acquire(&batcher->lock);
    batcher->remaining--;
    if(batcher->remaining == 0) {
        batcher->counter++;
        batcher->remaining = batcher->blocked_count;
        pthread_cond_broadcast(&batcher->cond_var);
        batcher->blocked_count = 0;
    }
    lock_release(&batcher->lock);
    return;
}
// -------------------------------------------------------------------------- //

static const tx_t read_only_tx  = UINTPTR_MAX - 10;
static const tx_t read_write_tx = UINTPTR_MAX - 11;

/** shared memory region structure (1 per shared memory)
 * @param batcher Batcher instance for the shared memory
 * @param start Start of the shared memory region
 * @param segments Array of segments in the memory region
 * @param size Size of the shared memory region (in bytes)
 * @param align Claimed alignment of the shared memory region (in bytes)
 * @param align_alloc Actual alignment of the memory allocations (in bytes)
 * @param delta_alloc Space to add at the beginning of the segment for the link chain (in bytes)
**/
typedef struct region_s {
    batcher_t *batcher;
    void *start;
    //struct link allocs;
    segment_t *segment;
    size_t size;
    size_t align;
    size_t align_alloc;
    size_t delta_alloc;

} region_t;

/** segment structure (multiple per shared memory)
 * @param words Array of words in segment (each word is actully a pair of words(2))
 * @param num_words Number of words in segment (TODO actully could be a global constant? maybe)
**/
typedef struct segment_s {
    word_t *words;
    size_t num_words;
} segment_t;

/** word structure (multiple per segment in shared memory)
 * @param copy_0 Read-only copy of memory word
 * @param copy_1 Read-write copy of memory word
 * @param read_only_copy Flag to distinguish read-only copy
 * @param word_size Size of the word
 * @param write_tx First transaction which perform a write on 1 of the 2 words. From that moment, only he can write
 * @param is_written_in_epoch Boolean to flag if the word has been written
**/
typedef struct word_s {
    void *copy_0; // copy 0 (read-only)
    void *copy_1; // copy 1 (read-write)
    int read_only_copy;
    tx_t write_tx;
    bool is_written_in_epoch;
    size_t word_size;
} word_t;

/** Create (i.e. allocate + init) a new shared memory region, with one first non-free-able allocated segment of the requested size and alignment.
 * @param size  Size of the first shared segment of memory to allocate (in bytes), must be a positive multiple of the alignment
 * @param align Alignment (in bytes, must be a power of 2) that the shared memory region must support
 * @return Opaque shared memory region handle, 'invalid_shared' on failure
 * @todo Allocate batcher instance (it's a pointer)
**/
shared_t tm_create(size_t size as(unused), size_t align as(unused))
{
    // allocate shared memory region
    region_t *region = (region_t *) malloc(sizeof(region_t));
    if(unlikely(!region)) {
        return invalid_shared;
    }

    // calculate alignment for the shared memory region
    size_t align_alloc = align < sizeof(void*) ? sizeof(void*) : align;

    // allocate 1st segment in shared memory region
    segment_t *segment = (segment_t *) malloc(sizeof(segment_t));
    if(unlikely(!segment)) {
        return invalid_shared;
    }


    return invalid_shared;
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
**/
void tm_destroy(shared_t shared as(unused))
{
    // TODO: tm_destroy(shared_t)
}

/** [thread-safe] Return the start address of the first allocated segment in the shared memory region.
 * @param shared Shared memory region to query
 * @return Start address of the first allocated segment
**/
void *tm_start(shared_t shared as(unused))
{
    // TODO: tm_start(shared_t)
    return NULL;
}

/** [thread-safe] Return the size (in bytes) of the first allocated segment of the shared memory region.
 * @param shared Shared memory region to query
 * @return First allocated segment size
**/
size_t tm_size(shared_t shared as(unused))
{
    // TODO: tm_size(shared_t)
    return 0;
}

/** [thread-safe] Return the alignment (in bytes) of the memory accesses on the given shared memory region.
 * @param shared Shared memory region to query
 * @return Alignment used globally
**/
size_t tm_align(shared_t shared as(unused))
{
    // TODO: tm_align(shared_t)
    return 0;
}

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
**/
tx_t tm_begin(shared_t shared as(unused), bool is_ro as(unused))
{
    // TODO: tm_begin(shared_t)
    return invalid_tx;
}

/** [thread-safe] End the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
**/
bool tm_end(shared_t shared as(unused), tx_t tx as(unused))
{
    // TODO: tm_end(shared_t, tx_t)
    return false;
}

/** [thread-safe] Read operation in the given transaction, source in the shared region and target in a private region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in the shared region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in a private region)
 * @return Whether the whole transaction can continue
**/
bool tm_read(shared_t shared as(unused), tx_t tx as(unused), void const *source as(unused), size_t size as(unused), void *target as(unused))
{
    // TODO: tm_read(shared_t, tx_t, void const*, size_t, void*)
    return false;
}

/** [thread-safe] Write operation in the given transaction, source in a private region and target in the shared region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in a private region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in the shared region)
 * @return Whether the whole transaction can continue
**/
bool tm_write(shared_t shared as(unused), tx_t tx as(unused), void const *source as(unused), size_t size as(unused), void *target as(unused))
{
    // TODO: tm_write(shared_t, tx_t, void const*, size_t, void*)
    return false;
}

/** [thread-safe] Memory allocation in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param size   Allocation requested size (in bytes), must be a positive multiple of the alignment
 * @param target Pointer in private memory receiving the address of the first byte of the newly allocated, aligned segment
 * @return Whether the whole transaction can continue (success/nomem), or not (abort_alloc)
**/
alloc_t tm_alloc(shared_t shared as(unused), tx_t tx as(unused), size_t size as(unused), void **target as(unused))
{
    // TODO: tm_alloc(shared_t, tx_t, size_t, void**)
    return abort_alloc;
}

/** [thread-safe] Memory freeing in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param target Address of the first byte of the previously allocated segment to deallocate
 * @return Whether the whole transaction can continue
**/
bool tm_free(shared_t shared as(unused), tx_t tx as(unused), void *target as(unused))
{
    // TODO: tm_free(shared_t, tx_t, void*)
    return false;
}
