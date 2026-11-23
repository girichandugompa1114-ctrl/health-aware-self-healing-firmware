#include "experiment_report.h"
#include "hierarchical_recovery.h"
#include "task_service.h"
#include <stdio.h>
#include <string.h>
typedef struct { uint64_t response_sum; uint32_t jobs,misses,worst_response; } CaseStats_t;
typedef struct { uint32_t cycle; int32_t from,to; AS_Reason_t reason; uint8_t task; } Change_t;
typedef struct {
    uint32_t cycle,cpu,score,good,bad,after,flags;
    int32_t sampled,now;
    uint8_t fault,status;
} CycleRow_t;
static CaseStats_t s_stats[2][TASK_COUNT];
static Change_t s_changes[16];
static CycleRow_t s_history[EXPERIMENT_STOP_CYCLE];
static uint32_t s_change_count,s_overflow,s_history_count,s_last_sequence;
static uint32_t s_down,s_restored,s_requests,s_request_clears;
static osPriority_t s_last_priority[TASK_COUNT];
static AS_RecoveryFlag_t s_last_flags[TASK_COUNT];
static uint8_t s_seen[TASK_COUNT],s_protected_ok,s_baseline_seen,s_baseline_ok;
static uint8_t s_continuity_ok;
static uint8_t s_demotion_verified,s_restore_verified,s_pending_verified,s_early_pending;
static TaskHealth_t s_window[TASK_COUNT],s_baseline[TASK_COUNT];
static SystemDiagQueue_t s_queue;
static void send(UART_HandleTypeDef *u,const char *b,size_t cap,int n)
{if(u && n>0)HAL_UART_Transmit(u,(uint8_t*)b,(uint16_t)((size_t)n<cap?(size_t)n:cap-1U),HAL_MAX_DELAY);}
void ER_Init(void)
{
    memset(s_stats,0,sizeof(s_stats));memset(s_seen,0,sizeof(s_seen));
    memset(s_last_flags,0,sizeof(s_last_flags));memset(s_baseline,0,sizeof(s_baseline));
    s_change_count=s_overflow=s_history_count=s_last_sequence=0;
    s_down=s_restored=s_requests=s_request_clears=0;
    s_continuity_ok=1;
    s_protected_ok=s_baseline_ok=1;s_baseline_seen=0;
    s_demotion_verified=s_restore_verified=s_pending_verified=s_early_pending=0;
}
static void print_order(UART_HandleTypeDef *u)
{
    uint32_t ids[TASK_COUNT]={0,1,2,3,4};
    for(uint32_t i=0;i<TASK_COUNT;i++)for(uint32_t j=i+1;j<TASK_COUNT;j++)
        if(AS_GetActualPriorityReadback((TaskIndex_t)ids[j])>AS_GetActualPriorityReadback((TaskIndex_t)ids[i])){
            uint32_t q=ids[i];ids[i]=ids[j];ids[j]=q;
        }
    char b[192];int n=snprintf(b,sizeof(b),"[TASK ORDER] %s(%d) > %s(%d) > %s(%d) > %s(%d) > %s(%d)\r\n",
        s_window[ids[0]].name,(int)AS_GetActualPriorityReadback((TaskIndex_t)ids[0]),
        s_window[ids[1]].name,(int)AS_GetActualPriorityReadback((TaskIndex_t)ids[1]),
        s_window[ids[2]].name,(int)AS_GetActualPriorityReadback((TaskIndex_t)ids[2]),
        s_window[ids[3]].name,(int)AS_GetActualPriorityReadback((TaskIndex_t)ids[3]),
        s_window[ids[4]].name,(int)AS_GetActualPriorityReadback((TaskIndex_t)ids[4]));send(u,b,sizeof(b),n);
}
void ER_Update(UART_HandleTypeDef *u,uint32_t cycle)
{
    uint32_t sequence;
    if(!RTM_CopyWindow(s_window,&s_queue,&sequence) || sequence==s_last_sequence)return;
    s_last_sequence=sequence;
    char b[224];int n;
    if(cycle==1U){
        n=snprintf(b,sizeof(b),"\r\nHEALTH-AWARE EV CHARGING FIRMWARE | MR.G V5 | %s\r\nLatched-fault demonstration: %lu observations; cooperative hierarchical recovery.\r\n",
            HR_ENABLE_RECOVERY?"ADAPTIVE + RECOVERY":AS_ENABLE_ADAPTATION?"ADAPTIVE ONLY":"FIXED BASELINE",(unsigned long)EXPERIMENT_STOP_CYCLE);send(u,b,sizeof(b),n);
        n=snprintf(b,sizeof(b),"Communication period/deadline=%lu/%lu ms; CPU budget=%lu us/job.\r\nConfirm=%lu bad; pending=%lu MORE bad after change; restore=%lu clean Healthy.\r\n",
            (unsigned long)COMMUNICATION_PERIOD_MS,(unsigned long)COMMUNICATION_DEADLINE_MS,(unsigned long)COMMUNICATION_EXEC_BUDGET_US,
            (unsigned long)AS_CONFIRM_WINDOWS,(unsigned long)AS_RECOVERY_WAIT_WINDOWS,(unsigned long)AS_RESTORE_WINDOWS);send(u,b,sizeof(b),n);
        print_order(u);
    }
    n=snprintf(b,sizeof(b),"\r\n========================= CYCLE %02lu / %02lu =========================\r\nCaptured-window fault: %s | Fault-effect snapshot: %s\r\n",
        (unsigned long)cycle,(unsigned long)EXPERIMENT_STOP_CYCLE,g_faultInjection.observed_active?"ACTIVE":"OFF",
        g_faultInjection.active?"ACTIVE":g_faultInjection.state==FAULT_STATE_CLEARED?"CLEARED":"OFF");send(u,b,sizeof(b),n);
    if(!RTM_TraceIsActive()){
        n=snprintf(b,sizeof(b),"[CONFIG ERROR] CPU trace hooks missing. Install FreeRTOSConfig.h and CLEAN rebuild kernel. Adaptation held.\r\n");send(u,b,sizeof(b),n);return;
    }
    for(uint32_t i=0;i<3U;i++)if(!s_window[i].sample_valid || s_window[i].window_misses ||
        s_window[i].window_errors || s_window[i].health_status!=HEALTH_HEALTHY)s_continuity_ok=0U;
    if(!s_baseline_seen && !g_faultInjection.observed_active){
        memcpy(s_baseline,s_window,sizeof(s_baseline));s_baseline_seen=1;
        for(uint32_t i=0;i<TASK_COUNT;i++)if(!s_window[i].sample_valid || s_window[i].health_status!=HEALTH_HEALTHY ||
            s_window[i].window_misses || s_window[i].window_errors)s_baseline_ok=0;
    }
    osPriority_t sampled=s_seen[TASK_COMMUNICATION]?s_last_priority[TASK_COMMUNICATION]:AS_GetBasePriority(TASK_COMMUNICATION);
    uint8_t changed=0;
    for(uint32_t i=0;i<TASK_COUNT;i++){
        AS_TaskSnapshot_t v;if(!AS_GetTaskSnapshot((TaskIndex_t)i,&v))continue;
        if(i!=TASK_COMMUNICATION && v.current_priority!=v.base_priority)s_protected_ok=0;
        if(s_seen[i] && v.current_priority!=s_last_priority[i]){
            n=snprintf(b,sizeof(b),"[PRIORITY EVENT] %s %d -> %d | %s | readback=%s\r\n",s_window[i].name,
                (int)s_last_priority[i],(int)v.current_priority,AS_GetReasonName(v.reason),v.readback_match?"MATCH":"FAILED");send(u,b,sizeof(b),n);
            if(s_change_count<16U)s_changes[s_change_count++]=(Change_t){cycle,s_last_priority[i],v.current_priority,v.reason,(uint8_t)i};
            else s_overflow++;
            if(v.current_priority==v.base_priority)s_restored++;else if(v.current_priority<v.base_priority)s_down++;
            if(i==TASK_COMMUNICATION && v.readback_match && v.set_attempted){
                if(v.current_priority==(osPriority_t)AS_COMM_DEMOTED_PRIORITY &&
                   v.current_priority<AS_GetActualPriorityReadback(TASK_LOGGING))s_demotion_verified=1;
                if(v.current_priority==v.base_priority && v.good_windows>=AS_RESTORE_WINDOWS)s_restore_verified=1;
            }
            changed=1;
        }
        if(v.recovery_flags!=s_last_flags[i]){
            n=snprintf(b,sizeof(b),"[PENDING REQUEST] %s 0x%02lX -> 0x%02lX | controller request flags\r\n",s_window[i].name,
                (unsigned long)s_last_flags[i],(unsigned long)v.recovery_flags);send(u,b,sizeof(b),n);
            if(v.recovery_flags)s_requests++;else s_request_clears++;
            if(i==TASK_COMMUNICATION && (v.recovery_flags&AS_FLAG_NEEDS_RECOVERY_PERSISTENT)){
                if(s_demotion_verified && v.adaptation_active && v.response_windows>=AS_RECOVERY_WAIT_WINDOWS)s_pending_verified=1;
                else s_early_pending=1;
            }
        }
        s_last_priority[i]=v.current_priority;s_last_flags[i]=v.recovery_flags;s_seen[i]=1;
    }
    if(changed)print_order(u);
    AS_TaskSnapshot_t v;if(!AS_GetTaskSnapshot(TASK_COMMUNICATION,&v))return;
    TaskHealth_t *h=&s_window[TASK_COMMUNICATION];
    if(s_history_count<EXPERIMENT_STOP_CYCLE)s_history[s_history_count++]=(CycleRow_t){
        cycle,h->execution_time_us,(uint32_t)(h->health_score*10.0f+0.5f),v.good_windows,v.bad_windows,v.response_windows,
        (uint32_t)v.recovery_flags,(int32_t)sampled,(int32_t)v.current_priority,g_faultInjection.observed_active,h->health_status};
    else s_overflow++;
    if(g_faultInjection.observed_active){
        uint32_t adapted=sampled!=AS_GetBasePriority(TASK_COMMUNICATION);
        for(uint32_t i=0;i<TASK_COUNT;i++){
            TaskHealth_t *h_i=&s_window[i];CaseStats_t *p=&s_stats[adapted][i];
            p->response_sum+=h_i->window_response_sum_us;p->jobs+=h_i->window_releases;p->misses+=h_i->window_misses;
            if(h_i->response_time_us>p->worst_response)p->worst_response=h_i->response_time_us;
        }
    }
}
void ER_OnFaultActivated(UART_HandleTypeDef *u,uint32_t c){(void)c;FI_PrintActivationBanner(u);}
void ER_OnFaultTerminated(UART_HandleTypeDef *u,uint32_t c){(void)c;FI_PrintTerminationBanner(u);}
static void check(UART_HandleTypeDef *u,const char *name,uint8_t seen,uint8_t pass)
{
    char b[128];int n=snprintf(b,sizeof(b),"%-49s : %s\r\n",name,seen?(pass?"PASS":"FAIL"):"NOT OBSERVED");send(u,b,sizeof(b),n);
}
void ER_PrintFinalSummary(UART_HandleTypeDef *u,uint32_t cycle)
{
    char b[224];int n;
    n=snprintf(b,sizeof(b),"\r\n========================= FINAL EXPERIMENT SUMMARY =========================\r\nCycles=%lu | demotions=%lu | restores=%lu | pending events=%lu | clears=%lu\r\n",
        (unsigned long)cycle,(unsigned long)s_down,(unsigned long)s_restored,(unsigned long)s_requests,(unsigned long)s_request_clears);send(u,b,sizeof(b),n);
    n=snprintf(b,sizeof(b),"Injection cycle=%lu | first clean exposure window=%lu (NO scheduled OFF)\r\n\r\nCommunication timeline (Fault = load in the captured window)\r\n",
        (unsigned long)g_faultInjection.activation_cycle,(unsigned long)g_faultInjection.effect_clear_cycle);send(u,b,sizeof(b),n);
    n=snprintf(b,sizeof(b),"Cycle Fault CPU(us) Score Status   Sample Now Bad AfterBad Good Flags\r\n---------------------------------------------------------------------------\r\n");send(u,b,sizeof(b),n);
    for(uint32_t i=0;i<s_history_count;i++){
        const CycleRow_t *h=&s_history[i];n=snprintf(b,sizeof(b),"%5lu %-5s %7lu %3lu.%lu %-8s %6ld %3ld %3lu %8lu %4lu 0x%02lX\r\n",
            (unsigned long)h->cycle,h->fault?"ON":"OFF",(unsigned long)h->cpu,(unsigned long)(h->score/10U),(unsigned long)(h->score%10U),
            RTM_GetHealthName(h->status),(long)h->sampled,(long)h->now,(unsigned long)h->bad,(unsigned long)h->after,(unsigned long)h->good,(unsigned long)h->flags);send(u,b,sizeof(b),n);
    }
    n=snprintf(b,sizeof(b),"\r\nReadback-confirmed priority transitions\r\nCycle Task          From  To Reason\r\n---------------------------------------------------------------------------\r\n");send(u,b,sizeof(b),n);
    for(uint32_t i=0;i<s_change_count;i++){
        const Change_t *p=&s_changes[i];n=snprintf(b,sizeof(b),"%5lu %-13s %4ld %3ld %s\r\n",(unsigned long)p->cycle,s_window[p->task].name,
            (long)p->from,(long)p->to,AS_GetReasonName(p->reason));send(u,b,sizeof(b),n);
    }
    if(!s_change_count){n=snprintf(b,sizeof(b),"(none recorded)\r\n");send(u,b,sizeof(b),n);}
    n=snprintf(b,sizeof(b),"\r\nAll-task baseline and final state\r\nTask          BaseScore FinalScore Base Now MissTotal ErrorTotal Status\r\n---------------------------------------------------------------------------\r\n");send(u,b,sizeof(b),n);
    uint8_t final_ok=1;
    for(uint32_t i=0;i<TASK_COUNT;i++){
        const TaskHealth_t *h=&s_window[i];uint32_t base=(uint32_t)(s_baseline[i].health_score*10.0f+0.5f),score=(uint32_t)(h->health_score*10.0f+0.5f);
        n=snprintf(b,sizeof(b),"%-13s %5lu.%lu %7lu.%lu %4d %3d %9lu %10lu %s\r\n",h->name,
            (unsigned long)(base/10U),(unsigned long)(base%10U),(unsigned long)(score/10U),(unsigned long)(score%10U),
            (int)AS_GetBasePriority((TaskIndex_t)i),(int)AS_GetActualPriorityReadback((TaskIndex_t)i),
            (unsigned long)h->deadline_miss_count,(unsigned long)h->error_count,RTM_GetHealthName(h->health_status));send(u,b,sizeof(b),n);
        if(!h->sample_valid || h->health_status!=HEALTH_HEALTHY || AS_GetRecoveryFlag((TaskIndex_t)i) ||
            AS_GetActualPriorityReadback((TaskIndex_t)i)!=AS_GetBasePriority((TaskIndex_t)i))final_ok=0;
    }
    n=snprintf(b,sizeof(b),"\r\nMeasured responses WHILE the injected fault was active\r\nTask          CommMode Jobs Miss MeanResp(us) WorstResp(us)\r\n---------------------------------------------------------------------------\r\n");send(u,b,sizeof(b),n);
    for(uint32_t a=0;a<2;a++)for(uint32_t i=0;i<TASK_COUNT;i++){
        const CaseStats_t *p=&s_stats[a][i];if(!p->jobs)continue;
        n=snprintf(b,sizeof(b),"%-13s %-8s %4lu %4lu %12lu %13lu\r\n",s_window[i].name,a?"DEMOTED":"BASE",
            (unsigned long)p->jobs,(unsigned long)p->misses,(unsigned long)(p->response_sum/p->jobs),(unsigned long)p->worst_response);send(u,b,sizeof(b),n);
    }
    n=snprintf(b,sizeof(b),"\r\nEvidence checks\r\n---------------------------------------------------------------------------\r\n");send(u,b,sizeof(b),n);
    check(u,"All five tasks Healthy before injection",s_baseline_seen,s_baseline_ok);
    uint8_t main_recovery=HR_ENABLE_RECOVERY && MRG_DEMO_CASE==0 && !HR_IsSoftwareResetBoot();
    HR_TaskSnapshot_t recovery;HR_GetTaskSnapshot(TASK_COMMUNICATION,&recovery);
    check(u,"Latched fault injected at configured cycle",!HR_IsSoftwareResetBoot(),g_faultInjection.activation_cycle==FI_TRIGGER_CYCLE);
    check(u,"Communication demoted below Logging (verified)",AS_ENABLE_ADAPTATION && !HR_IsSoftwareResetBoot(),s_demotion_verified);
    check(u,"Pending after three further bad observations",AS_ENABLE_ADAPTATION && !HR_IsSoftwareResetBoot(),s_pending_verified && !s_early_pending);
    check(u,"Actual restoration after three clean Healthy",main_recovery,s_restore_verified);
    check(u,"L2 verified, no L3 action or MCU reset needed",main_recovery,
        recovery.successes==1U && recovery.task_actions==1U && recovery.degrade_actions==0U && HR_GetResetRequests()==0U);
    check(u,"Safety/Charging/Sensor/Logging priorities protected",1,s_protected_ok);
    check(u,"Safety/Charging/Sensor progress and health retained",1,s_continuity_ok);
    check(u,"Final all Healthy, at base, no pending flags",main_recovery || HR_IsSoftwareResetBoot(),final_ok);
    check(u,"L3 limited service verified, request retained",HR_ENABLE_RECOVERY && MRG_DEMO_CASE==1 && !HR_IsSoftwareResetBoot(),
        recovery.state==HR_LIMITED_STABLE && APP_IsLimited(TASK_COMMUNICATION) && AS_GetRecoveryFlag(TASK_COMMUNICATION)!=0U);
    check(u,"L4 controlled reset requested",HR_ENABLE_RECOVERY && MRG_DEMO_CASE==2 && !HR_IsSoftwareResetBoot(),HR_GetResetRequests()==1U);
    check(u,"Complete observations, no report overflow",1,s_history_count==cycle && s_overflow==0U);
    check(u,"CPU trace hooks active",1,RTM_TraceIsActive());
    check(u,"Recovery event buffer did not overflow",1,HR_GetEventDrops()==0U);
    n=snprintf(b,sizeof(b),"\r\nActual recovery results\r\nTask          L2Actions L3Actions FullOK LimitedOK State          SameTask\r\n---------------------------------------------------------------------------\r\n");send(u,b,sizeof(b),n);
    uint32_t total_l2=0U,total_l3=0U;
    for(uint32_t i=0;i<TASK_COUNT;i++){
        HR_TaskSnapshot_t r;HR_GetTaskSnapshot((TaskIndex_t)i,&r);total_l2+=r.task_actions;total_l3+=r.degrade_actions;
        n=snprintf(b,sizeof(b),"%-13s %9lu %9lu %6lu %9lu %-14s %s\r\n",s_window[i].name,(unsigned long)r.task_actions,
            (unsigned long)r.degrade_actions,(unsigned long)r.successes,(unsigned long)r.limited_successes,HR_GetStateName(r.state),
            s_window[i].handle==s_baseline[i].handle?"YES":"NO");send(u,b,sizeof(b),n);
        if(r.episode){
            n=snprintf(b,sizeof(b),"  request_tick=%lu ack_tick=%lu verified_tick=%lu generation=%lu recovery_ms=%lu\r\n",
                (unsigned long)r.request_tick,(unsigned long)r.ack_tick,(unsigned long)r.verified_tick,(unsigned long)r.generation,
                (unsigned long)r.recovery_time_ms);send(u,b,sizeof(b),n);
        }
    }
    n=snprintf(b,sizeof(b),"L2 application reinitializations=%lu | L3 transitions=%lu | reset requests=%lu | reset calls=%lu\r\n",
        (unsigned long)total_l2,(unsigned long)total_l3,(unsigned long)HR_GetResetRequests(),(unsigned long)HR_GetResetCalls());send(u,b,sizeof(b),n);
    n=snprintf(b,sizeof(b),"Diagnostic queue: %lu/%lu, cumulative drops=%lu\r\nL2 retains the RTOS task/stack. Limited service is reported separately from full recovery.\r\n",
        (unsigned long)s_queue.current_count,(unsigned long)s_queue.capacity,(unsigned long)s_queue.drop_count);send(u,b,sizeof(b),n);
    n=snprintf(b,sizeof(b),"No timed fault withdrawal. Recovery success requires task acknowledgement and fresh measured health.\r\n============================= EXPERIMENT COMPLETE =============================\r\n");send(u,b,sizeof(b),n);
}
