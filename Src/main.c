/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by ST under BSD 3-Clause license,
  * the "License"; You may not use this file except in compliance with the
  * License. You may obtain a copy of the License at:
  * opensource.org/licenses/BSD-3-Clause
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "usart.h"
#include "gpio.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
volatile uint8_t rx_byte;
char nmea_buffer[120];
char main_buffer[120]; 
volatile uint8_t nmea_index = 0;
volatile uint8_t nmea_ready = 0;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */


const float voltage_table[] = {3.20, 3.50, 3.65, 3.70, 3.75, 3.80, 3.85, 3.90, 4.00, 4.10, 4.20};
const int percent_table[]   = {   0,    5,   10,   20,   30,   40,   50,   60,   80,   90,  100};
const int TABLE_SIZE = 11;

volatile float vdda_real = 0; // Bien xem dien ap nguon 3V3 thuc te
volatile float v_bat = 0;     // Bien xem dien ap pin thuc te
volatile int debug_battery_pct = 0;


static int display_percent = -1;

int get_accurate_battery(void) {
    // 1. Bi?n tinh (static) d? gom m?u qua nhi?u chu k? vòng l?p while(1)
    static uint32_t total_vrefint = 0;
    static uint32_t total_bat = 0;
    static int vref_count = 0; 
    static int bat_count = 0;  
    static uint32_t last_adc_time = 0; // Bi?n dánh d?u th?i gian (thay cho HAL_Delay)
    
    // 2. Ch? d? Không-Ch?n (Non-Blocking): Ch? do khi th?i gian trôi qua d? 5ms
    if (HAL_GetTick() - last_adc_time >= 5) {
        last_adc_time = HAL_GetTick(); // Reset b? d?m 5ms

        ADC_ChannelConfTypeDef sConfig = {0};
        sConfig.Rank = ADC_REGULAR_RANK_1;
        sConfig.SamplingTime = ADC_SAMPLETIME_239CYCLES_5; 

        // Ðo Kênh 17 (VREFINT) - Ch? 1 m?u
        sConfig.Channel = ADC_CHANNEL_17; 
        HAL_ADC_ConfigChannel(&hadc1, &sConfig);
        HAL_ADC_Start(&hadc1);
        if (HAL_ADC_PollForConversion(&hadc1, 10) == HAL_OK) {
            total_vrefint += HAL_ADC_GetValue(&hadc1);
            vref_count++; 
        }
        HAL_ADC_Stop(&hadc1);

        // Ðo Kênh 4 (PA4) - Ch? 1 m?u
        sConfig.Channel = ADC_CHANNEL_4;
        HAL_ADC_ConfigChannel(&hadc1, &sConfig);
        HAL_ADC_Start(&hadc1);
        if (HAL_ADC_PollForConversion(&hadc1, 10) == HAL_OK) {
            total_bat += HAL_ADC_GetValue(&hadc1);
            bat_count++; 
        }
        HAL_ADC_Stop(&hadc1);
    }

    // 3. N?u CHUA gom d? 20 m?u (vref_count < 20): L?p t?c thoát hàm, tr? v? % cu.
    // Ði?u này giúp hàm này ch?y qua nhu m?t con gió (chua t?i 1ms), không ch?n UART.
    if (vref_count < 20 || bat_count < 20) {
        return (display_percent == -1) ? 0 : display_percent;
    }

    // 4. N?u ÐÃ GOM Ð? 20 m?u: Ti?n hành tính toán n?ng
    float avg_vrefint = (float)total_vrefint / vref_count;
    float avg_bat = (float)total_bat / bat_count;

    vdda_real = 1.2f * 4095.0f / avg_vrefint;
    float v_pin_pa4 = (avg_bat * vdda_real) / 4095.0f;
    float v_bat_raw = (v_pin_pa4 * 2.0f) + 0.11f;

    static float filtered_vbat = 0;
    static int init_counter = 0;
    
    if (init_counter < 10) {
        filtered_vbat += v_bat_raw / 10.0f;
        init_counter++;
        v_bat = v_bat_raw; 
    } else {
        filtered_vbat = (filtered_vbat * 0.90f) + (v_bat_raw * 0.10f);
        v_bat = filtered_vbat; 
    }

    int calculated_percent = 0;
    if (v_bat >= 4.20f) {
        calculated_percent = 100;
    } else if (v_bat <= 3.20f) {
        calculated_percent = 0;
    } else {
        for (int i = 0; i < TABLE_SIZE - 1; i++) {
            if (v_bat >= voltage_table[i] && v_bat <= voltage_table[i+1]) {
                float fraction = (v_bat - voltage_table[i]) / (voltage_table[i+1] - voltage_table[i]);
                calculated_percent = percent_table[i] + (int)(fraction * (percent_table[i+1] - percent_table[i]));
                break;
            }
        }
    }

    if (display_percent == -1) {
        display_percent = calculated_percent; 
    } else {
        uint8_t is_charging = (v_bat > 4.15f);
        if (is_charging) {
            if (calculated_percent > display_percent) {
                display_percent = calculated_percent;
            }
        } else {
            if (calculated_percent < display_percent) {
                display_percent = calculated_percent;
            }
        }
    }

    // 5. R?T QUAN TR?NG: Reset các bi?n c?ng d?n d? vòng l?p sau gom 20 m?u m?i!
    total_vrefint = 0;
    total_bat = 0;
    vref_count = 0;
    bat_count = 0;

    return display_percent;
}
// ==========================================


void get_field(char* nmea, int field_num, char* output, int max_len) {
    int current_field = 0;
    int i = 0, j = 0;
    output[0] = '\0'; 

    while (nmea[i] != '\0' && nmea[i] != '\r' && nmea[i] != '\n') {
        if (nmea[i] == ',') {
            current_field++;
            i++;
            continue;
        }
        if (current_field == field_num) {
            // Ch? copy n?u còn kho?ng tr?ng (ch?a 1 slot cho \0)
            if (j < max_len - 1) { 
                output[j++] = nmea[i];
            }
        } else if (current_field > field_num) {
            break; 
        }
        i++;
    }
    output[j] = '\0'; 
}
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
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
	
	

  /* USER CODE END 2 */
	MX_ADC1_Init(); 
  HAL_ADCEx_Calibration_Start(&hadc1);
  HAL_Delay(50);
	
	HAL_UART_Receive_IT(&huart1, (uint8_t *)&rx_byte, 1);
  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
	static char time_vn[10] = "--:--";
  static char date_vn[15] = "--/--/----";
  static float lat_final = 0.0f, lon_final = 0.0f, speed_kmh = 0.0f;
  static uint8_t has_fix = 0;
  static uint32_t last_send_time = 0;
	
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
        
        // 1. cap nhat phan tram pin
        debug_battery_pct = get_accurate_battery();

        // 2. xu ly du lieu gps (cho chay khi co dau hieu tu moi module)
        if (nmea_ready) {
            // ?? D?n d?p ký t? th?a (Ðua l?i gi?ng b?n cu dã t?ng ch?y t?t)
            for(int i=0; i<strlen(main_buffer); i++){
                if(main_buffer[i] == '\r') main_buffer[i] = '\0';
            }

            char status[5] = "";
            char time_str[15] = "";
            char date_str[15] = ""; 
            
            // L?y d? li?u t? két s?t (main_buffer)
            get_field(main_buffer, 2, status, sizeof(status)); 
            get_field(main_buffer, 1, time_str, sizeof(time_str)); 
            get_field(main_buffer, 9, date_str, sizeof(date_str));

            // Luon xu ly gio va ngay (B?t k? dã có t?a d? hay chua)
            if (strlen(time_str) >= 4) {
                int hours = (time_str[0] - '0') * 10 + (time_str[1] - '0');
                int mins  = (time_str[2] - '0') * 10 + (time_str[3] - '0');
                hours += 7; // mui gio Viet Nam

                if (strlen(date_str) == 6) {
                    int day   = (date_str[0] - '0') * 10 + (date_str[1] - '0');
                    int month = (date_str[2] - '0') * 10 + (date_str[3] - '0');
                    int year  = (date_str[4] - '0') * 10 + (date_str[5] - '0');

                    if (hours >= 24) {
                        hours -= 24; day++; 
                        int days_in_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
                        if (year % 4 == 0) days_in_month[2] = 29; 
                        if (day > days_in_month[month]) {
                            day = 1; month++;
                            if (month > 12) { month = 1; year++; }
                        }
                    }
                    sprintf(date_vn, "%02d/%02d/20%02d", day, month, year);
                } else {
                    if (hours >= 24) hours -= 24;
                }
                sprintf(time_vn, "%02d:%02d", hours, mins);
            }

            // xu ly toa do va van toc
            if (status[0] == 'A') { 
                has_fix = 1; 
                char lat_str[20] = "", lat_dir[5] = "", lon_str[20] = "", lon_dir[5] = "", speed_str[15] = "";
                
                get_field(main_buffer, 3, lat_str, sizeof(lat_str)); 
                get_field(main_buffer, 4, lat_dir, sizeof(lat_dir)); 
                get_field(main_buffer, 5, lon_str, sizeof(lon_str)); 
                get_field(main_buffer, 6, lon_dir, sizeof(lon_dir)); 
                get_field(main_buffer, 7, speed_str, sizeof(speed_str)); 

                if (strlen(lat_str) > 0 && strlen(lon_str) > 0) {
                    float lat_raw = atof(lat_str);
                    int lat_deg = (int)(lat_raw / 100);
                    lat_final = lat_deg + (lat_raw - lat_deg * 100) / 60.0f;
                    if (lat_dir[0] == 'S') lat_final = -lat_final;

                    float lon_raw = atof(lon_str);
                    int lon_deg = (int)(lon_raw / 100);
                    lon_final = lon_deg + (lon_raw - lon_deg * 100) / 60.0f;
                    if (lon_dir[0] == 'W') lon_final = -lon_final;

                    speed_kmh = atof(speed_str) * 1.852f; 
                }
            } else {
                has_fix = 0; // Ðánh d?u m?t v? tinh
            }
            
            nmea_ready = 0; // Xóa c?
        }

        // ==========================================
        // 3. G?I D? LI?U Ð?NH K? (M?I 1 GIÂY) SANG ESP32
        // Ch?y d?c l?p, không quan tâm GPS có s?ng hay không
        // ==========================================
        if (HAL_GetTick() - last_send_time >= 1000) {
            last_send_time = HAL_GetTick(); // Reset b? d?m th?i gian
            char tx_buffer[120];
            
            if (has_fix) {
                sprintf(tx_buffer, "$$GPS|%.5f,%.5f|%.0f|%s|%s|%d$$\n", lat_final, lon_final, speed_kmh, time_vn, date_vn, debug_battery_pct);
            } else {
                sprintf(tx_buffer, "$$GPS|NO_FIX|0|%s|%s|%d$$\n", time_vn, date_vn, debug_battery_pct);
            }
            
            HAL_UART_Transmit(&huart2, (uint8_t*)tx_buffer, strlen(tx_buffer), 100);
        }
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
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV2;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
// ham nay tu dong kich hoat moi khi uart1 nhan duoc 1 byte du lieu tu gps
// Hàm ng?t nh?n ký t? UART
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == USART1) {
        if (rx_byte == '\n') {
            nmea_buffer[nmea_index] = '\0'; 
            
            // ?? B?N VÁ 1: TÌM ÐÚNG V? TRÍ D?U $ Ð? V?T B? RÁC ? Ð?U CHU?I
            char *ptr = strstr((char*)nmea_buffer, "$GPRMC");
            if (!ptr) ptr = strstr((char*)nmea_buffer, "$GNRMC");
            if (!ptr) ptr = strstr((char*)nmea_buffer, "$GLRMC");
            if (!ptr) ptr = strstr((char*)nmea_buffer, "$BDRMC");

            if (ptr != NULL) {
                strcpy(main_buffer, ptr); // Ch? copy t? d?u $ tr? v? sau
                nmea_ready = 1;                 
            }
            nmea_index = 0;                 
        } 
        // ?? B?N VÁ 2: B? qua ký t? \r và các byte r?ng do nhi?u di?n (rx_byte != '\0')
        else if (rx_byte != '\r' && rx_byte != '\0') {       
            if (nmea_index < 119) {
                nmea_buffer[nmea_index++] = rx_byte;
            }
        }
        
        // C? g?ng m? l?i ng?t
        if (HAL_UART_Receive_IT(&huart1, (uint8_t *)&rx_byte, 1) != HAL_OK) {
            huart1.RxState = HAL_UART_STATE_READY;
            huart1.Lock = HAL_UNLOCKED;
            HAL_UART_Receive_IT(&huart1, (uint8_t *)&rx_byte, 1);
        }
    }
}

// Hàm x? lý l?i UART (Overrun, Noise, Framing)
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == USART1) {
        // ?? B?N VÁ 3: Dùng l?i Macro chu?n c?a ST d? d?n d?p c? l?i
        __HAL_UART_CLEAR_OREFLAG(huart); 
        __HAL_UART_CLEAR_NEFLAG(huart);  
        __HAL_UART_CLEAR_FEFLAG(huart);  
        
        // M? khóa ph?n m?m cu?ng b?c
        huart->RxState = HAL_UART_STATE_READY;
        huart->Lock = HAL_UNLOCKED;
        huart->ErrorCode = HAL_UART_ERROR_NONE;
        
        // Kh?i d?ng l?i lu?ng nh?n d? li?u
        HAL_UART_Receive_IT(&huart1, (uint8_t *)&rx_byte, 1);
    }
}
/* USER CODE END 4 */

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

#ifdef  USE_FULL_ASSERT
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

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
