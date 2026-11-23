#include "hierarchical_recovery.h"
#include "task.h"
#include <stdio.h>
#include <string.h>
typedef enum { CMD_NONE=0,CMD_REINITIALIZE,CMD_DEGRADE } Command_t;
typedef struct {
    uint32_t token,ack_token,generation,jobs,tick;
    Command_t command;
    uint8_t claimed,ok;
} Mailbox_t;
typedef struct {
    HR_TaskSnapshot_t view;
    HR_TaskOps_t ops;
    Mailbox_t mailbox;
    uint32_t attempts;
    uint8_t registered;
} Context_t;
typedef struct { uint32_t tick,token; uint8_t task; const char *message; } Event_t;
static Context_t s_ctx[TASK_COUNT];
static TaskHealth_t s_window[TASK_COUNT];
static HR_PlatformOps_t s_platform;
static Event_t s_events[32];
static uint32_t s_event_head,s_event_count,s_event_drops,s_sequence,s_next_token;
static uint32_t s_reset_requests,s_reset_calls,s_reset_tick;
static uint8_t s_reset_notice_printed;
static uint32_t lock(void){uint32_t p=__get_PRIMASK();__disable_irq();return p;}
static void unlock(uint32_t p){if(!p)__enable_irq();}
static uint8_t valid(TaskIndex_t i){return (uint32_t)i<TASK_COUNT;}
static uint32_t elapsed_ms(uint32_t begin)
{return (uint32_t)((uint64_t)(xTaskGetTickCount()-begin)*1000ULL/osKernelGetTickFreq());}
static void event(TaskIndex_t i,const char *message)
{
    if(s_event_count==32U){s_event_drops++;return;}
    uint32_t slot=(s_event_head+s_event_count)%32U;
    s_events[slot]=(Event_t){xTaskGetTickCount(),s_ctx[i].view.token,(uint8_t)i,message};s_event_count++;
}
void HR_Init(const HR_PlatformOps_t *platform)
{
    memset(s_ctx,0,sizeof(s_ctx));memset(&s_platform,0,sizeof(s_platform));
    if(platform)s_platform=*platform;
    s_event_head=s_event_count=s_event_drops=s_sequence=s_next_token=0U;
    s_reset_requests=s_reset_calls=s_reset_tick=0U;s_reset_notice_printed=0U;
}
uint8_t HR_RegisterTask(TaskIndex_t i,const HR_TaskOps_t *ops)
{
    if((i!=TASK_COMMUNICATION && i!=TASK_LOGGING) || !ops || !ops->reinitialize || !ops->enter_limited ||
       s_ctx[i].view.state!=HR_IDLE)return 0U;
    s_ctx[i].ops=*ops;s_ctx[i].registered=1U;return 1U;
}
static void request_system_reset(TaskIndex_t i,const char *reason)
{
    if(s_reset_requests)return;
    if(i==TASK_LOGGING){
        s_ctx[i].view.state=HR_NONCRITICAL_UNAVAILABLE;
        event(i,reason);event(i,"Logging unavailable: request retained; no system reset for diagnostics alone");return;
    }
    s_reset_requests=1U;s_reset_tick=xTaskGetTickCount();s_ctx[i].view.state=HR_RESET_PENDING;
    if(s_platform.enter_safe_state)s_platform.enter_safe_state();
    AS_SetSimulatedChargingState(EV_STATE_FAULT);
    event(i,reason);event(i,"L4_RESET_PENDING: simulated safe state requested; notice before reset");
}
static void issue(TaskIndex_t i,Command_t command)
{
    Context_t *c=&s_ctx[i];HR_TaskSnapshot_t *v=&c->view;
    uint32_t token=++s_next_token;if(!token)token=++s_next_token;
    uint32_t p=lock();c->mailbox=(Mailbox_t){.token=token,.command=command};unlock(p);
    v->token=token;v->action_tick=xTaskGetTickCount();v->good=v->bad=v->observations=0U;
    if(command==CMD_REINITIALIZE){c->attempts++;v->state=HR_WAIT_REINITIALIZE;event(i,"L2_REINITIALIZE_REQUESTED: keep task handle, stack and current priority");}
    else{v->state=HR_WAIT_DEGRADE;event(i,"L3_LIMITED_SERVICE_REQUESTED: retain essential work");}
}
static void degrade(TaskIndex_t i,const char *reason)
{
    event(i,reason);
    if(s_ctx[i].registered)issue(i,CMD_DEGRADE);
    else request_system_reset(i,"No supported selective recovery action");
}
void HR_TaskCheckpoint(TaskIndex_t i)
{
    if(!HR_ENABLE_RECOVERY || !valid(i) || osThreadGetId()!=g_taskHealth[i].handle)return;
    Context_t *c=&s_ctx[i];uint32_t p=lock();Mailbox_t m=c->mailbox;
    if(m.command==CMD_NONE || m.claimed || m.ack_token==m.token){unlock(p);return;}
    c->mailbox.claimed=1U;unlock(p);
    uint8_t ok=m.command==CMD_REINITIALIZE?c->ops.reinitialize(i):c->ops.enter_limited(i);
    uint32_t generation=ok?RTM_AdvanceTaskGeneration(i):0U;
    p=lock();
    /* An old acknowledgement cannot complete a superseding command. */
    if(c->mailbox.token==m.token){
        c->mailbox.ack_token=m.token;c->mailbox.ok=ok && generation!=0U;
        c->mailbox.generation=generation;c->mailbox.jobs=g_taskHealth[i].total_releases;
        c->mailbox.tick=xTaskGetTickCount();
    }
    unlock(p);
}
static uint8_t clean(const TaskHealth_t *h,const AS_TaskSnapshot_t *a)
{
    return h->sample_valid && h->health_status==HEALTH_HEALTHY && h->window_errors==0U && h->window_misses==0U &&
        (a->task_state==eRunning || a->task_state==eReady || a->task_state==eBlocked);
}
static void verify(TaskIndex_t i,const AS_TaskSnapshot_t *a)
{
    Context_t *c=&s_ctx[i];HR_TaskSnapshot_t *v=&c->view;TaskHealth_t *h=&s_window[i];
    v->observations++;
    uint8_t current=h->sample_valid && !h->window_mixed_generation && h->window_generation==v->generation;
    uint8_t progressed=h->total_releases!=v->last_jobs;
    uint8_t good=current && progressed && clean(h,a);
    v->last_jobs=h->total_releases;
    v->good=good?v->good+1U:0U;
    if(current)v->bad=good?0U:v->bad+1U;
    if(v->good>=HR_VERIFY_GOOD_WINDOWS){
        if(v->state==HR_VERIFY_REINITIALIZE){
            if(AS_CompleteManagedRecovery(i)!=osOK){degrade(i,"L2 verification passed; base-priority restoration failed");return;}
            v->state=HR_RECOVERED;v->successes++;v->verified_tick=xTaskGetTickCount();
            v->recovery_time_ms=elapsed_ms(v->request_tick);
            event(i,"L2_RECOVERY_VERIFIED: three fresh Healthy windows; base restored; flags cleared");
        }else{
            v->state=HR_LIMITED_STABLE;v->limited_successes++;v->verified_tick=xTaskGetTickCount();
            v->recovery_time_ms=elapsed_ms(v->request_tick);
            event(i,"L3_LIMITED_STABLE: three fresh Healthy windows; limited mode and pending flag retained");
        }
    }else if(v->bad>=HR_VERIFY_BAD_WINDOWS || v->observations>=HR_VERIFY_TIMEOUT_WINDOWS){
        if(v->state==HR_VERIFY_REINITIALIZE)degrade(i,"L2 did not restore sustained health; escalating");
        else request_system_reset(i,"L3 essential service still unhealthy; escalating to controlled reset");
    }
}
void HR_Update(void)
{
    uint32_t sequence;
    if(!HR_ENABLE_RECOVERY || !RTM_CopyWindow(s_window,NULL,&sequence) || sequence==s_sequence || !RTM_TraceIsActive())return;
    s_sequence=sequence;if(s_reset_requests)return;
    for(uint32_t n=0;n<TASK_COUNT && !s_reset_requests;n++){
        TaskIndex_t i=(TaskIndex_t)n;Context_t *c=&s_ctx[i];HR_TaskSnapshot_t *v=&c->view;
        AS_TaskSnapshot_t a;uint8_t observed=AS_GetTaskSnapshot(i,&a);
        AS_RecoveryFlag_t flags=AS_GetRecoveryFlag(i);
        if(v->state==HR_IDLE || v->state==HR_RECOVERED){
            if(!flags)continue;
            /* A persistent flag during natural clean verification can clear
               without needlessly reinitializing an already improving task. */
            if(observed && clean(&s_window[i],&a) && flags==AS_FLAG_NEEDS_RECOVERY_PERSISTENT)continue;
            v->episode++;v->request_tick=xTaskGetTickCount();v->request_flags=(uint32_t)flags;
            if(!c->registered || !observed || AS_BeginManagedRecovery(i)!=osOK){
                request_system_reset(i,"Critical/unavailable task: selective cooperative action not permitted");continue;
            }
            if(c->attempts<HR_MAX_REINITIALIZATIONS_PER_BOOT)issue(i,CMD_REINITIALIZE);
            else degrade(i,"Per-boot reinitialization budget exhausted");
        }else if(v->state==HR_WAIT_REINITIALIZE || v->state==HR_WAIT_DEGRADE){
            uint32_t p=lock();Mailbox_t m=c->mailbox;unlock(p);
            if(m.ack_token==v->token){
                uint8_t reinitialize=v->state==HR_WAIT_REINITIALIZE;
                if(reinitialize)v->task_actions++;else v->degrade_actions++;
                if(!m.ok){
                    if(reinitialize)degrade(i,"L2 callback failed; escalating");
                    else request_system_reset(i,"L3 callback failed; controlled reset required");
                    continue;
                }
                v->generation=m.generation;v->ack_tick=m.tick;v->last_jobs=m.jobs;
                v->state=reinitialize?HR_VERIFY_REINITIALIZE:HR_VERIFY_DEGRADED;
                v->good=v->bad=v->observations=0U;
                event(i,reinitialize?"L2_ACK: application context reinitialized by owning task":"L3_ACK: limited service entered by owning task");
                if(observed)verify(i,&a);
            }else if(elapsed_ms(v->action_tick)>=HR_ACK_TIMEOUT_MS){
                if(v->state==HR_WAIT_REINITIALIZE)degrade(i,"L2 checkpoint acknowledgement timeout");
                else request_system_reset(i,"L3 acknowledgement timeout; task cannot cooperate");
            }
        }else if(v->state==HR_VERIFY_REINITIALIZE || v->state==HR_VERIFY_DEGRADED){
            if(observed)verify(i,&a);
            else if(++v->observations>=HR_VERIFY_TIMEOUT_WINDOWS)request_system_reset(i,"Task observation lost during recovery verification");
        }else if(v->state==HR_LIMITED_STABLE){
            uint8_t good=observed && clean(&s_window[i],&a);
            v->bad=good?0U:v->bad+1U;
            if(v->bad>=HR_VERIFY_BAD_WINDOWS)request_system_reset(i,"Previously stable limited service failed");
        }
    }
}
osStatus_t HR_RequestFullService(TaskIndex_t i)
{
    if(!HR_ENABLE_RECOVERY || !valid(i) || s_reset_requests || !s_ctx[i].registered ||
       s_ctx[i].view.state!=HR_LIMITED_STABLE || s_ctx[i].attempts>=HR_MAX_REINITIALIZATIONS_PER_BOOT)return osErrorParameter;
    HR_TaskSnapshot_t *v=&s_ctx[i].view;v->episode++;v->request_tick=xTaskGetTickCount();
    v->request_flags=(uint32_t)AS_GetRecoveryFlag(i);
    issue(i,CMD_REINITIALIZE);return osOK;
}
uint8_t HR_GetTaskSnapshot(TaskIndex_t i,HR_TaskSnapshot_t *out)
{if(!valid(i) || !out)return 0U;*out=s_ctx[i].view;return 1U;}
const char *HR_GetStateName(HR_State_t s)
{
    switch(s){
        case HR_IDLE:return "Idle";
        case HR_WAIT_REINITIALIZE:return "L2WaitAck";
        case HR_VERIFY_REINITIALIZE:return "L2Verify";
        case HR_RECOVERED:return "Recovered";
        case HR_WAIT_DEGRADE:return "L3WaitAck";
        case HR_VERIFY_DEGRADED:return "L3Verify";
        case HR_LIMITED_STABLE:return "LimitedStable";
        case HR_RESET_PENDING:return "L4ResetPending";
        case HR_RESET_ISSUED:return "L4ResetIssued";
        case HR_NONCRITICAL_UNAVAILABLE:return "Unavailable";
        default:return "Invalid";
    }
}
uint32_t HR_GetResetRequests(void){return s_reset_requests;}
uint32_t HR_GetResetCalls(void){return s_reset_calls;}
uint32_t HR_GetEventDrops(void){return s_event_drops;}
uint8_t HR_IsSoftwareResetBoot(void){return s_platform.software_reset_boot;}
static uint8_t send(UART_HandleTypeDef *u,const char *b,size_t cap,int n)
{
    if(!u || n<=0)return 0U;
    return HAL_UART_Transmit(u,(uint8_t*)b,(uint16_t)((size_t)n<cap?(size_t)n:cap-1U),HAL_MAX_DELAY)==0;
}
void HR_PrintEvents(UART_HandleTypeDef *u)
{
    char b[224];
    while(s_event_count){
        Event_t e=s_events[s_event_head];
        int n=snprintf(b,sizeof(b),"[RECOVERY EVENT] tick=%lu %s token=%lu %s\r\n",(unsigned long)e.tick,
            g_taskHealth[e.task].name,(unsigned long)e.token,e.message);
        if(!send(u,b,sizeof(b),n))break;
        s_event_head=(s_event_head+1U)%32U;s_event_count--;
        if(strstr(e.message,"L4_RESET_PENDING") && u)s_reset_notice_printed=1U;
    }
}
void HR_PrintStatus(UART_HandleTypeDef *u)
{
    char b[224];int n;
    n=snprintf(b,sizeof(b),"\r\n========================== SELECTIVE HIERARCHICAL RECOVERY ==========================\r\n");send(u,b,sizeof(b),n);
    n=snprintf(b,sizeof(b),"Task          State          Gen Good Bad L2Act L3Act FullOK LimitOK\r\n------------------------------------------------------------------------------------\r\n");send(u,b,sizeof(b),n);
    for(uint32_t i=0;i<TASK_COUNT;i++){
        const HR_TaskSnapshot_t *v=&s_ctx[i].view;
        n=snprintf(b,sizeof(b),"%-13s %-14s %3lu %4lu %3lu %5lu %5lu %6lu %7lu\r\n",g_taskHealth[i].name,HR_GetStateName(v->state),
            (unsigned long)v->generation,(unsigned long)v->good,(unsigned long)v->bad,(unsigned long)v->task_actions,
            (unsigned long)v->degrade_actions,(unsigned long)v->successes,(unsigned long)v->limited_successes);send(u,b,sizeof(b),n);
        if(v->episode){
            n=snprintf(b,sizeof(b),"  token=%lu pending_origin=0x%02lX episode=%lu verified_recovery_ms=%lu\r\n",
                (unsigned long)v->token,(unsigned long)v->request_flags,(unsigned long)v->episode,(unsigned long)v->recovery_time_ms);send(u,b,sizeof(b),n);
        }
    }
    n=snprintf(b,sizeof(b),"Mode=%s | reset requests=%lu calls=%lu | software-reset boot=%s\r\nL2 = application reinitialization in SAME task; L3 = limited service, not full recovery.\r\n",
        HR_ENABLE_RECOVERY?"ENABLED":"DISABLED",(unsigned long)s_reset_requests,(unsigned long)s_reset_calls,
        s_platform.software_reset_boot?"YES":"NO");send(u,b,sizeof(b),n);
    n=snprintf(b,sizeof(b),"====================================================================================\r\n");send(u,b,sizeof(b),n);
}
void HR_DispatchControlledReset(void)
{
    if(!s_reset_requests || s_reset_calls || !s_reset_notice_printed || elapsed_ms(s_reset_tick)<HR_RESET_NOTICE_MS ||
       !s_platform.is_safe || !s_platform.is_safe() || !s_platform.system_reset)return;
    s_reset_calls=1U;
    for(uint32_t i=0;i<TASK_COUNT;i++)if(s_ctx[i].view.state==HR_RESET_PENDING)s_ctx[i].view.state=HR_RESET_ISSUED;
    s_platform.system_reset();
}
