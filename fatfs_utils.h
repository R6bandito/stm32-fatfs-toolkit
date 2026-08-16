#ifndef __FATFS_UTILS_H__
#define __FATFS_UTILS_H__


#include "ff.h"
#include <stdint.h>
#include <stdbool.h>
#include "stm32h7xx_hal.h"

/* ------------------------------------------------------------ */
/* Memory allocator hooks.                                       */
/* Override before including this header (or with -D on the      */
/* command line) to plug in a custom allocator, e.g.:            */
/*   #define UTILS_MALLOC(s)  my_alloc(s)                        */
/*   #define UTILS_FREE(p)    my_free(p)                         */
/* Defaults to the standard C malloc/free.                       */
/* ------------------------------------------------------------ */
#include <stdlib.h>

#ifndef UTILS_MALLOC
    #define UTILS_MALLOC(size)  malloc(size)
#endif

#ifndef UTILS_FREE
    #define UTILS_FREE(ptr)     free(ptr)
#endif


#define ENB_DEBUG       (1)


#if (ENB_DEBUG)
    #include "debug_uart.h"     /* Repalce with yours. */

    #define DEBUG( fmt, ... )     LOG( fmt, ##__VA_ARGS__ )
#else /* ENB_DEBUG */
    #define DEBUG( fmt, ... )   ((void)0)
#endif


#define FAT_UTIL_GET_TICK()       HAL_GetTick()

typedef enum
{
    UTIL_MODE_Kb,
    UTIL_MODE_Mb,
    UTIL_MODE_Gb,

} utilsVolumeMode_t;


typedef struct 
{
    uint64_t tot_bytes_fat;
    uint64_t tot_bytes_hw;

    uint32_t tot_sector_fat;
    uint32_t tot_sector_hw;

    uint64_t free_bytes;
    uint32_t sector_size;
    uint32_t cluster_size;

    TCHAR card_type[8];
    TCHAR fs_type[8];

    uint32_t vol_sn;
    TCHAR vol_label[16];

    uint32_t driver_num;

} utilsCardInfo_t;


FRESULT utils_file_write( FIL *fp, const TCHAR *path, BYTE *data, UINT size, bool isAppend );
FRESULT utils_file_get_free_space_int( const TCHAR *path, utilsVolumeMode_t mode, uint64_t *oSpace );
FRESULT utils_sd_speed_test( FIL *fs, const TCHAR *path, uint32_t size_mb, float* oWriteSP, float* oReadSP ); 
FRESULT utils_sd_info( const TCHAR *path, utilsCardInfo_t *oInfo, SD_HandleTypeDef *hsd );

void utils_extract_driver_from_path( const TCHAR *path, BYTE *obuf, UINT obuf_size );
void utils_sd_info_print( const utilsCardInfo_t *info );

#endif /* __FATFS_UTILS_H__ */
