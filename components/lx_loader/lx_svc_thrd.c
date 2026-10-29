/*
 * THRD interface: app threads, mutexes, semaphores and queues.
 *
 * Threads are static FreeRTOS tasks whose TCB, stack and bookkeeping the
 * launcher owns, so the App Manager can stop them with the main task (see
 * lx_thrd_suspend_all / lx_thrd_kill_all) and free them afterwards. Blocking
 * waits run outside SVC_ENTER/SVC_LEAVE: a task blocked in the kernel holds
 * no launcher lock and can be deleted at any time.
 *
 * Sync objects carry a magic word (cheap check on the hot path) and sit on a
 * list so whatever the app leaves behind is freed at the end of the run.
 */
#include "lx_services.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"

static const char *TAG = "lx_thrd";

#define THREAD_MAGIC        0x54485244u     /* 'THRD' */
#define DEFAULT_STACK       (8 * 1024)
#define MIN_STACK           (2 * 1024)
#define MAX_INTERNAL_STACK  (32 * 1024)
#define QUEUE_PSRAM_BYTES   1024            /* queue storage above this goes to PSRAM */

static const UBaseType_t s_prio[] = { 3, 5, 6 };    /* PAPP_PRIO_LOW / NORMAL / HIGH */

struct papp_thread {
    uint32_t          magic;
    int               slot;
    TaskHandle_t      task;
    StaticTask_t     *tcb;
    StackType_t      *stack;
    papp_thread_fn_t  fn;
    void             *arg;
    SemaphoreHandle_t done;
    StaticSemaphore_t done_buf;
    int               code;
};

/* Thread table. s_tasks/s_tls are plain values so tls_slot()/thread_self()
 * can scan them without touching a thread struct that is being freed. */
static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static papp_thread_t *s_threads[PAPP_MAX_THREADS];
static TaskHandle_t   s_tasks[PAPP_MAX_THREADS];
static void          *s_tls[PAPP_MAX_THREADS];
static TaskHandle_t   s_main;
static void          *s_main_tls;

/* ── Sync objects ───────────────────────────────────────────────────────── */

enum { OBJ_MUTEX = 1, OBJ_RMUTEX, OBJ_SEM, OBJ_QUEUE };
#define OBJ_MAGIC 0x4F424A21u     /* 'OBJ!' */

typedef struct obj {
    uint32_t     magic;
    uint8_t      kind;
    QueueHandle_t h;               /* semaphores are queues too */
    struct obj  *prev, *next;
} obj_t;

static obj_t *s_objs;

static obj_t *obj_new(uint8_t kind, QueueHandle_t h)
{
    if (!h) return NULL;
    obj_t *o = heap_caps_calloc(1, sizeof(*o), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!o) return NULL;
    o->magic = OBJ_MAGIC;
    o->kind = kind;
    o->h = h;
    taskENTER_CRITICAL(&s_mux);
    o->next = s_objs;
    if (s_objs) s_objs->prev = o;
    s_objs = o;
    taskEXIT_CRITICAL(&s_mux);
    return o;
}

static void obj_free(obj_t *o)
{
    taskENTER_CRITICAL(&s_mux);
    if (o->prev) o->prev->next = o->next;
    else s_objs = o->next;
    if (o->next) o->next->prev = o->prev;
    taskEXIT_CRITICAL(&s_mux);
    o->magic = 0;
    if (o->kind == OBJ_QUEUE) vQueueDeleteWithCaps(o->h);
    else vSemaphoreDelete(o->h);
    heap_caps_free(o);
}

static obj_t *obj_check(void *p, uint8_t kind)
{
    obj_t *o = p;
    if (!o || o->magic != OBJ_MAGIC ||
        !(o->kind == kind || (kind == OBJ_MUTEX && o->kind == OBJ_RMUTEX))) {
        ESP_LOGE(TAG, "bad handle %p", p);
        return NULL;
    }
    return o;
}

static TickType_t ticks(uint32_t ms)
{
    if (ms == PAPP_WAIT_FOREVER) return portMAX_DELAY;
    TickType_t t = pdMS_TO_TICKS(ms);
    return t ? t : (ms ? 1 : 0);
}

/* ── Threads ────────────────────────────────────────────────────────────── */

static int self_slot(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < PAPP_MAX_THREADS; i++)
        if (s_tasks[i] == me) return i;
    return -1;
}

static void __attribute__((noreturn)) finish(papp_thread_t *t, int code)
{
    t->code = code;
    xSemaphoreGive(t->done);
    for (;;) vTaskSuspend(NULL);     /* the joiner or the App Manager deletes us */
}

static void thread_entry(void *arg)
{
    papp_thread_t *t = arg;
    s_tasks[t->slot] = xTaskGetCurrentTaskHandle();     /* may run before the creator returns */
    finish(t, t->fn(t->arg));
}

static void thread_free(papp_thread_t *t)
{
    taskENTER_CRITICAL(&s_mux);
    s_threads[t->slot] = NULL;
    s_tasks[t->slot] = NULL;
    s_tls[t->slot] = NULL;
    taskEXIT_CRITICAL(&s_mux);
    t->magic = 0;
    heap_caps_free(t->stack);
    heap_caps_free(t->tcb);
    heap_caps_free(t);
}

static papp_thread_t *thrd_create(const papp_thread_cfg_t *cfg, papp_thread_fn_t fn, void *arg)
{
    if (!fn) return NULL;
    SVC_ENTER();
    papp_thread_cfg_t c = cfg ? *cfg : (papp_thread_cfg_t){ 0 };
    uint32_t stack = c.stack_size ? c.stack_size : DEFAULT_STACK;
    if (stack < MIN_STACK) stack = MIN_STACK;
    stack = (stack + 15) & ~15u;
    bool psram = stack > MAX_INTERNAL_STACK || (c.flags & PAPP_THREAD_STACK_PSRAM);
    UBaseType_t prio = s_prio[c.prio <= PAPP_PRIO_HIGH ? c.prio : PAPP_PRIO_NORMAL];
    BaseType_t core = (c.core == 0 || c.core == 1) ? c.core : tskNO_AFFINITY;

    papp_thread_t *t = heap_caps_calloc(1, sizeof(*t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (t) {
        t->tcb = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        t->stack = heap_caps_malloc(stack, psram ? MALLOC_CAP_SPIRAM : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    int slot = -1;
    if (t && t->tcb && t->stack) {
        taskENTER_CRITICAL(&s_mux);
        for (int i = 0; i < PAPP_MAX_THREADS; i++)
            if (!s_threads[i]) { slot = i; s_threads[i] = t; break; }
        taskEXIT_CRITICAL(&s_mux);
    }
    if (slot < 0) {
        if (t) { heap_caps_free(t->stack); heap_caps_free(t->tcb); heap_caps_free(t); }
        ESP_LOGE(TAG, "thread create failed (%s)", t ? "no free slot or memory" : "out of memory");
        SVC_LEAVE();
        return NULL;
    }

    t->magic = THREAD_MAGIC;
    t->slot = slot;
    t->fn = fn;
    t->arg = arg;
    t->done = xSemaphoreCreateBinaryStatic(&t->done_buf);
    t->task = xTaskCreateStaticPinnedToCore(thread_entry, c.name ? c.name : "papp_thr",
                                            stack / sizeof(StackType_t), t, prio, t->stack, t->tcb, core);
    s_tasks[slot] = t->task;
    SVC_LEAVE();
    return t;
}

static papp_thread_t *thread_check(papp_thread_t *t)
{
    if (!t || t->magic != THREAD_MAGIC || t->slot < 0 || t->slot >= PAPP_MAX_THREADS ||
        s_threads[t->slot] != t) {
        ESP_LOGE(TAG, "bad thread handle %p", t);
        return NULL;
    }
    return t;
}

static bool thrd_join(papp_thread_t *t, uint32_t timeout_ms, int *exit_code)
{
    if (!thread_check(t)) return false;
    if (t->task == xTaskGetCurrentTaskHandle()) return false;      /* joining yourself */
    if (xSemaphoreTake(t->done, ticks(timeout_ms)) != pdTRUE) return false;

    SVC_ENTER();
    /* finish() has given `done` and holds no lock, so the task can go as soon
     * as it is off the CPU (ready, parked or preempted on our core). If it is
     * still running on the other core it is a few us from parking. */
    for (int n = 0; eTaskGetState(t->task) == eRunning; n++) {
        if (n < 200) esp_rom_delay_us(5);
        else vTaskDelay(1);
    }
    vTaskDelete(t->task);
    if (exit_code) *exit_code = t->code;
    thread_free(t);
    SVC_LEAVE();
    return true;
}

static void __attribute__((noreturn)) thrd_app_exit(int code);

static void thrd_thread_exit(int code)
{
    int slot = self_slot();
    if (slot < 0) thrd_app_exit(code);          /* main thread: same as exit() */
    finish(s_threads[slot], code);
}

static papp_thread_t *thrd_self(void)
{
    int slot = self_slot();
    return slot < 0 ? NULL : s_threads[slot];
}

static void thrd_yield(void)
{
    taskYIELD();
}

static void **thrd_tls_slot(void)
{
    if (xTaskGetCurrentTaskHandle() == s_main) return &s_main_tls;
    int slot = self_slot();
    return slot < 0 ? NULL : &s_tls[slot];
}

static void __attribute__((noreturn)) thrd_app_exit(int code)
{
    lx_app_post_exit(code);
    for (;;) vTaskSuspend(NULL);
}

/* ── Mutexes, semaphores, queues ────────────────────────────────────────── */

static papp_mutex_t *thrd_mutex_create(bool recursive)
{
    SVC_ENTER();
    obj_t *o = recursive ? obj_new(OBJ_RMUTEX, xSemaphoreCreateRecursiveMutex())
                         : obj_new(OBJ_MUTEX, xSemaphoreCreateMutex());
    SVC_LEAVE();
    return (papp_mutex_t *)o;
}

static void thrd_obj_destroy(void *p, uint8_t kind)
{
    if (!p) return;
    SVC_ENTER();
    obj_t *o = obj_check(p, kind);
    if (o) obj_free(o);
    SVC_LEAVE();
}

static void thrd_mutex_destroy(papp_mutex_t *m) { thrd_obj_destroy(m, OBJ_MUTEX); }

static bool thrd_mutex_lock(papp_mutex_t *m, uint32_t timeout_ms)
{
    obj_t *o = obj_check(m, OBJ_MUTEX);
    if (!o) return false;
    return (o->kind == OBJ_RMUTEX ? xSemaphoreTakeRecursive(o->h, ticks(timeout_ms))
                                  : xSemaphoreTake(o->h, ticks(timeout_ms))) == pdTRUE;
}

static void thrd_mutex_unlock(papp_mutex_t *m)
{
    obj_t *o = obj_check(m, OBJ_MUTEX);
    if (!o) return;
    if (o->kind == OBJ_RMUTEX) xSemaphoreGiveRecursive(o->h);
    else xSemaphoreGive(o->h);
}

static papp_sem_t *thrd_sem_create(uint32_t initial, uint32_t max)
{
    if (max == 0 || initial > max) return NULL;
    SVC_ENTER();
    obj_t *o = obj_new(OBJ_SEM, xSemaphoreCreateCounting(max, initial));
    SVC_LEAVE();
    return (papp_sem_t *)o;
}

static void thrd_sem_destroy(papp_sem_t *s) { thrd_obj_destroy(s, OBJ_SEM); }

static bool thrd_sem_take(papp_sem_t *s, uint32_t timeout_ms)
{
    obj_t *o = obj_check(s, OBJ_SEM);
    return o && xSemaphoreTake(o->h, ticks(timeout_ms)) == pdTRUE;
}

static void thrd_sem_give(papp_sem_t *s)
{
    obj_t *o = obj_check(s, OBJ_SEM);
    if (o) xSemaphoreGive(o->h);
}

static papp_queue_t *thrd_queue_create(uint32_t length, uint32_t item_size)
{
    if (!length || !item_size || length > 65536 || item_size > 65536) return NULL;
    SVC_ENTER();
    UBaseType_t caps = length * item_size > QUEUE_PSRAM_BYTES ? MALLOC_CAP_SPIRAM
                                                              : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    obj_t *o = obj_new(OBJ_QUEUE, xQueueCreateWithCaps(length, item_size, caps));
    SVC_LEAVE();
    return (papp_queue_t *)o;
}

static void thrd_queue_destroy(papp_queue_t *q) { thrd_obj_destroy(q, OBJ_QUEUE); }

static bool thrd_queue_send(papp_queue_t *q, const void *item, uint32_t timeout_ms)
{
    obj_t *o = obj_check(q, OBJ_QUEUE);
    return o && xQueueSend(o->h, item, ticks(timeout_ms)) == pdTRUE;
}

static bool thrd_queue_recv(papp_queue_t *q, void *item, uint32_t timeout_ms)
{
    obj_t *o = obj_check(q, OBJ_QUEUE);
    return o && xQueueReceive(o->h, item, ticks(timeout_ms)) == pdTRUE;
}

static uint32_t thrd_queue_count(papp_queue_t *q)
{
    obj_t *o = obj_check(q, OBJ_QUEUE);
    return o ? uxQueueMessagesWaiting(o->h) : 0;
}

const papp_if_thrd_t lx_if_thrd = {
    .hdr = { PAPP_IF_THRD, PAPP_IF_THRD_VER, sizeof(papp_if_thrd_t) },
    .thread_create = thrd_create,
    .thread_join   = thrd_join,
    .thread_exit   = thrd_thread_exit,
    .thread_self   = thrd_self,
    .yield         = thrd_yield,
    .tls_slot      = thrd_tls_slot,
    .app_exit      = thrd_app_exit,
    .mutex_create  = thrd_mutex_create,
    .mutex_destroy = thrd_mutex_destroy,
    .mutex_lock    = thrd_mutex_lock,
    .mutex_unlock  = thrd_mutex_unlock,
    .sem_create    = thrd_sem_create,
    .sem_destroy   = thrd_sem_destroy,
    .sem_take      = thrd_sem_take,
    .sem_give      = thrd_sem_give,
    .queue_create  = thrd_queue_create,
    .queue_destroy = thrd_queue_destroy,
    .queue_send    = thrd_queue_send,
    .queue_recv    = thrd_queue_recv,
    .queue_count   = thrd_queue_count,
};

/* ── App Manager hooks ──────────────────────────────────────────────────── */

void lx_thrd_begin_run(void *main_task)
{
    s_main = main_task;
    s_main_tls = NULL;
}

void lx_thrd_suspend_all(bool suspend)
{
    for (int i = 0; i < PAPP_MAX_THREADS; i++) {
        TaskHandle_t h = s_tasks[i];
        if (!h) continue;
        if (suspend) vTaskSuspend(h);
        else vTaskResume(h);
    }
}

/* All threads are suspended and none is inside a service call. */
int lx_thrd_kill_all(void)
{
    int n = 0;
    for (int i = 0; i < PAPP_MAX_THREADS; i++) {
        papp_thread_t *t = s_threads[i];
        if (!t) continue;
        vTaskDelete(t->task);
        thread_free(t);
        n++;
    }
    s_main = NULL;
    return n;
}

int lx_thrd_end_run(void)
{
    int n = 0;
    while (s_objs) {
        obj_free(s_objs);
        n++;
    }
    return n;
}
