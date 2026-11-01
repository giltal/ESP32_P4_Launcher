/*
 * FreeRTOS subset on the launcher's THRD interface (see compat/freertos).
 *
 * Tasks are app threads plus a counting semaphore for task notifications.
 * Queues add a one-item "peek" slot, because THRD queues have no peek: a
 * peeked item is held there and handed to the next receive. Deleting another
 * task is not possible in v2 (threads end themselves); such tasks keep
 * running until they return or the app exits.
 */
#include "papp/papp.h"
#include "freertos/FreeRTOS.h"

#include <string.h>

/* ── Tasks ─────────────────────────────────────────────────────────────── */

#define MAX_TASKS 24

struct rtos_task {
    papp_thread_t *th;              /* NULL for the app's main thread */
    papp_sem_t    *notify;
    TaskFunction_t fn;
    void          *arg;
    bool           used;
};

static struct rtos_task s_tasks[MAX_TASKS];
static papp_mutex_t *s_crit;
static papp_mutex_t *s_tab;

static void lazy_init(void)
{
    if (!s_crit) s_crit = papp_mutex_create(true);
    if (!s_tab) s_tab = papp_mutex_create(true);
}

static struct rtos_task *task_alloc(void)
{
    lazy_init();
    papp_mutex_lock(s_tab);
    struct rtos_task *t = NULL;
    for (int i = 0; i < MAX_TASKS && !t; i++)
        if (!s_tasks[i].used) {
            t = &s_tasks[i];
            memset(t, 0, sizeof(*t));
            t->used = true;
            t->notify = papp_sem_create(0, 0x7FFFFFFF);
        }
    papp_mutex_unlock(s_tab);
    return t;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    lazy_init();
    papp_thread_t *self = papp_thread_self();
    papp_mutex_lock(s_tab);
    struct rtos_task *t = NULL;
    for (int i = 0; i < MAX_TASKS && !t; i++)
        if (s_tasks[i].used && s_tasks[i].th == self && (self || !s_tasks[i].fn)) t = &s_tasks[i];
    papp_mutex_unlock(s_tab);
    if (!t && (t = task_alloc())) t->th = self;           /* first call from main / foreign thread */
    return t;
}

static int task_entry(void *p)
{
    struct rtos_task *t = p;
    t->fn(t->arg);
    t->used = false;              /* a FreeRTOS task should not return; tolerate it */
    return 0;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core)
{
    struct rtos_task *t = task_alloc();
    if (!t) return pdFAIL;
    t->fn = fn;
    t->arg = arg;
    papp_thread_cfg_t cfg = {
        .name = name,
        .stack_size = stack < 2048 ? 2048 : stack,
        .prio = prio >= 6 ? PAPP_PRIO_HIGH : prio <= 2 ? PAPP_PRIO_LOW : PAPP_PRIO_NORMAL,
        .core = (core == 0 || core == 1) ? (int8_t)core : -1,
        .flags = stack > 32 * 1024 ? PAPP_THREAD_STACK_PSRAM : 0,
    };
    papp_mutex_lock(s_tab);                  /* t->th set before the task can look itself up */
    t->th = papp_thread_create(task_entry, t, &cfg);
    papp_mutex_unlock(s_tab);
    if (!t->th) {
        papp_sem_destroy(t->notify);
        t->used = false;
        return pdFAIL;
    }
    if (out) *out = t;
    return pdPASS;
}

TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                           UBaseType_t prio, StackType_t *sbuf, StaticTask_t *tcb, BaseType_t core)
{
    TaskHandle_t t = NULL;
    xTaskCreatePinnedToCore(fn, name, stack, arg, prio, &t, core);
    return t;
}

void vTaskDelete(TaskHandle_t t)
{
    if (!t || t == xTaskGetCurrentTaskHandle()) {
        TaskHandle_t me = xTaskGetCurrentTaskHandle();
        if (me && me->th) {
            me->used = false;
            papp_thread_exit(0);
        }
        return;                    /* the main thread can't delete itself */
    }
    /* another task: v2 cannot kill it (it ends itself or with the app) */
}

void vTaskDelay(TickType_t ticks)     { papp_delay_ms(ticks); }
TickType_t xTaskGetTickCount(void)    { return (TickType_t)(papp_time_us() / 1000); }

void vTaskDelayUntil(TickType_t *prev, TickType_t inc)
{
    TickType_t now = xTaskGetTickCount(), next = *prev + inc;
    if ((int32_t)(next - now) > 0) papp_delay_ms(next - now);
    *prev = next;
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (!me || !papp_sem_take(me->notify, wait == portMAX_DELAY ? PAPP_WAIT_FOREVER : wait)) return 0;
    uint32_t n = 1;
    if (clear) while (papp_sem_take(me->notify, 0)) n++;
    return n;
}

BaseType_t xTaskNotifyGive(TaskHandle_t t)
{
    if (t && t->notify) papp_sem_give(t->notify);
    return pdPASS;
}

void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *woken) { xTaskNotifyGive(t); }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t)        { return 1024; }
eTaskState eTaskGetState(TaskHandle_t t)                        { return t && t->used ? eReady : eDeleted; }
BaseType_t xPortGetCoreID(void)                                 { return 0; }
void vTaskSuspend(TaskHandle_t t)                               { }
void vTaskResume(TaskHandle_t t)                                { }
void vTaskSuspendAll(void)                                      { rtos_critical_enter(); }
BaseType_t xTaskResumeAll(void)                                 { rtos_critical_exit(); return pdFALSE; }
void taskYIELD(void)                                            { papp_yield(); }

void rtos_critical_enter(void)
{
    lazy_init();
    papp_mutex_lock(s_crit);
}

void rtos_critical_exit(void)
{
    papp_mutex_unlock(s_crit);
}

/* ── Queues, semaphores, mutexes ───────────────────────────────────────── */

enum { K_QUEUE, K_SEM, K_MUTEX, K_RMUTEX };

struct rtos_obj {
    int           kind;
    papp_queue_t *q;
    papp_sem_t   *s;
    papp_mutex_t *m;
    uint32_t      len, size;
    bool          peeked;          /* item held by xQueuePeek */
    uint8_t      *peek_buf;
    uint8_t      *scratch;         /* for items dropped by overwrite / reset */
};

static uint32_t wait_ms(TickType_t w) { return w == portMAX_DELAY ? PAPP_WAIT_FOREVER : w; }

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t size)
{
    struct rtos_obj *o = papp_alloc(sizeof(*o) + 2 * size, PAPP_MEM_FAST, 0);
    if (!o) return NULL;
    memset(o, 0, sizeof(*o));
    o->kind = K_QUEUE;
    o->len = len;
    o->size = size;
    o->peek_buf = (uint8_t *)(o + 1);
    o->scratch = o->peek_buf + size;
    o->q = papp_queue_create(len, size);
    if (!o->q) { papp_free(o); return NULL; }
    return o;
}

QueueHandle_t xQueueCreateStatic(UBaseType_t len, UBaseType_t size, uint8_t *buf, StaticQueue_t *q)
{
    return xQueueCreate(len, size);
}

void vQueueDelete(QueueHandle_t o)
{
    if (!o) return;
    if (o->q) papp_queue_destroy(o->q);
    if (o->s) papp_sem_destroy(o->s);
    if (o->m) papp_mutex_destroy(o->m);
    papp_free(o);
}

BaseType_t xQueueSend(QueueHandle_t o, const void *item, TickType_t w)
{
    if (!o) return pdFAIL;
    if (o->kind != K_QUEUE) return xSemaphoreGive(o);
    return papp_queue_send(o->q, item, wait_ms(w)) ? pdPASS : errQUEUE_FULL;
}

BaseType_t xQueueOverwrite(QueueHandle_t o, const void *item)
{
    if (!o) return pdFAIL;
    rtos_critical_enter();
    while (papp_queue_count(o->q) >= o->len) {             /* drop what the consumer hasn't taken */
        if (!papp_queue_recv(o->q, o->scratch, 0)) break;
    }
    BaseType_t r = papp_queue_send(o->q, item, 0) ? pdPASS : pdFAIL;
    rtos_critical_exit();
    return r;
}

BaseType_t xQueuePeek(QueueHandle_t o, void *item, TickType_t w)
{
    if (!o) return pdFAIL;
    if (!o->peeked) {
        if (!papp_queue_recv(o->q, o->peek_buf, wait_ms(w))) return pdFAIL;
        o->peeked = true;
    }
    memcpy(item, o->peek_buf, o->size);
    return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t o, void *item, TickType_t w)
{
    if (!o) return pdFAIL;
    if (o->kind != K_QUEUE) return xSemaphoreTake(o, w);
    if (o->peeked) {
        if (item) memcpy(item, o->peek_buf, o->size);
        o->peeked = false;
        return pdPASS;
    }
    return papp_queue_recv(o->q, item, wait_ms(w)) ? pdPASS : pdFAIL;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t o)
{
    if (!o) return 0;
    if (o->kind == K_SEM) return uxSemaphoreGetCount(o);
    return papp_queue_count(o->q) + (o->peeked ? 1 : 0);
}

UBaseType_t uxQueueSpacesAvailable(QueueHandle_t o)  { return o ? o->len - uxQueueMessagesWaiting(o) : 0; }

BaseType_t xQueueReset(QueueHandle_t o)
{
    if (!o || o->kind != K_QUEUE) return pdPASS;
    while (papp_queue_recv(o->q, o->scratch, 0)) { }
    o->peeked = false;
    return pdPASS;
}

static struct rtos_obj *obj_new(int kind)
{
    struct rtos_obj *o = papp_alloc(sizeof(*o), PAPP_MEM_FAST, 0);
    if (o) {
        memset(o, 0, sizeof(*o));
        o->kind = kind;
    }
    return o;
}

SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max, UBaseType_t initial)
{
    struct rtos_obj *o = obj_new(K_SEM);
    if (o && !(o->s = papp_sem_create(initial, max ? max : 1))) { papp_free(o); o = NULL; }
    return o;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void) { return xSemaphoreCreateCounting(1, 0); }

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct rtos_obj *o = obj_new(K_MUTEX);
    if (o && !(o->m = papp_mutex_create(false))) { papp_free(o); o = NULL; }
    return o;
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    struct rtos_obj *o = obj_new(K_RMUTEX);
    if (o && !(o->m = papp_mutex_create(true))) { papp_free(o); o = NULL; }
    return o;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t o, TickType_t w)
{
    if (!o) return pdFAIL;
    switch (o->kind) {
    case K_SEM:   return papp_sem_take(o->s, wait_ms(w)) ? pdTRUE : pdFALSE;
    case K_QUEUE: return xQueueReceive(o, NULL, w);
    default:      return papp_mutex_trylock(o->m, wait_ms(w)) ? pdTRUE : pdFALSE;
    }
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t o)
{
    if (!o) return pdFAIL;
    if (o->kind == K_SEM) papp_sem_give(o->s);
    else if (o->kind != K_QUEUE) papp_mutex_unlock(o->m);
    return pdTRUE;
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t o, TickType_t w) { return xSemaphoreTake(o, w); }
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t o)               { return xSemaphoreGive(o); }
void vSemaphoreDelete(SemaphoreHandle_t o)                            { vQueueDelete(o); }

UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t o)
{
    if (!o || o->kind != K_SEM) return 0;
    UBaseType_t n = 0;
    while (papp_sem_take(o->s, 0)) n++;              /* no "count" in THRD: take and give back */
    for (UBaseType_t i = 0; i < n; i++) papp_sem_give(o->s);
    return n;
}
