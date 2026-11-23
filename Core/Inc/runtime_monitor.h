#ifndef RUNTIME_MONITOR_H
#define RUNTIME_MONITOR_H
#include <stdint.h>
#include "main.h"
#include "cmsis_os.h"
#include "project_experiment.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { TASK_SAFETY=0, TASK_CHARGING, TASK_SENSOR, TASK_COMMUNICATION, TASK_LOGGING, TASK_COUNT } TaskIndex_t;
typedef enum { HEALTH_HEALTHY=0, HEALTH_WARNING, HEALTH_DEGRADED, HEALTH_CRITICAL } HealthStatus_t;
#define SAFETY_PERIOD_MS 100U
#define CHARGING_PERIOD_MS 50U
#define SENSOR_PERIOD_MS 100U
#define COMMUNICATION_PERIOD_MS 200U
#define LOGGING_PERIOD_MS 250U
#define MONITOR_PERIOD_MS 1000U
#define DIAG_QUEUE_LENGTH 20U
/* Main demonstration follows the project requirement: deadline = period. */
#define COMMUNICATION_DEADLINE_MS COMMUNICATION_PERIOD_MS
#define SAFETY_EXEC_BUDGET_US 250U
#define CHARGING_EXEC_BUDGET_US 125U
#define SENSOR_EXEC_BUDGET_US 250U
#define COMMUNICATION_EXEC_BUDGET_US 500U
#define LOGGING_EXEC_BUDGET_US 625U
#ifndef DETAILED_DIAGNOSTICS
#define DETAILED_DIAGNOSTICS 1
#endif
#define REPORT_INTERVAL_CYCLES 1U
#define EXPERIMENT_STOP_CYCLE MRG_EXPERIMENT_CYCLES
#define W_EXEC (0.20f/0.85f)
#define W_DEAD (0.25f/0.85f)
#define W_RESP (0.15f/0.85f)
#define W_ERROR (0.15f/0.85f)
#define W_STATE (0.10f/0.85f)
#define HEALTH_THRESHOLD_HEALTHY 80.0f
#define HEALTH_THRESHOLD_WARNING 60.0f
#define HEALTH_THRESHOLD_DEGRADED 40.0f
#define RTM_PERSISTENCE_WINDOWS 3U
#define RTM_MAX_ERRORS_FOR_NORM 10U
typedef struct {
    const char *name;
    osThreadId_t handle;
    uint32_t period_ms, deadline_ms, exec_budget_us;
    uint32_t execution_time_us, response_time_us, elapsed_time_us;
    uint32_t deadline_miss_count, total_releases, error_count;
    uint32_t window_releases, window_misses, window_errors;
    uint32_t generation, window_generation, window_fault_jobs;
    uint8_t window_mixed_generation;
    uint64_t window_response_sum_us;
    uint32_t release_tick, consecutive_bad, last_state;
    float execution_health, deadline_health, response_health, queue_health, error_health, state_health;
    float health_score, previous_health, health_delta;
    uint8_t health_status, sample_valid;
} TaskHealth_t;
typedef struct {
    osMessageQueueId_t handle;
    uint32_t capacity, current_count, drop_count;
    float utilization;
} SystemDiagQueue_t;
typedef struct { uint8_t task_id; } DiagMsg_t;
extern TaskHealth_t g_taskHealth[TASK_COUNT];
extern SystemDiagQueue_t g_diagQueue;
extern osMessageQueueId_t DiagQueueHandle;
extern osMutexId_t HealthMutexHandle;
void RTM_Init(void);
void RTM_TaskStart(TaskIndex_t idx);
void RTM_TaskStartAt(TaskIndex_t idx, uint32_t planned_release_tick);
void RTM_TaskEnd(TaskIndex_t idx, uint32_t start_cycle, uint32_t end_cycle);
void RTM_ReportError(TaskIndex_t idx);
/* Task-boundary only: preserve lifetime counters and old pending samples. */
uint32_t RTM_AdvanceTaskGeneration(TaskIndex_t idx);
void RTM_TagFaultExposure(TaskIndex_t idx);
/* Returns 1 only when a fresh scoring window was captured. */
uint8_t RTM_Update(void);
/* One frozen scoring window for every observer; no live/old table mixing. */
uint8_t RTM_CopyWindow(TaskHealth_t out[TASK_COUNT], SystemDiagQueue_t *queue, uint32_t *sequence);
const char *RTM_GetStateName(uint32_t state);
const char *RTM_GetHealthName(uint8_t status);
void RTM_PrintFullTable(UART_HandleTypeDef *uart);
void RTM_PrintCompactSummary(UART_HandleTypeDef *uart, uint32_t cycle);
/* Called by FreeRTOS trace macros. No blocking, UART, heap or RTOS API calls. */
void RTM_TraceSwitchIn(void *handle);
void RTM_TraceSwitchOut(void *handle);
uint8_t RTM_TraceIsActive(void);
#ifdef __cplusplus
}
#endif
#endif
