#include "health_engine.h"
#include <stdio.h>
/* Scoring is performed once by RTM_Update. These legacy accessors do not rescore data. */
float HE_GetHealthScore(TaskIndex_t i){return (uint32_t)i<TASK_COUNT?g_taskHealth[i].health_score:0.0f;}
HealthStatus_t HE_GetHealthStatus(TaskIndex_t i){return (uint32_t)i<TASK_COUNT?(HealthStatus_t)g_taskHealth[i].health_status:HEALTH_CRITICAL;}
uint32_t HE_GetPersistenceCount(TaskIndex_t i){return (uint32_t)i<TASK_COUNT?g_taskHealth[i].consecutive_bad:0U;}
uint32_t HE_GetDeadlineMissCount(TaskIndex_t i){return (uint32_t)i<TASK_COUNT?g_taskHealth[i].deadline_miss_count:0U;}
uint32_t HE_GetErrorCount(TaskIndex_t i){return (uint32_t)i<TASK_COUNT?g_taskHealth[i].error_count:0U;}
uint32_t HE_GetTaskState(TaskIndex_t i){return (uint32_t)i<TASK_COUNT?g_taskHealth[i].last_state:0U;}
static TaskHealth_t s_print_window[TASK_COUNT];
static void send(UART_HandleTypeDef *u,const char *b,size_t cap,int n)
{if(u && n>0)HAL_UART_Transmit(u,(uint8_t*)b,(uint16_t)((size_t)n<cap?(size_t)n:cap-1U),HAL_MAX_DELAY);}
void HE_PrintTable(UART_HandleTypeDef *u)
{
    if(!RTM_CopyWindow(s_print_window,NULL,NULL))return;
    char b[192];int n;
    n=snprintf(b,sizeof(b),"\r\n========================== TASK HEALTH ENGINE ==========================\r\nTask          Exec(us) Resp(us) Miss Errors Score Status\r\n");send(u,b,sizeof(b),n);
    n=snprintf(b,sizeof(b),"-----------------------------------------------------------------------\r\n");send(u,b,sizeof(b),n);
    for(uint32_t i=0;i<TASK_COUNT;i++){
        const TaskHealth_t *h=&s_print_window[i];uint32_t q=(uint32_t)(h->health_score*10.0f+0.5f);
        n=snprintf(b,sizeof(b),"%-13s %8lu %8lu %4lu %6lu %3lu.%lu %s\r\n",h->name,
            (unsigned long)h->execution_time_us,(unsigned long)h->response_time_us,(unsigned long)h->window_misses,
            (unsigned long)h->window_errors,(unsigned long)(q/10U),(unsigned long)(q%10U),RTM_GetHealthName(h->health_status));send(u,b,sizeof(b),n);
    }
    n=snprintf(b,sizeof(b),"Healthy >=80 | Warning >=60 | Degraded >=40 | Critical <40\r\nSame captured window as Runtime Monitor; Miss/Errors are window counts.\r\n");send(u,b,sizeof(b),n);
    n=snprintf(b,sizeof(b),"Degraded/Critical status needs %lu consecutive windows.\r\n=======================================================================\r\n",(unsigned long)RTM_PERSISTENCE_WINDOWS);send(u,b,sizeof(b),n);
}
void HE_PrintCompactHealth(UART_HandleTypeDef *u){(void)u;}
