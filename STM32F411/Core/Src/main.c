/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Smart Medicine Dispenser - STM32 Main
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"

/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <clcd.h>
#include <dht.h>
#include <servo.h>
/* USER CODE END Includes */

/* USER CODE BEGIN PD */
#ifdef __GNUC__
#define PUTCHAR_PROTOTYPE int __io_putchar(int ch)
#else
#define PUTCHAR_PROTOTYPE int fputc(int ch, FILE *f)
#endif

#define ARR_CNT       6
#define CMD_SIZE      100
#define TEMP_FAN      25    // 팬 작동 온도 기준 (°C)
#define TEMP_WARNING  30    // 경고 온도 기준 (°C)
/* USER CODE END PD */

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c3;
TIM_HandleTypeDef htim2;
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart6;

/* USER CODE BEGIN PV */
uint8_t rx2char;
volatile unsigned char rx2Flag = 0;
volatile char rx2Data[50];

volatile unsigned char btFlag = 0;
uint8_t btchar;
char btData[100];

char currentUserID[20] = {0};
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_USART6_UART_Init(void);
static void MX_I2C3_Init(void);
static void MX_TIM2_Init(void);

/* USER CODE BEGIN PFP */
void bluetooth_Event(void);
void request_medicine(char *userID);
/* USER CODE END PFP */

int main(void)
{
    HAL_Init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_USART2_UART_Init();
    MX_USART6_UART_Init();
    MX_I2C3_Init();
    MX_TIM2_Init();

    /* USER CODE BEGIN 2 */
    HAL_UART_Receive_IT(&huart2, &rx2char, 1);
    HAL_UART_Receive_IT(&huart6, &btchar,  1);

    DHT11_Init();
    LCD_init(&hi2c3);

    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);  // SERVO1
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_2);  // SERVO2
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3);  // SERVO3

    // 팬 초기 OFF
    HAL_GPIO_WritePin(GPIOC, FAN_PIN_Pin, GPIO_PIN_RESET);

    DHT11_TypeDef dht11Data;
    char buff[30];

    LCD_writeStringXY(0, 0, "SmartDispenser ");
    LCD_writeStringXY(1, 0, "Ready...       ");
    printf("start main()\r\n");
    /* USER CODE END 2 */

    /* USER CODE BEGIN WHILE */
    while (1)
    {
        /* ── USART2: UserID 입력 (테스트용 시리얼) ── */
        if (rx2Flag)
        {
            printf("recv2 : %s\r\n", rx2Data);
            strcpy(currentUserID, (char*)rx2Data);
            request_medicine(currentUserID);
            rx2Flag = 0;
        }

        /* ── USART6: 블루투스 수신 처리 ── */
        if (btFlag)
        {
            btFlag = 0;
            bluetooth_Event();
        }

        /* ── 환경 모니터링: DHT11 읽기 ── */
        dht11Data = DHT11_readData();
        int temp = dht11Data.temp_byte1;
        int humi = dht11Data.rh_byte1;

        /* ── 온도 기반 팬 제어 ── */
        if (temp >= TEMP_WARNING)
        {
            // 경고 온도 이상 → 팬 ON + LCD 경고
            HAL_GPIO_WritePin(GPIOC, FAN_PIN_Pin, GPIO_PIN_SET);
            LCD_writeStringXY(0, 0, "!!TEMP ALERT!!! ");
            printf("경고: 온도 %d도 초과!\r\n", temp);
        }
        else if (temp >= TEMP_FAN)
        {
            // 기준 온도 이상 → 팬 ON
            HAL_GPIO_WritePin(GPIOC, FAN_PIN_Pin, GPIO_PIN_SET);
            LCD_writeStringXY(0, 0, "Fan ON          ");
        }
        else
        {
            // 정상 온도 → 팬 OFF
            HAL_GPIO_WritePin(GPIOC, FAN_PIN_Pin, GPIO_PIN_RESET);
            LCD_writeStringXY(0, 0, "SmartDispenser  ");
        }

        // LCD 2행: 온습도 표시
        sprintf(buff, "h:%d%% t:%d'C    ", humi, temp);
        LCD_writeStringXY(1, 0, buff);

        /* ── 환경 데이터: 5초마다 라즈베리파이 전송 ── */
        static uint32_t lastEnvTick = 0;
        if (HAL_GetTick() - lastEnvTick >= 10000)
        {
            char envBuf[CMD_SIZE];
            sprintf(envBuf, "[ENV@TEMP:%d.%d@HUM:%d]\n",
                    dht11Data.temp_byte1,
                    dht11Data.temp_byte2,
                    dht11Data.rh_byte1);
            HAL_UART_Transmit(&huart6, (uint8_t*)envBuf,
                              strlen(envBuf), 0xFFFF);
            lastEnvTick = HAL_GetTick();
        }

        HAL_Delay(500);
        /* USER CODE END WHILE */

        /* USER CODE BEGIN 3 */
    }
    /* USER CODE END 3 */
}

/* ============================================================
 * request_medicine()
 * UserID 입력 시 DOC에 PICKUP 요청
 * ============================================================ */
void request_medicine(char *userID)
{
    char reqBuf[CMD_SIZE] = {0};
    sprintf(reqBuf, "[DOC]PICKUP@%s\n", userID);
    HAL_UART_Transmit(&huart6, (uint8_t*)reqBuf,
                      strlen(reqBuf), 0xFFFF);
    printf("PICKUP 요청: %s\r\n", reqBuf);
    LCD_writeStringXY(0, 0, "Requesting...  ");
}

/* ============================================================
 * bluetooth_Event()
 * ============================================================ */
void bluetooth_Event(void)
{
    int i = 0;
    char *pToken;
    char *pArray[ARR_CNT] = {0};
    char recvBuf[CMD_SIZE] = {0};

    strncpy(recvBuf, btData, CMD_SIZE - 1);
    printf("btData : %s\r\n", btData);

    pToken = strtok(recvBuf, "[@]");
    while (pToken != NULL) {
        pArray[i] = pToken;
        if (++i >= ARR_CNT) break;
        pToken = strtok(NULL, "[@]");
    }

    if (i < 2 || pArray[1] == NULL) return;

    /* ──────────────────────────────────────────────
     * [1] DOC → STM32 : PICKUP 응답
     * 성공: [STM]PICKUP@Tylenol@2@1
     *   pArray[2] = 약이름
     *   pArray[3] = 개수
     *   pArray[4] = ServoChannel (1=CH1, 2=CH2, 3=CH3)
     * 실패: [STM]PICKUP@FAIL_XXX@0
     * ────────────────────────────────────────────── */
    if (!strcmp(pArray[1], "PICKUP"))
    {
        if (pArray[2] == NULL) return;

        /* ── 실패 처리 ── */
        if (!strncmp(pArray[2], "FAIL", 4))
        {
            printf("PICKUP 실패: %s\r\n", pArray[2]);

            if      (!strcmp(pArray[2], "FAIL_EMPTY"))
                LCD_writeStringXY(0, 0, "No Prescription");
            else if (!strcmp(pArray[2], "FAIL_DB"))
                LCD_writeStringXY(0, 0, "DB Error       ");
            else if (!strcmp(pArray[2], "FAIL_STOCK"))
                LCD_writeStringXY(0, 0, "Out of Stock!  ");
            else if (!strcmp(pArray[2], "FAIL_24H_LIMIT"))
                LCD_writeStringXY(0, 0, "24H Limit!     ");
            else if (!strcmp(pArray[2], "FAIL_TOTAL_LIMIT"))
                LCD_writeStringXY(0, 0, "Total Limit!   ");
            else if (!strcmp(pArray[2], "FAIL_OVERDOSE"))
                LCD_writeStringXY(0, 0, "Overdose Risk! ");
            else if (!strcmp(pArray[2], "FAIL_AGE_LIMIT"))
                LCD_writeStringXY(0, 0, "Age Restricted!");
            else if (!strcmp(pArray[2], "FAIL_ALLERGY"))
                LCD_writeStringXY(0, 0, "Allergy Alert! ");
            else
                LCD_writeStringXY(0, 0, "Error!         ");

            memset(currentUserID, 0, sizeof(currentUserID));
            return;
        }

        /* ── 성공: 배출 승인 ── */
        char    *medName      = pArray[2];
        uint8_t  count        = (pArray[3] != NULL) ? atoi(pArray[3]) : 0;
        uint8_t  servo_ch_num = (pArray[4] != NULL) ? atoi(pArray[4]) : 1;

        if (count == 0) {
            LCD_writeStringXY(0, 0, "Count Error!   ");
            memset(currentUserID, 0, sizeof(currentUserID));
            return;
        }

        /* DB에서 받은 ServoChannel 사용 */
        uint32_t channel;
        switch (servo_ch_num) {
            case 1:  channel = TIM_CHANNEL_1; break;
            case 2:  channel = TIM_CHANNEL_2; break;
            case 3:  channel = TIM_CHANNEL_3; break;
            default: channel = TIM_CHANNEL_1; break;
        }

        printf("배출 승인: %s %d알 ServoChannel:%d\r\n",
               medName, count, servo_ch_num);
        LCD_writeStringXY(0, 0, "Dispensing...  ");

        /* 서보 작동 */
        dispense_n(channel, count);

        /* 배출 완료 → DOC에 알림 */
        char doneBuf[CMD_SIZE] = {0};
        sprintf(doneBuf, "[DOC]PICKUP_DONE@%s@%s@%d\n",
                currentUserID, medName, count);
        HAL_UART_Transmit(&huart6, (uint8_t*)doneBuf,
                          strlen(doneBuf), 0xFFFF);

        LCD_writeStringXY(0, 0, "Done!          ");
        printf("배출 완료: %s\r\n", doneBuf);

        memset(currentUserID, 0, sizeof(currentUserID));
    }

    /* ──────────────────────────────────────────────
     * [2] DOC → STM32 : PRESCRIPTION 알림 (예약 완료)
     * ────────────────────────────────────────────── */
    else if (!strcmp(pArray[1], "PRESCRIPTION"))
    {
        if (pArray[2] == NULL) return;

        if (!strncmp(pArray[2], "FAIL", 4)) {
            printf("PRESCRIPTION 실패: %s\r\n", pArray[2]);

            if      (!strcmp(pArray[2], "FAIL_SYMPTOM"))
                LCD_writeStringXY(0, 0, "Unknown Symptom");
            else if (!strcmp(pArray[2], "FAIL_ALLERGY"))
                LCD_writeStringXY(0, 0, "Allergy Alert! ");
            else if (!strcmp(pArray[2], "FAIL_24H_LIMIT"))
                LCD_writeStringXY(0, 0, "24H Limit!     ");
            else if (!strcmp(pArray[2], "FAIL_TOTAL_LIMIT"))
                LCD_writeStringXY(0, 0, "Total Limit!   ");
            else if (!strcmp(pArray[2], "FAIL_OVERDOSE"))
                LCD_writeStringXY(0, 0, "Overdose Risk! ");
            else if (!strcmp(pArray[2], "FAIL_AGE_LIMIT"))
                LCD_writeStringXY(0, 0, "Age Restricted!");
            else if (!strcmp(pArray[2], "FAIL_ALREADY_RESERVED"))
                LCD_writeStringXY(0, 0, "Already Reservd");
            else
                LCD_writeStringXY(0, 0, "Prescrip. Fail ");
        }
        else {
            printf("처방 예약 완료: %s %s알\r\n",
                   pArray[2], pArray[3] ? pArray[3] : "?");
            LCD_writeStringXY(0, 0, "Reserved!      ");
        }
    }

    /* ──────────────────────────────────────────────
     * [3] 테스트용 직접 명령
     * ────────────────────────────────────────────── */
    else if (!strcmp(pArray[1], "DISPENSE"))
    {
        if (pArray[2] == NULL || pArray[3] == NULL || pArray[4] == NULL)
            return;

        uint8_t cnt1 = atoi(strchr(pArray[2], ':') + 1);
        uint8_t cnt2 = atoi(strchr(pArray[3], ':') + 1);
        uint8_t cnt3 = atoi(strchr(pArray[4], ':') + 1);

        dispense_n(TIM_CHANNEL_1, cnt1);
        dispense_n(TIM_CHANNEL_2, cnt2);
        dispense_n(TIM_CHANNEL_3, cnt3);

        char doneBuf[CMD_SIZE] = {0};
        sprintf(doneBuf, "[DONE@%s@%s@%s]\n",
                pArray[2], pArray[3], pArray[4]);
        HAL_UART_Transmit(&huart6, (uint8_t*)doneBuf,
                          strlen(doneBuf), 0xFFFF);
    }
}

/* ============================================================
 * HAL_UART_RxCpltCallback
 * ============================================================ */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        static int i = 0;
        rx2Data[i] = rx2char;
        if ((rx2Data[i] == '\r') || (rx2Data[i] == '\n'))
        {
            rx2Data[i] = '\0';
            rx2Flag = 1;
            i = 0;
        }
        else
        {
            if (i < 48) i++;
        }
        HAL_UART_Receive_IT(&huart2, &rx2char, 1);
    }

    if (huart->Instance == USART6)
    {
        static int i = 0;
        btData[i] = btchar;
        if ((btData[i] == '\n') || (btData[i] == '\r'))
        {
            btData[i] = '\0';
            btFlag = 1;
            i = 0;
        }
        else
        {
            if (i < 98) i++;
        }
        HAL_UART_Receive_IT(&huart6, &btchar, 1);
    }
}

/* ============================================================
 * printf → USART2 리다이렉트
 * ============================================================ */
PUTCHAR_PROTOTYPE
{
    HAL_UART_Transmit(&huart2, (uint8_t*)&ch, 1, 0xFFFF);
    return ch;
}

/* ============================================================
 * 주변장치 초기화 (CubeMX 생성)
 * ============================================================ */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState       = RCC_HSE_BYPASS;
    RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM = 8;
    RCC_OscInitStruct.PLL.PLLN = 336;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
    RCC_OscInitStruct.PLL.PLLQ = 4;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();

    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) Error_Handler();
}

static void MX_I2C3_Init(void)
{
    hi2c3.Instance              = I2C3;
    hi2c3.Init.ClockSpeed       = 10000;
    hi2c3.Init.DutyCycle        = I2C_DUTYCYCLE_2;
    hi2c3.Init.OwnAddress1      = 0;
    hi2c3.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
    hi2c3.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
    hi2c3.Init.OwnAddress2      = 0;
    hi2c3.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
    hi2c3.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c3) != HAL_OK) Error_Handler();
}

static void MX_TIM2_Init(void)
{
    TIM_ClockConfigTypeDef  sClockSourceConfig = {0};
    TIM_MasterConfigTypeDef sMasterConfig      = {0};
    TIM_OC_InitTypeDef      sConfigOC          = {0};

    htim2.Instance               = TIM2;
    htim2.Init.Prescaler         = 83;
    htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim2.Init.Period            = 19999;
    htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(&htim2) != HAL_OK) Error_Handler();

    sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK) Error_Handler();
    if (HAL_TIM_PWM_Init(&htim2) != HAL_OK) Error_Handler();

    sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
    sMasterConfig.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK) Error_Handler();

    sConfigOC.OCMode     = TIM_OCMODE_PWM1;
    sConfigOC.Pulse      = 1500;
    sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1) != HAL_OK) Error_Handler();
    if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_2) != HAL_OK) Error_Handler();
    if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_3) != HAL_OK) Error_Handler();

    HAL_TIM_MspPostInit(&htim2);
}

static void MX_USART2_UART_Init(void)
{
    huart2.Instance          = USART2;
    huart2.Init.BaudRate     = 115200;
    huart2.Init.WordLength   = UART_WORDLENGTH_8B;
    huart2.Init.StopBits     = UART_STOPBITS_1;
    huart2.Init.Parity       = UART_PARITY_NONE;
    huart2.Init.Mode         = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart2) != HAL_OK) Error_Handler();
}

static void MX_USART6_UART_Init(void)
{
    huart6.Instance          = USART6;
    huart6.Init.BaudRate     = 9600;
    huart6.Init.WordLength   = UART_WORDLENGTH_8B;
    huart6.Init.StopBits     = UART_STOPBITS_1;
    huart6.Init.Parity       = UART_PARITY_NONE;
    huart6.Init.Mode         = UART_MODE_TX_RX;
    huart6.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart6.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart6) != HAL_OK) Error_Handler();
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* FAN_PIN, DHT11 초기 LOW */
    HAL_GPIO_WritePin(GPIOC, FAN_PIN_Pin | DHT11_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

    /* B1 버튼 */
    GPIO_InitStruct.Pin  = B1_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

    /* FAN_PIN + DHT11 (GPIOC) */
    GPIO_InitStruct.Pin   = FAN_PIN_Pin | DHT11_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /* LD2 LED */
    GPIO_InitStruct.Pin   = LD2_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) {}
}
