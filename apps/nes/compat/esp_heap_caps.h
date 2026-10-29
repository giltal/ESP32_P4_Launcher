/* PAPP port of nofrendo: heap_caps → the app heap (the launcher's MEM interface). */
#pragma once

#include <stdlib.h>

#define MALLOC_CAP_SPIRAM    0
#define MALLOC_CAP_INTERNAL  0
#define MALLOC_CAP_8BIT      0
#define heap_caps_malloc(size, caps)        malloc(size)
#define heap_caps_calloc(n, size, caps)     calloc(n, size)
#define heap_caps_realloc(p, size, caps)    realloc(p, size)
#define heap_caps_free(p)                   free(p)
