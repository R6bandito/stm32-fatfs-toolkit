/*-----------------------------------------------------------------------*/
/* Low level disk I/O module for FatFs  (C)ChaN, 2025                    */
/*-----------------------------------------------------------------------*/
/* Adapted for STM32H743ZIT6 + SDMMC (polling/blocking mode).                 */
/* Uses vTaskDelay to yield CPU during SD-card busy periods.             */
/*-----------------------------------------------------------------------*/

#include "ff.h"
#include "diskio.h"
#include "sdio_driver.h"
#include <string.h>

#define USE_OS                	(0)
	#if (USE_OS)
		#define   FREE_RTOS     (1)
		#define   RT_THREAD		(0)
	#endif /* USE_OS */


#if (USE_OS)
	#if (FREE_RTOS)
		#include "FreeRTOS.h"
		#include "task.h"

		#define Delay( x )		vTaskDelay(pdMS_TO_TICKS(x))
	#endif /* FREE_RTOS */
#else
	#define Delay( x ) 	HAL_Delay( x )
#endif /* USE_OS */

#define DEV_MMC         0

static uint8_t gs_alignedBuf[512] __attribute__((aligned(4)));
const uint32_t blockTiemouts = 0xFFFFFFFFul;


/**
 * @brief   Wait for the SD card to become ready (transfer state).
 * @note    Polls the card status with a timeout. If the card is busy, the
 *          calling task yields via vTaskDelay(1) to allow other tasks to run.
 * @retval  0 if ready, -1 if timeout or error occurred.
 */
static int __sd_wait_ready ( void )
{
    uint32_t timeout = 500;
    HAL_SD_CardStateTypeDef state;

    do 
    {
        /* Poll SD card status until the underlying low-level operation is completed. */
        state = HAL_SD_GetCardState( &hsd );

        if ( state == HAL_SD_CARD_TRANSFER ) 
        {
            /* Previous SD card operation completed. */
            return 0;
        }

        if ( state == HAL_SD_CARD_ERROR )
        {
            /* Previous SD card operation error. */
            return -1;
        }

        Delay(1);

    } while (--timeout);

    return -1;
}


/**
 * @brief   Get the status of the physical drive.
 * @param   pdrv: Physical drive number (must be DEV_MMC).
 * @retval  0 if status is normal, STA_NOINIT if drive is invalid.
 */
DSTATUS disk_status ( BYTE pdrv )
{
    if ( pdrv == DEV_MMC ) 
    {
        /* SD card status return normal status directly. */
        return 0;
    }

    return STA_NOINIT;
}


/**
 * @brief   Initialize the physical drive (SD card).
 * @note    The initialization is performed only once (one-shot). Subsequent
 *          calls are ignored and return success.
 * @param   pdrv: Physical drive number (must be DEV_MMC).
 * @retval  0 on success, STA_NOINIT on failure.
 */
DSTATUS disk_initialize ( BYTE pdrv )
{
    if ( pdrv != DEV_MMC ) 
        return STA_NOINIT;

    /* Initialize the SDIO hardware driver. */
    if ( SD_Card_Init() < 0 ) 
        return STA_NOINIT;

    return 0;
}


/**
 * @brief   Read sectors from the SD card.
 * @param   pdrv:   Physical drive number (must be DEV_MMC).
 * @param   buff:   Pointer to the data buffer (may be unaligned).
 * @param   sector: Starting sector number (LBA).
 * @param   count:  Number of sectors to read.
 * @note    If the buffer is not 4-byte aligned, a temporary aligned buffer
 *          is used internally before copying to the user buffer.
 * @retval  RES_OK on success, RES_ERROR on read failure, RES_PARERR if pdrv invalid.
 */
DRESULT disk_read ( BYTE pdrv, BYTE *buff, LBA_t sector, UINT count )
{
    if ( pdrv != DEV_MMC ) 
        return RES_PARERR;

    if ( __sd_wait_ready() != 0 ) 
        return RES_ERROR;

    if ( ((uint32_t)buff & 0x03) == 0 ) 
    {
        /* Buffer is WORD-aligned. Read directly. */
        if ( HAL_SD_ReadBlocks(&hsd, buff, sector, count, blockTiemouts) == HAL_OK )
            return RES_OK;
        return RES_ERROR;
    }

    for( UINT i = 0; i < count; i++ ) 
    {
        /* Buffer is not WORD-aligned. Copy to gs_alignedBuf first. */
        if ( HAL_SD_ReadBlocks(&hsd, gs_alignedBuf, sector + i, 1, blockTiemouts) != HAL_OK )
            return RES_ERROR;
        memcpy(buff + i * 512, gs_alignedBuf, 512);
    }

    return RES_OK;
}


#if FF_FS_READONLY == 0

/**
 * @brief   Write sectors to the SD card.
 * @param   pdrv:   Physical drive number (must be DEV_MMC).
 * @param   buff:   Pointer to the data buffer (may be unaligned).
 * @param   sector: Starting sector number (LBA).
 * @param   count:  Number of sectors to write.
 * @note    If the buffer is not 4-byte aligned, data is first copied to an
 *          internal aligned buffer before the actual write.
 * @retval  RES_OK on success, RES_ERROR on write failure, RES_PARERR if pdrv invalid.
 */
DRESULT disk_write ( BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count )
{
    if ( pdrv != DEV_MMC ) 
        return RES_PARERR;

    if ( __sd_wait_ready() != 0 ) 
        return RES_ERROR;

    if ( ((uint32_t)buff & 0x03) == 0 ) 
    {
        /* Buffer is WORD-aligned. Write directly. */
        if ( HAL_SD_WriteBlocks(&hsd, (uint8_t *)buff, sector, count, blockTiemouts) == HAL_OK ) 
        {
            (void)__sd_wait_ready();
            return RES_OK;
        }

        return RES_ERROR;
    }

    for( UINT i = 0; i < count; i++ ) 
    {
        /* Buffer is not WORD-aligned. Copy to gs_alignedBuf before writing. */
        memcpy(gs_alignedBuf, buff + i * 512, 512);
        if ( HAL_SD_WriteBlocks(&hsd, gs_alignedBuf, sector + i, 1, blockTiemouts) != HAL_OK )
            return RES_ERROR;
    }

    (void)__sd_wait_ready();

    return RES_OK;
}

#endif


/**
 * @brief   Control the physical drive (device-specific operations).
 * @param   pdrv: Physical drive number (must be DEV_MMC).
 * @param   cmd:  Control command (only CTRL_SYNC is supported).
 * @param   buff: Pointer to the command-specific buffer (unused).
 * @retval  RES_OK on success, RES_ERROR if sync fails,
 *          RES_PARERR if command or drive is invalid.
 */
DRESULT disk_ioctl ( BYTE pdrv, BYTE cmd, void *buff )
{
    if ( pdrv != DEV_MMC ) 
        return RES_PARERR;

    switch (cmd) 
    {
        case CTRL_SYNC:
        {
            if (__sd_wait_ready() != 0) 
                return RES_ERROR;

            return RES_OK;
        }
    }

    return RES_PARERR;
}
