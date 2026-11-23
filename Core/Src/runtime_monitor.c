#include "runtime_monitor.h"
#include "FreeRTOS.h"
#include "task.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
extern osThreadId_t SafetyHandle, ChargingTaskHandle, SensorTaskHandle, CommunicationTaHandle, LoggingTaskHandle;
TaskHealth_t g_taskHealth[TASK_COUNT];
SystemDiagQueue_t g_diagQueue;
osMessageQueueId_t DiagQueueHandle;
osMutexId_t HealthMutexHandle;
typedef struct { uint32_t jobs, misses, errors, max_exec, max_resp, max_elapsed; uint64_t response_sum; uint32_t generation,fault_jobs; uint8_t mixed; } Pending_t;
static Pending_t s_pending[TASK_COUNT];
static uint8_t s_job_fault[TASK_COUNT];
static TaskHealth_t s_window[TASK_COUNT];
static SystemDiagQueue_t s_queue_window;
static uint32_t s_sequence;
static uint32_t s_cpu_total[TASK_COUNT], s_cpu_start[TASK_COUNT], s_segment_start;
static int32_t s_running = -1;
static uint8_t s_trace_ready;
static volatile uint8_t s_trace_seen;
static StaticQueue_t s_queue_cb;
static uint8_t s_queue_storage[DIAG_QUEUE_LENGTH*sizeof(DiagMsg_t)];
static StaticSemaphore_t s_mutex_cb;
static uint8_t valid_index(TaskIndex_t i) { return (uint32_t)i < TASK_COUNT; }
static uint32_t max_u32(uint32_t a,uint32_t b) { return a>b?a:b; }
static float unit(float x) { return x<0.0f?0.0f:x>1.0f?1.0f:x; }
static uint32_t cycles_to_us(uint32_t cycles,uint32_t frequency)
{ return frequency?(uint32_t)((uint64_t)cycles*1000000ULL/frequency):0U; }
static int32_t task_index(void *handle)
{
    for(uint32_t i=0;i<TASK_COUNT;i++) if(g_taskHealth[i].handle==handle) return (int32_t)i;
    return -1;
}
void RTM_TraceSwitchIn(void *handle)
{
    if(!s_trace_ready)return;
    s_trace_seen|=1U;
    s_running=task_index(handle);
    s_segment_start=DWT->CYCCNT;
}
void RTM_TraceSwitchOut(void *handle)
{
    (void)handle;
    if(s_trace_ready)s_trace_seen|=2U;
    if(s_trace_ready && s_running>=0) s_cpu_total[s_running]+=DWT->CYCCNT-s_segment_start;
    s_running=-1;
}
uint8_t RTM_TraceIsActive(void){return s_trace_seen==3U;}
static uint32_t cpu_total(TaskIndex_t i)
{
    uint32_t mask=__get_PRIMASK();__disable_irq();
    uint32_t value=s_cpu_total[i];
    if(s_running==(int32_t)i)value+=DWT->CYCCNT-s_segment_start;
    if(!mask)__enable_irq();
    return value;
}
void RTM_Init(void)
{
    static const char *names[]={"Safety","Charging","Sensor","Communication","Logging"};
    const uint32_t periods[]={SAFETY_PERIOD_MS,CHARGING_PERIOD_MS,SENSOR_PERIOD_MS,COMMUNICATION_PERIOD_MS,LOGGING_PERIOD_MS};
    const uint32_t budgets[]={SAFETY_EXEC_BUDGET_US,CHARGING_EXEC_BUDGET_US,SENSOR_EXEC_BUDGET_US,COMMUNICATION_EXEC_BUDGET_US,LOGGING_EXEC_BUDGET_US};
    osThreadId_t handles[]={SafetyHandle,ChargingTaskHandle,SensorTaskHandle,CommunicationTaHandle,LoggingTaskHandle};
    CoreDebug->DEMCR|=CoreDebug_DEMCR_TRCENA_Msk;DWT->CYCCNT=0;DWT->CTRL|=DWT_CTRL_CYCCNTENA_Msk;
    osMessageQueueAttr_t qa={.name="DiagQueue",.cb_mem=&s_queue_cb,.cb_size=sizeof(s_queue_cb),.mq_mem=s_queue_storage,.mq_size=sizeof(s_queue_storage)};
    osMutexAttr_t ma={.name="HealthMutex",.attr_bits=osMutexPrioInherit,.cb_mem=&s_mutex_cb,.cb_size=sizeof(s_mutex_cb)};
    DiagQueueHandle=osMessageQueueNew(DIAG_QUEUE_LENGTH,sizeof(DiagMsg_t),&qa);
    HealthMutexHandle=osMutexNew(&ma);
    if(!DiagQueueHandle || !HealthMutexHandle)Error_Handler();
    memset(g_taskHealth,0,sizeof(g_taskHealth));memset(s_pending,0,sizeof(s_pending));
    memset(s_cpu_total,0,sizeof(s_cpu_total));memset(s_cpu_start,0,sizeof(s_cpu_start));
    memset(&g_diagQueue,0,sizeof(g_diagQueue));g_diagQueue.handle=DiagQueueHandle;g_diagQueue.capacity=DIAG_QUEUE_LENGTH;
    for(uint32_t i=0;i<TASK_COUNT;i++){
        TaskHealth_t *h=&g_taskHealth[i];h->name=names[i];h->handle=handles[i];h->period_ms=periods[i];
        h->deadline_ms=i==TASK_COMMUNICATION?COMMUNICATION_DEADLINE_MS:periods[i];h->exec_budget_us=budgets[i];
        h->health_score=h->previous_health=100.0f;h->last_state=eBlocked;h->generation=1U;
    }
    memcpy(s_window,g_taskHealth,sizeof(s_window));s_queue_window=g_diagQueue;s_sequence=0;s_running=-1;s_trace_seen=0;s_trace_ready=1;
}
void RTM_TaskStart(TaskIndex_t i) { RTM_TaskStartAt(i,xTaskGetTickCount()); }
void RTM_TaskStartAt(TaskIndex_t i,uint32_t release)
{
    if(!valid_index(i))return;
    g_taskHealth[i].release_tick=release;s_job_fault[i]=0U;
    s_cpu_start[i]=cpu_total(i);
}
void RTM_TaskEnd(TaskIndex_t i,uint32_t start,uint32_t end)
{
    if(!valid_index(i))return;
    uint32_t exec=cycles_to_us(cpu_total(i)-s_cpu_start[i],SystemCoreClock);
    uint32_t frequency=osKernelGetSysTimerFreq();
    uint32_t per_tick=frequency/osKernelGetTickFreq();
    uint32_t release_cycles=g_taskHealth[i].release_tick*per_tick;
    uint32_t response=cycles_to_us(osKernelGetSysTimerCount()-release_cycles,frequency);
    uint32_t elapsed=cycles_to_us(end-start,SystemCoreClock);
    if(osMutexAcquire(HealthMutexHandle,osWaitForever)!=osOK)return;
    TaskHealth_t *h=&g_taskHealth[i];Pending_t *p=&s_pending[i];
    if(!p->jobs)p->generation=h->generation;
    else if(p->generation!=h->generation)p->mixed=1U;
    p->fault_jobs+=s_job_fault[i]?1U:0U;
    h->total_releases++;p->jobs++;p->max_exec=max_u32(p->max_exec,exec);
    p->max_resp=max_u32(p->max_resp,response);p->max_elapsed=max_u32(p->max_elapsed,elapsed);p->response_sum+=response;
    if(response>h->deadline_ms*1000U){h->deadline_miss_count++;p->misses++;}
    osMutexRelease(HealthMutexHandle);
    DiagMsg_t msg={(uint8_t)i};
    if(osMessageQueuePut(DiagQueueHandle,&msg,0,0)!=osOK){
        if(osMutexAcquire(HealthMutexHandle,osWaitForever)==osOK){g_diagQueue.drop_count++;osMutexRelease(HealthMutexHandle);}
    }
}
uint32_t RTM_AdvanceTaskGeneration(TaskIndex_t i)
{
    if(!valid_index(i) || osThreadGetId()!=g_taskHealth[i].handle ||
       osMutexAcquire(HealthMutexHandle,osWaitForever)!=osOK)return 0U;
    uint32_t generation=++g_taskHealth[i].generation;
    if(!generation)generation=++g_taskHealth[i].generation;
    osMutexRelease(HealthMutexHandle);return generation;
}
void RTM_TagFaultExposure(TaskIndex_t i)
{
    if(valid_index(i))s_job_fault[i]=1U;
}
void RTM_ReportError(TaskIndex_t i)
{
    if(!valid_index(i))return;
    if(osMutexAcquire(HealthMutexHandle,osWaitForever)==osOK){g_taskHealth[i].error_count++;s_pending[i].errors++;osMutexRelease(HealthMutexHandle);}
}
uint8_t RTM_Update(void)
{
    if(osMutexAcquire(HealthMutexHandle,0)!=osOK)return 0U;
    g_diagQueue.current_count=osMessageQueueGetCount(DiagQueueHandle);
    g_diagQueue.utilization=(float)g_diagQueue.current_count/(float)g_diagQueue.capacity;
    for(uint32_t i=0;i<TASK_COUNT;i++){
        TaskHealth_t *h=&g_taskHealth[i];Pending_t *p=&s_pending[i];
        h->window_generation=p->generation;h->window_mixed_generation=p->mixed;h->window_fault_jobs=p->fault_jobs;
        h->window_releases=p->jobs;h->window_misses=p->misses;h->window_errors=p->errors;h->window_response_sum_us=p->response_sum;
        h->sample_valid=p->jobs>0;h->execution_time_us=p->max_exec;h->response_time_us=p->max_resp;h->elapsed_time_us=p->max_elapsed;
        h->last_state=(uint32_t)eTaskGetState((TaskHandle_t)h->handle);
        h->state_health=(h->last_state==eRunning || h->last_state==eReady || h->last_state==eBlocked)?1.0f:0.0f;
        h->execution_health=p->max_exec?unit((float)h->exec_budget_us/(float)p->max_exec):1.0f;
        h->response_health=p->max_resp?unit((float)(h->deadline_ms*1000U)/(float)p->max_resp):1.0f;
        h->deadline_health=p->jobs?unit(1.0f-(float)p->misses/(float)p->jobs):1.0f;
        h->error_health=unit(1.0f-(float)p->errors/(float)RTM_MAX_ERRORS_FOR_NORM);h->queue_health=1.0f-g_diagQueue.utilization;
        h->previous_health=h->health_score;
        if(h->sample_valid || p->errors || h->state_health<1.0f){
            h->health_score=100.0f*(W_EXEC*h->execution_health+W_DEAD*h->deadline_health+W_RESP*h->response_health+W_ERROR*h->error_health+W_STATE*h->state_health);
            uint8_t raw=h->health_score>=HEALTH_THRESHOLD_HEALTHY?HEALTH_HEALTHY:
                h->health_score>=HEALTH_THRESHOLD_WARNING?HEALTH_WARNING:
                h->health_score>=HEALTH_THRESHOLD_DEGRADED?HEALTH_DEGRADED:HEALTH_CRITICAL;
            h->consecutive_bad=raw>=HEALTH_DEGRADED?h->consecutive_bad+1U:0U;
            h->health_status=raw>=HEALTH_DEGRADED && h->consecutive_bad<RTM_PERSISTENCE_WINDOWS?HEALTH_WARNING:raw;
        }
        h->health_delta=h->health_score-h->previous_health;memset(p,0,sizeof(*p));
    }
    memcpy(s_window,g_taskHealth,sizeof(s_window));s_queue_window=g_diagQueue;s_sequence++;
    osMutexRelease(HealthMutexHandle);
    return 1U;
}
uint8_t RTM_CopyWindow(TaskHealth_t out[TASK_COUNT],SystemDiagQueue_t *queue,uint32_t *sequence)
{
    if(!out || !s_sequence || osMutexAcquire(HealthMutexHandle,0)!=osOK)return 0;
    memcpy(out,s_window,sizeof(s_window));if(queue)*queue=s_queue_window;if(sequence)*sequence=s_sequence;
    osMutexRelease(HealthMutexHandle);return 1;
}
static void send(UART_HandleTypeDef *u,char *b,size_t cap,int n)
{ if(u && n>0)HAL_UART_Transmit(u,(uint8_t*)b,(uint16_t)((size_t)n<cap?(size_t)n:cap-1U),HAL_MAX_DELAY); }
const char *RTM_GetStateName(uint32_t state)
{
    switch(state){
        case eRunning:return "RUNNING";
        case eReady:return "READY";
        case eBlocked:return "BLOCKED";
        case eSuspended:return "SUSPEND";
        case eDeleted:return "DELETED";
        default:return "INVALID";
    }
}
const char *RTM_GetHealthName(uint8_t status)
{
    switch(status){
        case HEALTH_HEALTHY:return "Healthy";
        case HEALTH_WARNING:return "Warning";
        case HEALTH_DEGRADED:return "Degraded";
        default:return "Critical";
    }
}
void RTM_PrintFullTable(UART_HandleTypeDef *u)
{
    char b[192];int n;
    n=snprintf(b,sizeof(b),"\r\n==============================================================================\r\n                         TASK RUNTIME HEALTH MONITOR\r\n");send(u,b,sizeof(b),n);
    n=snprintf(b,sizeof(b),"==============================================================================\r\nTask          Exec(us) Resp(us) Miss Errors Jobs State    Health Status\r\n");send(u,b,sizeof(b),n);
    n=snprintf(b,sizeof(b),"------------------------------------------------------------------------------\r\n");send(u,b,sizeof(b),n);
    for(uint32_t i=0;i<TASK_COUNT;i++){
        const TaskHealth_t *h=&s_window[i];
        n=snprintf(b,sizeof(b),"%-13s %8lu %8lu %4lu %6lu %4lu %-8s %s\r\n",h->name,
            (unsigned long)h->execution_time_us,(unsigned long)h->response_time_us,(unsigned long)h->window_misses,
            (unsigned long)h->window_errors,(unsigned long)h->window_releases,RTM_GetStateName(h->last_state),RTM_GetHealthName(h->health_status));
        send(u,b,sizeof(b),n);
    }
    n=snprintf(b,sizeof(b),"------------------------------------------------------------------------------\r\nExec = CPU service; Resp = planned-release response; both are window maxima.\r\n");send(u,b,sizeof(b),n);
    n=snprintf(b,sizeof(b),"Miss/Errors/Jobs = current window. BLOCKED during a periodic delay is normal.\r\nDiagnostic queue: %lu/%lu, cumulative drops=%lu\r\n",
        (unsigned long)s_queue_window.current_count,(unsigned long)s_queue_window.capacity,(unsigned long)s_queue_window.drop_count);send(u,b,sizeof(b),n);
    n=snprintf(b,sizeof(b),"==============================================================================\r\n");send(u,b,sizeof(b),n);
}
void RTM_PrintCompactSummary(UART_HandleTypeDef *u,uint32_t cycle)
{
    char b[128];int n=snprintf(b,sizeof(b),"[QUEUE] cycle=%lu used=%lu/%lu lifetime_drops=%lu\r\n",(unsigned long)cycle,
        (unsigned long)s_queue_window.current_count,(unsigned long)s_queue_window.capacity,(unsigned long)s_queue_window.drop_count);send(u,b,sizeof(b),n);
}
