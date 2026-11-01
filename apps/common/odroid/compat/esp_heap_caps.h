/* PAPP port shim: heap_caps on the launcher's MEM interface */
#pragma once
#include <stddef.h>
#include <stdlib.h>          /* as the real header: callers free() without including it */
#include <stdint.h>
#include <stdbool.h>
#define MALLOC_CAP_EXEC      (1 << 0)
#define MALLOC_CAP_32BIT     (1 << 1)
#define MALLOC_CAP_8BIT      (1 << 2)
#define MALLOC_CAP_DMA       (1 << 3)
#define MALLOC_CAP_SPIRAM    (1 << 10)
#define MALLOC_CAP_INTERNAL  (1 << 11)
#define MALLOC_CAP_DEFAULT   (1 << 12)
#define MALLOC_CAP_RETENTION (1 << 14)
#define MALLOC_CAP_RTCRAM    (1 << 15)
#define MALLOC_CAP_TCM       (1 << 16)
#define MALLOC_CAP_SIMD      (1 << 17)
#ifdef __cplusplus
extern "C" {
#endif
void  *heap_caps_malloc(size_t size, uint32_t caps);
void  *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void  *heap_caps_realloc(void *p, size_t size, uint32_t caps);
void  *heap_caps_aligned_alloc(size_t align, size_t size, uint32_t caps);
void  *heap_caps_aligned_calloc(size_t align, size_t n, size_t size, uint32_t caps);
void   heap_caps_free(void *p);
void   heap_caps_aligned_free(void *p);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
void   heap_caps_print_heap_info(uint32_t caps);
#ifdef __cplusplus
}
#endif
