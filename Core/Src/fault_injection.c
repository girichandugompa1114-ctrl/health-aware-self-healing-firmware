#include "fault_injection.h"
#include "hierarchical_recovery.h"
#include <stdio.h>
#include <string.h>
FaultInjection_t g_faultInjection;
static uint32_t s_comm_iterations;
static TaskHealth_t s_window[TASK_COUNT];
#if defined(__GNUC__)
__attribute__((noinline,noclone))
#endif
static void burn_iterations(uint32_t count)
{
    volatile uint32_t value=0;
    for(uint32_t n=0;n<count;n++)value+=n;
}
void FI_Init(void)
{
    memset(&g_faultInjection,0,sizeof(g_faultInjection));
    g_faultInjection.script_enabled=!HR_IsSoftwareResetBoot();
    g_faultInjection.fault_model=(APP_Fault_t)(MRG_DEMO_CASE+1);
#if defined(MRG_HOST_TEST)
    s_comm_iterations=FI_COMM_TARGET_US*10U;
#else
    const uint32_t sample=50000U;
    uint32_t begin=DWT->CYCCNT;burn_iterations(sample);uint32_t used=DWT->CYCCNT-begin;
    if(!used || !SystemCoreClock)Error_Handler();
    s_comm_iterations=(uint32_t)((uint64_t)sample*FI_COMM_TARGET_US*SystemCoreClock/((uint64_t)used*1000000ULL));
    if(!s_comm_iterations)Error_Handler();
#endif
}
FI_Event_t FI_Update(void)
{
    FaultInjection_t *f=&g_faultInjection;f->experiment_cycle++;
    if(!RTM_CopyWindow(s_window,NULL,NULL))return FI_EVENT_NONE;
    /* Exposure is tagged by actual jobs, not inferred from last cycle's switch. */
    f->observed_active=s_window[TASK_COMMUNICATION].window_fault_jobs>0U;
    FI_Event_t event=FI_EVENT_NONE;
    if(f->script_enabled && !f->injected && f->experiment_cycle>=FI_TRIGGER_CYCLE){
        if(APP_InjectFault(TASK_COMMUNICATION,f->fault_model,s_comm_iterations)){
            f->injected=1U;f->activation_cycle=f->experiment_cycle;event=FI_EVENT_ACTIVATED;
        }
    }
    f->active=APP_IsFaultEffective(TASK_COMMUNICATION);
    f->state=f->active?FAULT_STATE_ACTIVE:f->injected?FAULT_STATE_CLEARED:FAULT_STATE_OFF;
    if(f->injected && !f->active && !f->observed_active && !f->effect_clear_cycle){
        f->effect_clear_cycle=f->experiment_cycle;event=FI_EVENT_STOPPED;
    }
    return event;
}
uint32_t FI_GetExtraWorkloadIterations(TaskIndex_t i){return APP_GetExtraWorkIterations(i);}
void FI_RunCpuWorkload(TaskIndex_t i)
{
    uint32_t count=FI_GetExtraWorkloadIterations(i);
    if(count){RTM_TagFaultExposure(i);burn_iterations(count);}
}
static void send(UART_HandleTypeDef *u,const char *b,size_t cap,int n)
{if(u && n>0)HAL_UART_Transmit(u,(uint8_t*)b,(uint16_t)((size_t)n<cap?(size_t)n:cap-1U),HAL_MAX_DELAY);}
void FI_PrintStatus(UART_HandleTypeDef *u)
{
    char b[224];int n;FaultInjection_t *f=&g_faultInjection;
    n=snprintf(b,sizeof(b),"\r\n========================== FAULT INJECTION ==========================\r\nTarget: Communication | model=%s\r\n",
        f->fault_model==APP_FAULT_INTERNAL_CPU?"INTERNAL_LATCH":f->fault_model==APP_FAULT_EXTERNAL_OPTIONAL?"EXTERNAL_OPTIONAL":"EXTERNAL_ESSENTIAL");send(u,b,sizeof(b),n);
    n=snprintf(b,sizeof(b),"Cycle=%lu/%lu | measured exposure=%s | fault-effect snapshot=%s\r\nInjection cycle=%lu | first clean captured cycle=%lu (0 = not yet)\r\n",
        (unsigned long)f->experiment_cycle,(unsigned long)EXPERIMENT_STOP_CYCLE,f->observed_active?"YES":"NO",f->active?"YES":"NO",
        (unsigned long)f->activation_cycle,(unsigned long)f->effect_clear_cycle);send(u,b,sizeof(b),n);
    n=snprintf(b,sizeof(b),"Scheduled fault withdrawal: NONE | auto-injection=%s\r\nCPU target=%lu us/job; table shows measured execution.\r\n",
        f->script_enabled?"ARMED ONCE":"SUPPRESSED AFTER SOFTWARE RESET",(unsigned long)FI_COMM_TARGET_US);send(u,b,sizeof(b),n);
    n=snprintf(b,sizeof(b),"====================================================================\r\n");send(u,b,sizeof(b),n);
}
void FI_PrintActivationBanner(UART_HandleTypeDef *u)
{
    char b[176];int n=snprintf(b,sizeof(b),"[FAULT INJECTED] cycle %lu: application fault latched. No scheduled OFF.\r\nApplies after this capture; only job measurements establish its effect.\r\n",
        (unsigned long)g_faultInjection.experiment_cycle);send(u,b,sizeof(b),n);
}
void FI_PrintTerminationBanner(UART_HandleTypeDef *u)
{
    char b[176];int n=snprintf(b,sizeof(b),"[FAULT EFFECT CLEARED] first clean exposure window=%lu.\r\nThe experiment timer did not switch the fault off; inspect recovery ACK/generation.\r\n",
        (unsigned long)g_faultInjection.effect_clear_cycle);send(u,b,sizeof(b),n);
}
