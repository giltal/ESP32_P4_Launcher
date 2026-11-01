/*
 * PAPP port shim: the FreeRTOS API subset the RetroESP32 emulators use, on
 * top of the launcher's THRD interface (rtos_papp.c). Ticks are milliseconds.
 * Queues and semaphores share one handle type, as in FreeRTOS.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_attr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef unsigned UBaseType_t;
typedef uint8_t  StackType_t;
typedef struct { uint32_t opaque[8]; } StaticTask_t;
typedef struct { uint32_t opaque[8]; } StaticQueue_t;
typedef StaticQueue_t StaticSemaphore_t;
typedef struct rtos_task *TaskHandle_t;
typedef struct rtos_obj  *QueueHandle_t;
typedef QueueHandle_t     SemaphoreHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef enum { eRunning = 0, eReady, eBlocked, eSuspended, eDeleted, eInvalid } eTaskState;
typedef struct { int dummy; } portMUX_TYPE;

#define pdFALSE              0
#define pdTRUE               1
#define pdPASS               1
#define pdFAIL               0
#define errQUEUE_FULL        0
#define errQUEUE_EMPTY       0
#define portMAX_DELAY        0xFFFFFFFFu
#define portTICK_PERIOD_MS   1
#define portTICK_RATE_MS     1
#define configTICK_RATE_HZ   1000
#define configMAX_PRIORITIES 25
#define tskNO_AFFINITY       0x7FFFFFFF
#define tskIDLE_PRIORITY     0
#define pdMS_TO_TICKS(ms)    ((TickType_t)(ms))
#define pdTICKS_TO_MS(t)     ((uint32_t)(t))
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portNUM_PROCESSORS   2
#define configASSERT(x)      do { (void)(x); } while (0)
#define IRAM_ATTR_ISR

/* tasks */
BaseType_t   xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                     UBaseType_t prio, TaskHandle_t *out, BaseType_t core);
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                           UBaseType_t prio, StackType_t *stack_buf, StaticTask_t *tcb,
                                           BaseType_t core);
#define xTaskCreate(fn, name, stack, arg, prio, out) \
    xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, tskNO_AFFINITY)
#define xTaskCreateStatic(fn, name, stack, arg, prio, sbuf, tcb) \
    xTaskCreateStaticPinnedToCore(fn, name, stack, arg, prio, sbuf, tcb, tskNO_AFFINITY)
void         vTaskDelete(TaskHandle_t t);
void         vTaskDelay(TickType_t ticks);
void         vTaskDelayUntil(TickType_t *prev, TickType_t inc);
TickType_t   xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
uint32_t     ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
BaseType_t   xTaskNotifyGive(TaskHandle_t t);
void         vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *woken);
UBaseType_t  uxTaskGetStackHighWaterMark(TaskHandle_t t);
eTaskState   eTaskGetState(TaskHandle_t t);
BaseType_t   xPortGetCoreID(void);
void         vTaskSuspend(TaskHandle_t t);
void         vTaskResume(TaskHandle_t t);
void         vTaskSuspendAll(void);
BaseType_t   xTaskResumeAll(void);
void         taskYIELD(void);
void         rtos_critical_enter(void);
void         rtos_critical_exit(void);
#define vTaskPrioritySet(t, p)        do { (void)(t); (void)(p); } while (0)
#define uxTaskPriorityGet(t)          5
#define portYIELD()                   taskYIELD()
#define portYIELD_FROM_ISR(...)       do { } while (0)
#define taskENTER_CRITICAL(m)         rtos_critical_enter()
#define taskEXIT_CRITICAL(m)          rtos_critical_exit()
#define portENTER_CRITICAL(m)         rtos_critical_enter()
#define portEXIT_CRITICAL(m)          rtos_critical_exit()
#define taskENTER_CRITICAL_ISR(m)     rtos_critical_enter()
#define taskEXIT_CRITICAL_ISR(m)      rtos_critical_exit()
#define portENTER_CRITICAL_ISR(m)     rtos_critical_enter()
#define portEXIT_CRITICAL_ISR(m)      rtos_critical_exit()

/* queues */
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size);
QueueHandle_t xQueueCreateStatic(UBaseType_t len, UBaseType_t item_size, uint8_t *buf, StaticQueue_t *q);
void          vQueueDelete(QueueHandle_t q);
BaseType_t    xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
BaseType_t    xQueueOverwrite(QueueHandle_t q, const void *item);
BaseType_t    xQueueReceive(QueueHandle_t q, void *item, TickType_t wait);
BaseType_t    xQueuePeek(QueueHandle_t q, void *item, TickType_t wait);
UBaseType_t   uxQueueMessagesWaiting(QueueHandle_t q);
UBaseType_t   uxQueueSpacesAvailable(QueueHandle_t q);
BaseType_t    xQueueReset(QueueHandle_t q);
#define xQueueSendToBack(q, i, w)          xQueueSend(q, i, w)
#define xQueueSendToFront(q, i, w)         xQueueSend(q, i, w)
#define xQueueSendFromISR(q, i, w)         xQueueSend(q, i, 0)
#define xQueueSendToBackFromISR(q, i, w)   xQueueSend(q, i, 0)
#define xQueueOverwriteFromISR(q, i, w)    xQueueOverwrite(q, i)
#define xQueueReceiveFromISR(q, i, w)      xQueueReceive(q, i, 0)
#define uxQueueMessagesWaitingFromISR(q)   uxQueueMessagesWaiting(q)

/* semaphores and mutexes (same handle type as queues) */
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max, UBaseType_t initial);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t s);
BaseType_t        xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t wait);
BaseType_t        xSemaphoreGiveRecursive(SemaphoreHandle_t s);
void              vSemaphoreDelete(SemaphoreHandle_t s);
UBaseType_t       uxSemaphoreGetCount(SemaphoreHandle_t s);
#define xSemaphoreCreateBinaryStatic(b)          xSemaphoreCreateBinary()
#define xSemaphoreCreateMutexStatic(b)           xSemaphoreCreateMutex()
#define xSemaphoreCreateCountingStatic(m, i, b)  xSemaphoreCreateCounting(m, i)
#define xSemaphoreGiveFromISR(s, w)              xSemaphoreGive(s)
#define xSemaphoreTakeFromISR(s, w)              xSemaphoreTake(s, 0)

#ifdef __cplusplus
}
#endif
