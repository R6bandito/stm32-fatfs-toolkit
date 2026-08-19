/**
 * @file    fatfs_utils.c
 * @brief   FatFs utility library: file I/O helpers, SD card information and
 *          speed test, transactional (atomic) writes and recursive directory
 *          walking, built on top of FatFs and the STM32 HAL.
 *
 *          Design notes:
 *          - Small internal buffers are static (2 x 1KB, g_bufferA/B);
 *            large buffers are allocated on demand through
 *            UTILS_MALLOC / UTILS_FREE (both overridable).
 *          - Transactional writes stage data into a "<name>.temp" file and
 *            atomically rename it over the source; every session must end
 *            with commit or abort.
 *          - Directory walking is recursive with a fixed depth limit; the
 *            callback decides what to do with each entry.
 *          - Debug output is gated by ENB_DEBUG and routed through LOG().
 */

#include "fatfs_utils.h"
#include "diskio.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#pragma diag_suppress 546

/* ************************************************* */
#define BUF_SIZE			(32 * 1024)		/* Speed-test only: allocated dynamically. */
#define INTERNAL_BUF_SIZE	(1 * 1024)
static uint8_t g_bufferA[INTERNAL_BUF_SIZE];
static uint8_t g_bufferB[INTERNAL_BUF_SIZE];
/* ************************************************* */


/**
 * @brief  Extract the drive prefix (e.g. "0:") from a FatFs path string.
 * @param  path       Full path, e.g. "0:/dir/file.txt".
 * @param  obuf       Output buffer receiving the drive prefix.
 * @param  obuf_size  Size of the output buffer.
 * @retval None
 */
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


/**
 * @brief  Split a path into a directory prefix and the file name.
 * @param  path      Full path, e.g. "0:/dir/sub/name.txt".
 * @param  dir_len   [out] Length of the directory prefix, including the
 *                   trailing '/'. Zero if the path has no directory part.
 * @retval Pointer to the file name inside @p path, or NULL on invalid input.
 */
const TCHAR *
utils_extract_filename( const TCHAR *path, UINT *dir_len )
{
	if ( !path || !dir_len )
		return NULL;

	const TCHAR *p = path;
	const TCHAR *slash = NULL;

	/* case: "0:/dir/sub/name.txt" */
	while( *p )
	{
		if ( *p == '\\' || *p == '/' )
			slash = p;

		p++;
	}

	if ( slash )
	{
		*dir_len = (UINT)(slash - path + 1);
		return (slash + 1);
	}

	/* case: "0:name.txt" */
	const TCHAR *colon = strchr( path, ':' );
	if ( colon )
	{
		*dir_len = (UINT)(colon - path + 1);
		return (colon + 1);
	}

	/* case: "name.txt"  */
	*dir_len = 0;
	return path;
}


/**
 * @brief  Write data to a file. Overwrites or appends depending on @p isAppend.
 *         Checks free space before writing.
 * @param  fp       File object used for the operation.
 * @param  path     Target file path, e.g. "0:/data.txt".
 * @param  data     Data buffer to write.
 * @param  size     Number of bytes to write.
 * @param  isAppend true  -> append to the file end;
 *                  false -> overwrite the whole file.
 * @retval FR_OK on success, otherwise a FatFs error code.
 */
FRESULT 
utils_file_write( FIL *fp, const TCHAR *path, BYTE *data, UINT size, bool isAppend )
{
	if ( !path || !data || !size )
	{
		DEBUG( "[ERR] Invalid parameter with [utils_file_write]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return FR_INVALID_PARAMETER;
	}

	FRESULT res;
	BYTE mode = (isAppend) ? (FA_OPEN_APPEND | FA_WRITE) : (FA_OPEN_ALWAYS | FA_WRITE);
	res = f_open( fp, path, mode );
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Open file err with [utils_file_write]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
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
		DEBUG( "[ERR] Get freespace err with [utils_file_write]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
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
		DEBUG( "[ERR] Write err with [utils_file_write]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
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
	DEBUG( "[INFO] [utils_file_write] Success! Src Byte: %d  Write Bytes: %d.\n", size, wByt );
	return FR_OK;
}


/**
 * @brief  Get the free space of a volume in KB, MB or GB.
 * @param  path     Any path on the target volume, e.g. "0:/".
 * @param  mode     Output unit: UTIL_MODE_Kb / UTIL_MODE_Mb / UTIL_MODE_Gb.
 * @param  oSpace   [out] Free space in the requested unit.
 * @retval FR_OK on success, otherwise a FatFs error code.
 */
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
		DEBUG( "[ERR] Get freespace err with [utils_file_get_free_space_int]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
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


/**
 * @brief  Measure sequential write/read speed of the SD card.
 *         Writes a temporary file, flushes it (f_sync), reads it back and
 *         reports throughput in MB/s. The temp file is removed afterwards.
 * @param  fs       File object used for the test.
 * @param  path     Volume path, e.g. "0:/".
 * @param  size_mb  Test size in megabytes.
 * @param  oWriteSP [out] Write speed in MB/s (may be NULL).
 * @param  oReadSP  [out] Read speed in MB/s (may be NULL).
 * @retval FR_OK on success, otherwise a FatFs error code.
 */
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
		DEBUG( "[ERR] Snprintf str err with [utils_sd_speed_test]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return FR_INVALID_PARAMETER;
	}

	/* Dynamically allocate buffers; no resident footprint when idle. */
	uint8_t *wbuf = UTILS_MALLOC( BUF_SIZE );
	if ( !wbuf )
	{
		UTILS_FREE( wbuf );
		DEBUG( "[ERR] Malloc fail with [utils_sd_speed_test]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return FR_NOT_ENOUGH_CORE;
	}

	/* Polling mode needs at least 4-byte alignment. */
	if ( ((uint32_t)wbuf & 0x03U) != 0U )
	{
		UTILS_FREE( wbuf );
		DEBUG( "[ERR] Buffer not aligned with [utils_sd_speed_test]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return FR_NOT_ENOUGH_CORE;
	}

	FRESULT res;

	/* Declared up-front so goto END never crosses initializers (#546-D). */
	uint32_t remain = 0, totBytes = 0, bw = 0, test_start = 0;
	uint32_t totRead = 0, br = 0;

	/* Create & open the test file. */
	res = f_open( fs, file_path, FA_OPEN_ALWAYS | FA_WRITE );
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Open file err with [utils_sd_speed_test]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		goto END;
	}

	/* Fill the write buffer with a verifiable pattern. */
	for( uint32_t i = 0; i < BUF_SIZE; i++ )
		wbuf[i] = (uint8_t)i;

	/* ---------- Write speed test ---------- */
	remain = (uint32_t)size_mb * 1024U * 1024U;
	totBytes = remain;
	bw = 0;
	test_start = FAT_UTIL_GET_TICK();
	while( remain > 0 )
	{
		uint32_t chunk = (remain > BUF_SIZE) ? BUF_SIZE : remain;
		res = f_write( fs, wbuf, chunk, &bw );
		if ( res != FR_OK || bw != chunk )
		{
			DEBUG( "[ERR] Write err with [utils_sd_speed_test]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
			goto END;
		}

		remain -= bw;
	}

	/* Flush to media: FAT/dir entry + card programming must count. */
	res = f_sync( fs );
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Sync err with [utils_sd_speed_test]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
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
		DEBUG( "[ERR] Open file err with [utils_sd_speed_test]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		goto END;
	}

	totRead = 0;
	br = 0;
	test_start = FAT_UTIL_GET_TICK();
	while( 1 )
	{
		res = f_read( fs, wbuf, BUF_SIZE, &br );
		if ( res != FR_OK )
		{
			DEBUG( "[ERR] Read err with [utils_sd_speed_test]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
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


/**
 * @brief  Collect filesystem and hardware information of the SD card.
 *         Compares FAT-visible capacity with hardware capacity to detect
 *         fake (over-sized) cards.
 * @param  path   Any path on the target volume, e.g. "0:/".
 * @param  oInfo  [out] Filled utilsCardInfo_t structure.
 * @param  hsd    Initialized SD handle (HAL).
 * @retval FR_OK on success, otherwise a FatFs error code.
 */
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
		DEBUG( "[ERR] Get freespace err with [utils_sd_info]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
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


/**
 * @brief  Print the SD card information to the debug output.
 * @param  info  Card information filled by utils_sd_info().
 * @retval None
 */
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


/**
 * @brief  Transactional write. On the first call of a session the temp file
 *         (source name with ".temp" extension) is created and opened; later
 *         calls skip the open and append directly. Finish with
 *         utils_file_txn_commit() or utils_file_txn_abort().
 * @param  fp    File object; must be zeroed before the first use of a session.
 * @param  path  Source file path, e.g. "0:/data.txt".
 * @param  data  Data buffer to write.
 * @param  size  Number of bytes to write.
 * @retval FR_OK on success, otherwise a FatFs error code.
 */
FRESULT 
utils_file_txn_write( FIL *fp, const TCHAR *path, BYTE *data, UINT size )
{
	if ( !fp || !path || !data || !size )
		return FR_INVALID_PARAMETER;

	UINT dir_len;
	UINT base, nlen, bw;
	FRESULT res;
	const TCHAR *fileN;

	/* If the file is not created for the first time and is already open, skip open() and write directly. */
	if ( fp->obj.fs )
		goto Next;

	fileN = utils_extract_filename( path, &dir_len );
	if ( !fileN )
	{
		DEBUG( "[ERR] Extract filename err with [utils_file_txn_write].  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		res = FR_INVALID_PARAMETER;
		goto Error;
	}
	nlen = strlen( fileN );

	/* Successfully retrieved the file name, now create the temp file path.  */
	char *dot = strrchr( fileN, '.' );
	base = ( dot && dot != fileN ) ? (UINT)(dot - fileN) : nlen;
	strncpy( (char *)g_bufferA, fileN, base );
	g_bufferA[base] = '\0';

	int len = snprintf( (char *)g_bufferB, sizeof(g_bufferB), "%.*s%s.temp", (int)dir_len, path, g_bufferA );
	if ( len < 0 || (UINT)len >= sizeof(g_bufferB) )
	{
		DEBUG( "[ERR] Temp file path build err with [utils_file_txn_write].  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		res = FR_INVALID_PARAMETER;
		goto Error;
	}

	/* Creating temporary file. Preparing to write. */
	res = f_open( fp, (const TCHAR *)g_bufferB, FA_CREATE_ALWAYS | FA_WRITE );
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Open file err with [utils_file_txn_write].  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		goto Error;
	}
	DEBUG( "[INFO] Temp file create OK! [utils_file_txn_write]\nPATH: %s\n", g_bufferB );

Next:
	res = f_write( fp, data, size, &bw );
	if ( (res != FR_OK) || (size != bw) )
	{
		DEBUG( "[ERR] Temp file write err with [utils_file_txn_write].  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		goto Error;
	}

	return FR_OK;

Error:
	return res;
}


/**
 * @brief  Commit a transactional write session: flush to media, close the
 *         temp file, delete the source file and rename the temp file to the
 *         source path (atomic replace).
 * @param  fp    File object of the session (must be open).
 * @param  path  Source file path used in utils_file_txn_write().
 * @retval FR_OK on success, otherwise a FatFs error code.
 */
FRESULT 
utils_file_txn_commit( FIL *fp, const TCHAR *path )
{
	if ( !fp || !path )
			return FR_INVALID_PARAMETER;

	/* Simple guard: session must be open (created by txn_write). */
	if ( fp->obj.fs == NULL )
	{
		DEBUG( "[ERR] Session not open. Commit fail. [utils_file_txn_commit]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return FR_INVALID_OBJECT;
	}

	UINT dir_len, base, nlen;
	FRESULT res;
	const TCHAR *fileN = utils_extract_filename( path, &dir_len );
	if ( !fileN )
	{
		DEBUG( "[ERR] Extract filename err with [utils_file_txn_commit]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		res = FR_INVALID_PARAMETER;
		goto Error;
	}
	nlen = strlen( fileN );

	/* Successfully retrieved the file name, now create the temp file path.  */
	char *dot = strrchr( fileN, '.' );
	base = ( dot && dot != fileN ) ? (UINT)(dot - fileN) : nlen;
	strncpy( (char *)g_bufferA, fileN, base );
	g_bufferA[base] = '\0';

	int len = snprintf( (char *)g_bufferB, sizeof(g_bufferB), "%.*s%s.temp", (int)dir_len, path, g_bufferA );
	if ( len < 0 || (UINT)len >= sizeof(g_bufferB) )
	{
		DEBUG( "[ERR] Temp file path build err with [utils_file_txn_commit]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		res = FR_INVALID_PARAMETER;
		goto Error;
	}

	/* Flush to media before renaming. */
	res = f_sync( fp );
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Sync err with [utils_file_txn_commit]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		f_close( fp );
		f_unlink( (const TCHAR *)g_bufferB );
		return res;
	}

	res = f_close( fp );
	if ( res != FR_OK )
	{
		f_unlink( (const TCHAR *)g_bufferB );
		return res;
	}

	/* Delete the source file (tolerate first write: FR_NO_FILE). */
	res = f_unlink( path );
	if ( res != FR_OK && res != FR_NO_FILE )
	{
		DEBUG( "[ERR] Unlink src err with [utils_file_txn_commit]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		f_unlink( (const TCHAR *)g_bufferB );
		return res;
	}

	/* Promote temp file to the source path. */
	res = f_rename( (const TCHAR *)g_bufferB, path );
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Rename err with [utils_file_txn_commit]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		f_unlink( (const TCHAR *)g_bufferB );
		return res;
	}

	return FR_OK;

Error:
	return res;
}


/**
 * @brief  Abort a transactional write session: close the temp file and
 *         delete it. The source file is left untouched.
 * @param  fp    File object of the session (must be open).
 * @param  path  Source file path used in utils_file_txn_write().
 * @retval FR_OK on success, otherwise a FatFs error code.
 */
FRESULT 
utils_file_txn_abort( FIL *fp, const TCHAR *path )
{
	if ( !fp || !path )
		return FR_INVALID_PARAMETER;

	/* Simple guard: session must be open. */
	if ( fp->obj.fs == NULL )
	{
		DEBUG( "[ERR] Session not open. Abort fail. [utils_file_txn_abort]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return FR_INVALID_OBJECT;
	}

	UINT dir_len, base, nlen;
	FRESULT res;
	const TCHAR *fileN = utils_extract_filename( path, &dir_len );
	if ( !fileN )
	{
		DEBUG( "[ERR] Extract filename err with [utils_file_txn_abort]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return FR_INVALID_PARAMETER;
	}
	nlen = strlen( fileN );

	char *dot = strrchr( fileN, '.' );
	base = ( dot && dot != fileN ) ? (UINT)(dot - fileN) : nlen;
	strncpy( (char *)g_bufferA, fileN, base );
	g_bufferA[base] = '\0';

	int len = snprintf( (char *)g_bufferB, sizeof(g_bufferB), "%.*s%s.temp", (int)dir_len, path, g_bufferA );
	if ( len < 0 || (UINT)len >= sizeof(g_bufferB) )
	{
		DEBUG( "[ERR] Temp file path build err with [utils_file_txn_abort]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return FR_INVALID_PARAMETER;
	}

	res = f_close( fp );
	if ( res == FR_OK )
	{
		res = f_unlink( (const TCHAR *)g_bufferB );
		if ( res == FR_OK )
		{
			DEBUG( "[INFO] File write abort! [utils_file_txn_abort]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
			return FR_OK;
		}
	}

	return res;
}


/**
 * @brief  Recursive directory walker (internal). Enumerates every entry of
 *         @p path; directories are recursed first, then reported (post-order).
 * @param  path    Directory to enumerate (absolute path).
 * @param  cb      Callback invoked for every entry with a full path.
 * @param  arg     User argument passed through to @p cb.
 * @param  deepth  Current recursion depth; aborts above DIR_DEEP.
 * @retval FR_OK on success, otherwise a FatFs error code.
 */
static FRESULT
utils_handle_dir( const TCHAR *path, walkFn cb, void *arg, UINT deepth )
{
	if ( deepth >= DIR_DEEP )
	{
		DEBUG( "[ERR] Path too deepth! [utils_handle_dir]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return FR_INVALID_PARAMETER;
	}

	DIR directory;
	FILINFO fno;
	FRESULT res;
	TCHAR child[FF_LFN_BUF + 16] = { 0 };

	res =  f_opendir( &directory, path );
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Dir open err! [utils_handle_dir]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return res;
	}

	while( 1 )
	{
		res = f_readdir( &directory, &fno );
		if ( res != FR_OK || !fno.fname[0] )
			break;

		int len = snprintf( child, sizeof(child), "%s%s%s", path, 
					(path[0] && path[strlen(path)-1] != '/') ? "/" : "", fno.fname );
		if ( len < 0 || (UINT)len >= sizeof(child) )
		{
			DEBUG( "[ERR] Path build err! [utils_handle_dir]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
			return FR_INVALID_PARAMETER;
		}

		if ( fno.fattrib & AM_DIR )
		{
			/* Sub directory detect. */
			res = utils_handle_dir( child, cb, arg, deepth + 1 );
			if ( res != FR_OK )
				break;
			
			res = cb( child, &fno, arg );
		}
		else 
		{
			res = cb( child, &fno, arg );
		}

		if ( res != FR_OK )
			break;
	}

	f_closedir( &directory );
	return res;
}


/**
 * @brief  Walk a directory tree recursively, invoking @p cb for every entry.
 *         Entries are reported depth-first; a directory is reported after all
 *         of its children (post-order).
 * @param  path  Root directory to walk (absolute path).
 * @param  cb    Callback: FR_OK continues, any other code stops the walk.
 * @param  arg   User argument passed through to @p cb.
 * @retval FR_OK on success, otherwise a FatFs error code.
 */
FRESULT 
utils_dir_walk( const TCHAR *path, walkFn cb, void *arg )
{
	if ( !path || !cb )
		return FR_INVALID_PARAMETER;

	return utils_handle_dir( path, cb, arg, 0 );	
}


/**
 * @brief  Print callback for utils_dir_walk(): prints an indented tree with
 *         type, attributes, human-readable size and FAT date/time.
 * @param  path  Full path of the entry.
 * @param  fi    Entry information (name, attributes, size, date).
 * @param  arg   Unused.
 * @retval FR_OK always.
 */
static FRESULT 
utils_dir_print_cb( const TCHAR *path, const FILINFO *fi, void *arg )
{
	(void)arg;

	UINT slashes = 0;
	for ( const TCHAR *p = path; *p; p++ )
			if ( *p == '/' ) slashes++;
	UINT indent = ( slashes > 1 ) ? ( slashes - 1 ) : 0;

	for ( UINT i = 0; i < indent; i++ )
		DEBUG( "  " );

	DEBUG( "%s%s", (fi->fattrib & AM_DIR) ? "[D] " : "[F] ", path );

	DEBUG( "  %c%c%c%c",
			(fi->fattrib & AM_RDO) ? 'R' : '-',
			(fi->fattrib & AM_HID) ? 'H' : '-',
			(fi->fattrib & AM_SYS) ? 'S' : '-',
			(fi->fattrib & AM_ARC) ? 'A' : '-' );

	if ( !(fi->fattrib & AM_DIR) )
	{
		uint64_t s = fi->fsize;
		if ( s < 1024ULL )
		{
			DEBUG( "  %llu B", (unsigned long long)s );
		}
		else
		{
			uint64_t v;
			const char *unit;
			if ( s >= (1024ULL * 1024 * 1024 * 1024) )      { unit = "TB"; v = s * 10 / (1024ULL * 1024 * 1024 * 1024); }
			else if ( s >= (1024ULL * 1024 * 1024) )        { unit = "GB"; v = s * 10 / (1024ULL * 1024 * 1024); }
			else if ( s >= (1024ULL * 1024) )               { unit = "MB"; v = s * 10 / (1024ULL * 1024); }
			else                                            { unit = "KB"; v = s * 10 / 1024ULL; }
			DEBUG( "  %llu.%llu %s", (unsigned long long)(v / 10), (unsigned long long)(v % 10), unit );
		}
	}

	DEBUG( "  %04u-%02u-%02u %02u:%02u:%02u\n",
		(unsigned)(1980 + ((fi->fdate >> 9) & 0x7F)),
		(unsigned)((fi->fdate >> 5) & 0x0F),
		(unsigned)(fi->fdate & 0x1F),
		(unsigned)((fi->ftime >> 11) & 0x1F),
		(unsigned)((fi->ftime >> 5) & 0x3F),
		(unsigned)((fi->ftime & 0x1F) * 2) );

	return FR_OK;
}


/**
 * @brief  Print the whole directory tree below @p path to the debug output.
 * @param  path  Root directory (absolute path).
 * @retval None
 */
void 
utils_dir_print( const TCHAR *path )
{
	if ( !path )
		return;

	DEBUG( "=== Dir Tree: %s ===\n", path );
	utils_dir_walk( path, utils_dir_print_cb, NULL );
	DEBUG( "========================\n" );
}


/**
 * @brief  Create a directory path recursively ("mkdir -p" semantics).
 *         Existing directories are skipped; a file occupying any path level
 *         is reported as FR_EXIST.
 * @param  path  Directory path to create, e.g. "0:/A/B/C".
 * @retval FR_OK on success, otherwise a FatFs error code.
 */
FRESULT 
utils_mkdirs( const TCHAR *path )
{
	if ( !path )
		return FR_INVALID_PARAMETER;

	FILINFO fno;
	FRESULT res;
	TCHAR curBuf[FF_LFN_BUF + 16] = { 0 };

	char *colomn = strrchr( path, ':' );
	if ( !colomn )
		return FR_INVALID_PARAMETER;
	UINT pre = (UINT)(colomn - path + 1);
	strncpy( curBuf, path, pre );
	curBuf[pre] = '\0';

	const TCHAR *p = colomn + 1;
	if ( *p == '/' )
		p++;

	while( *p )
	{
		const TCHAR *slash = strchr( p, '/' );
		UINT seglen = slash ? (UINT)(slash - p) : (UINT)strlen( p );

		int len = snprintf( curBuf + pre, sizeof(curBuf) - pre, "/%.*s", (int)seglen, p );
		if ( len < 0 || (UINT)len >= (sizeof(curBuf) - pre) )
		{
			DEBUG( "[ERR] Path build err! [utils_mkdirs]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
			return FR_INVALID_PARAMETER;
		}
		pre += len;

		res = f_stat( curBuf, &fno );
		if ( res == FR_NO_FILE )
		{
			/* Create. */
			res = f_mkdir( curBuf );
			if ( res != FR_OK )
			{
				DEBUG( "[ERR] Dir create err! [utils_mkdirs]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
				return res;
			}
		}
		else if ( res != FR_OK )
		{
			DEBUG( "[ERR] Dir create err! [utils_mkdirs]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
			return res;
		}
		else if ( !(fno.fattrib & AM_DIR) )
		{
			/* This positiion has been used by file. */
			return FR_EXIST;
		}

		if ( !slash )
			break;
		p = slash + 1;
	}

	return FR_OK;
}


/**
 * @brief  Delete callback for utils_dir_remove_recursive(): unlinks the
 *         current entry. A failure stops the whole walk immediately.
 * @param  path  Full path of the entry.
 * @param  fi    Entry information (name used in the error log).
 * @param  arg   Unused.
 * @retval FR_OK on success, otherwise the f_unlink error code.
 */
static FRESULT 
remove_cb( const TCHAR *path, const FILINFO *fi, void *arg )
{
	if ( !path || !fi )
		return FR_INVALID_PARAMETER;

	FRESULT res;
	res = f_unlink( path ); 
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Delete err with [utils_dir_remove_recursive]  PATH:%s  LINE:%d. FILE: %s\n", __FILE__, __LINE__, fi->fname );
		return res;
	}

	return FR_OK;
}


/**
 * @brief  Recursively delete a directory and all of its contents
 *         (files and subdirectories). The post-order walk guarantees the
 *         directory itself is empty when it is removed last. Deleting the
 *         volume root (e.g. "0:/") is rejected on purpose.
 * @param  path  Directory to delete (absolute path).
 * @retval FR_OK on success, otherwise a FatFs error code.
 */
FRESULT 
utils_dir_remove_recursive( const TCHAR *path )
{
	if ( !path )
		return FR_INVALID_PARAMETER;

	const TCHAR *p = strchr( path, ':' );
	if ( p && ( p[1] == '\0' || ( p[1] == '/' && p[2] == '\0' ) ) )
	{
		DEBUG( "[ERR] You can't use this API to delete root! [utils_dir_remove_recursive]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return FR_INVALID_PARAMETER;
	}

	FRESULT res;
	res = utils_dir_walk( path, remove_cb, NULL );
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Dir walk err! [utils_dir_remove_recursive]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return res;
	}

	/* Delete directory itself. */
	res = f_unlink( path );
	if ( res != FR_OK )
	{
		/* Still has remain file? Inform and Return. */
		DEBUG( "[ERR] Dir delete err [utils_dir_remove_recursive]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		return res;
	}

	return FR_OK;
}


/**
 * @brief  Copy a file using a streaming fixed-size buffer (allocated with
 *         UTILS_MALLOC, falling back to smaller sizes if memory is tight).
 *         The destination is overwritten if it exists; the copy is verified
 *         by comparing destination and source sizes afterwards.
 * @param  fsrc  File object used for the source file.
 * @param  src   Source path, e.g. "0:/a.dat".
 * @param  fdst  File object used for the destination file.
 * @param  dest  Destination path, e.g. "0:/b.dat".
 * @retval FR_OK on success, otherwise a FatFs error code.
 */
FRESULT 
utils_file_copy( FIL *fsrc, const TCHAR *src, FIL *fdst, const TCHAR *dest )
{
	if ( !fsrc || !fdst || !src || !dest )
		return FR_INVALID_PARAMETER;

	if ( strcmp( src, dest ) == 0 )
		return FR_INVALID_PARAMETER;

	UINT try[] = { 32, 16, 8, 4 }; 
	UINT bufSize, br, bw;
	FILINFO fno;
	DWORD fsize;
	BYTE isMallocFail = 0;
	BYTE count = 0;
	BYTE *buf = NULL;
	FRESULT res;
	do 
	{
		buf = UTILS_MALLOC( (try[count] * 1024) );
		if ( !buf )
		{
			isMallocFail = 1;
			DEBUG( "[WARN] Copy buf malloc fail. try malloc %d kb. [utils_file_copy]  PATH:%s  LINE:%d.\n", try[count], __FILE__, __LINE__ );
			count++;
			if ( count >= 4 )	
			{
				DEBUG( "[ERR] Copy buf malloc fail... [utils_file_copy]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
				return FR_NOT_ENOUGH_CORE;
			}

			continue;
		}

		isMallocFail = 0;
		bufSize = (try[count] * 1024);
	} while( isMallocFail );

	res = f_open( fsrc, src, FA_OPEN_EXISTING | FA_READ );
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Srouce file open err. Plz check. [utils_file_copy]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		goto aborted;
	}

	res = f_stat( src, &fno );
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Srouce file attribute get err. Plz check. [utils_file_copy]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		goto aborted;
	}
	fsize = fno.fsize;

	res = f_open( fdst, dest, FA_CREATE_ALWAYS | FA_WRITE );
	if ( res != FR_OK )
	{
		DEBUG( "[ERR] Copy file create err. Plz check. [utils_file_copy]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
		goto aborted;
	}

	#if (FF_USE_EXPAND)
	res = f_expand( fdst, fno.fsize, 1 );
	if ( res == FR_DENIED )
		DEBUG( "[WARN] Expand file err. continue with default. [utils_file_copy]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
	#endif /* FF_USE_EXPAND */

	while( 1 )
	{
		res = f_read( fsrc, buf, bufSize, &br );
		if ( res != FR_OK )
		{
			DEBUG( "[ERR] Srouce file read err. Plz check. [utils_file_copy]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
			goto aborted;
		}

		/* End of File. */
		if ( br == 0 )
			break;

		res = f_write( fdst, buf, br, &bw );
		if ( res != FR_OK || bw != br )
		{
			DEBUG( "[ERR] No Space. [utils_file_copy]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
			goto aborted;
		}
	}

	f_sync( fdst );
	f_close( fsrc );
	f_close( fdst );

	/* Verify: destination size must match source size. */
	res = f_stat( dest, &fno );
	if ( res != FR_OK || fno.fsize != fsize )
	{
		DEBUG( "[ERR] Size mismatch: src %lu dst %lu with [utils_file_copy]  PATH:%s  LINE:%d.\n",
				(unsigned long)fsize, (unsigned long)fno.fsize, __FILE__, __LINE__ );
		/* Delete the broke file. */
		f_unlink( dest );      
		return FR_DENIED;
	}

	
	DEBUG( "[INFO] Copy OK! [utils_file_copy]\n"
			"  Dst : %s\n"
			"  Size: %lu B\n"
			"  Attr: %c%c%c%c\n"
			"  Time: %04u-%02u-%02u %02u:%02u:%02u\n",
			dest,
			(unsigned long)fno.fsize,
			(fno.fattrib & AM_RDO) ? 'R' : '-',
			(fno.fattrib & AM_HID) ? 'H' : '-',
			(fno.fattrib & AM_SYS) ? 'S' : '-',
			(fno.fattrib & AM_ARC) ? 'A' : '-',
			(unsigned)(1980 + ((fno.fdate >> 9) & 0x7F)),
			(unsigned)((fno.fdate >> 5) & 0x0F),
			(unsigned)(fno.fdate & 0x1F),
			(unsigned)((fno.ftime >> 11) & 0x1F),
			(unsigned)((fno.ftime >> 5) & 0x3F),
			(unsigned)((fno.ftime & 0x1F) * 2) );

	UTILS_FREE( buf );
	return FR_OK;

aborted:
	UTILS_FREE( buf );
	f_close( fsrc );
	f_close( fdst );
	DEBUG( "[INFO] Copy file aborted. [utils_file_copy]  PATH:%s  LINE:%d.\n", __FILE__, __LINE__ );
	return res;
}

