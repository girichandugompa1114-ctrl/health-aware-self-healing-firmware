/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "FreeRTOS.h"
#include "task.h"
#include "runtime_monitor.h"
#include "health_engine.h"
#include "fault_injection.h"
#include "adaptive_scheduler.h"
#include "experiment_report.h"
#include "hierarchical_recovery.h"
#include "task_service.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
typedef StaticTask_t osStaticThreadDef_t;
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart2;

/* Definitions for RuntimeMonitorT */
osThreadId_t RuntimeMonitorTHandle;
/* 512 words for the monitor's bounded line formatting and UART tables. */
uint32_t RuntimeMonitorTBuffer[ 512 ];
osStaticThreadDef_t RuntimeMonitorTControlBlock;
const osThreadAttr_t RuntimeMonitorT_attributes = {
  .name = "RuntimeMonitorT",
  .cb_mem = &RuntimeMonitorTControlBlock,
  .cb_size = sizeof(RuntimeMonitorTControlBlock),
  .stack_mem = &RuntimeMonitorTBuffer[0],
  .stack_size = sizeof(RuntimeMonitorTBuffer),
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for Safety */
osThreadId_t SafetyHandle;
uint32_t SafetyBuffer[ 256 ];
osStaticThreadDef_t SafetyControlBlock;
const osThreadAttr_t Safety_attributes = {
  .name = "Safety",
  .cb_mem = &SafetyControlBlock,
  .cb_size = sizeof(SafetyControlBlock),
  .stack_mem = &SafetyBuffer[0],
  .stack_size = sizeof(SafetyBuffer),
  .priority = (osPriority_t) osPriorityHigh7,
};
/* Definitions for ChargingTask */
osThreadId_t ChargingTaskHandle;
uint32_t ChargingTaskBuffer[ 256 ];
osStaticThreadDef_t ChargingTaskControlBlock;
const osThreadAttr_t ChargingTask_attributes = {
  .name = "ChargingTask",
  .cb_mem = &ChargingTaskControlBlock,
  .cb_size = sizeof(ChargingTaskControlBlock),
  .stack_mem = &ChargingTaskBuffer[0],
  .stack_size = sizeof(ChargingTaskBuffer),
  .priority = (osPriority_t) osPriorityHigh5,
};
/* Definitions for SensorTask */
osThreadId_t SensorTaskHandle;
uint32_t SensorTaskBuffer[ 256 ];
osStaticThreadDef_t SensorTaskControlBlock;
const osThreadAttr_t SensorTask_attributes = {
  .name = "SensorTask",
  .cb_mem = &SensorTaskControlBlock,
  .cb_size = sizeof(SensorTaskControlBlock),
  .stack_mem = &SensorTaskBuffer[0],
  .stack_size = sizeof(SensorTaskBuffer),
  .priority = (osPriority_t) osPriorityHigh3,
};
/* Definitions for CommunicationTa */
osThreadId_t CommunicationTaHandle;
uint32_t CommunicationTaBuffer[ 256 ];
osStaticThreadDef_t CommunicationTaControlBlock;
const osThreadAttr_t CommunicationTa_attributes = {
  .name = "CommunicationTa",
  .cb_mem = &CommunicationTaControlBlock,
  .cb_size = sizeof(CommunicationTaControlBlock),
  .stack_mem = &CommunicationTaBuffer[0],
  .stack_size = sizeof(CommunicationTaBuffer),
  .priority = (osPriority_t) osPriorityHigh1,
};
/* Definitions for LoggingTask */
osThreadId_t LoggingTaskHandle;
uint32_t LoggingTaskBuffer[ 256 ];
osStaticThreadDef_t LoggingTaskControlBlock;
const osThreadAttr_t LoggingTask_attributes = {
  .name = "LoggingTask",
  .cb_mem = &LoggingTaskControlBlock,
  .cb_size = sizeof(LoggingTaskControlBlock),
  .stack_mem = &LoggingTaskBuffer[0],
  .stack_size = sizeof(LoggingTaskBuffer),
  .priority = (osPriority_t) osPriorityAboveNormal7,
};
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
void RuntimeMonitor(void *argument);
void SafetyTask(void *argument);
void Charging(void *argument);
void Sensor(void *argument);
void Communication(void *argument);
void Logging(void *argument);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void RecoverySafeState(void){APP_EnterSimulatedSafeState();}
static uint8_t RecoveryIsSafe(void){return APP_IsSimulatedSafe();}
static void RecoverySystemReset(void){__DSB();NVIC_SystemReset();}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */

  /* One-time startup banner. Printed here, BEFORE osKernelStart(),
   * so there is no task yet and therefore no possibility of this
   * colliding with RuntimeMonitorT's later UART output - this is
   * the only UART_Transmit call in the whole project that happens
   * outside the RuntimeMonitor task, and it happens strictly before
   * any task exists. Plain literal strings, strlen()-safe, no
   * snprintf(), no floats. */
  {
    static const char banner[] =
        "\r\n============================================================\r\n"
        "EV CHARGING FIRMWARE MONITOR\r\n"
        "Board: STM32F446RE\r\n"
        "UART: USART2 / 115200 baud\r\n"
        "Status: RUNNING\r\n"
        "============================================================\r\n";
    HAL_UART_Transmit(&huart2, (uint8_t *)banner, sizeof(banner) - 1, HAL_MAX_DELAY);
  }

  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of RuntimeMonitorT */
  RuntimeMonitorTHandle = osThreadNew(RuntimeMonitor, NULL, &RuntimeMonitorT_attributes);

  /* creation of Safety */
  SafetyHandle = osThreadNew(SafetyTask, NULL, &Safety_attributes);

  /* creation of ChargingTask */
  ChargingTaskHandle = osThreadNew(Charging, NULL, &ChargingTask_attributes);

  /* creation of SensorTask */
  SensorTaskHandle = osThreadNew(Sensor, NULL, &SensorTask_attributes);

  /* creation of CommunicationTa */
  CommunicationTaHandle = osThreadNew(Communication, NULL, &CommunicationTa_attributes);

  /* creation of LoggingTask */
  LoggingTaskHandle = osThreadNew(Logging, NULL, &LoggingTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  RTM_Init();
  APP_Init();
  AS_Init();
  HR_PlatformOps_t recovery_platform={RecoverySafeState,RecoveryIsSafe,RecoverySystemReset,
      __HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST)!=RESET};
  __HAL_RCC_CLEAR_RESET_FLAGS();
  HR_Init(&recovery_platform);
  HR_TaskOps_t recovery_task={APP_Reinitialize,APP_EnterLimitedService};
  if(!HR_RegisterTask(TASK_COMMUNICATION,&recovery_task) ||
     !HR_RegisterTask(TASK_LOGGING,&recovery_task))Error_Handler();
  FI_Init();
  ER_Init();
  /* USER CODE END RTOS_THREADS */
  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 360;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Activate the Over-Drive mode
  */
  if (HAL_PWREx_EnableOverDrive() != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/* USER CODE BEGIN Header_RuntimeMonitor */
/**
  * @brief  Function implementing the RuntimeMonitorT thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_RuntimeMonitor */
void RuntimeMonitor(void *argument)
{
  /* USER CODE BEGIN 5 */
  (void)argument;
  TickType_t monitor_wake=xTaskGetTickCount();
  uint8_t summary_printed=0U;
  for(;;)
  {
    vTaskDelayUntil(&monitor_wake,pdMS_TO_TICKS(MONITOR_PERIOD_MS));
    if(!RTM_Update())continue;
    FI_Event_t event=FI_Update();
    AS_Update();
    HR_Update();
    if(!summary_printed)
    {
      ER_Update(&huart2,g_faultInjection.experiment_cycle);
      if(event==FI_EVENT_ACTIVATED)ER_OnFaultActivated(&huart2,g_faultInjection.experiment_cycle);
      else if(event==FI_EVENT_STOPPED)ER_OnFaultTerminated(&huart2,g_faultInjection.experiment_cycle);
      HR_PrintEvents(&huart2);
#if DETAILED_DIAGNOSTICS
      RTM_PrintFullTable(&huart2);
      HE_PrintTable(&huart2);
      FI_PrintStatus(&huart2);
      AS_PrintStatus(&huart2);
      HR_PrintStatus(&huart2);
#endif
    }
    else HR_PrintEvents(&huart2);
    HR_DispatchControlledReset();
    if(!summary_printed && g_faultInjection.experiment_cycle>=EXPERIMENT_STOP_CYCLE)
    {
      ER_PrintFinalSummary(&huart2,g_faultInjection.experiment_cycle);
      summary_printed=1U; /* Monitoring/recovery continue quietly after this report. */
    }
  }
  /* USER CODE END 5 */
}

/* USER CODE BEGIN Header_SafetyTask */
/**
* @brief Function implementing the Safety thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_SafetyTask */
void SafetyTask(void *argument)
{
  /* USER CODE BEGIN SafetyTask */
  (void)argument;
  TickType_t last_wake = xTaskGetTickCount();
  const TickType_t period_ticks = pdMS_TO_TICKS(SAFETY_PERIOD_MS);
  last_wake -= last_wake % period_ticks;  /* Shared periodic release grid. */
  for(;;)
  {
    vTaskDelayUntil(&last_wake, period_ticks);

    RTM_TaskStartAt(TASK_SAFETY, last_wake);
    uint32_t start_cycle = DWT->CYCCNT;

    /* Simulated safety-limit-check workload - NOT final logic */
    volatile float acc = 0.0f;
    for (int i = 0; i < 400; i++) { acc += (float)i * 0.001f; }

    uint32_t end_cycle = DWT->CYCCNT;
    RTM_TaskEnd(TASK_SAFETY, start_cycle, end_cycle);
    APP_JobCompleted(TASK_SAFETY);
  }
  /* USER CODE END SafetyTask */
}

/* USER CODE BEGIN Header_Charging */
/**
* @brief Function implementing the ChargingTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_Charging */
void Charging(void *argument)
{
  /* USER CODE BEGIN Charging */
  (void)argument;
  TickType_t last_wake = xTaskGetTickCount();
  const TickType_t period_ticks = pdMS_TO_TICKS(CHARGING_PERIOD_MS);
  last_wake -= last_wake % period_ticks;  /* Shared periodic release grid. */
  for(;;)
  {
    vTaskDelayUntil(&last_wake, period_ticks);

    RTM_TaskStartAt(TASK_CHARGING, last_wake);
    uint32_t start_cycle = DWT->CYCCNT;

    /* Simulated charging-control calculation - NOT final logic */
    volatile float acc = 0.0f;
    for (int i = 0; i < 600; i++) { acc += (float)i * 0.002f; }

    APP_UpdateSimulatedChargingOutput(1U);
    uint32_t end_cycle = DWT->CYCCNT;
    RTM_TaskEnd(TASK_CHARGING, start_cycle, end_cycle);
    APP_JobCompleted(TASK_CHARGING);
  }
  /* USER CODE END Charging */
}

/* USER CODE BEGIN Header_Sensor */
/**
* @brief Function implementing the SensorTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_Sensor */
void Sensor(void *argument)
{
  /* USER CODE BEGIN Sensor */
  (void)argument;
  TickType_t last_wake = xTaskGetTickCount();
  const TickType_t period_ticks = pdMS_TO_TICKS(SENSOR_PERIOD_MS);
  last_wake -= last_wake % period_ticks;  /* Shared periodic release grid. */
  for(;;)
  {
    vTaskDelayUntil(&last_wake, period_ticks);

    RTM_TaskStartAt(TASK_SENSOR, last_wake);
    uint32_t start_cycle = DWT->CYCCNT;

    /* Simulated sensor-data processing - NOT final logic */
    volatile float acc = 0.0f;
    for (int i = 0; i < 300; i++) { acc += (float)i * 0.0015f; }

    uint32_t end_cycle = DWT->CYCCNT;
    RTM_TaskEnd(TASK_SENSOR, start_cycle, end_cycle);
    APP_JobCompleted(TASK_SENSOR);
  }
  /* USER CODE END Sensor */
}

/* USER CODE BEGIN Header_Communication */
/**
* @brief Function implementing the CommunicationTa thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_Communication */
void Communication(void *argument)
{
  /* USER CODE BEGIN Communication */
  (void)argument;
  TickType_t last_wake = xTaskGetTickCount();
  const TickType_t period_ticks = pdMS_TO_TICKS(COMMUNICATION_PERIOD_MS);
  last_wake -= last_wake % period_ticks;  /* Shared periodic release grid. */
  for(;;)
  {
    vTaskDelayUntil(&last_wake, period_ticks);

    HR_TaskCheckpoint(TASK_COMMUNICATION);
    RTM_TaskStartAt(TASK_COMMUNICATION, last_wake);
    uint32_t start_cycle = DWT->CYCCNT;

    /* Simulated comms packet processing - NOT final logic */
    volatile float acc = 0.0f;
    int packet_work=APP_IsLimited(TASK_COMMUNICATION)?160:800;
    for (int i = 0; i < packet_work; i++) { acc += (float)i * 0.0025f; }
    FI_RunCpuWorkload(TASK_COMMUNICATION);
    uint32_t end_cycle = DWT->CYCCNT;
    RTM_TaskEnd(TASK_COMMUNICATION, start_cycle, end_cycle);
    APP_JobCompleted(TASK_COMMUNICATION);
  }
  /* USER CODE END Communication */
}

/* USER CODE BEGIN Header_Logging */
/**
* @brief Function implementing the LoggingTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_Logging */
void Logging(void *argument)
{
  /* USER CODE BEGIN Logging */
  (void)argument;
  TickType_t last_wake = xTaskGetTickCount();
  const TickType_t period_ticks = pdMS_TO_TICKS(LOGGING_PERIOD_MS);
  last_wake -= last_wake % period_ticks;  /* Shared periodic release grid. */
  for(;;)
  {
    vTaskDelayUntil(&last_wake, period_ticks);

    HR_TaskCheckpoint(TASK_LOGGING);
    RTM_TaskStartAt(TASK_LOGGING, last_wake);
    uint32_t start_cycle = DWT->CYCCNT;

    /* Drain up to the full queue capacity every LOGGING_PERIOD_MS (250 ms). */
    DiagMsg_t msg;
    for (uint32_t i = 0; i < DIAG_QUEUE_LENGTH; i++)
    {
        if (osMessageQueueGet(DiagQueueHandle, &msg, NULL, 0) != osOK) break;
        volatile uint32_t dummy = msg.task_id; (void)dummy;
    }

    FI_RunCpuWorkload(TASK_LOGGING);
    uint32_t end_cycle = DWT->CYCCNT;
    RTM_TaskEnd(TASK_LOGGING, start_cycle, end_cycle);
    APP_JobCompleted(TASK_LOGGING);
  }
  /* USER CODE END Logging */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */