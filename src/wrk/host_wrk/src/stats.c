// Copyright (C) 2012 - Will Glozer.  All rights reserved.

#include <inttypes.h>
#include <stdlib.h>
#include <math.h>

#include "stats.h"
#include "zmalloc.h"

stats *stats_alloc(uint64_t max, uint64_t threads) {
    stats *s = zcalloc(sizeof(stats) + sizeof(stats_per_thread*) * threads);
    uint64_t limit = max + 1;
    s->threads = threads;
    for (uint64_t j = 0; j < threads; j++) {
      s->local[j] = zcalloc(sizeof(stats_per_thread) + sizeof(uint64_t) * limit);
      s->local[j]->limit = limit;
      s->local[j]->min = UINT64_MAX;
    }
    return s;
}

void stats_free(stats *stats) {
    for (uint64_t j = 0; j < stats->threads; j++) {
      zfree(stats->local[j]);
    }
    zfree(stats);
}

int stats_record(stats *stats, uint64_t n, uint64_t tid) {
    stats_per_thread *local_stats = stats->local[tid];
    if (n >= local_stats->limit) return 0;
    local_stats->data[n]++;
    local_stats->count++;
    if (n < local_stats->min) local_stats->min = n;
    if (n > local_stats->max) local_stats->max = n;
    return 1;
}

void stats_correct(stats *stats, int64_t expected) {
    for (uint64_t j = 0; j < stats->threads; j++) {
        stats_per_thread *local_stats = stats->local[j];
        for (uint64_t n = expected * 2; n <= local_stats->max; n++) {
	    uint64_t count = local_stats->data[n];
	    int64_t m = (int64_t) n - expected;
	    while (count && m > expected) {
	        local_stats->data[m] += count;
		local_stats->count += count;
		m -= expected;
	    }
	}
    }
}

static uint64_t stats_count(stats *stats) {
    uint64_t count = 0;
    for (uint64_t j = 0; j < stats->threads; j++) {
	count += stats->local[j]->count;
    }
    return count;
}

static uint64_t stats_min(stats *stats) {
    uint64_t min = UINT64_MAX;
    for (uint64_t j = 0; j < stats->threads; j++) {
      if (min > stats->local[j]->min) min = stats->local[j]->min;
    }
    return min;
}

uint64_t stats_max(stats *stats) {
    uint64_t max = 0;
    for (uint64_t j = 0; j < stats->threads; j++) {
      if (max < stats->local[j]->max) max = stats->local[j]->max;
    }
    return max;
}

long double stats_mean(stats *stats) {
    uint64_t count = stats_count(stats);
    if (count == 0) return 0.0;

    uint64_t sum = 0;
    for (uint64_t j = 0; j < stats->threads; j++) {
        stats_per_thread *local_stats = stats->local[j];
	for (uint64_t i = local_stats->min; i <= local_stats->max; i++) {
	    sum += local_stats->data[i] * i;
	}
    }
    return sum / (long double) count;
}

long double stats_stdev(stats *stats, long double mean) {
    long double sum = 0.0;
    uint64_t count = stats_count(stats);
    if (count < 2) return 0.0;
    for (uint64_t j = 0; j < stats->threads; j++) {
        stats_per_thread *local_stats = stats->local[j];
	for (uint64_t i = local_stats->min; i <= local_stats->max; i++) {
	    if (local_stats->data[i]) {
	        sum += powl(i - mean, 2) * local_stats->data[i];
	    }
        }
    }
    return sqrtl(sum / (count - 1));
}

long double stats_within_stdev(stats *stats, long double mean, long double stdev, uint64_t n) {
    long double upper = mean + (stdev * n);
    long double lower = mean - (stdev * n);
    uint64_t sum = 0;
    uint64_t count = stats_count(stats);
    
    for (uint64_t j = 0; j < stats->threads; j++) {
        stats_per_thread *local_stats = stats->local[j];
	for (uint64_t i = local_stats->min; i <= local_stats->max; i++) {
	    if (i >= lower && i <= upper) {
	        sum += local_stats->data[i];
	    }
        }
    }

    return (sum / (long double) count) * 100;
}

uint64_t stats_percentile(stats *stats, long double p) {
    uint64_t count = stats_count(stats);
    uint64_t rank = round((p / 100.0) * count + 0.5);
    uint64_t total = 0;
    uint64_t min = stats_min(stats);
    uint64_t max = stats_max(stats);
    for (uint64_t i = min; i <= max; i++) {
        for (uint64_t j = 0; j < stats->threads; j++) {
	    stats_per_thread *local_stats = stats->local[j];
	    total += local_stats->data[i];
	}
	if (total >= rank) return i;
    }
    return 0;
}

uint64_t stats_popcount(stats *stats) {
    uint64_t count = 0;
    for (uint64_t j = 0; j < stats->threads; j++) {
        stats_per_thread *local_stats = stats->local[j];
	for (uint64_t i = local_stats->min; i <= local_stats->max; i++) {
	    if (local_stats->data[i]) count++;
	}
    }
    return count;
}

uint64_t stats_value_at(stats *stats, uint64_t index, uint64_t *count) {
    uint64_t min = stats_min(stats);
    uint64_t max = stats_max(stats);
    *count = 0;
    for (uint64_t i = min; i <= max; i++) {
        uint64_t data = 0;
        for (uint64_t j = 0; j < stats->threads; j++) {
	    stats_per_thread *local_stats = stats->local[j];
	    data += local_stats->data[i];
	}
        if (data && (*count)++ == index) {
	    *count = data;
	    return i;
        }
    }
    return 0;
}
