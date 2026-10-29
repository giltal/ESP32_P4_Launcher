/*
 * PAPP runtime start-up: the loader calls _papp_start(argc, argv) after
 * relocation and import resolution (.bss is already zeroed).
 */
#include "papp/papp.h"

extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);
extern void (*__fini_array_start[])(void);
extern void (*__fini_array_end[])(void);

int main(int argc, char **argv);
void papp_rt_init(void) __attribute__((weak));

static void *s_exit_jb[5];
static int   s_exit_code;

__attribute__((section(".text.papp_entry"), used))
int _papp_start(int argc, char **argv)
{
    if (papp_rt_init) papp_rt_init();

    for (void (**f)(void) = __init_array_start; f < __init_array_end; f++)
        (*f)();

    if (__builtin_setjmp(s_exit_jb) == 0)
        s_exit_code = main(argc, argv);

    for (void (**f)(void) = __fini_array_end; f > __fini_array_start;)
        (*--f)();

    return s_exit_code;
}

void papp_exit(int code)
{
    const papp_if_thrd_t *thrd = papp_if_thrd();
    if (thrd && thrd->thread_self())       /* worker thread: can't unwind main's stack */
        thrd->app_exit(code);
    s_exit_code = code;
    __builtin_longjmp(s_exit_jb, 1);
}
