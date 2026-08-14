#ifndef __FATFS_UTILS_H__
#define __FATFS_UTILS_H__


#include "ff.h"
#include <stdint.h>
#include <stdbool.h>


#define ENB_DEBUG       (1)


#if (ENB_DEBUG)
    #include "debug_uart.h"     /* Repalce with yours. */

    #define DEBUG( fmt, ... )     LOG( fmt, ##__VA_ARGS__ )
#else /* ENB_DEBUG */
    #define DEBUG( fmt, ... )   ((void)0)
#endif


typedef enum
{
    UTIL_MODE_Kb,
    UTIL_MODE_Mb,
    UTIL_MODE_Gb,

} utilsVolumeMode_t;


FRESULT utils_file_write( FIL *fp, const TCHAR *path, BYTE *data, UINT size, bool isAppend );
FRESULT utils_file_get_free_space_int( const TCHAR *path, utilsVolumeMode_t mode, uint64_t *oSpace );

void utils_extract_driver_from_path( const TCHAR *path, BYTE *obuf, UINT obuf_size );

#endif /* __FATFS_UTILS_H__ */
