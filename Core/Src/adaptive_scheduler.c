#include "adaptive_scheduler.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

extern osThreadId_t SafetyHandle, ChargingTaskHandle, SensorTaskHandle;
extern osThreadId_t CommunicationTaHandle, LoggingTaskHandle, RuntimeMonitorTHandle;
typedef struct {
    AS_TaskSnapshot_t view;
    uint32_t last_releases, last_misses, last_errors;
    uint8_t response_started, handle_readable;
} AS_TaskContext_t;
static AS_TaskContext_t s_ctx[TASK_COUNT];
static TaskHealth_t s_window[TASK_COUNT];
static SystemDiagQueue_t s_queue;
static EVChargingState_t s_charging_state;

static uint8_t s_initialized;
static uint32_t s_last_sequence;
static AS_Reason_t s_printed_reason[TASK_COUNT];
static uint32_t s_printed_flags[TASK_COUNT];
static osPriority_t s_printed_priority[TASK_COUNT];
static uint8_t s_printed_valid[TASK_COUNT];

static uint8_t valid_index(TaskIndex_t idx) { return (uint32_t)idx < (uint32_t)TASK_COUNT; }
static uint32_t increment(uint32_t n) { return n < UINT32_MAX ? n + 1U : n; }
static osThreadId_t owner_handle(TaskIndex_t idx)
{
    switch (idx) {
        case TASK_SAFETY: return SafetyHandle;
        case TASK_CHARGING: return ChargingTaskHandle;
        case TASK_SENSOR: return SensorTaskHandle;
        case TASK_COMMUNICATION: return CommunicationTaHandle;
        case TASK_LOGGING: return LoggingTaskHandle;
        default: return NULL;
    }
}
static uint8_t valid_priority(osPriority_t priority)
{
    return (int32_t)priority >= (int32_t)osPriorityIdle &&
           (int32_t)priority < (int32_t)configMAX_PRIORITIES &&
           (int32_t)priority <= (int32_t)osPriorityISR;
}
static void request_recovery(AS_TaskSnapshot_t *v, AS_RecoveryFlag_t flag)
{
    if (v->recovery_flags == AS_FLAG_NONE) v->first_recovery_tick = xTaskGetTickCount();
    v->recovery_flags = (AS_RecoveryFlag_t)((uint32_t)v->recovery_flags | (uint32_t)flag);
}
static uint8_t stopped_state(uint32_t state)
{
    return state == (uint32_t)eSuspended || state == (uint32_t)eDeleted || state == (uint32_t)eInvalid;
}
static osPriority_t upper_limit(TaskIndex_t i)
{
    return s_ctx[i].view.base_priority;
}
static osPriority_t lower_limit(TaskIndex_t i)
{
    return i==TASK_COMMUNICATION?(osPriority_t)AS_COMM_DEMOTED_PRIORITY:s_ctx[i].view.base_priority;
}
static uint32_t precedence_changes(TaskIndex_t idx,osPriority_t before,osPriority_t after)
{
    uint32_t changes=0;
    for(uint32_t i=0;i<TASK_COUNT;i++)if(i!=(uint32_t)idx && s_ctx[i].handle_readable && !s_ctx[i].view.recovery_active){
        osPriority_t other=osThreadGetPriority(s_ctx[i].view.handle);
        if(valid_priority(other) && ((before>other && after<other)||(before<other && after>other)))changes++;
    }
    return changes;
}
static osPriority_t choose_target(TaskIndex_t i,const TaskHealth_t *h,AS_Reason_t *reason)
{
    if(i==TASK_COMMUNICATION && h->execution_time_us>h->exec_budget_us){
        *reason=AS_REASON_CPU_DEMOTION;
        return (osPriority_t)AS_COMM_DEMOTED_PRIORITY;
    }
    *reason=AS_REASON_PROTECTED_HOLD;
    return s_ctx[i].view.base_priority;
}

/* One final target and at most ONE set call per task per AS_Update. Safety is excluded. */
static void apply_target(TaskIndex_t idx, osPriority_t target)
{
    AS_TaskContext_t *c = &s_ctx[idx];
    AS_TaskSnapshot_t *v = &c->view;
    v->requested_priority = target;
    v->set_attempted = 0U;
    v->set_status = osOK;
    v->readback_match = 0U;
    if (idx == TASK_SAFETY || !c->handle_readable || v->recovery_active) return;
    if (!valid_priority(target) || target < lower_limit(idx) || target > upper_limit(idx) ||
        target >= s_ctx[TASK_SAFETY].view.base_priority ||
        (s_charging_state == EV_STATE_CHARGING && idx != TASK_CHARGING && target >= s_ctx[TASK_CHARGING].view.base_priority)) {
        v->reason = AS_REASON_PRIORITY_ERROR;
        request_recovery(v, AS_FLAG_NEEDS_RECOVERY_PRIORITY);
        return;
    }
    osPriority_t before = osThreadGetPriority(v->handle);
    if (!valid_priority(before)) {
        v->reason = AS_REASON_PRIORITY_ERROR;
        request_recovery(v, AS_FLAG_NEEDS_RECOVERY_PRIORITY);
        return;
    }
    uint32_t changed_order = precedence_changes(idx, before, target);
    if (before != target && target != v->base_priority && changed_order == 0U) {
        v->reason = AS_REASON_NO_PRECEDENCE_CHANGE;
        return;
    }
    if (before != target) {
        v->set_attempted = 1U;
        v->set_status = osThreadSetPriority(v->handle, target);
    }
    osPriority_t after = osThreadGetPriority(v->handle);
    v->current_priority = after;
    v->readback_match = valid_priority(after) && after == target && v->set_status == osOK;
    if (valid_priority(after) && after != before) {
        v->last_priority_change_tick = xTaskGetTickCount();
        v->precedence_changes += precedence_changes(idx, before, after);
    }
    if (!v->readback_match) {
        v->reason = AS_REASON_PRIORITY_ERROR;
        request_recovery(v, AS_FLAG_NEEDS_RECOVERY_PRIORITY);
    }
}
void AS_Init(void)
{
    memset(s_ctx, 0, sizeof(s_ctx));
    memset(s_printed_valid, 0, sizeof(s_printed_valid));
    s_last_sequence = 0U;
    s_charging_state = EV_STATE_CHARGING;
    for (uint32_t i = 0; i < TASK_COUNT; ++i) {
        AS_TaskContext_t *c = &s_ctx[i];
        c->view.handle = owner_handle((TaskIndex_t)i);
        osPriority_t p = c->view.handle ? osThreadGetPriority(c->view.handle) : osPriorityError;
        c->view.base_priority = p;
        c->view.current_priority = p;
        c->view.requested_priority = p;
        c->view.health_score = 100.0f;
        c->last_releases = g_taskHealth[i].total_releases;
        c->last_misses = g_taskHealth[i].deadline_miss_count;
        c->last_errors = g_taskHealth[i].error_count;
        c->handle_readable = c->view.handle != NULL && valid_priority(p);
        c->view.readback_match = c->handle_readable;
        if (!c->handle_readable) request_recovery(&c->view, AS_FLAG_NEEDS_RECOVERY_HANDLE);
    }
    s_initialized = 1U;
}
void AS_Update(void)
{
    if (!s_initialized) return;
    for (uint32_t i = 0; i < TASK_COUNT; ++i) s_ctx[i].view.observation_valid = 0U;
    uint32_t sequence=0U;
    if(!RTM_CopyWindow(s_window,&s_queue,&sequence) || sequence==s_last_sequence)return;
    if(!RTM_TraceIsActive())return;
    s_last_sequence=sequence;
    /* Sample all handles before making priority decisions. State is observed independently of RTM_TaskEnd. */
    for (uint32_t i = 0; i < TASK_COUNT; ++i) {
        AS_TaskContext_t *c = &s_ctx[i];
        AS_TaskSnapshot_t *v = &c->view;
        v->set_attempted = 0U;
        v->set_status = osOK;
        if (v->recovery_active) { v->reason = AS_REASON_RECOVERY_ACTIVE; c->handle_readable = 0U; continue; }
        c->handle_readable = v->handle != NULL && v->handle == owner_handle((TaskIndex_t)i) &&
                             v->handle == s_window[i].handle;
        if (!c->handle_readable) {
            v->reason = AS_REASON_INVALID_HANDLE;
            request_recovery(v, AS_FLAG_NEEDS_RECOVERY_HANDLE);
            continue;
        }
        v->task_state = (uint32_t)eTaskGetState((TaskHandle_t)v->handle);
        if (v->task_state == (uint32_t)eDeleted || v->task_state == (uint32_t)eInvalid) {
            c->handle_readable = 0U;
            v->reason = AS_REASON_BLOCKED_STATE;
            request_recovery(v, AS_FLAG_NEEDS_RECOVERY_STATE);
            continue;
        }
        v->current_priority = osThreadGetPriority(v->handle);
        v->health_score = s_window[i].health_score;
        v->health_status = (HealthStatus_t)s_window[i].health_status;
        v->observation_valid = 1U;
        v->release_count = s_window[i].total_releases;
        uint8_t progressed = v->release_count != c->last_releases;
        v->no_progress_windows = progressed ? 0U : increment(v->no_progress_windows);
        v->miss_delta = s_window[i].deadline_miss_count - c->last_misses;
        v->error_delta = s_window[i].error_count - c->last_errors;
        c->last_releases = v->release_count;
        c->last_misses = s_window[i].deadline_miss_count;
        c->last_errors = s_window[i].error_count;
        if (!valid_priority(v->current_priority)) request_recovery(v, AS_FLAG_NEEDS_RECOVERY_PRIORITY);
        if (stopped_state(v->task_state)) request_recovery(v, AS_FLAG_NEEDS_RECOVERY_STATE);
        if (v->error_delta) request_recovery(v, AS_FLAG_NEEDS_RECOVERY_ERROR);
        if (v->no_progress_windows >= AS_NO_PROGRESS_WINDOWS) request_recovery(v, AS_FLAG_NEEDS_RECOVERY_PROGRESS);
        uint8_t bad = v->health_status >= HEALTH_WARNING || v->error_delta || v->miss_delta ||
                      stopped_state(v->task_state) || v->no_progress_windows >= AS_NO_PROGRESS_WINDOWS;
        uint8_t good = !bad && progressed && s_window[i].sample_valid && valid_priority(v->current_priority);
        v->bad_windows = bad ? increment(v->bad_windows) : 0U;
        v->good_windows = good ? increment(v->good_windows) : 0U;
    }
    for (uint32_t ii=0;ii<TASK_COUNT;++ii) {
        TaskIndex_t idx=(TaskIndex_t)ii;
        AS_TaskContext_t *c=&s_ctx[ii];AS_TaskSnapshot_t *v=&c->view;
        if(!v->observation_valid || v->recovery_active)continue;
        if(v->recovery_managed){
            if(c->response_started)v->response_windows=v->bad_windows?increment(v->response_windows):0U;
            v->reason=AS_REASON_RECOVERY_ACTIVE;continue;
        }
        uint8_t had_request=v->recovery_flags!=AS_FLAG_NONE;
        if(v->good_windows>=AS_RESTORE_WINDOWS){v->recovery_flags=AS_FLAG_NONE;v->first_recovery_tick=0U;}
        if(idx==TASK_SAFETY){
            if(v->bad_windows>=AS_CONFIRM_WINDOWS)request_recovery(v,AS_FLAG_NEEDS_RECOVERY_PERSISTENT);
            continue;
        }
        osPriority_t target=v->current_priority;
        uint8_t bad=v->bad_windows>0U;
        uint32_t hard=(uint32_t)v->recovery_flags & ~(uint32_t)AS_FLAG_NEEDS_RECOVERY_PERSISTENT;
        if(!AS_ENABLE_ADAPTATION){target=v->base_priority;v->reason=AS_REASON_BASELINE;}
        else if(s_charging_state==EV_STATE_FAULT){
            target=v->base_priority;v->reason=AS_REASON_CHARGING_FAULT_HOLD;
            c->response_started=0U;v->response_windows=0U;
            if(v->bad_windows>=AS_CONFIRM_WINDOWS)request_recovery(v,AS_FLAG_NEEDS_RECOVERY_PERSISTENT);
        }else if(hard){
            target=v->base_priority;c->response_started=0U;v->response_windows=0U;
            v->reason=(hard&AS_FLAG_NEEDS_RECOVERY_STATE)?AS_REASON_BLOCKED_STATE:
                (hard&AS_FLAG_NEEDS_RECOVERY_PROGRESS)?AS_REASON_NO_PROGRESS:
                (hard&AS_FLAG_NEEDS_RECOVERY_ERROR)?AS_REASON_ERROR_FAULT:AS_REASON_PRIORITY_ERROR;
        }else if(v->good_windows>=AS_RESTORE_WINDOWS){
            target=v->base_priority;
            v->reason=c->response_started || had_request || target!=v->current_priority?AS_REASON_RECOVERED:AS_REASON_NONE;
            v->response_windows=0U;
        }else if(c->response_started){
            if(bad){
                v->response_windows=increment(v->response_windows);
                v->reason=AS_REASON_CPU_DEMOTION;
                if(v->response_windows>=AS_RECOVERY_WAIT_WINDOWS)request_recovery(v,AS_FLAG_NEEDS_RECOVERY_PERSISTENT);
            }else{v->response_windows=0U;v->reason=AS_REASON_VERIFYING_HEALTHY;}
        }else if(v->bad_windows>=AS_CONFIRM_WINDOWS){
            target=choose_target(idx,&s_window[ii],&v->reason);
            if(target==v->base_priority)request_recovery(v,AS_FLAG_NEEDS_RECOVERY_PERSISTENT);
        }else v->reason=bad?AS_REASON_CONFIRMING:AS_REASON_NONE;
        if(s_charging_state==EV_STATE_CHARGING && idx==TASK_CHARGING && target!=v->base_priority){
            target=v->base_priority;v->reason=AS_REASON_PROTECTED_HOLD;
        }
        osPriority_t previous=v->current_priority;
        apply_target(idx,target);
        if(v->reason==AS_REASON_NO_PRECEDENCE_CHANGE)request_recovery(v,AS_FLAG_NEEDS_RECOVERY_PERSISTENT);
        if(v->set_attempted && v->readback_match && v->current_priority!=previous){
            if(v->current_priority!=v->base_priority){c->response_started=1U;v->response_windows=0U;}
            else c->response_started=0U;
        }
        if(v->current_priority==v->base_priority && v->good_windows>=AS_RESTORE_WINDOWS)c->response_started=0U;
        v->adaptation_active=c->response_started;
    }

}
osStatus_t AS_BeginManagedRecovery(TaskIndex_t i)
{
    if(!valid_index(i) || (i!=TASK_COMMUNICATION && i!=TASK_LOGGING) || !s_initialized || !s_ctx[i].handle_readable ||
       s_ctx[i].view.recovery_active || s_ctx[i].view.recovery_managed)return osErrorParameter;
    AS_TaskSnapshot_t *v=&s_ctx[i].view;v->recovery_managed=1U;
    v->good_windows=0U;
    v->reason=AS_REASON_RECOVERY_ACTIVE;return osOK;
}
osStatus_t AS_CompleteManagedRecovery(TaskIndex_t i)
{
    if(!valid_index(i) || !s_ctx[i].view.recovery_managed || !s_ctx[i].view.observation_valid ||
       s_ctx[i].view.good_windows<AS_RESTORE_WINDOWS)return osErrorParameter;
    AS_TaskContext_t *c=&s_ctx[i];AS_TaskSnapshot_t *v=&c->view;
    apply_target(i,v->base_priority);
    if(!v->readback_match)return osError;
    v->recovery_flags=AS_FLAG_NONE;v->first_recovery_tick=0U;v->response_windows=0U;
    v->adaptation_active=v->recovery_managed=0U;c->response_started=0U;
    v->reason=AS_REASON_RECOVERED;return osOK;
}
void AS_SetSimulatedChargingState(EVChargingState_t state)
{
    if (state == EV_STATE_IDLE || state == EV_STATE_CHARGING || state == EV_STATE_FAULT) s_charging_state = state;
}
EVChargingState_t AS_GetSimulatedChargingState(void) { return s_charging_state; }
osPriority_t AS_GetBasePriority(TaskIndex_t i) { return valid_index(i) ? s_ctx[i].view.base_priority : osPriorityError; }
osPriority_t AS_GetCurrentPriority(TaskIndex_t i) { return valid_index(i) ? s_ctx[i].view.current_priority : osPriorityError; }
osPriority_t AS_GetActualPriorityReadback(TaskIndex_t i)
{
    if (!valid_index(i) || !s_ctx[i].handle_readable || s_ctx[i].view.recovery_active) return osPriorityError;
    return osThreadGetPriority(s_ctx[i].view.handle);
}
AS_Reason_t AS_GetLastReason(TaskIndex_t i) { return valid_index(i) ? s_ctx[i].view.reason : AS_REASON_INVALID_HANDLE; }
AS_RecoveryFlag_t AS_GetRecoveryFlag(TaskIndex_t i) { return valid_index(i) ? s_ctx[i].view.recovery_flags : AS_FLAG_NEEDS_RECOVERY_HANDLE; }
TaskCriticality_t AS_GetCriticality(TaskIndex_t i) { return valid_index(i) ? (TaskCriticality_t)i : CRIT_LOW; }
uint32_t AS_GetGoodStreak(TaskIndex_t i) { return valid_index(i) ? s_ctx[i].view.good_windows : 0U; }
uint32_t AS_GetBadStreak(TaskIndex_t i) { return valid_index(i) ? s_ctx[i].view.bad_windows : 0U; }
uint32_t AS_GetConfirmWindows(void) { return AS_CONFIRM_WINDOWS; }
uint32_t AS_GetRestoreWindows(void) { return AS_RESTORE_WINDOWS; }
uint32_t AS_GetQueueConfirmWindows(void) { return AS_QUEUE_CONFIRM_WINDOWS; }
float AS_GetQueueHighUtilThreshold(void) { return AS_QUEUE_HIGH_UTIL; }
uint8_t AS_GetTaskSnapshot(TaskIndex_t i, AS_TaskSnapshot_t *out)
{
    if (!valid_index(i) || !out || !s_initialized) return 0U;
    *out = s_ctx[i].view;
    return out->observation_valid;
}
int32_t AS_GetUpHeadroom(TaskIndex_t i)
{
    if(!valid_index(i) || i==TASK_SAFETY || !s_ctx[i].handle_readable)return 0;
    int32_t gap=(int32_t)upper_limit(i)-(int32_t)AS_GetActualPriorityReadback(i);
    return gap>0?gap:0;
}
int32_t AS_GetDownHeadroom(TaskIndex_t i)
{
    if(!valid_index(i) || i==TASK_SAFETY || !s_ctx[i].handle_readable)return 0;
    int32_t gap=(int32_t)AS_GetActualPriorityReadback(i)-(int32_t)lower_limit(i);
    return gap>0?gap:0;
}
osStatus_t AS_PauseTaskAdaptation(TaskIndex_t i)
{
    if (!valid_index(i) || !s_initialized || !s_ctx[i].handle_readable || s_ctx[i].view.recovery_managed) return osErrorParameter;
    if (i != TASK_SAFETY) {
        apply_target(i, s_ctx[i].view.base_priority);
        if (!s_ctx[i].view.readback_match) return osError;
    }
    s_ctx[i].view.recovery_active = 1U;
    s_ctx[i].view.observation_valid = 0U;
    s_ctx[i].view.reason = AS_REASON_RECOVERY_ACTIVE;
    s_ctx[i].handle_readable = 0U;
    return osOK;
}
osStatus_t AS_RebindTask(TaskIndex_t i, osThreadId_t new_handle)
{
    if (!valid_index(i) || !s_initialized || !s_ctx[i].view.recovery_active ||
        !new_handle || new_handle != owner_handle(i)) return osErrorParameter;
    if (!HealthMutexHandle || osMutexAcquire(HealthMutexHandle, 0U) != osOK) return osErrorResource;
    TaskHealth_t h = g_taskHealth[i];
    osMutexRelease(HealthMutexHandle);
    if (h.handle != new_handle || osThreadGetPriority(new_handle) != s_ctx[i].view.base_priority) return osErrorParameter;
    AS_TaskContext_t *c = &s_ctx[i];
    c->view.handle = new_handle;
    c->view.current_priority = c->view.base_priority;
    c->view.good_windows = c->view.bad_windows = c->view.no_progress_windows = c->view.response_windows = 0U;
    c->view.recovery_active = 0U;
    c->view.observation_valid = 0U;
    c->last_releases = h.total_releases;
    c->last_misses = h.deadline_miss_count;
    c->last_errors = h.error_count;
    c->response_started = 0U;
    c->handle_readable = 1U;
    return osOK;
}
const char *AS_GetReasonName(AS_Reason_t r)
{
    switch (r) {
        case AS_REASON_NONE: return "Stable";
        case AS_REASON_DEADLINE_URGENCY: return "TimingPressureUp";
        case AS_REASON_EXEC_OVERLOAD: return "ExecOverload";
        case AS_REASON_RECOVERED: return "BaseRestored";
        case AS_REASON_BLOCKED_STATE: return "StoppedState";
        case AS_REASON_ERROR_FAULT: return "ErrorFault";
        case AS_REASON_QUEUE_STARVATION: return "QueuePressure";
        case AS_REASON_NO_HEADROOM: return "NoHeadroom";
        case AS_REASON_CHARGING_FAULT_HOLD: return "ChargingFaultHold";
        case AS_REASON_CONFIRMING: return "Confirming";
        case AS_REASON_NO_PROGRESS: return "NoProgress";
        case AS_REASON_LOW_CRITICALITY_HOLD: return "LowCriticalityHold";
        case AS_REASON_IDLE_HOLD: return "IdleHold";
        case AS_REASON_PRIORITY_ERROR: return "PriorityError";
        case AS_REASON_INVALID_HANDLE: return "InvalidHandle";
        case AS_REASON_RECOVERY_ACTIVE: return "RecoveryActive";
        case AS_REASON_BASELINE: return "FixedBaseline";
        case AS_REASON_CPU_DEMOTION: return "CpuOverloadDown";
        case AS_REASON_PROTECTED_HOLD: return "CriticalityHold";
        case AS_REASON_NO_PRECEDENCE_CHANGE: return "NoOrderChange";
        case AS_REASON_VERIFYING_HEALTHY: return "VerifyHealthy";
        default: return "Unknown";
    }
}
static const char *state_text(void)
{
    return s_charging_state == EV_STATE_IDLE ? "IDLE" : s_charging_state == EV_STATE_FAULT ? "FAULT" : "CHARGING";
}
static void send_line(UART_HandleTypeDef *uart, const char *text, size_t size, int length)
{
    if (!uart || length <= 0) return;
    size_t count = (size_t)length < size ? (size_t)length : size - 1U;
    HAL_UART_Transmit(uart, (uint8_t *)text, (uint16_t)count, HAL_MAX_DELAY);
}
void AS_PrintStatus(UART_HandleTypeDef *uart)
{
    char line[224];int n;
    n=snprintf(line,sizeof(line),"\r\n================================ ADAPTIVE SCHEDULER ================================\r\n");send_line(uart,line,sizeof(line),n);
    n=snprintf(line,sizeof(line),"Mode=%s | Charging state [SIMULATED]: %s | Recovery controller owns requests\r\n",
        AS_ENABLE_ADAPTATION?"ADAPTIVE":"FIXED",state_text());send_line(uart,line,sizeof(line),n);
    n=snprintf(line,sizeof(line),"Task          Base Now Read Good Bad AfterBad Flags Reason\r\n-----------------------------------------------------------------------------------\r\n");send_line(uart,line,sizeof(line),n);
    for(uint32_t i=0;i<TASK_COUNT;i++){
        AS_TaskSnapshot_t *v=&s_ctx[i].view;
        n=snprintf(line,sizeof(line),"%-13s %4d %3d %4d %4lu %3lu %8lu 0x%02lX %s\r\n",g_taskHealth[i].name,
            (int)v->base_priority,(int)v->current_priority,(int)AS_GetActualPriorityReadback((TaskIndex_t)i),
            (unsigned long)v->good_windows,(unsigned long)v->bad_windows,(unsigned long)v->response_windows,
            (unsigned long)v->recovery_flags,i==TASK_SAFETY?"Protected":AS_GetReasonName(v->reason));send_line(uart,line,sizeof(line),n);
        if(v->set_attempted || !v->readback_match){
            n=snprintf(line,sizeof(line),"  Priority verify: set=%s req=%d api=%d match=%s\r\n",v->set_attempted?"YES":"NO",
                (int)v->requested_priority,(int)v->set_status,v->readback_match?"YES":"NO");send_line(uart,line,sizeof(line),n);
        }
    }
    n=snprintf(line,sizeof(line),"Bad: consecutive bad samples. AfterBad: bad samples AFTER a verified change.\r\nGood: consecutive clean Healthy samples. 0x04: persistent-fault request pending.\r\n");send_line(uart,line,sizeof(line),n);
    n=snprintf(line,sizeof(line),"===================================================================================\r\n");send_line(uart,line,sizeof(line),n);
}
void AS_PrintCompactStatus(UART_HandleTypeDef *uart)
{
    char line[144];
    for (uint32_t i = 0; i < TASK_COUNT; ++i) {
        AS_TaskSnapshot_t *v = &s_ctx[i].view;
        if (!s_printed_valid[i] || s_printed_reason[i] != v->reason || s_printed_flags[i] != (uint32_t)v->recovery_flags ||
            s_printed_priority[i] != v->current_priority) {
            int n = snprintf(line, sizeof(line), "[SCHED] %s base=%d cur=%d reason=%s flags=0x%02lX fresh=%u\r\n",
                g_taskHealth[i].name, (int)v->base_priority, (int)v->current_priority,
                AS_GetReasonName(v->reason), (unsigned long)v->recovery_flags, (unsigned)v->observation_valid);
            send_line(uart, line, sizeof(line), n);
            s_printed_reason[i] = v->reason;
            s_printed_flags[i] = (uint32_t)v->recovery_flags;
            s_printed_priority[i] = v->current_priority;
            s_printed_valid[i] = 1U;
        }
    }
}
