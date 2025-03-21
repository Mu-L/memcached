/*  Copyright 2017 Facebook.
 *
 *  Use and distribution licensed under the BSD license.  See
 *  the LICENSE file for full text.
 */

/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "memcached.h"
#include "slab_automove_extstore.h"
#include <stdlib.h>
#include <string.h>

#define MIN_PAGES_FOR_SOURCE 2

struct window_data {
    uint64_t age;
    uint64_t dirty;
    uint64_t evicted;
    unsigned int excess_free;
    unsigned int relaxed;
};

// TODO: use ptrs for before/after to cut the memcpy
// after reach run and save some cpu.
typedef struct {
    struct window_data *window_data;
    struct settings *settings;
    uint32_t window_size;
    uint32_t window_cur;
    uint32_t item_size;
    double max_age_ratio;
    double free_ratio;
    bool pool_filled_once;
    unsigned int global_pool_watermark;
    item_stats_automove iam_before[MAX_NUMBER_OF_SLAB_CLASSES];
    item_stats_automove iam_after[MAX_NUMBER_OF_SLAB_CLASSES];
    slab_stats_automove sam_before[MAX_NUMBER_OF_SLAB_CLASSES];
    slab_stats_automove sam_after[MAX_NUMBER_OF_SLAB_CLASSES];
} slab_automove;

void *slab_automove_extstore_init(struct settings *settings) {
    uint32_t window_size = settings->slab_automove_window;
    double max_age_ratio = settings->slab_automove_ratio;
    slab_automove *a = calloc(1, sizeof(slab_automove));
    if (a == NULL)
        return NULL;
    a->window_data = calloc(window_size * MAX_NUMBER_OF_SLAB_CLASSES, sizeof(struct window_data));
    a->window_size = window_size;
    a->max_age_ratio = max_age_ratio;
    a->free_ratio = settings->slab_automove_freeratio;
    a->item_size = settings->ext_item_size;
    a->settings = settings;
    a->pool_filled_once = false;
    if (a->window_data == NULL) {
        if (a->window_data)
            free(a->window_data);
        free(a);
        return NULL;
    }

    // do a dry run to fill the before structs
    fill_item_stats_automove(a->iam_before);
    fill_slab_stats_automove(a->sam_before);

    return (void *)a;
}

void slab_automove_extstore_free(void *arg) {
    slab_automove *a = (slab_automove *)arg;
    free(a->window_data);
    free(a);
}

static void window_sum(struct window_data *wd, struct window_data *w,
        uint32_t size) {
    for (int x = 0; x < size; x++) {
        struct window_data *d = &wd[x];
        w->age += d->age;
        w->dirty += d->dirty;
        w->evicted += d->evicted;
        w->excess_free += d->excess_free;
        w->relaxed += d->relaxed;
    }
}

static int global_pool_check(slab_automove *a, unsigned int *count) {
    bool mem_limit_reached;
    unsigned int free = a->global_pool_watermark;
    *count = global_page_pool_size(&mem_limit_reached);
    if (!mem_limit_reached)
        return 0;
    if (*count < free) {
        a->pool_filled_once = true;
        return 1;
    } else {
        a->pool_filled_once = true;
    }
    return 0;
}

/* A percentage of memory is configured to be held "free" as buffers for the
 * external storage system.
 * % of global memory is desired in the global page pool
 * each slab class has a % of free chunks desired based on how much memory is
 * currently in the class. This allows time for extstore to flush data when
 * spikes or waves of set data arrive.
 * The global page pool reserve acts as a secondary buffer for any slab class,
 * which helps absorb shifts in which class is active.
 */
static void memcheck(slab_automove *a) {
    unsigned int total_pages = 0;

    // FIXME: is there a cached counter for total pages alloced?
    // technically we only really need to do this once as the pages are
    // prefilled and ratio isn't a runtime change.
    for (int n = 1; n < MAX_NUMBER_OF_SLAB_CLASSES; n++) {
        slab_stats_automove *sam = &a->sam_after[n];
        total_pages += sam->total_pages;
    }
    // always update what remains in the global page pool
    total_pages += a->sam_after[0].total_pages;
    a->global_pool_watermark = total_pages * a->free_ratio;
    if (a->global_pool_watermark < 2)
        a->global_pool_watermark = 2;
}

static struct window_data *get_window_data(slab_automove *a, int class) {
    int w_offset = class * a->window_size;
    return &a->window_data[w_offset + (a->window_cur % a->window_size)];
}

void slab_automove_extstore_run(void *arg, int *src, int *dst) {
    slab_automove *a = (slab_automove *)arg;
    int n;
    struct window_data w_sum;
    int oldest = -1;
    uint64_t oldest_age = 0;
    bool too_free = false;
    *src = -1;
    *dst = -1;

    // calculate how much memory pressure extstore is under.
    // 100% means we need to evict item headers.
    unsigned int total_low_pages = 0;
    unsigned int total_high_pages = 0;

    unsigned int global_count = 0;
    int global_low = global_pool_check(a, &global_count);
    // fill after structs
    fill_item_stats_automove(a->iam_after);
    fill_slab_stats_automove(a->sam_after);
    a->window_cur++;

    memcheck(a);

    // iterate slabs
    for (n = POWER_SMALLEST; n < MAX_NUMBER_OF_SLAB_CLASSES; n++) {
        bool small_slab = a->sam_before[n].chunk_size < a->item_size
            ? true : false;
        struct window_data *wd = get_window_data(a, n);
        int w_offset = n * a->window_size;
        memset(wd, 0, sizeof(struct window_data));
        unsigned int free_target = a->sam_after[n].chunks_per_page * MIN_PAGES_FOR_SOURCE;

        if (small_slab) {
            total_low_pages += a->sam_after[n].total_pages;
        } else {
            unsigned int pages = a->sam_after[n].total_pages;
            // only include potentially movable pages
            if (pages > MIN_PAGES_FOR_SOURCE) {
                total_high_pages += a->sam_after[n].total_pages;
            }
        }

        // if page delta, oom, or evicted delta, mark window dirty
        // classes marked dirty cannot donate memory back to global pool.
        if (small_slab) {
            if (a->iam_after[n].evicted - a->iam_before[n].evicted > 0 ||
                a->iam_after[n].outofmemory - a->iam_before[n].outofmemory > 0) {
                wd->evicted = 1;
                wd->dirty = 1;
            }
            if (a->sam_after[n].total_pages - a->sam_before[n].total_pages > 0) {
                wd->dirty = 1;
            }
        }

        // reclaim excessively free memory to global after a full window
        if (a->sam_after[n].free_chunks > free_target) {
            wd->excess_free = 1;
        }

        // set age into window
        wd->age = a->iam_after[n].age;

        // summarize the window-up-to-now.
        memset(&w_sum, 0, sizeof(struct window_data));
        window_sum(&a->window_data[w_offset], &w_sum, a->window_size);

        // If global page pool is nearly empty we need to force a move
        // from any possible source. Otherwise avoid moving from this class if
        // it appears dirty.
        if (w_sum.dirty != 0 && global_count != 0) {
            continue;
        }

        // if > N free chunks, reclaim memory
        // small slab classes aren't age balanced and rely more on global
        if (w_sum.excess_free >= a->window_size) {
            *src = n;
            *dst = 0;
            too_free = true;
        }

        // large slabs should push to extstore if we try to evict from them.
        // so we can be aggressive there if the global pool is low.
        if (!small_slab) {
            // the first class with enough pages, else the one with the oldest
            // tail age.
            uint64_t age = a->iam_after[n].age;
            if (a->sam_after[n].total_pages > MIN_PAGES_FOR_SOURCE
                && (age > oldest_age || oldest == -1) ) {
                oldest = n;
                oldest_age = age;
            }
        }
    }

    // update the pressure calculation.
    float total_pages = total_low_pages + total_high_pages + global_count;
    float memory_pressure = (total_low_pages / total_pages) * 100;
    STATS_LOCK();
    stats_state.extstore_memory_pressure = memory_pressure;
    STATS_UNLOCK();

    memcpy(a->iam_before, a->iam_after,
            sizeof(item_stats_automove) * MAX_NUMBER_OF_SLAB_CLASSES);
    memcpy(a->sam_before, a->sam_after,
            sizeof(slab_stats_automove) * MAX_NUMBER_OF_SLAB_CLASSES);
    // only make decisions if window has filled once.
    if (a->window_cur < a->window_size) {
        return;
    }

    settings.ext_global_pool_min = a->global_pool_watermark;
    if (!too_free && global_low && oldest != -1) {
        *src = oldest;
        *dst = 0;
    }
    return;
}
