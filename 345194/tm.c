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
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

// Internal headers
#include <tm.h>

// Global constants
#define SEGMENT_SHIFT 24
#define INIT_FREED_SEG_SIZE 10

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
 * @param tmp_segment Array of temporary segments created during the epoch. integrated to segments only if committed

 * Functions to be implemented:
 *  - get_epoch()
 *  - enter()
 *  - leave()
**/

typedef struct transaction_s {
    tx_t tx_id;
    bool is_ro;
} transaction_t;

typedef struct batcher_s {
    int counter;
    int remaining;
    lock_t lock;
    pthread_cond_t cond_var;
    int blocked_count;
    transaction_t *running_tx;
    int num_running_tx;
} batcher_t;

/** Initialization function for the batcher
 * @param batcher Batcher to be initialized
 * @return Boolean value for success or failure
**/ 
bool batcher_init(batcher_t *batcher)
{
    bool ret;
    batcher->counter = 0;
    batcher->remaining = 0;
    batcher->blocked_count = 0;

    // init lock
    if(unlikely(!lock_init(&(batcher->lock)))) {
        return false;
    }

    // init conditional variable
    if(pthread_cond_init(&(batcher->cond_var), NULL) != 0) {
        return false;
    }

    batcher->running_tx = NULL;
    batcher->num_active_tx = 0; // default is 0
    return true;
}

/** Simple getter function for getting the current epoch number
 * @param batcher Batcher instance
 * @return Number of current epoch (int)
**/
int get_epoch(batcher_t *batcher)
{
    return batcher->counter;
}

/** Enter in the critical section, or wait until woken up
 * @param batcher Batcher instance
**/
void enter(batcher_t *batcher)
{
    lock_acquire(&batcher->lock);
    if(batcher->remaining  == 0) {
        batcher->remaining = 1;
    } else {
        batcher->blocked_count++;
        pthread_cond_wait(&batcher->cond_var, &batcher->lock);
    }
    lock_release(&batcher->lock);
    return;
}

/** Leave critical section, and if you are the last thread wake up waiting threads
 * @param batcher Batcher instance
**/
void leave(batcher_t *batcher)
{
    lock_acquire(&batcher->lock);
    batcher->remaining--;
    if(batcher->remaining == 0) {
        batcher->counter++;
        batcher->remaining = batcher->blocked_count;
        
        // init transactions array with new number of transactions
        if(batcher->running_tx == NULL) {
            batcher->running_tx = (transaction_t *) malloc(batcher->remaining * (transaction_t));
        } else {
            batcher->running_tx = (transaction_t *) realloc(batcher->running_tx, batcher->remaining);   
        }
        batcher->num_running_tx = batcher->remaining;
        
        pthread_cond_broadcast(&batcher->cond_var);
        batcher->blocked_count = 0;
    }
    lock_release(&batcher->lock);
    return;
}

// -------------------------------------------------------------------------- //
/** Extra functions
 * used throughout the whole STM library
**/

/** Init a segment for a region
 * @param segment Segment of shared memory region
 * @param size Size of segment
 * @param align_alloc Alignment of words in segment
 * @return Boolean for success or failure
**/
bool segment_init(segment_t *segment, tx_t tx, size_t size, size_t align_alloc)
{
    segment->num_words = size / (align_alloc * 2); // each word is duplicated
    segment->word_size = align_alloc;
    segment->created_by_tx = tx;

    int copy_size = segment->num_words * segment->word_size;

    // alloc words in segment
    segment->copy_0 = (void *) malloc(copy_size);
    if(unlikely(!segment->copy_0)) {
        return false;
    }
    segment->copy_1 = (void *) malloc(copy_size);
    if(unlikely(!segment->copy_1)) {
        free(segment->copy_0);
        return false;
    }

    // initialize words in segment with all zeros
    memset(segment->copy_0, 0, copy_size);
    memset(segment->copy_1, 0, copy_size);


    // init supporting data structure for words
    segment->read_only_copy = (int *) malloc(segment->num_words * sizeof(int));
    if(unlikely(!segment->read_only_copy)) {
        free(segment->copy_0);
        free(segment->copy_1);
        return false;
    }
    segment->write_tx = (tx_t *) malloc(segment->num_words * sizeof(tx_t));
    if(unlikely(!segment->write_tx)) {
        free(segment->copy_0);
        free(segment->copy_1);
        free(segment->read_only_copy);
        return false;
    }
    segment->is_written_in_epoch = (bool *) malloc(segment->num_words * sizeof(bool));
    if(unlikely(!segment->is_written_in_epoch)) {
        free(segment->copy_0);
        free(segment->copy_1);
        free(segment->read_only_copy);
        free(segment->write_tx);
        return false;
    }
}

// TODO function add_segment()
// called when committing a transaction which created a segment

/** Encode segment number into an opaque pointer address
 * @param segment_num number of segment
 * @return address
**/
void *encode_segment_address(int segment_num)
{
    // address is NUM_SEGMENT << 24 + offset word
    // << means shift left
    intptr_t addr = segment_num << SEGMENT_SHIFT;
    return (void *)addr;
}

/** Decode opaque pointer into segment and word number
 * @param addr opaque pointer
 * @param num_segment pointer to segment number
 * @param num_word pointer to word number
 * @return address
**/
void decode_segment_address(void *addr, int *num_segment, int *num_word)
{
    intptr_t num_s, num_w;

    // calculate word and segment number
    num_s = (int) addr >> SEGMENT_SHIFT;
    intptr_t difference = num_s << SEGMENT_SHIFT;
    num_w = addr - difference;

    *num_segment = num_s;
    *num_word = num_w;
}



// -------------------------------------------------------------------------- //

static const tx_t read_only_tx  = UINTPTR_MAX - 10;
static const tx_t read_write_tx = UINTPTR_MAX - 11;

/** shared memory region structure (1 per shared memory)
 * @param batcher Batcher instance for the shared memory
 * @param start Start of the shared memory region
 * @param segment Array of segments in the memory region
 * @param first_seg_size Size of the shared memory region (in bytes)
 * @param align Claimed alignment of the shared memory region (in bytes)
 * @param align_alloc Actual alignment of the memory allocations (in bytes)
**/
typedef struct region_s {
    batcher_t *batcher;
    void *start; // TODO: maybe not used
    //struct link allocs;
    segment_t *segment;
    size_t first_seg_size;
    size_t align;
    size_t align_alloc;
    _Atomic(int) current_segment_index;
    // need something to store indexes that can be used again
    // maybe an array of int with mutex, or array of atomic int
    int *freed_segment_index; // array of indexes freed and that can be used again
    lock_t freed_segment_index_lock;
    _Atomic(int) current_transaction_id;
} region_t;

/** segment structure (multiple per shared memory)
 * @param num_words Number of words in segment (TODO actully could be a global constant? maybe)
 * @param copy_0 Copy 0 of segments words (accessed shifting a pointer)
 * @param copy_1 Copy 1 of segments words (accessed shifting a pointer)
 * @param read_only_copy Array of flags to distinguish read-only copy
 * @param write_tx array of first transaction which perform a write on 1 of the 2 words. From that moment, only he can write
 * @param is_written_in_epoch Array of boolean to flag if the word has been written
 * @param word_size Size of the word
**/
typedef struct segment_s {
    size_t num_words;
    void *copy_0;
    void *copy_1;
    int *read_only_copy;
    tx_t *write_tx;
    bool *is_written_in_epoch;
    int word_size;
    tx_t created_by_tx; // if -1 segment is shared, else it's temporary and must be deleted if tx abort
} segment_t;

/** transaction structure (multiple per batcher)
 * @param num_words Number of words in segment (TODO actully could be a global constant? maybe)
 * @param copy_0 Copy 0 of segments words (accessed shifting a pointer)
 * @param copy_1 Copy 1 of segments words (accessed shifting a pointer)
 * @param read_only_copy Array of flags to distinguish read-only copy
 * @param write_tx array of first transaction which perform a write on 1 of the 2 words. From that moment, only he can write
 * @param is_written_in_epoch Array of boolean to flag if the word has been written
 * @param word_size Size of the word
**/

/** Create (i.e. allocate + init) a new shared memory region, with one first non-free-able allocated segment of the requested size and alignment.
 * @param size  Size of the first shared segment of memory to allocate (in bytes), must be a positive multiple of the alignment
 * @param align Alignment (in bytes, must be a power of 2) that the shared memory region must support
 * @return Opaque shared memory region handle, 'invalid_shared' on failure
 * @todo Allocate batcher instance (it's a pointer)
**/
shared_t tm_create(size_t size as(unused), size_t align as(unused))
{
    bool ret;

    // allocate shared memory region
    region_t *region = (region_t *) malloc(sizeof(region_t));
    if(unlikely(!region)) {
        return invalid_shared;
    }

    // calculate alignment for the shared memory region
    size_t align_alloc = align < sizeof(void*) ? sizeof(void*) : align;

    // allocate and initialize batcher for shared memory region
    region->batcher = (batcher_t *) malloc(sizeof(batcher_t));
    if(unlikely(!region->batcher)) {
        free(region);
        return invalid_shared;
    }
    ret = batcher_init(region->batcher);
    if(ret == false) {
        free(region->batcher);
        free(region);
        return invalid_shared;
    }

    // allocate and initialize 1st segment in shared memory region
    region->segment = (segment_t *) malloc(sizeof(segment_t));
    if(unlikely(!region->segment)) {
        free(region->batcher);
        free(region);
        return invalid_shared;
    }
    
    // allocate freed segment array (to track freed segment indexes)
    region->freed_segment_index = (int *) malloc(INIT_FREED_SEG_SIZE * sizeof(int));
    if(region->freed_segment_index == NULL) {
        free(region->segment);
        free(region->batcher);
        free(region);
    }

    // init lock
    if(unlikely(!lock_init(&(region->freed_segment_index_lock)))) {
        free(region->freed_segment_index);
        free(region->segment);
        free(region->batcher);
        free(region);
        return invalid_shared;
    }

    // init to all occupied (-1)
    for(int i = 0; i < INIT_FREED_SEG_SIZE; i++) {
        atomic_store(&region->freed_segment_index[i], -1);
    }

    if(unlikely(!segment_init(region->segment, -1, size, align_alloc))) {
        lock_cleanup(&(region->freed_segment_index_lock));
        free(region->freed_segment_index);
        free(region->segment);
        free(region->batcher);
        free(region);
        return invalid_shared;
    }

        

    region->start = encode_segment_address(0);

    region->first_seg_size = size;
    region->align = align;
    region->align_alloc = align_alloc;
    region->current_segment_index = 1;

    atomic_store(&region->current_segment_index, 1);

    return region;
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
**/
void tm_destroy(shared_t shared)
{
    region_t *region = (region_t *) shared;

    // free batcher
    free(region->batcher);

    // free segment
    for (int i = 0; i < region->segment.len(); i++) {
        segment_t seg = region->segment[i];
        free(seg.copy_0);
        free(seg.copy_1);
        free(seg.read_only_copy);
        free(seg.write_tx);
        free(seg.is_written_in_epoch);
    }

    free(region->segment);
    free(region);
}

/** [thread-safe] Return the start address of the first allocated segment in the shared memory region.
 * @param shared Shared memory region to query
 * @return Start address of the first allocated segment
**/
void *tm_start(shared_t shared)
{
    // error check
    region_t *region = (region_t *) shared;

    return region->start;
}

/** [thread-safe] Return the size (in bytes) of the first allocated segment of the shared memory region.
 * @param shared Shared memory region to query
 * @return First allocated segment size
**/
size_t tm_size(shared_t shared)
{
    return ((region_t*) shared)->first_seg_size;

}

/** [thread-safe] Return the alignment (in bytes) of the memory accesses on the given shared memory region.
 * @param shared Shared memory region to query
 * @return Alignment used globally
**/
size_t tm_align(shared_t shared)
{
    return ((region_t*) shared)->align;
}

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
**/
tx_t tm_begin(shared_t shared, bool is_ro)
{
    region_t *region = (region_t *) shared;

    // enter batcher
    enter(region->batcher);

    // check failure in transactions realloc
    if(region->batcher->running_tx == NULL) {
            fprint("ERROR in realloc\n");
            return invalid_tx;
    }

    // create new tx element (get and add 1)
    int tx_index = atomic_fetch_add(&region->current_transaction_id, 1);

    region->batcher->running_tx[tx_index].tx_id = tx_index;
    region->batcher->running_tx[tx_index].is_ro = is_ro;

    return (uintptr_t) tx_index;
}

/** [thread-safe] End the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
**/
bool tm_end(shared_t shared as(unused), tx_t tx as(unused))
{
    // TODO: tm_end(shared_t, tx_t)

    // TODO: commit all read-write transactions only when the last tx exit the batcher
    // using a parallel array of segments for newly created segments
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

    // COMMENT: to read the segment at index i (passed), use index a = min(i, len(segments))
    // and if in a not present segment with id = i, decrease a and repeat
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
alloc_t tm_alloc(shared_t shared, tx_t tx, size_t size, void **target)
{
    region_t *region = (region_t *) shared;
    segment_t segment;
    int new_size;
    int index = -1;

    // check correct alignment of size
    if (size <= 0 || size % region->align != 0) {
        fprint("tm_alloc: incorrect size\n");
        return abort_alloc;
    }

    // init segment
    if(!segment_init(&segment, tx, size, region->align_alloc)) {
        fprint("tm_alloc: segment init failed\n");
        return nomem_alloc;
    }

    // check if there is a shared index for segment
    lock_acquire(&region->freed_segment_index_lock);
    for (int i = 0; i < sizeof(region->freed_segment_index) / sizeof(int); i++) {
        if(region->freed_segment_index[i] != -1) {
            index = region->freed_segment_index[i];
            region->freed_segment_index[i] == -1;
        }
    }
    lock_release(&region->freed_segment_index_lock);
    
    // if no index found in freed, calculate new one
    if(index == -1) {
        index = atomic_fetch_add(&region->current_segment_index, 1);

        // check if need to realloc segment array (through index)
        // TODO probably need lock for all reallocs
        if(index >= sizeof(region->segment) / sizeof(segment_t)) {
            region->segment = realloc(region->segment, 2 * sizeof(region->segment) / sizeof(segment_t));
            if(region->segment == NULL) {
                fprint("tm_alloc: segment array realloc failed\n");
                return nomem_alloc;
            }
        }

    }

    // insert segment into segment array
    region->segment[index] = segment;

    // return encoded address to segment
    target = encode_segment_address(index);

    return success_alloc;
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


