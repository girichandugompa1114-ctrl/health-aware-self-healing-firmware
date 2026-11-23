#ifndef HIERARCHICAL_RECOVERY_H
#define HIERARCHICAL_RECOVERY_H
#include "adaptive_scheduler.h"
#ifndef HR_ENABLE_RECOVERY
#define HR_ENABLE_RECOVERY 1U
#endif
#define HR_VERIFY_GOOD_WINDOWS 3U
#define HR_VERIFY_BAD_WINDOWS 3U
#define HR_VERIFY_TIMEOUT_WINDOWS 6U
#define HR_ACK_TIMEOUT_MS 2000U
#define HR_RESET_NOTICE_MS 1000U
#define HR_MAX_REINITIALIZATIONS_PER_BOOT 3U
/* L1 is the existing scheduler. Cooperative L2/L3 execute at task boundaries. */
typedef enum {
    HR_IDLE=0,HR_WAIT_REINITIALIZE,HR_VERIFY_REINITIALIZE,HR_RECOVERED,
    HR_WAIT_DEGRADE,HR_VERIFY_DEGRADED,HR_LIMITED_STABLE,HR_RESET_PENDING,HR_RESET_ISSUED,HR_NONCRITICAL_UNAVAILABLE
} HR_State_t;
typedef struct {
    HR_State_t state;
    uint32_t episode,request_flags,token,generation,good,bad,observations;
    uint32_t task_actions,degrade_actions,successes,limited_successes;
    uint32_t request_tick,action_tick,ack_tick,verified_tick,recovery_time_ms;
    uint32_t last_jobs;
} HR_TaskSnapshot_t;
typedef uint8_t (*HR_TaskAction_t)(TaskIndex_t task);
typedef struct {
    HR_TaskAction_t reinitialize,enter_limited;
} HR_TaskOps_t;
typedef struct {
    void (*enter_safe_state)(void);
    uint8_t (*is_safe)(void);
    void (*system_reset)(void);
    uint8_t software_reset_boot;
} HR_PlatformOps_t;
void HR_Init(const HR_PlatformOps_t *platform);
uint8_t HR_RegisterTask(TaskIndex_t task,const HR_TaskOps_t *ops);
/* Monitor owner only; call once after AS_Update on a fresh scoring window. */
void HR_Update(void);
/* Monitor-owner request after fixing an external cause; full service must pass
 * a new task acknowledgement and three new-generation Healthy windows. */
osStatus_t HR_RequestFullService(TaskIndex_t task);
/* Task owner only, BEFORE RTM_TaskStartAt and outside application locks.
 * Callbacks must be bounded and return; no task deletion/recreation occurs. */
void HR_TaskCheckpoint(TaskIndex_t task);
uint8_t HR_GetTaskSnapshot(TaskIndex_t task,HR_TaskSnapshot_t *out);
const char *HR_GetStateName(HR_State_t state);
uint32_t HR_GetResetRequests(void);
uint32_t HR_GetResetCalls(void);
uint32_t HR_GetEventDrops(void);
uint8_t HR_IsSoftwareResetBoot(void);
void HR_PrintEvents(UART_HandleTypeDef *uart);
void HR_PrintStatus(UART_HandleTypeDef *uart);
/* Call only after UART event/table printing; requires safe-state confirmation,
 * notice delay and a printed reset-request event. Issues at most one reset. */
void HR_DispatchControlledReset(void);
#endif
