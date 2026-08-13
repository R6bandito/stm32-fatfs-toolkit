#ifndef __SDIO_DRIVER_H__
#define __SDIO_DRIVER_H__

#include "stm32h7xx_hal.h"

/* ****************************** */
#define SDMMC_CLK_PORT          (GPIOC)
#define SDMMC_CLK_PIN           (GPIO_PIN_12)
#define SDMMC_CLK_CLK_EN()      do { __HAL_RCC_GPIOC_CLK_ENABLE(); } while(0)

#define SDMMC_D0_PORT           (GPIOC)
#define SDMMC_D0_PIN            (GPIO_PIN_8)
#define SDMMC_D0_CLK_EN()       do { __HAL_RCC_GPIOC_CLK_ENABLE(); } while(0)

#define SDMMC_D1_PORT           (GPIOC)
#define SDMMC_D1_PIN            (GPIO_PIN_9)
#define SDMMC_D1_CLK_EN()       do { __HAL_RCC_GPIOC_CLK_ENABLE(); } while(0)

#define SDMMC_D2_PORT           (GPIOC)
#define SDMMC_D2_PIN            (GPIO_PIN_10)
#define SDMMC_D2_CLK_EN()       do { __HAL_RCC_GPIOC_CLK_ENABLE(); } while(0)

#define SDMMC_D3_PORT           (GPIOC)
#define SDMMC_D3_PIN            (GPIO_PIN_11)
#define SDMMC_D3_CLK_EN()       do { __HAL_RCC_GPIOC_CLK_ENABLE(); } while(0)

#define SDMMC_CMD_PORT          (GPIOD)
#define SDMMC_CMD_PIN           (GPIO_PIN_2)
#define SDMMC_CMD_CLK_EN()      do { __HAL_RCC_GPIOD_CLK_ENABLE(); } while(0)
/* ****************************** */


/* ****************************** */
extern SD_HandleTypeDef hsd;

int8_t SD_Card_Init( void );
/* ****************************** */

#endif /* __SDIO_DRIVER_H__ */

