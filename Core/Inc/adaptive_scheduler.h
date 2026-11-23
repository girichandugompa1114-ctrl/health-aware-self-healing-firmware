#ifndef ADAPTIVE_SCHEDULER_H
#define ADAPTIVE_SCHEDULER_H
#include <stdint.h>
#include "runtime_monitor.h"
#include "health_engine.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Scheduling policy: isolate sustained Communication CPU overload below Logging (39).
 * All other application tasks retain their configured base priorities. */
#define AS_COMM_DEMOTED_PRIORITY 38
#ifndef AS_ENABLE_ADAPTATION
#define AS_ENABLE_ADAPTATION 1U
#endif
#define AS_CONFIRM_WINDOWS 3U
#define AS_RESTORE_WINDOWS 3U
#define AS_RECOVERY_WAIT_WINDOWS 3U
#define AS_NO_PROGRESS_WINDOWS 2U
#define AS_QUEUE_CONFIRM_WINDOWS 3U
#define AS_QUEUE_HIGH_UTIL 0.75f
/* Queue statistics are observed independently; queue pressure does not change priorities. */
typedef enum { EV_STATE_IDLE = 0, EV_STATE_CHARGING, EV_STATE_FAULT } EVChargingState_t;
typedef enum { CRIT_VERY_HIGH = 0, CRIT_HIGH, CRIT_MEDIUM_HIGH, CRIT_MEDIUM, CRIT_LOW } TaskCriticality_t;
/* The original reason values are retained for existing callers. */
typedef enum {
    AS_REASON_NONE = 0, AS_REASON_DEADLINE_URGENCY, AS_REASON_EXEC_OVERLOAD,
    AS_REASON_RECOVERED, AS_REASON_BLOCKED_STATE, AS_REASON_ERROR_FAULT,
    AS_REASON_QUEUE_STARVATION, AS_REASON_NO_HEADROOM, AS_REASON_CHARGING_FAULT_HOLD,
    AS_REASON_CONFIRMING, AS_REASON_NO_PROGRESS, AS_REASON_LOW_CRITICALITY_HOLD,
    AS_REASON_IDLE_HOLD, AS_REASON_PRIORITY_ERROR, AS_REASON_INVALID_HANDLE,
    AS_REASON_RECOVERY_ACTIVE, AS_REASON_BASELINE, AS_REASON_CPU_DEMOTION, AS_REASON_PROTECTED_HOLD, AS_REASON_NO_PRECEDENCE_CHANGE, AS_REASON_VERIFYING_HEALTHY
} AS_Reason_t;
/* Bit mask: several independent recovery causes can remain pending together. */
typedef enum {
    AS_FLAG_NONE = 0,
    AS_FLAG_NEEDS_RECOVERY_STATE = 1U << 0,
    AS_FLAG_NEEDS_RECOVERY_ERROR = 1U << 1,
    AS_FLAG_NEEDS_RECOVERY_PERSISTENT = 1U << 2,
    AS_FLAG_NEEDS_RECOVERY_PROGRESS = 1U << 3,
    AS_FLAG_NEEDS_RECOVERY_PRIORITY = 1U << 4,
    AS_FLAG_NEEDS_RECOVERY_HANDLE = 1U << 5
} AS_RecoveryFlag_t;
typedef struct {
    osThreadId_t handle;
    osPriority_t base_priority, current_priority, requested_priority;
    osStatus_t set_status;
    float health_score;
    HealthStatus_t health_status;
    AS_Reason_t reason;
    AS_RecoveryFlag_t recovery_flags;
    uint32_t task_state, good_windows, bad_windows, response_windows;
    uint32_t no_progress_windows, release_count, miss_delta, error_delta;
    uint32_t first_recovery_tick, last_priority_change_tick, precedence_changes;
    uint8_t adaptation_active, recovery_managed;
    uint8_t set_attempted, readback_match, observation_valid, recovery_active;
} AS_TaskSnapshot_t;
/* AS_Init follows RTM_Init and task creation. All mutation APIs have ONE owner:
 * RuntimeMonitorT, with hierarchical recovery called from that same task.
 * AS_Update never changes health scores, fault injection, task lifetime or workloads.
 * Communication CPU overload -> 38 (below Logging 39). No promotion.
 * Restore after three clean windows; timing-only faults request attention. */
/* Monitor-owned cooperative recovery hold: retain adapted priority and flags. */
osStatus_t AS_BeginManagedRecovery(TaskIndex_t idx);
osStatus_t AS_CompleteManagedRecovery(TaskIndex_t idx);
void AS_Init(void);
void AS_Update(void);
void AS_SetSimulatedChargingState(EVChargingState_t state);
EVChargingState_t AS_GetSimulatedChargingState(void);
osPriority_t AS_GetBasePriority(TaskIndex_t idx);
osPriority_t AS_GetCurrentPriority(TaskIndex_t idx);
osPriority_t AS_GetActualPriorityReadback(TaskIndex_t idx);
AS_Reason_t AS_GetLastReason(TaskIndex_t idx);
AS_RecoveryFlag_t AS_GetRecoveryFlag(TaskIndex_t idx);
TaskCriticality_t AS_GetCriticality(TaskIndex_t idx);
int32_t AS_GetUpHeadroom(TaskIndex_t idx);
int32_t AS_GetDownHeadroom(TaskIndex_t idx);
uint32_t AS_GetGoodStreak(TaskIndex_t idx);
uint32_t AS_GetBadStreak(TaskIndex_t idx);
uint32_t AS_GetConfirmWindows(void);
uint32_t AS_GetRestoreWindows(void);
uint32_t AS_GetQueueConfirmWindows(void);
float AS_GetQueueHighUtilThreshold(void);
uint8_t AS_GetTaskSnapshot(TaskIndex_t idx, AS_TaskSnapshot_t *out);
const char *AS_GetReasonName(AS_Reason_t reason);
/* Reserved for future RTOS-object replacement (not used by cooperative v5):
 * pause BEFORE terminating/replacing a task. After replacement,
 * update its main.c handle AND g_taskHealth[idx].handle under HealthMutex, then
 * call AS_RebindTask. Rebinding preserves base priority and pending flags;
 * three fresh, clean Healthy observations clear the flags after verification.
 * Neither API releases application locks or makes deleting a task safe. */
osStatus_t AS_PauseTaskAdaptation(TaskIndex_t idx);
osStatus_t AS_RebindTask(TaskIndex_t idx, osThreadId_t new_handle);
void AS_PrintStatus(UART_HandleTypeDef *huart);
void AS_PrintCompactStatus(UART_HandleTypeDef *huart);
#ifdef __cplusplus
}
#endif
#endif
