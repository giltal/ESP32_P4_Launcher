/*
 * The pthread subset libstdc++ calls for its internal locks and per-thread
 * exception globals (the toolchain's libstdc++ is built for "posix" threads),
 * on the launcher's THRD mutexes. Keys keep one value per key (C++ cores are
 * built with -fno-exceptions, so the per-thread part is never exercised).
 * Linked into C++ apps only.
 */
#include "papp/papp.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    papp_mutex_t *mx = papp_mutex_create(true);
    if (!mx) return ENOMEM;
    memcpy(m, &mx, sizeof(mx));
    return 0;
}

static papp_mutex_t *mx_of(pthread_mutex_t *m)
{
    papp_mutex_t *mx;
    memcpy(&mx, m, sizeof(mx));
    if (!mx || mx == (papp_mutex_t *)-1) {           /* statically initialised */
        pthread_mutex_init(m, NULL);
        memcpy(&mx, m, sizeof(mx));
    }
    return mx;
}

int pthread_mutex_lock(pthread_mutex_t *m)     { papp_mutex_lock(mx_of(m)); return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m)   { papp_mutex_unlock(mx_of(m)); return 0; }
int pthread_mutex_trylock(pthread_mutex_t *m)  { return papp_mutex_trylock(mx_of(m), 0) ? 0 : EBUSY; }

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    papp_mutex_t *mx;
    memcpy(&mx, m, sizeof(mx));
    if (mx && mx != (papp_mutex_t *)-1) papp_mutex_destroy(mx);
    return 0;
}

#define MAX_KEYS 8
static const void *s_key_val[MAX_KEYS];
static int s_nkeys;

int pthread_key_create(pthread_key_t *k, void (*dtor)(void *))
{
    if (s_nkeys >= MAX_KEYS) return EAGAIN;
    *k = (pthread_key_t)s_nkeys++;
    return 0;
}

int pthread_key_delete(pthread_key_t k)                 { return 0; }
void *pthread_getspecific(pthread_key_t k)              { return k < MAX_KEYS ? (void *)s_key_val[k] : NULL; }

int pthread_setspecific(pthread_key_t k, const void *v)
{
    if (k >= MAX_KEYS) return EINVAL;
    s_key_val[k] = v;
    return 0;
}

int pthread_once(pthread_once_t *once, void (*fn)(void))
{
    if (once->init_executed) return 0;
    once->init_executed = 1;
    fn();
    return 0;
}
