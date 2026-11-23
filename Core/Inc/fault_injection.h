#ifndef FAULT_INJECTION_H
#define FAULT_INJECTION_H
#include "runtime_monitor.h"
#include "task_service.h"
typedef enum { FAULT_STATE_OFF=0,FAULT_STATE_ACTIVE,FAULT_STATE_CLEARED } FaultState_t;
typedef enum { FI_EVENT_NONE=0,FI_EVENT_ACTIVATED,FI_EVENT_STOPPED } FI_Event_t;
#define FI_TRIGGER_CYCLE 2U
#define FI_COMM_TARGET_US 5000U
typedef struct {
    FaultState_t state;
    uint32_t experiment_cycle,activation_cycle,effect_clear_cycle;
    uint8_t active,observed_active,injected,script_enabled;
    APP_Fault_t fault_model;
} FaultInjection_t;
extern FaultInjection_t g_faultInjection;
void FI_Init(void);
FI_Event_t FI_Update(void);
uint32_t FI_GetExtraWorkloadIterations(TaskIndex_t task);
void FI_RunCpuWorkload(TaskIndex_t task);
void FI_PrintStatus(UART_HandleTypeDef *uart);
void FI_PrintActivationBanner(UART_HandleTypeDef *uart);
void FI_PrintTerminationBanner(UART_HandleTypeDef *uart);
#endif
