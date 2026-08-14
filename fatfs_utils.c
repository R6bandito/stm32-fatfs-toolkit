#include "fatfs_utils.h"
#include <stdio.h>
#include <string.h>


void 
utils_extract_driver_from_path( const TCHAR *path, BYTE *obuf, UINT obuf_size )
{
	if ( !path || !obuf || !obuf_size )
		return;

	const TCHAR *colon = strchr( path, ':' );
	if ( colon )
	{
		/* Success found ':'. Caculate length. */
		int len = colon - path + 1;
		if ( len < 0 || len > obuf_size )
		{
			/* Wrong len. */
			return;
		}

		strncpy( (char *)obuf, path, len );
		obuf[len] = '\0';
		return;
	}

	strcpy( (char *)obuf, "0:" );
}


FRESULT 
utils_file_write( FIL *fp, const TCHAR *path, BYTE *data, UINT size, bool isAppend )
{
	if ( !path || !data || !size )
	{
		DEBUG( "[ERR] Invalid parameter with utils_file_write.c  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return FR_INVALID_PARAMETER;
	}

	FRESULT res;
	BYTE mode = (isAppend) ? (FA_OPEN_APPEND | FA_WRITE) : (FA_OPEN_ALWAYS | FA_WRITE);
	res = f_open( fp, path, mode );
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Open file err with utils_file_write.c  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return res;
	}

	/* Check the remain space. */
	DWORD freeCluster = 0;
	BYTE driver[10] = { 0 };
	FATFS *fs = NULL;
	utils_extract_driver_from_path( path, driver, sizeof(driver) );
	res = f_getfree( (const TCHAR *)driver, &freeCluster, &fs );
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Get freespace err with utils_file_write.c  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return res;
	}

	uint64_t freeBytes = (uint64_t)freeCluster * (fs->csize) * FF_MIN_SS;
	if ( freeBytes < size )
	{
		/* Space is not enough.  */
		DEBUG( "[WARN] Space not enough. Action skip.  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return FR_DENIED;
	}

	UINT wByt = 0;
	res = f_write( fp, data, size, &wByt );
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Write err with utils_file_write.c  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		f_close( fp );
		return res;
	}

	if ( wByt != size )
	{
		/* Disk is full/Hardware error. */
		DEBUG( "[ERR] Incomplete write! Expected: %u, Actual: %u. PATH:%s  LINE:%d.\n", size, __FILE__, __LINE__ );
		f_sync( fp );
		f_close( fp );
		return FR_DENIED;
	}

	f_close( fp );
	DEBUG( "[INFO] utils_file_write Success! Src Byte: %d  Write Bytes: %d.\n", size, wByt );
	return FR_OK;
}


FRESULT 
utils_file_get_free_space_int( const TCHAR *path, utilsVolumeMode_t mode, uint64_t *oSpace )
{
	if ( !path )
		return FR_INVALID_PARAMETER;

	DWORD freeCluster;
	FATFS *fs = NULL;
	BYTE driver[10];
	FRESULT res;
	
	utils_extract_driver_from_path( path, driver, sizeof(driver) );
	res = f_getfree( driver, &freeCluster, &fs );
	if ( res != FR_OK || fs == NULL )
	{
		DEBUG( "[ERR] Get freespace err with utils_file_get_free_space.c  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return res;
	}

	/* Caculate. */
	uint64_t freeBytes = (uint64_t)freeCluster * fs->csize * FF_MIN_SS;
	switch (mode)
	{
		case UTIL_MODE_Kb:
		{
			*oSpace = (freeBytes / 1024);
			if ( *oSpace == 0 )
			{
				DEBUG( "[INFO] Card volume less than 1Kb.\n" );
				*oSpace = 1;
			}

			break;
		}

		case UTIL_MODE_Mb:
		{
			*oSpace = ((freeBytes / 1024) /1024);
			if ( *oSpace == 0 )
			{
				DEBUG( "[INFO] Card volume less than 1Mb.\n" );
				*oSpace = 1;
			}

			break;
		}

		case UTIL_MODE_Gb:
		{
			*oSpace = (((freeBytes / 1024) / 1024) / 1024);
			if ( *oSpace == 0 )
			{
				DEBUG( "[INFO] Card volume less than 1Gb.\n" );
				*oSpace = 1;
			}
		}

		default:	DEBUG("[ERR] Invalid mode: %d\n", mode);	return FR_INVALID_PARAMETER;
	}

	return FR_OK;
}




