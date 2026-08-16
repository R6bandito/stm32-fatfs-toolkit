#include "fatfs_utils.h"
#include "diskio.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ************************************************* */
#define BUF_SIZE			(32 * 1024)		/* Speed-test only: allocated dynamically. */
/* ************************************************* */


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
		DEBUG( "[ERR] Incomplete write! Expected: %u, Actual: %u. PATH:%s  LINE:%d.\n", size, wByt, __FILE__, __LINE__ );
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
	res = f_getfree( (const TCHAR *)driver, &freeCluster, &fs );
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

			break;
		}

		default:	DEBUG("[ERR] Invalid mode: %d\n", mode);	return FR_INVALID_PARAMETER;
	}

	return FR_OK;
}


FRESULT 
utils_sd_speed_test( FIL *fs, const TCHAR *path, uint32_t size_mb, float* oWriteSP, float* oReadSP ) 
{
	if ( !path || !size_mb || !fs )
		return FR_INVALID_PARAMETER;

	/* Concatenate path strings. */
	BYTE driver[8] = { 0 };
	char file_path[32] = { 0 };
	utils_extract_driver_from_path( path, driver, sizeof(driver) );
	int len = snprintf( file_path, sizeof(file_path), "%s%s", driver, "/TEST_FILE.txt" );
	if ( len < 0 || len > (int)sizeof(file_path) )
	{
		DEBUG( "[ERR] Snprintf str err with utils_sd_speed_test.c  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return FR_INVALID_PARAMETER;
	}

	/* Dynamically allocate buffers; no resident footprint when idle. */
	uint8_t *wbuf = UTILS_MALLOC( BUF_SIZE );
	if ( !wbuf )
	{
		UTILS_FREE( wbuf );
		DEBUG( "[ERR] Malloc fail with utils_sd_speed_test.c  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return FR_NOT_ENOUGH_CORE;
	}

	/* Polling mode needs at least 4-byte alignment. */
	if ( ((uint32_t)wbuf & 0x03U) != 0U )
	{
		UTILS_FREE( wbuf );
		DEBUG( "[ERR] Buffer not aligned with utils_sd_speed_test.c  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return FR_NOT_ENOUGH_CORE;
	}

	FRESULT res;

	/* Create & open the test file. */
	res = f_open( fs, file_path, FA_OPEN_ALWAYS | FA_WRITE );
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Open file err with utils_sd_speed_test.c  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		goto END;
	}

	/* Fill the write buffer with a verifiable pattern. */
	for( uint32_t i = 0; i < BUF_SIZE; i++ )
		wbuf[i] = (uint8_t)i;

	/* ---------- Write speed test ---------- */
	uint32_t remain = (uint32_t)size_mb * 1024U * 1024U;
	uint32_t totBytes = remain;
	uint32_t bw = 0;
	uint32_t test_start = FAT_UTIL_GET_TICK();
	while( remain > 0 )
	{
		uint32_t chunk = (remain > BUF_SIZE) ? BUF_SIZE : remain;
		res = f_write( fs, wbuf, chunk, &bw );
		if ( res != FR_OK || bw != chunk )
		{
			DEBUG( "[ERR] Write err with utils_sd_speed_test.c  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
			goto END;
		}

		remain -= bw;
	}

	/* Flush to media: FAT/dir entry + card programming must count. */
	res = f_sync( fs );
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Sync err with utils_sd_speed_test.c  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		goto END;
	}

	{
		/* Calculate the write speed. */
		uint32_t elapsed_ms = FAT_UTIL_GET_TICK() - test_start;
		float wMbps = (elapsed_ms > 0U) ? ((float)totBytes / 1024.0f / 1024.0f / ((float)elapsed_ms / 1000.0f)) : 0.0f;
		if ( oWriteSP )	*oWriteSP = wMbps;

		DEBUG( "[INFO] SD Speed TEST: TotWrite: %u Bytes(%u MB)\t WriteSpd: %.1f MB/s\n",
				totBytes, (totBytes / (1024U * 1024U)), wMbps );

		DEBUG( "[INFO] Write time: %.1f s\n", (float)(elapsed_ms / 1000.0) );
	}

	f_close( fs );

	/* ---------- Read speed test ---------- */
	res = f_open( fs, file_path, FA_OPEN_ALWAYS | FA_READ );
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Open file err with utils_sd_speed_test.c  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		goto END;
	}

	uint32_t totRead = 0;
	uint32_t br = 0;
	test_start = FAT_UTIL_GET_TICK();
	while( 1 )
	{
		res = f_read( fs, wbuf, BUF_SIZE, &br );
		if ( res != FR_OK )
		{
			DEBUG( "[ERR] Read err with utils_sd_speed_test.c  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
			goto END;
		}

		if ( br == 0 )
			break;	/* End of file. */


		totRead += br;
	}

	{
		/* Calculate the read speed. */
		uint32_t elapsed_ms = FAT_UTIL_GET_TICK() - test_start;
		float rMbps = (elapsed_ms > 0U) ? ((float)totRead / 1024.0f / 1024.0f / ((float)elapsed_ms / 1000.0f)) : 0.0f;
		if ( oReadSP )	*oReadSP = rMbps;

		DEBUG( "[INFO] SD Speed TEST: TotRead: %u Bytes(%u MB)\t ReadSpd: %.1f MB/s\n",
				totRead, (totRead / (1024U * 1024U)), rMbps );

		DEBUG( "[INFO] Read time: %.1f s\n", (float)(elapsed_ms / 1000.0) );
	}

END:
	if ( fs->obj.fs != NULL )
		f_close( fs );

	/* Delete the test file. */
	f_unlink( file_path );

	UTILS_FREE( wbuf );

	return res;
}


FRESULT 
utils_sd_info( const TCHAR *path, utilsCardInfo_t *oInfo, SD_HandleTypeDef *hsd )
{
	if ( !path || !oInfo || !hsd )
		return FR_INVALID_PARAMETER;

	BYTE driver[8] = { 0 };
	DWORD cluster;
	FRESULT res;
	DWORD vsn;
	TCHAR label[16] = { 0 };
	FATFS *fs = NULL;

	utils_extract_driver_from_path( path, driver, sizeof(driver) );

	oInfo->driver_num = atoi( (const char *)driver );
	res = f_getfree( (const TCHAR *)driver, &cluster, &fs );
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Get freespace err with utils_sd_info.c  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return res;
	}

	oInfo->cluster_size = fs->csize;
	DRESULT dres = disk_ioctl( oInfo->driver_num, GET_SECTOR_SIZE, &oInfo->sector_size );
	if ( dres != RES_OK )
		oInfo->sector_size = FF_MIN_SS;

	oInfo->free_bytes = (uint64_t)cluster * fs->csize * oInfo->sector_size;
	oInfo->tot_bytes_fat = (fs->n_fatent - 2) * fs->csize * oInfo->sector_size;
	oInfo->tot_sector_fat = (fs->n_fatent - 2) * fs->csize;

	res = f_getlabel( (const TCHAR *)driver, label, &vsn );
	if ( res == FR_OK )
	{
		strncpy( oInfo->vol_label, label, sizeof(oInfo->vol_label) - 1 );
		oInfo->vol_label[sizeof(oInfo->vol_label) - 1] = '\0';
		oInfo->vol_sn = vsn;
	}
	else 
	{
		strcpy(oInfo->vol_label, "N/A");
		oInfo->vol_sn = 0;
	}

	if ( oInfo->vol_label[0] == '\0' )
		strcpy(oInfo->vol_label, "N/A");

	HAL_SD_CardInfoTypeDef hinfo;
	if ( HAL_SD_GetCardInfo( hsd, &hinfo ) == HAL_OK )
	{
		oInfo->tot_sector_hw = hinfo.LogBlockNbr;
		oInfo->tot_bytes_hw = (uint64_t)hinfo.LogBlockNbr * hinfo.LogBlockSize;

		if ( hinfo.CardType & CARD_SDSC )
			strcpy( oInfo->card_type, "SDSC" );
		else if ( hinfo.CardType & CARD_SDHC_SDXC )
			strcpy( oInfo->card_type, "SDHC_XC" );
		else 
			strcpy( oInfo->card_type, "N/A" );
	}
	else 
	{
		oInfo->tot_sector_hw = 0;
		oInfo->tot_bytes_hw  = 0;
		strcpy(oInfo->card_type, "N/A");
	}

	if ( fs->fs_type == FS_FAT32 )		strcpy( oInfo->fs_type, "FAT32" );
	else if ( fs->fs_type == FS_EXFAT )	strcpy( oInfo->fs_type, "exFAT" );
	else if ( fs->fs_type == FS_FAT16 )	strcpy( oInfo->fs_type, "FAT16" );
	else if ( fs->fs_type == FS_FAT12 )	strcpy( oInfo->fs_type, "FAT12" );
	else strcpy( oInfo->fs_type, "N/A" );

	return FR_OK;
}


void 
utils_sd_info_print( const utilsCardInfo_t *info )
{
	DEBUG( "[INFO] === SD Card Info ===\n" );
	DEBUG( "  Drive        : %d\n", info->driver_num );
	DEBUG( "  Volume Label : %s\n", info->vol_label );
	DEBUG( "  Volume SN    : %08lX\n", info->vol_sn );
	DEBUG( "  FS Type      : %s\n", info->fs_type );
	DEBUG( "  Card Type    : %s\n", info->card_type );
	DEBUG( "  Sector Size  : %lu\n", info->sector_size );
	DEBUG( "  Cluster Size : %lu sectors (%lu KB)\n", 
		info->cluster_size, info->cluster_size * info->sector_size / 1024 );
	DEBUG( "  Total (FAT)  : %llu MB (%llu bytes)\n", 
		info->tot_bytes_fat / (1024*1024), info->tot_bytes_fat );
	DEBUG( "  Total (HW)   : %llu MB (%llu bytes)\n", 
		info->tot_bytes_hw / (1024*1024), info->tot_bytes_hw );
	DEBUG( "  Free         : %llu MB (%llu bytes)\n", 
		info->free_bytes / (1024*1024), info->free_bytes );
	if ( info->tot_bytes_hw > 0 && info->tot_bytes_fat > info->tot_bytes_hw ) {
		DEBUG( "[WARN] *** FAKE SD CARD DETECTED! FAT > HW ***\n" );
	}
	DEBUG( "================================\n" );
}
