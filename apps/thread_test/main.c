/*
 * thread_test — THRD interface test (build with --libc).
 *   args: [mode]
 *     all    (default) mutex counter, queue, semaphore ping-pong, per-thread
 *            errno + printf, exit codes, 200 create/join cycles. Exit code =
 *            number of failed checks.
 *     leave  returns with 3 threads still running (one spinning, one blocked
 *            on a mutex, one on a queue) and objects not destroyed.
 *     exit   a worker calls exit(7); the app should exit with 7.
 *     spin   workers ignore exit requests (stop it with `papp_dev.py stop`).
 */
#include "papp/papp.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PAPP_MANIFEST(.id = "test.thread", .name = "Thread test", .version = "0.1.0", .main_stack = 16384, .flags = PAPP_MF_HIDDEN);

static int s_fail;

#define CHECK(cond, ...) do {                                                   \
        if (cond) printf("ok   " __VA_ARGS__);                                  \
        else { printf("FAIL " __VA_ARGS__); s_fail++; }                         \
    } while (0)

/* ── Mutex counter ─────────────────────────────────────────────────────── */

#define COUNT_THREADS 4
#define COUNT_ITERS   50000

static papp_mutex_t *s_mx;
static volatile uint32_t s_counter;

static int count_worker(void *arg)
{
    for (int i = 0; i < COUNT_ITERS; i++) {
        papp_mutex_lock(s_mx);
        s_counter = s_counter + 1;          /* read-modify-write: races without the lock */
        papp_mutex_unlock(s_mx);
    }
    return (int)(intptr_t)arg;
}

static void test_mutex(void)
{
    s_mx = papp_mutex_create(false);
    papp_thread_t *t[COUNT_THREADS];
    int64_t t0 = papp_time_us();
    for (int i = 0; i < COUNT_THREADS; i++) {
        papp_thread_cfg_t cfg = { .name = "count", .core = (int8_t)(i & 1) };   /* both cores */
        t[i] = papp_thread_create(count_worker, (void *)(intptr_t)(100 + i), &cfg);
    }
    int codes = 0;
    for (int i = 0; i < COUNT_THREADS; i++) {
        int code = -1;
        papp_thread_join(t[i], PAPP_WAIT_FOREVER, &code);
        codes += code == 100 + i;
    }
    int64_t us = papp_time_us() - t0;
    CHECK(s_counter == COUNT_THREADS * COUNT_ITERS && codes == COUNT_THREADS,
          "mutex: counter %lu / %d, exit codes %d/%d, %lld ns per lock+unlock\n",
          (unsigned long)s_counter, COUNT_THREADS * COUNT_ITERS, codes, COUNT_THREADS,
          us * 1000 / (COUNT_THREADS * COUNT_ITERS));
    t0 = papp_time_us();
    for (int i = 0; i < 10000; i++) {
        papp_mutex_lock(s_mx);
        papp_mutex_unlock(s_mx);
    }
    printf("     mutex: %lld ns per uncontended lock+unlock\n", (papp_time_us() - t0) / 10);
    papp_mutex_destroy(s_mx);
}

/* ── Queue producer / consumer ─────────────────────────────────────────── */

#define Q_ITEMS 2000

typedef struct { uint32_t seq, value; } item_t;

static int producer(void *q)
{
    for (uint32_t i = 0; i < Q_ITEMS; i++) {
        item_t it = { i, i * 3 };
        papp_queue_send(q, &it, PAPP_WAIT_FOREVER);
    }
    return 0;
}

static int consumer(void *q)
{
    uint32_t sum = 0, expect = 0, order_ok = 1;
    for (uint32_t i = 0; i < Q_ITEMS; i++) {
        item_t it;
        if (!papp_queue_recv(q, &it, 1000)) return -1;
        order_ok &= it.seq == expect++;
        sum += it.value;
    }
    return order_ok ? (int)sum : -2;
}

static void test_queue(void)
{
    papp_queue_t *q = papp_queue_create(16, sizeof(item_t));
    papp_thread_cfg_t pc = { .name = "prod", .core = 1 }, cc = { .name = "cons", .core = 0 };
    int64_t t0 = papp_time_us();
    papp_thread_t *c = papp_thread_create(consumer, q, &cc);
    papp_thread_t *p = papp_thread_create(producer, q, &pc);
    int sum = 0;
    papp_thread_join(p, PAPP_WAIT_FOREVER, NULL);
    papp_thread_join(c, PAPP_WAIT_FOREVER, &sum);
    int64_t us = papp_time_us() - t0;
    uint32_t expect = 3u * (Q_ITEMS - 1) * Q_ITEMS / 2;
    CHECK(sum == (int)expect && papp_queue_count(q) == 0,
          "queue: %d items across cores, sum %d (expect %lu), %lld us per item\n",
          Q_ITEMS, sum, (unsigned long)expect, us / Q_ITEMS);
    papp_queue_destroy(q);
}

/* ── Semaphore ping-pong ───────────────────────────────────────────────── */

#define PINGS 1000
static papp_sem_t *s_ping, *s_pong;

static int ponger(void *arg)
{
    for (int i = 0; i < PINGS; i++) {
        if (!papp_sem_take(s_ping, 1000)) return -1;
        papp_sem_give(s_pong);
    }
    return 0;
}

static void test_sem(void)
{
    s_ping = papp_sem_create(0, 1);
    s_pong = papp_sem_create(0, 1);
    papp_thread_cfg_t cfg = { .name = "pong", .core = 1 };
    papp_thread_t *t = papp_thread_create(ponger, NULL, &cfg);
    int ok = 0;
    int64_t t0 = papp_time_us();
    for (int i = 0; i < PINGS; i++) {
        papp_sem_give(s_ping);
        ok += papp_sem_take(s_pong, 1000);
    }
    int64_t us = papp_time_us() - t0;
    int code = -9;
    papp_thread_join(t, PAPP_WAIT_FOREVER, &code);
    CHECK(ok == PINGS && code == 0, "semaphore: %d/%d round trips across cores, %lld us each\n",
          ok, PINGS, us / PINGS);
    bool timed_out = !papp_sem_take(s_ping, 20);
    CHECK(timed_out, "semaphore: take on an empty semaphore times out\n");
    papp_sem_destroy(s_ping);
    papp_sem_destroy(s_pong);
}

/* ── Per-thread libc state ─────────────────────────────────────────────── */

static int errno_worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    errno = 1000 + id;
    for (int i = 0; i < 20; i++) {
        printf("thread %d line %d errno %d\n", id, i, errno);
        papp_yield();
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d-%.1f", id, id * 1.5);   /* uses the reent's locale/dtoa state */
    return errno == 1000 + id && strtol(buf, NULL, 10) == id ? 0 : 1;
}

static void test_libc(void)
{
    errno = 42;
    papp_thread_t *t[3];
    for (int i = 0; i < 3; i++) t[i] = papp_thread_create(errno_worker, (void *)(intptr_t)i, NULL);
    int bad = 0;
    for (int i = 0; i < 3; i++) {
        int code = -1;
        papp_thread_join(t[i], PAPP_WAIT_FOREVER, &code);
        bad += code != 0;
    }
    CHECK(bad == 0 && errno == 42, "libc: errno is per thread (main kept %d), concurrent printf\n", errno);
}

/* ── Exit codes, join timeout, churn ───────────────────────────────────── */

static int exit_worker(void *arg)
{
    papp_delay_ms(30);
    papp_thread_exit((int)(intptr_t)arg);
}

static int nop_worker(void *arg) { return (int)(intptr_t)arg; }

static void test_lifecycle(void)
{
    papp_thread_t *t = papp_thread_create(exit_worker, (void *)55, NULL);
    int code = 0;
    bool early = papp_thread_join(t, 5, &code);
    bool late = papp_thread_join(t, 1000, &code);
    CHECK(!early && late && code == 55, "lifecycle: join timeout, then thread_exit(55) -> %d\n", code);

    size_t free0 = papp_mem_free_size(PAPP_MEM_FAST);
    int good = 0;
    int64_t t0 = papp_time_us();
    for (int i = 0; i < 200; i++) {
        papp_thread_cfg_t cfg = { .stack_size = (i & 1) ? 4096 : 40960 };   /* odd: internal, even: PSRAM */
        papp_thread_t *w = papp_thread_create(nop_worker, (void *)(intptr_t)i, &cfg);
        int c = -1;
        if (w && papp_thread_join(w, 1000, &c) && c == i) good++;
    }
    int64_t us = papp_time_us() - t0;
    size_t free1 = papp_mem_free_size(PAPP_MEM_FAST);
    CHECK(good == 200 && free1 + 256 >= free0,
          "lifecycle: 200 create/join cycles, %lld us each, internal free %u -> %u\n",
          us / 200, (unsigned)free0, (unsigned)free1);

    papp_thread_t *all[PAPP_MAX_THREADS + 1];
    int n = 0;
    papp_sem_t *gate = papp_sem_create(0, PAPP_MAX_THREADS);
    while (n <= PAPP_MAX_THREADS && (all[n] = papp_thread_create((papp_thread_fn_t)papp_sem_take, gate, NULL))) n++;
    for (int i = 0; i < n; i++) papp_sem_give(gate);
    for (int i = 0; i < n; i++) papp_thread_join(all[i], PAPP_WAIT_FOREVER, NULL);
    CHECK(n == PAPP_MAX_THREADS, "lifecycle: thread limit %d (got %d)\n", PAPP_MAX_THREADS, n);
    papp_sem_destroy(gate);
}

/* ── Teardown modes ────────────────────────────────────────────────────── */

static int spin_worker(void *arg)
{
    volatile uint32_t x = 0;
    for (;;) x++;
    return 0;
}

static int lock_worker(void *m)  { papp_mutex_lock(m); return 0; }
static int queue_worker(void *q) { int v; papp_queue_recv(q, &v, PAPP_WAIT_FOREVER); return 0; }

static int exiter(void *arg)
{
    papp_delay_ms(50);
    printf("worker calling exit(7)\n");
    exit(7);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "all";
    if (!papp_if_thrd()) {
        printf("launcher has no THRD interface\n");
        return 99;
    }

    if (!strcmp(mode, "leave")) {
        papp_mutex_t *m = papp_mutex_create(false);
        papp_queue_t *q = papp_queue_create(4, sizeof(int));
        papp_mutex_lock(m);                                   /* main holds it and leaves */
        papp_thread_cfg_t sc = { .name = "spin", .prio = PAPP_PRIO_LOW, .core = 1 };
        papp_thread_create(spin_worker, NULL, &sc);
        papp_thread_create(lock_worker, m, NULL);
        papp_thread_create(queue_worker, q, NULL);
        papp_delay_ms(100);
        printf("returning with 3 threads running\n");
        return 3;
    }
    if (!strcmp(mode, "exit")) {
        papp_thread_create(exiter, NULL, NULL);
        for (;;) papp_delay_ms(1000);                        /* main never returns */
    }
    if (!strcmp(mode, "spin")) {
        papp_thread_cfg_t c0 = { .name = "spin0", .core = 0 }, c1 = { .name = "spin1", .core = 1 };
        papp_thread_create(spin_worker, NULL, &c0);
        papp_thread_create(spin_worker, NULL, &c1);
        printf("spinning; main waits for the exit request\n");
        while (!papp_exit_requested()) papp_delay_ms(10);
        printf("exit requested; returning with spinners running\n");
        return 4;
    }

    test_mutex();
    test_queue();
    test_sem();
    test_libc();
    test_lifecycle();
    printf("%d failed\n", s_fail);
    return s_fail;
}
