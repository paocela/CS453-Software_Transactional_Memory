/**
 * @file   my_tm.h
 * @author Paolo Celada <paolo.celada@epfl.ch>
 *
 * @section LICENSE
 *
 * Copyright © 2018-2019 Sébastien ROUAULT.
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
 * Additional functions, structures and constants
**/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

// -------------------------------------------------------------------------- //

/** Global constants **/
#define SEGMENT_SHIFT 24
#define INIT_FREED_SEG_SIZE 10 // better if it grows together with the segment array
#define INIT_SEG_SIZE 10 // set to 1 if you want reallocation of segments (not statically init)
#define INVALID_TX UINT_MAX

// -------------------------------------------------------------------------- //

/** Struct definition**/

/** Mutex structure
 * @param mutex mutex
**/
typedef struct lock_s {
    pthread_mutex_t mutex;
} lock_t;

/** Transaction characteristics.
 * @param counter Epoch counter
 * @param remaining Reaming transactions in batcher of current epoch
 * @param lock Lock for remaining counter
 * @param cond_var Conditional variable to control batcher
 * @param blocked_count Number of blocked transactions waiting for the next epoch
 * @param is_ro Array of bool to save if tx at certain index is read-only
 * @param num_running_tx Number of currently running transactions in epoch
 * @param no_read_write_tx bool to check if no read-write operation have been performed during epoch (for commit optimization)
**/
typedef struct batcher_s {
    int counter;
    int remaining;
    lock_t lock;
    pthread_cond_t cond_var;
    int blocked_count;
    bool *is_ro;
    int num_running_tx;
    bool no_read_write_tx;
} batcher_t;

/** segment structure (multiple per shared memory).
 * @param num_words Number of words in segment (TODO actully could be a global constant? maybe)
 * @param copy_0 Copy 0 of segments words (accessed shifting a pointer)
 * @param copy_1 Copy 1 of segments words (accessed shifting a pointer)
 * @param read_only_copy Array of flags to distinguish read-only copy
 * @param is_written_in_epoch Array of boolean to flag if the word has been written
 * @param access_set Array of read-write tx which have accessed the word (the first to access the word(read or write) will own it for the epoch)
 * @param word_size Size of the word
 * @param created_by_tx If -1 segment is shared, else it's temporary and must be deleted if tx abort
 * @param to_delete If set to some tx, the segment has to be deleted when the last transaction exit the batcher, rollback set to 0 if the tx rollback
 * @param has_been_modified Flag to track if segment has been modified in epoch
 * @param index_modified_words Array to store sequential indexes of accessed words in segment
 * @param cnt_index_modified_words Atomic counter incremented every time there is an operation on word 
**/
typedef struct segment_s {
    size_t num_words;
    void *copy_0;
    void *copy_1;
    int *read_only_copy; // all read it, except last tx who writes
    tx_t *access_set;
    bool *is_written_in_epoch;
    lock_t *word_locks;
    int word_size;
    tx_t created_by_tx; // in tm_alloc
    _Atomic(tx_t) to_delete; // in tm_free
    bool has_been_modified;
    int *index_modified_words;
    _Atomic(int) cnt_index_modified_words;
} segment_t;

/** shared memory region structure (1 per shared memory).
 * @param batcher Batcher instance for the shared memory
 * @param start Start of the shared memory region
 * @param segment Array of segments in the memory region
 * @param num_alloc_segments Number of allocated segments (used to keep track for realloc)
 * @param first_seg_size Size of the shared memory region (in bytes)
 * @param align Claimed alignment of the shared memory region (in bytes)
 * @param align_alloc Actual alignment of the memory allocations (in bytes)
 * @param current_segment_index Max index of the current segment (incremented if no freed indexes available)
 * @param freed_segment_index Array of indexes freed and that can be used again
 * @param segment_lock Lock for reallocation of array of segments and array of freed indexes
 * @param curren_transaction_id Max value of transaction id assigned to some tx
**/
typedef struct region_s {
    batcher_t batcher;
    void *start;
    //struct link allocs;
    segment_t *segment;
    int num_alloc_segments;
    size_t first_seg_size;
    size_t align;
    size_t align_alloc;
    int current_segment_index; // start from 1
    int *freed_segment_index; 
    lock_t segment_lock;
    _Atomic(tx_t) current_transaction_id; // start from 1
} region_t;

// -------------------------------------------------------------------------- //

/** Functions headers **/

// additional functions
static bool lock_init(lock_t*);
static void lock_cleanup(lock_t*);
static bool lock_acquire(lock_t*);
static void lock_release(lock_t*);

void abort_tx(region_t *, tx_t);
void commit_tx(region_t *, tx_t);

bool batcher_init(batcher_t *);
int get_epoch(batcher_t *);
void enter(batcher_t *);
void leave(batcher_t *, region_t *, tx_t tx);
void batcher_cleanup(batcher_t *);

bool segment_init(segment_t *, tx_t , size_t, size_t);
bool soft_segment_init(segment_t *, tx_t, size_t, size_t);
void *encode_segment_address(int);
void decode_segment_address(void const *, int *, int *);

alloc_t read_word(int, void *, segment_t *, bool, tx_t);
alloc_t write_word(int, const void *, segment_t *, tx_t);