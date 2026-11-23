#ifndef HEALTH_ENGINE_H
#define HEALTH_ENGINE_H
#include "runtime_monitor.h"
float HE_GetHealthScore(TaskIndex_t idx);
HealthStatus_t HE_GetHealthStatus(TaskIndex_t idx);
uint32_t HE_GetPersistenceCount(TaskIndex_t idx);
uint32_t HE_GetDeadlineMissCount(TaskIndex_t idx);
uint32_t HE_GetErrorCount(TaskIndex_t idx);
uint32_t HE_GetTaskState(TaskIndex_t idx);
void HE_PrintTable(UART_HandleTypeDef *uart);
void HE_PrintCompactHealth(UART_HandleTypeDef *uart);
#endif
