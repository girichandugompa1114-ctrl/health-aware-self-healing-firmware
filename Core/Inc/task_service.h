#ifndef TASK_SERVICE_H
#define TASK_SERVICE_H
#include "runtime_monitor.h"
typedef enum { APP_FAULT_NONE=0, APP_FAULT_INTERNAL_CPU=1, APP_FAULT_EXTERNAL_OPTIONAL=2, APP_FAULT_EXTERNAL_ESSENTIAL=3 } APP_Fault_t;
typedef struct {
    uint32_t session_jobs,reset_count,degrade_count;
    APP_Fault_t fault;
    uint32_t workload_iterations;
    uint8_t limited;
} APP_TaskService_t;
void APP_Init(void);
/* Injector sets application fault state; recovery callbacks never touch FI. */
uint8_t APP_InjectFault(TaskIndex_t idx,APP_Fault_t fault,uint32_t iterations);
uint8_t APP_Reinitialize(TaskIndex_t idx);
uint8_t APP_EnterLimitedService(TaskIndex_t idx);
uint32_t APP_GetExtraWorkIterations(TaskIndex_t idx);
uint8_t APP_IsLimited(TaskIndex_t idx);
uint8_t APP_IsFaultEffective(TaskIndex_t idx);
void APP_JobCompleted(TaskIndex_t idx);
uint8_t APP_GetSnapshot(TaskIndex_t idx,APP_TaskService_t *out);
void APP_UpdateSimulatedChargingOutput(uint8_t request);
void APP_EnterSimulatedSafeState(void);
uint8_t APP_IsSimulatedSafe(void);
#endif
