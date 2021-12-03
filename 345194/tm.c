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

// -------------------------------------------------------------------------- //

/** External headers **/
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <malloc.h>

/** Internal headers **/
#include <tm.h>
#include <my_tm.h>

#define PRINT_DEBUG false

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

/** Mutex lock functions (with simple abstraction) **/

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
/** Batcher functions.
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

/** Initialization function for the batcher.
 * @param batcher Batcher to be initialized
 * @return Boolean value for success or failure
**/ 
bool batcher_init(batcher_t *batcher)
{
    batcher->counter = 0;
    batcher->remaining = 0;
    batcher->blocked_count = 0;
    batcher->running_tx = NULL;
    batcher->num_running_tx = 0; // default is 0
    batcher->no_read_write_tx = true;

    // init lock
    if(unlikely(!lock_init(&(batcher->lock)))) {
        fprintf(stderr, "batcher_init: error in alloc lock\n");
        return false;
    }

    // init conditional variable
    if(pthread_cond_init(&(batcher->cond_var), NULL) != 0) {
        lock_cleanup(&(batcher->lock));
        fprintf(stderr, "batcher_init: error in alloc cond_var\n");
        return false;
    }

    return true;
}

/** Simple getter function for getting the current epoch number.
 * @param batcher Batcher instance
 * @return Number of current epoch (int)
**/
int get_epoch(batcher_t *batcher)
{
    return batcher->counter;
}

/** Enter in the critical section, or wait until woken up.
 * @param batcher Batcher instance
**/
void enter(batcher_t *batcher)
{
    lock_acquire(&batcher->lock);
    if(batcher->remaining  == 0) {
        // here only the first transaction of the STM enters
        batcher->remaining = 1;
        batcher->num_running_tx = batcher->remaining;

        // as it's the first, we need to allocate the array of running_tx in batcher
        batcher->running_tx = (transaction_t *) malloc(sizeof(transaction_t));
    } else {
        batcher->blocked_count++;
        pthread_cond_wait(&batcher->cond_var, &batcher->lock.mutex);
    }
    lock_release(&batcher->lock);
    // sleep(1);
    return;
}

/** Leave critical section, and if you are the last thread wake up waiting threads.
 * @param batcher Batcher instance
**/
void leave(batcher_t *batcher, region_t *region, tx_t tx)
{
    lock_acquire(&batcher->lock);
    batcher->remaining--;

    #if PRINT_DEBUG == true
    printf("<<leave>> (tx = %ld): start\n", tx);
    #endif

    if(batcher->remaining == 0) {
        batcher->counter++;
        batcher->remaining = batcher->blocked_count;

        // realloc transactions array with new number of transactions
        if (batcher->remaining == 0) {
            free(batcher->running_tx);
        } else {
            batcher->running_tx = (transaction_t *) realloc(batcher->running_tx, batcher->remaining * sizeof(transaction_t));
        }

        batcher->num_running_tx = batcher->remaining;
        
        // commit all transactions
        commit_tx(region, tx);

        batcher->blocked_count = 0;
        batcher->no_read_write_tx = true;

        #if PRINT_DEBUG == true
        printf("-------STARTING EPOCH %d (num tx = %d)-------\n", get_epoch(batcher), batcher->remaining);
        #endif

        pthread_cond_broadcast(&batcher->cond_var);
        // pthread_cond_signal(&batcher->cond_var);
    } else {
        // TODO do something about committing transaction (maybe not):
        // - set alloc segment to persistent
        // - set modified words to done (maybe)
    }

    lock_release(&batcher->lock);
    return;
}

/** Clean up batcher instance.
 * @param batcher Batcher instance to be cleaned up
**/
void batcher_cleanup(batcher_t *batcher)
{
    lock_cleanup(&(batcher->lock));
    pthread_cond_destroy(&(batcher->cond_var));
}

// -------------------------------------------------------------------------- //
/** Extra functions
 * used throughout the whole STM library
**/

/** Init a segment for a region.
 * @param segment Segment of shared memory region
 * @param size Size of segment
 * @param align_alloc Alignment of words in segment
 * @return Boolean for success or failure
**/
bool segment_init(segment_t *segment, tx_t tx, size_t size, size_t align_alloc)
{
    segment->num_words = size / (align_alloc); // NOT /2 because we still want the correct total size
    segment->word_size = align_alloc;
    segment->created_by_tx = tx;
    atomic_store(&segment->to_delete, INVALID_TX);

    // int copy_size = segment->num_words * segment->word_size;

    // alloc words in segment
    segment->copy_0 = (void *) calloc(segment->num_words, align_alloc);
    if(unlikely(!segment->copy_0)) {
        fprintf(stderr, "segment_init: error in calloc copy_0\n");
        return false;
    }
    segment->copy_1 = (void *) calloc(segment->num_words, align_alloc);
    if(unlikely(!segment->copy_1)) {
        free(segment->copy_0);
        fprintf(stderr, "segment_init: error in calloc copy_1\n");
        return false;
    }

    // initialize words in segment with all zeros
    // if (unlikely(posix_memalign(&(segment->copy_0), align_alloc, size) != 0)) {
    //     return false;
    // }
    // if (unlikely(posix_memalign(&(segment->copy_1), align_alloc, size) != 0)) {
    //     return false;
    // }
    // memset(segment->copy_0, 0, size);
    // memset(segment->copy_1, 0, size);


    // init supporting data structure for words (to 0)
    segment->read_only_copy = (int *) calloc(segment->num_words, sizeof(int));
    if(unlikely(!segment->read_only_copy)) {
        free(segment->copy_0);
        free(segment->copy_1);
        fprintf(stderr, "segment_init: error in calloc read_only_copy\n");
        return false;
    }

    // allocate access set and init to -1
    segment->access_set = (tx_t *) malloc(segment->num_words * sizeof(tx_t));
    if(unlikely(!segment->access_set)) {
        free(segment->copy_0);
        free(segment->copy_1);
        free(segment->read_only_copy);
        fprintf(stderr, "segment_init: error in calloc access_set\n");
        return false;
    }
    // TODO change with memset
    for (int i = 0; i < (int) segment->num_words; i++) {
        segment->access_set[i] = INVALID_TX;
    }
    // memset(segment->access_set, (tx_t) INVALID_TX, segment->num_words * sizeof(tx_t));

    // allocate and init to false array of boolean flags indicating if a word has been written in the epoch
    segment->is_written_in_epoch = (bool *) malloc(segment->num_words * sizeof(bool));
    if(unlikely(!segment->is_written_in_epoch)) {
        free(segment->copy_0);
        free(segment->copy_1);
        free(segment->read_only_copy);
        free(segment->access_set);
        fprintf(stderr, "segment_init: error in alloc is_written_in_epoch\n");
        return false;
    }
    // TODO change with memset (0) or calloc directly
    // for (int i = 0; i < (int)segment->num_words; i++) {
    //     segment->is_written_in_epoch[i] = false;
    // }
    memset(segment->is_written_in_epoch, 0, segment->num_words * sizeof(bool));

    // allocate and init array of locks for words
    segment->word_locks = (lock_t *) malloc(segment->num_words * sizeof(lock_t));
    if(unlikely(!segment->is_written_in_epoch)) {
        free(segment->is_written_in_epoch);
        free(segment->copy_0);
        free(segment->copy_1);
        free(segment->read_only_copy);
        free(segment->access_set);
        fprintf(stderr, "segment_init: error in alloc word_locks\n");
        return false;
    }
    for(int i = 0; i < (int)segment->num_words; i++) {
        if(unlikely(!lock_init(&(segment->word_locks[i])))) {
            free(segment->word_locks);
            free(segment->is_written_in_epoch);
            free(segment->copy_0);
            free(segment->copy_1);
            free(segment->read_only_copy);
            free(segment->access_set);
            fprintf(stderr, "segment_init: error in lock_init\n");
            return false;
        }
    }
    return true;
}

/** Reset an already init segment for a region.
 * @param segment Segment of shared memory region
 * @param size Size of segment
 * @param align_alloc Alignment of words in segment
 * @return Boolean for success or failure
**/
bool soft_segment_init(segment_t *segment, tx_t tx, size_t size, size_t align_alloc)
{
    segment->num_words = size / (align_alloc); // NOT /2 because we still want the correct total size
    segment->word_size = align_alloc;
    segment->created_by_tx = tx;
    atomic_store(&segment->to_delete, INVALID_TX);

    // initialize words in segment with all zeros
    memset(segment->copy_0, 0, size);
    memset(segment->copy_1, 0, size);

    // init supporting data structure for words (to 0)
    memset(segment->read_only_copy, 0, segment->num_words * sizeof(int));
    
    // init access set to -1
    for (int i = 0; i < (int) segment->num_words; i++) {
        segment->access_set[i] = INVALID_TX;
    }
    // memset(segment->access_set, (tx_t) INVALID_TX, segment->num_words * sizeof(tx_t));

    // allocate and init to false array of boolean flags indicating if a word has been written in the epoch
    // TODO change with memset (0) or calloc directly
    // for (int i = 0; i < (int)segment->num_words; i++) {
    //     segment->is_written_in_epoch[i] = false;
    // }
    memset(segment->is_written_in_epoch, 0, segment->num_words * sizeof(bool));
    
    return true;
}

/** Encode segment number into an opaque pointer address.
 * @param segment_num number of segment
 * @return address
**/
void *encode_segment_address(int segment_num)
{
    // address is NUM_SEGMENT << 24 + offset word
    // << means shift left
    intptr_t addr = (segment_num << SEGMENT_SHIFT) + 1;
    return (void *)addr;
}

/** Decode opaque pointer into segment and word number.
 * @param addr opaque pointer
 * @param num_segment pointer to segment number
 * @param num_word pointer to word number
 * @return address
**/
void decode_segment_address(void const *addr, int *num_segment, int *num_word)
{
    intptr_t num_s, num_w;

    // calculate word and segment number
    num_s = (intptr_t) addr >> SEGMENT_SHIFT;
    intptr_t difference = num_s << SEGMENT_SHIFT;
    num_w = (intptr_t) addr - difference;

    *num_segment = num_s;
    *num_word = num_w;
}

// -------------------------------------------------------------------------- //

/** Create (i.e. allocate + init) a new shared memory region, with one first non-free-able allocated segment of the requested size and alignment.
 * @param size  Size of the first shared segment of memory to allocate (in bytes), must be a positive multiple of the alignment
 * @param align Alignment (in bytes, must be a power of 2) that the shared memory region must support
 * @return Opaque shared memory region handle, 'invalid_shared' on failure
**/
shared_t tm_create(size_t size, size_t align)
{
    bool ret;

    #if PRINT_DEBUG == true
    printf("<<tm_create>> (size = %ld): start\n", size);
    #endif

    // allocate shared memory region
    region_t *region = (region_t *) malloc(sizeof(region_t));
    if(unlikely(!region)) {
        fprintf(stderr, "tm_create: error in alloc region\n");
        return invalid_shared;
    }

    // calculate alignment for the shared memory region
    size_t align_alloc = align < sizeof(void*) ? sizeof(void*) : align;

    // allocate and initialize batcher for shared memory region
    region->batcher = (batcher_t *) malloc(sizeof(batcher_t));
    if(unlikely(!region->batcher)) {
        free(region);
        fprintf(stderr, "tm_create: error in alloc batcher\n");
        return invalid_shared;
    }
    ret = batcher_init(region->batcher);
    if(ret == false) {
        free(region->batcher);
        free(region);
        fprintf(stderr, "tm_create: error in batcher_init\n");
        return invalid_shared;
    }

    // allocate and initialize 1st segment in shared memory region
    region->segment = (segment_t *) malloc(INIT_SEG_SIZE * sizeof(segment_t));
    if(unlikely(!region->segment)) {
        lock_cleanup(&(region->batcher->lock));
        pthread_cond_destroy(&(region->batcher->cond_var));
        free(region->batcher);
        free(region);
        fprintf(stderr, "tm_create: error in alloc segment\n");
        return invalid_shared;
    }
    
    // allocate freed segment array (to track freed segment indexes)
    region->freed_segment_index = (int *) malloc(INIT_FREED_SEG_SIZE * sizeof(int));
    if(region->freed_segment_index == NULL) {
        lock_cleanup(&(region->batcher->lock));
        pthread_cond_destroy(&(region->batcher->cond_var));
        free(region->segment);
        free(region->batcher);
        free(region);
        fprintf(stderr, "tm_create: error in alloc freed_segment_index\n");
        return invalid_shared;
    }
    // init to all occupied (-1)
    // TODO check if's correct (together with how we increase the current_segment_index and the *segment)
    // for(int i = 0; i < INIT_FREED_SEG_SIZE; i++) {
    //     atomic_store(&region->freed_segment_index[i], -1);
    // }
    memset(region->freed_segment_index, -1, INIT_FREED_SEG_SIZE * sizeof(int));

    // init lock array of freed segments
    if(unlikely(!lock_init(&(region->segment_lock)))) {
        lock_cleanup(&(region->batcher->lock));
        pthread_cond_destroy(&(region->batcher->cond_var));
        free(region->freed_segment_index);
        free(region->segment);
        free(region->batcher);
        free(region);
        fprintf(stderr, "tm_create: error in lock_init_1\n");
        return invalid_shared;
    }


    // initialize first segment
    if(unlikely(!segment_init(&region->segment[0], -1, size, align_alloc))) {
        lock_cleanup(&(region->batcher->lock));
        pthread_cond_destroy(&(region->batcher->cond_var));
        lock_cleanup(&(region->segment_lock));
        free(region->freed_segment_index);
        free(region->segment);
        free(region->batcher);
        free(region);
        fprintf(stderr, "tm_create: error in segment_init\n");
        return invalid_shared;
    }    

    region->start = encode_segment_address(0);

    region->first_seg_size = size;
    region->align = align;
    region->align_alloc = align_alloc;
    region->num_alloc_segments = INIT_SEG_SIZE;

    region->current_segment_index = 1;
    atomic_store(&region->current_transaction_id, (tx_t)1);

    #if PRINT_DEBUG == true
    printf("<<tm_create>>: created region successful!\n");
    #endif

    return region;
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
**/
void tm_destroy(shared_t shared)
{
    region_t *region = (region_t *) shared;

    #if PRINT_DEBUG == true
    printf("<<tm_destroy>> start\n");
    #endif

    printf("max segment number = %d\n", region->current_segment_index);
    // cleanup and free batcher
    batcher_cleanup(region->batcher);
    free(region->batcher);

    // free segment and related
    for (int i = 0; i < region->current_segment_index; i++) {
        segment_t seg = region->segment[i];
        free(seg.copy_0);
        free(seg.copy_1);
        free(seg.read_only_copy);
        free(seg.access_set);
        free(seg.is_written_in_epoch);
        for(int j = 0; j < (int)seg.num_words; j++) {
            lock_cleanup(&(seg.word_locks[j]));
        }
        free(seg.word_locks);
    }
    free(region->segment);
    free(region->freed_segment_index);

    // locks clean-up
    lock_cleanup(&(region->segment_lock));

    free(region);

    #if PRINT_DEBUG == true
    printf("<<tm_destroy>>: destroyed region successful\n");
    #endif
}

/** [thread-safe] Return the start address of the first allocated segment in the shared memory region.
 * @param shared Shared memory region to query
 * @return Start address of the first allocated segment
**/
void *tm_start(shared_t shared)
{
    #if PRINT_DEBUG == true
    printf("<<tm_start>>: start\n");
    #endif

    return ((region_t*) shared)->start;
}

/** [thread-safe] Return the size (in bytes) of the first allocated segment of the shared memory region.
 * @param shared Shared memory region to query
 * @return First allocated segment size
**/
size_t tm_size(shared_t shared)
{
    #if PRINT_DEBUG == true
    printf("<<tm_size>>: start\n");
    #endif
    
    return ((region_t*) shared)->first_seg_size;

}

/** [thread-safe] Return the alignment (in bytes) of the memory accesses on the given shared memory region.
 * @param shared Shared memory region to query
 * @return Alignment used globally
**/
size_t tm_align(shared_t shared)
{
    #if PRINT_DEBUG == true
    printf("<<tm_align>>: start\n");
    #endif

    return ((region_t*) shared)->align_alloc;
}

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
**/
tx_t tm_begin(shared_t shared, bool is_ro)
{
    region_t *region = (region_t *) shared;
    tx_t tx_index;

    #if PRINT_DEBUG == true
    printf("<<tm_begin>>: start\n");
    #endif

    // enter batcher
    enter(region->batcher);

    if (!is_ro) {
        region->batcher->no_read_write_tx = false;
    }

    // check failure in transactions realloc
    if(region->batcher->running_tx == NULL) {
        printf("tm_begin: error in realloc\n");
        return invalid_tx;
    }

    // create new tx element (get and add 1)
    // need to mod the index as it's always increasing
    tx_index = atomic_fetch_add(&region->current_transaction_id, 1) % region->batcher->num_running_tx;

    region->batcher->running_tx[tx_index].tx_id = tx_index; // TODO can be removed
    region->batcher->running_tx[tx_index].is_ro = is_ro;

    #if PRINT_DEBUG == true
    printf("<<tm_begin>> (tx = %ld): end\n", tx_index);
    #endif

    return tx_index;
}

/** [thread-safe] End the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
**/
bool tm_end(shared_t shared, tx_t tx)
{
    region_t *region = (region_t *) shared;

    #if PRINT_DEBUG == true
    printf("<<tm_end>> (tx = %ld): start\n", tx);
    #endif

    leave(region->batcher, region, tx);

    #if PRINT_DEBUG == true
    printf("<<tm_end>> (tx = %ld): end\n", tx);
    #endif

    // TODO: commit all read-write transactions only when the last tx exit the batcher
    // using a parallel array of segments for newly created segments
    return true;
}

/** [thread-safe] Read operation in the given transaction, source in the shared region and target in a private region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in the shared region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in a private region)
 * @return Whether the whole transaction can continue
**/
bool tm_read(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
    region_t *region = (region_t *) shared;
    segment_t *segment;
    int segment_index;
    int word_index;
    int num_words_to_read;
    alloc_t result;
    int offset;
    bool is_ro;
    void *my_source;

    #if PRINT_DEBUG == true
    printf("<<tm_read>> (tx = %ld, source = %p, target = %p): start\n", tx, source, target);
    #endif

    // get transaction type
    is_ro = region->batcher->running_tx[tx].is_ro;

    /**SANITY CHECKS**/
    // check size, must be multiple of the shared memory region’s alignment, otherwise the behavior is undefined.
    if (size <= 0 || size % region->align_alloc != 0) {
        fprintf(stderr, "tm_read: incorrect size\n");
        if (!is_ro) {
            abort_tx(region, tx);
        }
        return false; // abort_tx
    }

    // align target (because grading don't want address starting from 0x0)
    my_source = source - 1;

    // retrieve segment and word number
    decode_segment_address(my_source, &segment_index, &word_index);

    // check that source and target addresses are a positive multiple of the shared memory region’s alignment, otherwise the behavior is undefined.
    if(word_index % region->align_alloc != 0 || (uintptr_t)my_source % region->align_alloc != 0 || segment_index > region->current_segment_index) {
        fprintf(stderr, "tm_read: incorrect target or source buffer alignment\n");
        if (!is_ro) {
            abort_tx(region, tx);
        }
        return false; // abort_tx
    }
    
    word_index = word_index / region->segment[segment_index].word_size;

    // check address correctness
    if(segment_index < 0 || word_index < 0) {
        fprintf(stderr, "tm_read: incorrect indexes\n");
        if (!is_ro) {
            abort_tx(region, tx);
        }
        return false; // abort_tx
    }

    // TODO ask in class
     // check size source and target, must be at least size, otherwise the behavior is undefined.
    /** if (malloc_usable_size(target) < size) {
        fprintf(stderr, "tm_read: incorrect target buffer size\n");
        if (!is_ro) {
            abort_tx(region, tx);
        }
        return false; // abort_tx
    }
    if(malloc_usable_size(region->segment[segment_index].copy_0) < size) {
        fprintf(stderr, "tm_read: incorrect source buffer size\n");
        if (!is_ro) {
            abort_tx(region, tx);
        }
        return false; // abort_tx
    } 
    **/

    /**READ OPERATION**/

    // calculate number of words to be read in segment
    num_words_to_read = size / region->align_alloc;
    // get segment
    segment = &region->segment[segment_index];
    // loop through all word indexes (starting from the passed one)
    for (int curr_word_index = word_index; curr_word_index < word_index + num_words_to_read; curr_word_index++) {
        offset = (curr_word_index - word_index) * segment->word_size;
        result = read_word(curr_word_index, target + (offset), segment, is_ro, tx);
        if(result == abort_alloc) {
            abort_tx(region, tx);

            #if PRINT_DEBUG == true
            printf("tm_read: abort_tx tx reading\n");
            #endif
            
            return false; // abort_tx
        }
    }

    #if PRINT_DEBUG == true
    printf("<<tm_read>> (tx = %ld): end\n", tx);
    #endif

    return true;
}

/** [thread-safe] Read word operation.
 * @param word_index Index of word into consideration
 * @param target Target start address (in a private region)
 * @param segment Pointer to the segment into consideration
 * @param is_ro Is read-only flag (about tx)
 * @param tx Current transaction identifier
 * @return Whether the whole transaction can continue (success/nomem), or not (abort_alloc)
**/
alloc_t read_word(int word_index, void *target, segment_t *segment, bool is_ro, tx_t tx)
{
    int readable_copy;

    #if PRINT_DEBUG == true
    printf("<<read_word>> (tx = %ld, ro = %d, index = %d): start\n", tx, (int)is_ro, word_index);
    #endif

    // find readable copy
    readable_copy = segment->read_only_copy[word_index];

    // if read-only
    if(is_ro == true) {
        // perform read operation into target
        if(readable_copy == 0) {
            memcpy(target, segment->copy_0 + (word_index * segment->word_size), segment->word_size);
        } else {
            memcpy(target, segment->copy_1 + (word_index * segment->word_size), segment->word_size);
        }

        return success_alloc;
    } else {
        // if read-write

        // acquire word lock
        lock_acquire(&segment->word_locks[word_index]);

        // if word written in current epoch
        if(segment->is_written_in_epoch[word_index] == true) {

            // release word lock
            lock_release(&segment->word_locks[word_index]);

            // if transaction in access set
            if(segment->access_set[word_index] == tx) {
                // read write copy into target
                if(readable_copy == 0) {
                    memcpy(target, segment->copy_1 + (word_index * segment->word_size), segment->word_size);
                } else {
                    memcpy(target, segment->copy_0 + (word_index * segment->word_size), segment->word_size);
                }
                return success_alloc;
            } else {
                return abort_alloc;
            }
        } else {
            if(readable_copy == 0) {
                memcpy(target, segment->copy_0 + (word_index * segment->word_size), segment->word_size);
            } else {
                memcpy(target, segment->copy_1 + (word_index * segment->word_size), segment->word_size);
            }

            // add tx into access set (only the first tx)
            if (segment->access_set[word_index] == INVALID_TX) {
                segment->access_set[word_index] = tx;
            }

            // release word lock
            lock_release(&segment->word_locks[word_index]);

            return success_alloc;
        }
    }
}

/** [thread-safe] Write operation in the given transaction, source in a private region and target in the shared region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in a private region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in the shared region)
 * @return Whether the whole transaction can continue
**/
bool tm_write(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
    region_t *region = (region_t *) shared;
    segment_t *segment;
    int segment_index;
    int word_index;
    int num_words_to_write;
    alloc_t result;
    int offset;
    void *my_source = source; // not const

    #if PRINT_DEBUG == true
    printf("<<tm_write>> (tx = %ld, source = %p, target = %p): start\n", tx, source, target);
    #endif
    
    /**SANITY CHECKS**/

    // check size, must be multiple of the shared memory region’s alignment, otherwise the behavior is undefined.
    if (size <= 0 || size % region->align_alloc != 0) {
        fprintf(stderr, "tm_write: incorrect size\n");
        abort_tx(region, tx);
        return false; // abort_tx
    }

    // align target (because grading don't want address starting from 0x0)
    target = target - 1;

    // retrieve segment and word number
    decode_segment_address(target, &segment_index, &word_index);

    // check that source and target addresses are a positive multiple of the shared memory region’s alignment, otherwise the behavior is undefined.
    if(word_index % region->align_alloc != 0 || (uintptr_t)target % region->align_alloc != 0) {
        fprintf(stderr, "tm_write: incorrect target or source buffer alignment\n");
        abort_tx(region, tx);
        return false; // abort_tx
    }

    // calculate correct index (before it was * word_size)
    word_index = word_index / region->segment[segment_index].word_size;

    // check address correctness
    if(segment_index < 0 || word_index < 0) {
        fprintf(stderr, "tm_write: incorrect indexes\n");
        abort_tx(region, tx);
        return false; // abort_tx
    }


    /** TODO ask in class how to check this condition
     * here malloc_usable_size(target) always return 0 (not correct)
    
    // check size source and target, must be at least size, otherwise the behavior is undefined.

    if (malloc_usable_size(my_source) < size) {
        fprintf(stderr, "tm_write: incorrect source buffer size\n");
        abort_tx(region, tx);
        return false; // abort_tx
    }
    printf("HERE\n");
    if(malloc_usable_size(region->segment[segment_index].copy_0) < size) {
        fprintf(stderr, "tm_write: incorrect target buffer size\n");
        abort_tx(region, tx);
        return false; // abort_tx
    }
    **/
    

    /**WRITE OPERATION**/

    // calculate number of words to be read in segment
    num_words_to_write = size / region->align_alloc;

    // get segment
    segment = &region->segment[segment_index];

    // loop thorugh all word indexes (starting from the passed one)
    for (int curr_word_index = word_index; curr_word_index < word_index + num_words_to_write; curr_word_index++) {
        offset = (curr_word_index - word_index) * segment->word_size;
        
        result = write_word(curr_word_index, my_source + (offset), segment, tx);
        if(result == abort_alloc) {
            abort_tx(region, tx);

            #if PRINT_DEBUG == true
            printf("tm_write (tx = %ld): abort writing (word index = %d, segment index = %d, my source = %p)\n", tx, curr_word_index, segment_index, my_source);
            #endif

            return false; // abort_tx
        }
    }

    #if PRINT_DEBUG == true
    printf("<<tm_write>> (tx = %ld): end\n", tx);
    #endif

    
    return true;
}

/** [thread-safe] Write word operation.
 * @param word_index Index of word into consideration
 * @param source Source start address (in a private region)
 * @param segment Pointer to the segment into consideration
 * @param tx Current transaction identifier
 * @return Whether the whole transaction can continue (success/nomem), or not (abort_alloc)
**/
alloc_t write_word(int word_index, void *source, segment_t *segment, tx_t tx)
{
    int readable_copy;

    #if PRINT_DEBUG == true
    printf("<<write_word>> (tx = %ld, word_index = %d, source = %p): start\n", tx, word_index, source);
    #endif

    readable_copy = segment->read_only_copy[word_index];

    // acquire word lock
    lock_acquire(&segment->word_locks[word_index]);
    
    // if word has been written before
    if(segment->is_written_in_epoch[word_index] == true) {
        // release word lock
        lock_release(&segment->word_locks[word_index]);

        // if tx in the access set
        if (segment->access_set[word_index] == tx) {
            // write source into write copy
            if(readable_copy == 0) {
                memcpy(segment->copy_1 + (word_index * segment->word_size), source, segment->word_size);
            } else {
                memcpy(segment->copy_0 + (word_index * segment->word_size), source, segment->word_size);
            }

            return success_alloc;
        } else {
            return abort_alloc;
        }
    } else {
        // if one other tx in access set
        if(segment->access_set[word_index] != tx && segment->access_set[word_index] != INVALID_TX) {
            lock_release(&segment->word_locks[word_index]);
            return abort_alloc;
        } else {
            // write source into write copy
            if(readable_copy == 0) {
                memcpy(segment->copy_1 + (word_index * segment->word_size), source, segment->word_size);
            } else {
                memcpy(segment->copy_0 + (word_index * segment->word_size), source, segment->word_size);
            }

            // add tx into access set
            if(segment->access_set[word_index] == INVALID_TX) {
                segment->access_set[word_index] = tx;
            }

            // mark word as it has been written
            segment->is_written_in_epoch[word_index] = true;

            lock_release(&segment->word_locks[word_index]);

            return success_alloc;
        }
    }

}

/** [thread-safe] Memory allocation in the given transaction. (should be called a lot)
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
    int index = -1;

    #if PRINT_DEBUG == true
    printf("<<tm_alloc>> (tx = %ld): start\n", tx);
    #endif
    
    // check correct alignment of size
    if (size <= 0 || size % region->align_alloc != 0) {
        fprintf(stderr, "tm_alloc: incorrect size\n");
        abort_tx(region, tx);
        return abort_alloc; // abort_tx
    }

    
    // check if there is a shared index for segment
    lock_acquire(&(region->segment_lock));
    for (int i = 0; i < region->current_segment_index; i++) {
        if(region->freed_segment_index[i] != -1) {
            index = i;
            break;
        }
    }
    // if no index found in freed, calculate new one
    if(index == -1) {
        // fetch and increment
        index = region->current_segment_index;
        region->current_segment_index++;

        // acquire realloc lock
        if(index >= region->num_alloc_segments) {
            region->segment = (segment_t *) realloc(region->segment, sizeof(segment_t) * 2 * region->num_alloc_segments);
            region->freed_segment_index = (int *) realloc(region->freed_segment_index, sizeof(int) * 2 * region->num_alloc_segments);
            if(region->segment == NULL || region->freed_segment_index == NULL) {
                // release realloc lock
                // fprintf(stderr, "tm_alloc: segment or freed_segment_index array realloc failed\n");
                abort_tx(region, tx);
                return nomem_alloc;
            }

            // update number of allocated segments
            region->num_alloc_segments *= 2;
        }

        // init segment
        if(!segment_init(&segment, tx, size, region->align_alloc)) {
            fprintf(stderr, "tm_alloc: segment init failed\n");
            return nomem_alloc;
        }

        // insert segment into segment array and set freed to occupied
        region->segment[index] = segment;

    } else {
        // init segment
        if(!soft_segment_init(&region->segment[index], tx, size, region->align_alloc)) {
            fprintf(stderr, "tm_alloc: soft segment init failed\n");
            return nomem_alloc;
        }
    }

    // update support array freed_segment_index
    region->freed_segment_index[index] = -1;

    lock_release(&(region->segment_lock));

    // return encoded address to segment
    *target = encode_segment_address(index);

    #if PRINT_DEBUG == true
    printf("<<tm_alloc>> (tx = %ld, target = %p): end\n", tx, *target);
    #endif

    return success_alloc;
}

/** [thread-safe] Memory freeing in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param target Address of the first byte of the previously allocated segment to deallocate
 * @return Whether the whole transaction can continue
**/
bool tm_free(shared_t shared, tx_t tx, void *target)
{
    int segment_index;
    int word_index;
    unsigned long int cmp_val = INVALID_TX;
    region_t *region = (region_t *) shared;

    #if PRINT_DEBUG == true
    printf("<<tm_free>> (tx = %ld, target = %p): start\n", tx, target);
    #endif

    // align target (because grading don't want address starting from 0x0)
    target = target - 1;

    // retrieve segment and word number
    decode_segment_address(target, &segment_index, &word_index);
    
    // check address correctness (can't free 1st segment or an address which is not pointing to the 1st word)
    if(segment_index == 0 || word_index != 0) {
        fprintf(stderr, "tm_free: incorrect indexes\n");
        abort_tx(region, tx);
        return false; // abort_tx
    }

    // free (set to tx to_delete) segment from array of segments
    atomic_compare_exchange_strong(&region->segment[segment_index].to_delete, &cmp_val, tx); // 0 should be a pointer to a value

    // abort_tx concurrent transactions (or sequentials) which called free on segment after some other transaction
    if(region->segment[segment_index].to_delete != tx) {

        #if PRINT_DEBUG == true
        printf("tm_free: tx has already been freed\n");
        #endif

        abort_tx(region, tx);
        return false; // abort_tx
    }

    // add freed segment index to array of freed segment indexes (NO, TODO: do during commit when the last transaction exit the batcher)
    // TODO: if a transaction abort_tx, it has to set all to_delete to 0 (for their tx value)

    #if PRINT_DEBUG == true
    printf("<<tm_free>> (tx = %ld): end\n", tx);
    #endif

    return true;
}

/** [thread-safe] abort_tx operations for a given transaction
 * @param region Shared memory region
 * @param tx Current transaction
**/
void abort_tx(region_t *region, tx_t tx)
{
    unsigned long int invalid_value = INVALID_TX;
    int max_segment_index;
    segment_t *segment;
    tx_t tx_tmp;

    #if PRINT_DEBUG == true
    printf("<<abort_tx>> (tx = %ld): start\n", tx);
    #endif

    // store current max segment index
    //lock_acquire(&(region->segment_lock));
    max_segment_index = region->current_segment_index;
    //lock_release(&(region->segment_lock));

    // for all segments
    for (int segment_index = 0; segment_index < max_segment_index && region->freed_segment_index[segment_index] == -1; segment_index++) {
        segment = &region->segment[segment_index];
        // unset segments on which tx has called tm_free previously (tmp is needed because of the atomic_compare_exchange, which perform if then else (check code at the bottom))
        tx_tmp = tx;
        atomic_compare_exchange_strong(&segment->to_delete, &tx, invalid_value); // tx should be a pointer to a value
        tx = tx_tmp;

        // add used segment indexes for tx to freed_segment_index array (for tm_alloc)
        if (segment->created_by_tx == tx) {
            region->freed_segment_index[segment_index] = segment_index;
            segment->created_by_tx = INVALID_TX;
            continue;
        }

        // roolback write operations on each word performed by tx
        for (int word_index = 0; word_index < (int) segment->num_words; word_index++) {
            if(segment->access_set[word_index] == tx && segment->is_written_in_epoch[word_index] == true) {
                lock_acquire(&segment->word_locks[word_index]);
                if(segment->read_only_copy[word_index] == 0) {
                    memcpy(segment->copy_1 + (word_index * segment->word_size), segment->copy_0 + (word_index * segment->word_size), segment->word_size);
                } else {
                    memcpy(segment->copy_0 + (word_index * segment->word_size), segment->copy_1 + (word_index * segment->word_size), segment->word_size);
                }
                segment->access_set[word_index] = INVALID_TX;
                segment->is_written_in_epoch[word_index] = false;
                lock_release(&segment->word_locks[word_index]);
            }

        }
    }

    // also aborting tx should leave the batcher
    leave(region->batcher, region, tx);

    #if PRINT_DEBUG == true
    printf("<<abort_tx>> (tx = %ld): end\n", tx);
    #endif
}

/** [thread-safe] commit operations for a given transaction
 * @param region Shared memory region
 * @param tx Current transaction
**/
void commit_tx(region_t *region, tx_t tx)
{
    segment_t *segment;

    #if PRINT_DEBUG == true
    printf("<<commit_tx>> (tx = %ld): start\n", tx);
    #endif

    if (region->batcher->no_read_write_tx) {
        return;
    }

    // for all segments
    for (int segment_index = 0; segment_index < region->current_segment_index && region->freed_segment_index[segment_index] == -1; segment_index++) {
        segment = &region->segment[segment_index];

        // add to freed_segment_index array segments which have been freed by tx
        if (segment->to_delete != INVALID_TX) {
            region->freed_segment_index[segment_index] = segment_index; // so freed
            continue;
        }

        // add used segment indexes for tx to freed_segment_index array
        if (segment->created_by_tx != INVALID_TX) {
            segment->created_by_tx = INVALID_TX;
        }

        // commit algorithm for words (copy written word copy into read-only copy)
        for (int word_index = 0; word_index < (int) segment->num_words; word_index++) {
            if(segment->is_written_in_epoch[word_index] == true) {
                // swap valid copy (in case it has been written)
                segment->read_only_copy[word_index] = (segment->read_only_copy[word_index] == 0 ? 1: 0);
                // empty access set
                segment->access_set[word_index] = INVALID_TX;
                // empty is_written_in_epoch flag
                segment->is_written_in_epoch[word_index] = false;

            } else if (segment->access_set[word_index] != INVALID_TX) {
                // empty access set
                segment->access_set[word_index] = INVALID_TX;

            }
        }
    }
    #if PRINT_DEBUG == true
    printf("<<commit_tx>> (tx = %ld): end\n", tx);
    #endif
}

// atomic_compare_exchange_strong explanation
/**
 * atomic_compare_exchange_strong(obj, expected, desired) {
 *  if(obj == expected) {
 *      obj = desired;
 *  } else {
 *      expected = obj;
 *  }
 * }
 */
/**
 * our_function(to_delete, tx, invalid_value) {
 *  if(to_delete = tx) {
 *      to_delete = invalid_value;
 *  } else {
 *      to_delete = to_delete;
 *      // what it does
 *      tx = to_delete
 *  }
 * }
 */