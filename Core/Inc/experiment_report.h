#ifndef EXPERIMENT_REPORT_H
#define EXPERIMENT_REPORT_H
#include "adaptive_scheduler.h"
#include "fault_injection.h"
void ER_Init(void);
void ER_Update(UART_HandleTypeDef *uart,uint32_t cycle);
void ER_OnFaultActivated(UART_HandleTypeDef *uart,uint32_t cycle);
void ER_OnFaultTerminated(UART_HandleTypeDef *uart,uint32_t cycle);
void ER_PrintFinalSummary(UART_HandleTypeDef *uart,uint32_t cycle);
#endif
