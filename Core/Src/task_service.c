#include "task_service.h"
#include <string.h>
static APP_TaskService_t s_service[TASK_COUNT];
static uint8_t s_simulated_safe,s_charging_output;
static uint8_t valid(TaskIndex_t i){return (uint32_t)i<TASK_COUNT;}
static uint32_t lock(void){uint32_t p=__get_PRIMASK();__disable_irq();return p;}
static void unlock(uint32_t p){if(!p)__enable_irq();}
void APP_Init(void){memset(s_service,0,sizeof(s_service));s_simulated_safe=s_charging_output=0U;}
uint8_t APP_InjectFault(TaskIndex_t i,APP_Fault_t fault,uint32_t iterations)
{
    if(!valid(i) || fault>APP_FAULT_EXTERNAL_ESSENTIAL)return 0U;
    uint32_t p=lock();s_service[i].fault=fault;s_service[i].workload_iterations=iterations;unlock(p);return 1U;
}
uint8_t APP_Reinitialize(TaskIndex_t i)
{
    if(i!=TASK_COMMUNICATION && i!=TASK_LOGGING)return 0U;
    uint32_t p=lock();APP_TaskService_t *s=&s_service[i];
    /* Application/session state is recovered inside its owning task. An
       external fault source survives reinitialization, as it should. */
    s->session_jobs=0U;s->reset_count++;s->limited=0U;
    if(s->fault==APP_FAULT_INTERNAL_CPU)s->fault=APP_FAULT_NONE;
    unlock(p);return 1U;
}
uint8_t APP_EnterLimitedService(TaskIndex_t i)
{
    if(i!=TASK_COMMUNICATION && i!=TASK_LOGGING)return 0U;
    uint32_t p=lock();s_service[i].limited=1U;s_service[i].degrade_count++;unlock(p);return 1U;
}
uint32_t APP_GetExtraWorkIterations(TaskIndex_t i)
{
    if(!valid(i))return 0U;
    uint32_t p=lock();APP_TaskService_t s=s_service[i];unlock(p);
    if(s.fault==APP_FAULT_NONE || (s.limited && s.fault!=APP_FAULT_EXTERNAL_ESSENTIAL))return 0U;
    return s.workload_iterations;
}
uint8_t APP_IsFaultEffective(TaskIndex_t i){return APP_GetExtraWorkIterations(i)>0U;}
uint8_t APP_IsLimited(TaskIndex_t i)
{
    if(!valid(i))return 0U;
    uint32_t p=lock();uint8_t result=s_service[i].limited;unlock(p);return result;
}
void APP_JobCompleted(TaskIndex_t i)
{
    if(valid(i)){uint32_t p=lock();s_service[i].session_jobs++;unlock(p);}
}
uint8_t APP_GetSnapshot(TaskIndex_t i,APP_TaskService_t *out)
{
    if(!valid(i) || !out)return 0U;
    uint32_t p=lock();*out=s_service[i];unlock(p);return 1U;
}
void APP_UpdateSimulatedChargingOutput(uint8_t request){uint32_t p=lock();s_charging_output=request && !s_simulated_safe;unlock(p);}
void APP_EnterSimulatedSafeState(void){uint32_t p=lock();s_simulated_safe=1U;s_charging_output=0U;unlock(p);}
uint8_t APP_IsSimulatedSafe(void){uint32_t p=lock();uint8_t safe=s_simulated_safe && !s_charging_output;unlock(p);return safe;}
