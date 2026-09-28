/*_
 * Copyright (c) 2015-2020 Hirochika Asai <asai@jar.jp>
 * All rights reserved.
 */

#include "../palmtrie.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/*
 * Get current time at the microsecond granularity
 */
double
getmicrotime(void)
{
    struct timeval tv;
    double microsec;

    if ( 0 != gettimeofday(&tv, NULL) ) {
        return 0.0;
    }

    microsec = (double)tv.tv_sec + (1.0 * tv.tv_usec / 1000000);

    return microsec;
}

/*
 * Xorshift
 */
static __inline__ u32
xor128(void)
{
    static u32 x = 123456789;
    static u32 y = 362436069;
    static u32 z = 521288629;
    static u32 w = 88675123;
    u32 t;

    t = x ^ (x<<11);
    x = y;
    y = z;
    z = w;
    return w = (w ^ (w>>19)) ^ (t ^ (t >> 8));
}

/* Macro for testing */
#define TEST_FUNC(str, func, ret)                \
    do {                                         \
        printf("%s: ", str);                     \
        fflush(stdout);                          \
        if ( 0 == func() ) {                     \
            printf("passed");                    \
        } else {                                 \
            printf("failed");                    \
            ret = -1;                            \
        }                                        \
        printf("\n");                            \
    } while ( 0 )

#define TEST_PROGRESS()                              \
    do {                                             \
        printf(".");                                 \
        fflush(stdout);                              \
    } while ( 0 )

/*
 * Maximum number of entries tracked in the "ground truth" array.
 * The ground truth is a simple array of (addr, mask, priority, data)
 * tuples that mirrors what the trie should contain.  A linear scan
 * of this array serves as the reference implementation for lookups.
 */
#define MAX_ENTRIES    10000

struct gt_entry {
    addr_t addr;
    addr_t mask;
    int priority;
    u64 data;
    int active;         /* 1 = in trie, 0 = deleted */
};

struct ground_truth {
    struct gt_entry entries[MAX_ENTRIES];
    int count;          /* total entries ever added (active + deleted) */
};

/*
 * Initialize the ground truth
 */
static void
gt_init(struct ground_truth *gt)
{
    memset(gt, 0, sizeof(struct ground_truth));
}

/*
 * Add an entry to the ground truth.
 * If the same (addr, mask) already exists, update priority/data if higher.
 * Returns the index of the entry.
 */
static int
gt_add(struct ground_truth *gt, addr_t addr, addr_t mask, int priority,
       u64 data)
{
    int i;
    /* Check for duplicate key */
    for ( i = 0; i < gt->count; i++ ) {
        if ( gt->entries[i].active
             && ADDR_CMP(gt->entries[i].addr, addr)
             && ADDR_CMP(gt->entries[i].mask, mask) ) {
            /* Same key - update if higher priority */
            if ( priority > gt->entries[i].priority ) {
                gt->entries[i].priority = priority;
                gt->entries[i].data = data;
            }
            return i;
        }
    }
    /* New entry */
    if ( gt->count >= MAX_ENTRIES ) {
        return -1;
    }
    gt->entries[gt->count].addr = addr;
    gt->entries[gt->count].mask = mask;
    gt->entries[gt->count].priority = priority;
    gt->entries[gt->count].data = data;
    gt->entries[gt->count].active = 1;
    return gt->count++;
}

/*
 * Delete an entry from the ground truth.
 * Returns 0 on success, -1 if not found.
 */
static int
gt_del(struct ground_truth *gt, addr_t addr, addr_t mask)
{
    int i;
    for ( i = 0; i < gt->count; i++ ) {
        if ( gt->entries[i].active
             && ADDR_CMP(gt->entries[i].addr, addr)
             && ADDR_CMP(gt->entries[i].mask, mask) ) {
            gt->entries[i].active = 0;
            return 0;
        }
    }
    return -1;
}

/*
 * Lookup in the ground truth (reference implementation).
 * Returns the data of the highest-priority matching active entry,
 * or 0 if no match.
 */
static u64
gt_lookup(struct ground_truth *gt, addr_t addr)
{
    int i;
    int best_prio = -1;
    u64 best_data = 0;

    for ( i = 0; i < gt->count; i++ ) {
        if ( !gt->entries[i].active ) {
            continue;
        }
        if ( ADDR_MASK_CMP(addr, gt->entries[i].mask,
                           gt->entries[i].addr, gt->entries[i].mask) ) {
            if ( gt->entries[i].priority > best_prio ) {
                best_prio = gt->entries[i].priority;
                best_data = gt->entries[i].data;
            }
        }
    }
    return best_data;
}

/*
 * Generate a random key (addr + mask) with the given don't-care probability.
 */
static void
gen_random_key(addr_t *addr, addr_t *mask, int dc_prob)
{
    int i;
    memset(addr, 0, sizeof(addr_t));
    memset(mask, 0, sizeof(addr_t));
    for ( i = 0; i < 8; i++ ) {
        addr->a[i] = ((u64)xor128() << 32) | xor128();
        u64 m = 0;
        int b;
        for ( b = 0; b < 64; b++ ) {
            if ( (int)(xor128() % 100) < dc_prob ) {
                m |= (1ULL << b);
            }
        }
        mask->a[i] = m;
        addr->a[i] &= ~m;
    }
}

/*
 * Generate a random query key (no mask).
 */
static void
gen_random_query(addr_t *addr)
{
    int i;
    memset(addr, 0, sizeof(addr_t));
    for ( i = 0; i < 8; i++ ) {
        addr->a[i] = ((u64)xor128() << 32) | xor128();
    }
}

/*
 * Cross-validate a palmtrie instance against the ground truth.
 * Returns 0 if all lookups match, -1 otherwise.
 */
static int
cross_validate(struct palmtrie *pt, struct ground_truth *gt, int nqueries)
{
    int q;
    int mismatches = 0;

    for ( q = 0; q < nqueries; q++ ) {
        addr_t query;
        u64 expected;
        u64 actual;

        gen_random_query(&query);
        expected = gt_lookup(gt, query);
        actual = palmtrie_lookup(pt, query);
        if ( expected != actual ) {
            mismatches++;
            if ( mismatches <= 5 ) {
                printf("\n  MISMATCH at q=%d: expected=%llu actual=%llu",
                       q, (unsigned long long)expected,
                       (unsigned long long)actual);
            }
        }
    }
    return mismatches > 0 ? -1 : 0;
}

/*
 * Test 1: Batch add then batch delete
 *   Add 100 entries, verify, delete 10, verify, repeat.
 */
static int
test_batch_add_delete(void)
{
    struct palmtrie sl, tpt, mtpt, popmtpt;
    struct ground_truth gt;
    int step;
    int i;

    gt_init(&gt);
    palmtrie_init(&sl, PALMTRIE_SORTED_LIST);
    palmtrie_init(&tpt, PALMTRIE_BASIC);
    palmtrie_init(&mtpt, PALMTRIE_DEFAULT);
    palmtrie_init(&popmtpt, PALMTRIE_PLUS);

    /* 10 rounds: add 100, verify, delete 10, verify */
    for ( step = 0; step < 10; step++ ) {
        /* Add 100 entries */
        for ( i = 0; i < 100; i++ ) {
            addr_t addr, mask;
            int priority = gt.count + 1;
            u64 data = (u64)(gt.count + 1);
            gen_random_key(&addr, &mask, 30);
            gt_add(&gt, addr, mask, priority, data);
            palmtrie_add_data(&sl, addr, mask, priority, data);
            palmtrie_add_data(&tpt, addr, mask, priority, data);
            palmtrie_add_data(&mtpt, addr, mask, priority, data);
            palmtrie_add_data(&popmtpt, addr, mask, priority, data);
        }
        palmtrie_commit(&popmtpt);

        /* Verify after add */
        if ( 0 != cross_validate(&sl, &gt, 1000) ) { return -1; }
        if ( 0 != cross_validate(&tpt, &gt, 1000) ) { return -1; }
        if ( 0 != cross_validate(&mtpt, &gt, 1000) ) { return -1; }
        if ( 0 != cross_validate(&popmtpt, &gt, 1000) ) { return -1; }
        TEST_PROGRESS();

        /* Delete 10 entries (the oldest active ones) */
        {
            int deleted = 0;
            int idx;
            for ( idx = 0; idx < gt.count && deleted < 10; idx++ ) {
                if ( gt.entries[idx].active ) {
                    gt_del(&gt, gt.entries[idx].addr,
                           gt.entries[idx].mask);
                    palmtrie_del_data(&sl, gt.entries[idx].addr,
                                      gt.entries[idx].mask);
                    palmtrie_del_data(&tpt, gt.entries[idx].addr,
                                      gt.entries[idx].mask);
                    palmtrie_del_data(&mtpt, gt.entries[idx].addr,
                                      gt.entries[idx].mask);
                    palmtrie_del_data(&popmtpt, gt.entries[idx].addr,
                                      gt.entries[idx].mask);
                    deleted++;
                }
            }
        }
        palmtrie_commit(&popmtpt);

        /* Verify after delete */
        if ( 0 != cross_validate(&sl, &gt, 1000) ) { return -1; }
        if ( 0 != cross_validate(&tpt, &gt, 1000) ) { return -1; }
        if ( 0 != cross_validate(&mtpt, &gt, 1000) ) { return -1; }
        if ( 0 != cross_validate(&popmtpt, &gt, 1000) ) { return -1; }
        TEST_PROGRESS();
    }

    return 0;
}

/*
 * Test 2: Interleaved add/delete
 *   In each step: add 1 entry, then with 20% probability delete a random
 *   active entry.  Verify every 50 steps.
 */
static int
test_interleaved_add_delete(void)
{
    struct palmtrie sl, tpt, mtpt, popmtpt;
    struct ground_truth gt;
    int step;
    int active_count = 0;

    gt_init(&gt);
    palmtrie_init(&sl, PALMTRIE_SORTED_LIST);
    palmtrie_init(&tpt, PALMTRIE_BASIC);
    palmtrie_init(&mtpt, PALMTRIE_DEFAULT);
    palmtrie_init(&popmtpt, PALMTRIE_PLUS);

    for ( step = 0; step < 1000; step++ ) {
        /* Add one entry */
        addr_t addr, mask;
        int priority = gt.count + 1;
        u64 data = (u64)(gt.count + 1);
        gen_random_key(&addr, &mask, 30);
        gt_add(&gt, addr, mask, priority, data);
        palmtrie_add_data(&sl, addr, mask, priority, data);
        palmtrie_add_data(&tpt, addr, mask, priority, data);
        palmtrie_add_data(&mtpt, addr, mask, priority, data);
        palmtrie_add_data(&popmtpt, addr, mask, priority, data);
        active_count++;

        /* With 20% probability, delete a random active entry */
        if ( active_count > 10 && (xor128() % 100) < 20 ) {
            /* Find a random active entry */
            int target = xor128() % gt.count;
            int idx;
            int found = -1;
            for ( idx = target; idx < gt.count; idx++ ) {
                if ( gt.entries[idx].active ) {
                    found = idx;
                    break;
                }
            }
            if ( found < 0 ) {
                for ( idx = 0; idx < target; idx++ ) {
                    if ( gt.entries[idx].active ) {
                        found = idx;
                        break;
                    }
                }
            }
            if ( found >= 0 ) {
                gt_del(&gt, gt.entries[found].addr,
                       gt.entries[found].mask);
                palmtrie_del_data(&sl, gt.entries[found].addr,
                                  gt.entries[found].mask);
                palmtrie_del_data(&tpt, gt.entries[found].addr,
                                  gt.entries[found].mask);
                palmtrie_del_data(&mtpt, gt.entries[found].addr,
                                  gt.entries[found].mask);
                palmtrie_del_data(&popmtpt, gt.entries[found].addr,
                                  gt.entries[found].mask);
                active_count--;
            }
        }

        /* Verify every 50 steps */
        if ( step > 0 && step % 50 == 0 ) {
            palmtrie_commit(&popmtpt);
            if ( 0 != cross_validate(&sl, &gt, 500) ) { return -1; }
            if ( 0 != cross_validate(&tpt, &gt, 500) ) { return -1; }
            if ( 0 != cross_validate(&mtpt, &gt, 500) ) { return -1; }
            if ( 0 != cross_validate(&popmtpt, &gt, 500) ) { return -1; }
            TEST_PROGRESS();
        }
    }

    /* Final verification */
    palmtrie_commit(&popmtpt);
    if ( 0 != cross_validate(&sl, &gt, 2000) ) { return -1; }
    if ( 0 != cross_validate(&tpt, &gt, 2000) ) { return -1; }
    if ( 0 != cross_validate(&mtpt, &gt, 2000) ) { return -1; }
    if ( 0 != cross_validate(&popmtpt, &gt, 2000) ) { return -1; }
    TEST_PROGRESS();

    return 0;
}

/*
 * Test 3: Delete all entries one by one, verifying after each deletion
 */
static int
test_delete_all_one_by_one(void)
{
    struct palmtrie sl, tpt, mtpt, popmtpt;
    struct ground_truth gt;
    int i;

    gt_init(&gt);
    palmtrie_init(&sl, PALMTRIE_SORTED_LIST);
    palmtrie_init(&tpt, PALMTRIE_BASIC);
    palmtrie_init(&mtpt, PALMTRIE_DEFAULT);
    palmtrie_init(&popmtpt, PALMTRIE_PLUS);

    /* Add 200 entries */
    for ( i = 0; i < 200; i++ ) {
        addr_t addr, mask;
        int priority = i + 1;
        u64 data = (u64)(i + 1);
        gen_random_key(&addr, &mask, 30);
        gt_add(&gt, addr, mask, priority, data);
        palmtrie_add_data(&sl, addr, mask, priority, data);
        palmtrie_add_data(&tpt, addr, mask, priority, data);
        palmtrie_add_data(&mtpt, addr, mask, priority, data);
        palmtrie_add_data(&popmtpt, addr, mask, priority, data);
    }
    palmtrie_commit(&popmtpt);

    /* Verify all present */
    if ( 0 != cross_validate(&sl, &gt, 500) ) { return -1; }
    if ( 0 != cross_validate(&tpt, &gt, 500) ) { return -1; }
    if ( 0 != cross_validate(&mtpt, &gt, 500) ) { return -1; }
    if ( 0 != cross_validate(&popmtpt, &gt, 500) ) { return -1; }
    TEST_PROGRESS();

    /* Delete one by one, verifying after each */
    for ( i = 0; i < gt.count; i++ ) {
        if ( !gt.entries[i].active ) {
            continue;
        }
        gt_del(&gt, gt.entries[i].addr, gt.entries[i].mask);
        palmtrie_del_data(&sl, gt.entries[i].addr, gt.entries[i].mask);
        palmtrie_del_data(&tpt, gt.entries[i].addr, gt.entries[i].mask);
        palmtrie_del_data(&mtpt, gt.entries[i].addr, gt.entries[i].mask);
        palmtrie_del_data(&popmtpt, gt.entries[i].addr, gt.entries[i].mask);

        /* Verify every 20 deletions */
        if ( i > 0 && i % 20 == 0 ) {
            palmtrie_commit(&popmtpt);
            if ( 0 != cross_validate(&sl, &gt, 200) ) { return -1; }
            if ( 0 != cross_validate(&tpt, &gt, 200) ) { return -1; }
            if ( 0 != cross_validate(&mtpt, &gt, 200) ) { return -1; }
            if ( 0 != cross_validate(&popmtpt, &gt, 200) ) { return -1; }
            TEST_PROGRESS();
        }
    }

    /* Final: all deleted, should return 0 for all queries */
    palmtrie_commit(&popmtpt);
    if ( 0 != cross_validate(&sl, &gt, 500) ) { return -1; }
    if ( 0 != cross_validate(&tpt, &gt, 500) ) { return -1; }
    if ( 0 != cross_validate(&mtpt, &gt, 500) ) { return -1; }
    if ( 0 != cross_validate(&popmtpt, &gt, 500) ) { return -1; }
    TEST_PROGRESS();

    return 0;
}

/*
 * Test 4: Re-add after delete (verify deleted nodes can be reactivated)
 */
static int
test_readd_after_delete(void)
{
    struct palmtrie sl, tpt, mtpt, popmtpt;
    struct ground_truth gt;
    int i;

    gt_init(&gt);
    palmtrie_init(&sl, PALMTRIE_SORTED_LIST);
    palmtrie_init(&tpt, PALMTRIE_BASIC);
    palmtrie_init(&mtpt, PALMTRIE_DEFAULT);
    palmtrie_init(&popmtpt, PALMTRIE_PLUS);

    /* Add 100 entries */
    for ( i = 0; i < 100; i++ ) {
        addr_t addr, mask;
        int priority = i + 1;
        u64 data = (u64)(i + 1);
        gen_random_key(&addr, &mask, 30);
        gt_add(&gt, addr, mask, priority, data);
        palmtrie_add_data(&sl, addr, mask, priority, data);
        palmtrie_add_data(&tpt, addr, mask, priority, data);
        palmtrie_add_data(&mtpt, addr, mask, priority, data);
        palmtrie_add_data(&popmtpt, addr, mask, priority, data);
    }

    /* Delete half */
    for ( i = 0; i < 100; i += 2 ) {
        gt_del(&gt, gt.entries[i].addr, gt.entries[i].mask);
        palmtrie_del_data(&sl, gt.entries[i].addr, gt.entries[i].mask);
        palmtrie_del_data(&tpt, gt.entries[i].addr, gt.entries[i].mask);
        palmtrie_del_data(&mtpt, gt.entries[i].addr, gt.entries[i].mask);
        palmtrie_del_data(&popmtpt, gt.entries[i].addr, gt.entries[i].mask);
    }
    palmtrie_commit(&popmtpt);
    if ( 0 != cross_validate(&sl, &gt, 500) ) { return -1; }
    if ( 0 != cross_validate(&tpt, &gt, 500) ) { return -1; }
    if ( 0 != cross_validate(&mtpt, &gt, 500) ) { return -1; }
    if ( 0 != cross_validate(&popmtpt, &gt, 500) ) { return -1; }
    TEST_PROGRESS();

    /* Re-add the deleted entries with higher priority and different data */
    for ( i = 0; i < 100; i += 2 ) {
        int priority = 1000 + i;
        u64 data = (u64)(1000 + i);
        gt_add(&gt, gt.entries[i].addr, gt.entries[i].mask, priority, data);
        palmtrie_add_data(&sl, gt.entries[i].addr, gt.entries[i].mask,
                          priority, data);
        palmtrie_add_data(&tpt, gt.entries[i].addr, gt.entries[i].mask,
                          priority, data);
        palmtrie_add_data(&mtpt, gt.entries[i].addr, gt.entries[i].mask,
                          priority, data);
        palmtrie_add_data(&popmtpt, gt.entries[i].addr, gt.entries[i].mask,
                          priority, data);
    }
    palmtrie_commit(&popmtpt);
    if ( 0 != cross_validate(&sl, &gt, 500) ) { return -1; }
    if ( 0 != cross_validate(&tpt, &gt, 500) ) { return -1; }
    if ( 0 != cross_validate(&mtpt, &gt, 500) ) { return -1; }
    if ( 0 != cross_validate(&popmtpt, &gt, 500) ) { return -1; }
    TEST_PROGRESS();

    return 0;
}

/*
 * Test 5: Delete non-existent entries (should return -1, no side effects)
 */
static int
test_delete_nonexistent(void)
{
    struct palmtrie sl, tpt, mtpt, popmtpt;
    struct ground_truth gt;
    int i;

    gt_init(&gt);
    palmtrie_init(&sl, PALMTRIE_SORTED_LIST);
    palmtrie_init(&tpt, PALMTRIE_BASIC);
    palmtrie_init(&mtpt, PALMTRIE_DEFAULT);
    palmtrie_init(&popmtpt, PALMTRIE_PLUS);

    /* Add 50 entries */
    for ( i = 0; i < 50; i++ ) {
        addr_t addr, mask;
        int priority = i + 1;
        u64 data = (u64)(i + 1);
        gen_random_key(&addr, &mask, 30);
        gt_add(&gt, addr, mask, priority, data);
        palmtrie_add_data(&sl, addr, mask, priority, data);
        palmtrie_add_data(&tpt, addr, mask, priority, data);
        palmtrie_add_data(&mtpt, addr, mask, priority, data);
        palmtrie_add_data(&popmtpt, addr, mask, priority, data);
    }
    palmtrie_commit(&popmtpt);

    /* Try to delete 50 non-existent entries */
    for ( i = 0; i < 50; i++ ) {
        addr_t addr, mask;
        gen_random_key(&addr, &mask, 30);
        /* These should all return -1 */
        if ( 0 == palmtrie_del_data(&sl, addr, mask) ) { return -1; }
        if ( 0 == palmtrie_del_data(&tpt, addr, mask) ) { return -1; }
        if ( 0 == palmtrie_del_data(&mtpt, addr, mask) ) { return -1; }
        if ( 0 == palmtrie_del_data(&popmtpt, addr, mask) ) { return -1; }
    }

    /* Verify all original entries still intact */
    if ( 0 != cross_validate(&sl, &gt, 500) ) { return -1; }
    if ( 0 != cross_validate(&tpt, &gt, 500) ) { return -1; }
    if ( 0 != cross_validate(&mtpt, &gt, 500) ) { return -1; }
    if ( 0 != cross_validate(&popmtpt, &gt, 500) ) { return -1; }
    TEST_PROGRESS();

    return 0;
}

/*
 * Test 6: Stress test with high churn
 *   5000 steps: add 5, delete 3 (random active), verify every 500 steps.
 */
static int
test_stress_high_churn(void)
{
    struct palmtrie sl, tpt, mtpt, popmtpt;
    struct ground_truth gt;
    int step;
    int active = 0;

    gt_init(&gt);
    palmtrie_init(&sl, PALMTRIE_SORTED_LIST);
    palmtrie_init(&tpt, PALMTRIE_BASIC);
    palmtrie_init(&mtpt, PALMTRIE_DEFAULT);
    palmtrie_init(&popmtpt, PALMTRIE_PLUS);

    for ( step = 0; step < 5000; step++ ) {
        /* Add 5 entries */
        int j;
        for ( j = 0; j < 5; j++ ) {
            addr_t addr, mask;
            int priority = gt.count + 1;
            u64 data = (u64)(gt.count + 1);
            gen_random_key(&addr, &mask, 30);
            gt_add(&gt, addr, mask, priority, data);
            palmtrie_add_data(&sl, addr, mask, priority, data);
            palmtrie_add_data(&tpt, addr, mask, priority, data);
            palmtrie_add_data(&mtpt, addr, mask, priority, data);
            palmtrie_add_data(&popmtpt, addr, mask, priority, data);
            active++;
        }

        /* Delete 3 random active entries */
        for ( j = 0; j < 3 && active > 0; j++ ) {
            int target = xor128() % gt.count;
            int idx;
            int found = -1;
            /* Search forward from target */
            for ( idx = target; idx < gt.count; idx++ ) {
                if ( gt.entries[idx].active ) { found = idx; break; }
            }
            if ( found < 0 ) {
                for ( idx = 0; idx < target; idx++ ) {
                    if ( gt.entries[idx].active ) { found = idx; break; }
                }
            }
            if ( found >= 0 ) {
                gt_del(&gt, gt.entries[found].addr,
                       gt.entries[found].mask);
                palmtrie_del_data(&sl, gt.entries[found].addr,
                                  gt.entries[found].mask);
                palmtrie_del_data(&tpt, gt.entries[found].addr,
                                  gt.entries[found].mask);
                palmtrie_del_data(&mtpt, gt.entries[found].addr,
                                  gt.entries[found].mask);
                palmtrie_del_data(&popmtpt, gt.entries[found].addr,
                                  gt.entries[found].mask);
                active--;
            }
        }

        /* Verify every 500 steps */
        if ( step > 0 && step % 500 == 0 ) {
            palmtrie_commit(&popmtpt);
            if ( 0 != cross_validate(&sl, &gt, 1000) ) { return -1; }
            if ( 0 != cross_validate(&tpt, &gt, 1000) ) { return -1; }
            if ( 0 != cross_validate(&mtpt, &gt, 1000) ) { return -1; }
            if ( 0 != cross_validate(&popmtpt, &gt, 1000) ) { return -1; }
            TEST_PROGRESS();
        }
    }

    /* Final verification */
    palmtrie_commit(&popmtpt);
    if ( 0 != cross_validate(&sl, &gt, 2000) ) { return -1; }
    if ( 0 != cross_validate(&tpt, &gt, 2000) ) { return -1; }
    if ( 0 != cross_validate(&mtpt, &gt, 2000) ) { return -1; }
    if ( 0 != cross_validate(&popmtpt, &gt, 2000) ) { return -1; }
    TEST_PROGRESS();

    return 0;
}

/*
 * Main routine
 */
int
main(int argc, const char *const argv[])
{
    int ret;

    ret = 0;

#if defined(PALMTRIE_SHORT) && PALMTRIE_SHORT
    fprintf(stderr, "Tests not available for PALMTRIE_SHORT=1.\n");
    return -1;
#endif

    printf("Add/Delete multi-step tests:\n");

    TEST_FUNC("batch add/delete (100 add, 10 del x10)", test_batch_add_delete,
              ret);
    TEST_FUNC("interleaved add/delete (1000 steps)", test_interleaved_add_delete,
              ret);
    TEST_FUNC("delete all one by one (200 entries)", test_delete_all_one_by_one,
              ret);
    TEST_FUNC("re-add after delete", test_readd_after_delete, ret);
    TEST_FUNC("delete non-existent", test_delete_nonexistent, ret);
    TEST_FUNC("stress high churn (5000 steps)", test_stress_high_churn, ret);

    return ret;
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: sw=4 ts=4 fdm=marker
 * vim<600: sw=4 ts=4
 */
