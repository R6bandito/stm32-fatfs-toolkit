/**
  ******************************************************************************
  * @file    sdio_driver.c
  * @brief   STM32H743ZIT6 SDMMC driver — polling/blocking mode (no DMA).
  *          FatFs diskio layer calls these functions synchronously.
  ******************************************************************************
  */

#include "sdio_driver.h"

/* ------------------------------------------------------------------ */
SD_HandleTypeDef hsd;
/* ------------------------------------------------------------------ */

static void HX_SDGPIO_Init(void);


/**
  * @brief  Initialize SD card via SDMMC.
  * @retval  0 on success, -1 on failure.
  */
int8_t 
SD_Card_Init(void)
{
    HX_SDGPIO_Init();
    __HAL_RCC_SDMMC1_CLK_ENABLE();

    HAL_SD_DeInit( &hsd );

    hsd.Instance                 = SDMMC1;
    hsd.Init.BusWide             = SDMMC_BUS_WIDE_4B;

    /* 240Mhz / (2 * 3) = 40Mhz. */
    hsd.Init.ClockDiv            = 3; 

    hsd.Init.ClockEdge           = SDMMC_CLOCK_EDGE_RISING;
    hsd.Init.ClockPowerSave      = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    hsd.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_ENABLE;

    if ( HAL_SD_Init( &hsd ) != HAL_OK )
        return -1;
        
    return 0;
}

/* ------------------------------------------------------------------ */
/* GPIO initialisation (alternate function AF12 — SDIO)                */
/* ------------------------------------------------------------------ */
static void 
HX_SDGPIO_Init(void)
{
    SDMMC_CLK_CLK_EN();
    SDMMC_D0_CLK_EN();
    SDMMC_D1_CLK_EN();
    SDMMC_D2_CLK_EN();
    SDMMC_D3_CLK_EN();
    SDMMC_CMD_CLK_EN();

    GPIO_InitTypeDef cfg = {0};
    cfg.Alternate = GPIO_AF12_SDMMC1;
    cfg.Mode      = GPIO_MODE_AF_PP;
    cfg.Pull      = GPIO_PULLUP;
    cfg.Speed     = GPIO_SPEED_FREQ_HIGH;

    cfg.Pin = SDMMC_CLK_PIN;
    HAL_GPIO_Init(SDMMC_CLK_PORT, &cfg);

    cfg.Pin = SDMMC_D0_PIN;
    HAL_GPIO_Init(SDMMC_D0_PORT, &cfg);

    cfg.Pin = SDMMC_D1_PIN;
    HAL_GPIO_Init(SDMMC_D1_PORT, &cfg);

    cfg.Pin = SDMMC_D2_PIN;
    HAL_GPIO_Init(SDMMC_D2_PORT, &cfg);

    cfg.Pin = SDMMC_D3_PIN;
    HAL_GPIO_Init(SDMMC_D3_PORT, &cfg);

    cfg.Pin = SDMMC_CMD_PIN;
    HAL_GPIO_Init(SDMMC_CMD_PORT, &cfg);
}
