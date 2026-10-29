/*
 * hello — smallest PAPP (no libc). Exercises SYS/MEM imports and relocations.
 *   no args : print a few lines and return 0
 *   "loop"  : run until the launcher asks us to exit (STOP)
 *   "hang"  : ignore exit requests (tests forced stop)
 *   "leak"  : allocate and never free (tests teardown reclaim)
 *   "crash" : store through a NULL pointer (tests crash reporting)
 */
#include "papp/papp.h"

PAPP_MANIFEST(.id = "demo.hello", .name = "Hello", .version = "0.1.0", .main_stack = 8192);

static const char *const s_words[] = { "relocated", "pointer", "table", "works" };
static int (*const s_ops[])(int, int) = { 0 };
static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }
static int (*s_fn[2])(int, int) = { add, mul };
static int s_counter = 40;

static bool streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int main(int argc, char **argv)
{
    (void)s_ops;
    papp_log("hello from PAPP v2 on %s (launcher ABI %lx), main=%p\n",
             papp_board_id(), (unsigned long)papp_launcher_abi(), (void *)main);
    for (int i = 0; i < argc; i++) papp_log("argv[%d] = %s\n", i, argv[i]);
    papp_log("%s %s %s %s\n", s_words[0], s_words[1], s_words[2], s_words[3]);

    int r = s_fn[0](s_counter, 2) + s_fn[1](3, 4);       /* 42 + 12 */
    if (r != 54) { papp_log("FAIL: fn table gave %d\n", r); return 3; }

    const char *mode = argc > 1 ? argv[1] : "";
    if (streq(mode, "crash")) {
        papp_log("crashing on purpose\n");
        *(volatile int *)0 = 1;
    }
    if (streq(mode, "leak")) {
        for (int i = 0; i < 10; i++) papp_alloc(1000 + i * 5000, PAPP_MEM_ANY, 0);
        papp_log("leaked 10 blocks on purpose\n");
        return 0;
    }
    if (streq(mode, "loop") || streq(mode, "hang")) {
        int64_t t0 = papp_time_us();
        for (int n = 0;; n++) {
            if (streq(mode, "loop") && papp_exit_requested()) break;
            if (n % 10 == 0) papp_log("tick %d (%lld ms)\n", n, (papp_time_us() - t0) / 1000);
            papp_delay_ms(100);
        }
        papp_log("exit requested, leaving cleanly\n");
        return 7;
    }
    papp_log("bye\n");
    return 0;
}
