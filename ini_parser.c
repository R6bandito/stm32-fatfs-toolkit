/**
 * @file    ini_parser.c
 * @brief   Lightweight INI configuration module: parse / serialize an
 *          in-memory key-value store, plus file I/O on top of FatFs.
 *
 *          Design notes:
 *          - The core (ini_parse / ini_serialize) is pure memory and has
 *            no file-system dependency; ini_load / ini_save are thin
 *            wrappers that can be re-targeted by rewriting those two only.
 *          - All values are stored as strings; numeric interpretation is
 *            done at the access layer (ini_str_2_int etc.).
 *          - Buffers are allocated through INI_MALLOC / INI_FREE
 *            (overridable, default to malloc / free).
 */

#include "ini_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ********************* FILE ********************* */
FIL g_fs;

/**
 * @brief  Load an INI file from storage into an ini_t structure.
 *         Reads the whole file (bounded by INI_FILE_MAX), parses it and
 *         fills @p cfg. The file must not exceed the load buffer.
 * @param  path  Path of the INI file, e.g. "0:/Config.ini".
 * @param  cfg   [out] Filled configuration structure.
 * @retval INI_OK on success, otherwise an ini_err_t code.
 */
int8_t 
ini_load( const char *path, ini_t *cfg )
{
	if ( !path || !cfg )
		return INI_ERR_PARAM;

    FRESULT res;
	int8_t  ret;
    UINT    br;
	BYTE 	*buf = NULL;
    res = f_open( &g_fs, path, FA_OPEN_EXISTING | FA_READ );
    if ( res != FR_OK )
	{
		ret = INI_ERR_OPEN;
		goto ERR;
	}

	buf = (BYTE *)INI_MALLOC( LOAD_BUF );
	if ( !buf )
	{
		ret = INI_ERR_MEM;
		goto ERR;
	}

	res = f_read( &g_fs, buf, LOAD_BUF, &br );
	if ( res != FR_OK )
	{
		ret = INI_ERR_READ;
		goto ERR;
	}

	if ( f_eof( &g_fs ) )
	{
		/* The config file has been read successfully. */
		f_close( &g_fs );
		ini_parse( (const char *)buf, br, cfg );
		INI_FREE( buf );
		return INI_OK;
	}

	ret = INI_ERR_READ;

ERR:
	f_close( &g_fs );
	if ( buf ) INI_FREE( buf );
	return ret;
}

/**
 * @brief  Serialize an ini_t structure and write it to a file
 *         (the file is created or overwritten).
 * @param  path  Path of the INI file, e.g. "0:/Config.ini".
 * @param  cfg   Configuration structure to save.
 * @retval INI_OK on success, otherwise an ini_err_t code.
 */
int8_t 
ini_save( const char *path, const ini_t *cfg )
{
    if ( !path || !cfg )
		return INI_ERR_PARAM;

	FRESULT res;
	int8_t 	ret;
	BYTE 	*buf;
	UINT	bw;
	UINT 	bufLen, olen;

	res = f_open( &g_fs, path, FA_CREATE_ALWAYS | FA_WRITE );
	if ( res != FR_OK )
	{
		ret = INI_ERR_OPEN;
		goto ERR;
	}

	bufLen = sizeof(ini_t);
	buf = (BYTE *)INI_MALLOC( bufLen );
	if ( !buf )
	{
		ret = INI_ERR_MEM;
		goto ERR;
	}

	if ( ini_serialize( (char *)buf, bufLen, &olen, cfg ) == 0 )
	{
		res = f_write( &g_fs, buf, olen, &bw );
		if ( res != FR_OK )
		{
			ret = INI_ERR_WRITE;
			goto ERR;
		}

		if ( olen != bw )
		{
			ret = INI_ERR_WRITE;
			goto ERR;
		}

		f_close( &g_fs );
		INI_FREE( buf );
		return INI_OK;
	}

	ret = INI_ERR_SERIALIZE;

ERR:
	f_close( &g_fs );
	if ( buf ) INI_FREE( buf );
	return ret;
}
/* ********************* FILE ********************* */


/**
 * @brief  Strip leading/trailing spaces, tabs and trailing CR in place.
 * @param  s  NUL-terminated string to trim.
 * @retval Pointer to the trimmed string (inside @p s).
 */
static char *
ini_trim( char *s )
{
	while ( *s == ' ' || *s == '\t' )
		s++;

	size_t n = strlen( s );
	while ( n > 0 && ( s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' ) )
		s[--n] = '\0';

	return s;
}


/**
 * @brief  Set a key/value entry: overwrites an existing entry with the
 *         same section + key, or appends a new one (count managed here).
 * @param  cfg    Target configuration structure.
 * @param  sec    Section name; empty string means the global section.
 * @param  key    Key name.
 * @param  value  Value to store (truncated to INI_VAL_LEN-1).
 * @retval INI_OK on success, INI_ERR_FULL when INI_ENTRY_MAX is reached.
 */
int8_t 
ini_set_str( ini_t *cfg, const char *sec, const char *key, const char *value )
{
	if ( !cfg || !sec || !key || !value )
		return INI_ERR_PARAM;

	/* Same section + key already exists: overwrite the value. */
	for ( uint32_t i = 0; i < cfg->count; i++ )
	{
		if ( strcmp( cfg->entries[i].section, sec ) == 0 &&
				strcmp( cfg->entries[i].key, key ) == 0 )
		{
			strncpy( cfg->entries[i].value, value, INI_VAL_LEN - 1 );
			cfg->entries[i].value[INI_VAL_LEN - 1] = '\0';
			return INI_OK;
		}
	}

	/* New entry: append (count managed automatically). */
	if ( cfg->count >= INI_ENTRY_MAX )
		return INI_ERR_FULL;

	ini_entry_t *e = &cfg->entries[cfg->count++];
	strncpy( e->section, sec, INI_SEC_LEN - 1 );
	e->section[INI_SEC_LEN - 1] = '\0';
	strncpy( e->key, key, INI_KEY_LEN - 1 );
	e->key[INI_KEY_LEN - 1] = '\0';
	strncpy( e->value, value, INI_VAL_LEN - 1 );
	e->value[INI_VAL_LEN - 1] = '\0';

	return INI_OK;
}


/**
 * @brief  Parse INI text into an ini_t structure (core, storage
 *         independent). Handles comments (';' '#'), section headers,
 *         key=value lines (split at the first '='), whitespace trimming
 *         and duplicate-key overwrite. Malformed lines are skipped.
 * @param  buf  INI text buffer.
 * @param  len  Length of the text in bytes.
 * @param  cfg  [out] Filled configuration structure.
 * @retval INI_OK on success, INI_ERR_FULL when INI_ENTRY_MAX is reached.
 */
int8_t 
ini_parse( const char *buf, uint32_t len, ini_t *cfg )
{
	if ( !buf || !cfg )
			return INI_ERR_PARAM;

	cfg->count = 0;
	char cur_sec[INI_SEC_LEN] = { 0 };
	char line[128] = { 0 };
	uint32_t i = 0;

	while ( i < len )
	{
		/* Extract one line (up to '\n'). */
		uint32_t start = i;
		while ( i < len && buf[i] != '\n' )
			i++;
		uint32_t linelen = i - start;

		/* skip '\n'. */
		if ( i < len )
			i++;                   

		if ( linelen >= sizeof(line) )
				linelen = sizeof(line) - 1;
		memcpy( line, buf + start, linelen );
		line[linelen] = '\0';

		char *p = ini_trim( line );

		/* Empty line or comment. */
		if ( *p == '\0' || *p == ';' || *p == '#' )
			continue;

		/* Section header. */
		if ( *p == '[' )
		{
			char *close = strchr( p, ']' );
			if ( !close )
				continue;       /* malformed, skip */

			*close = '\0';
			char *sec = ini_trim( p + 1 );
			strncpy( cur_sec, sec, INI_SEC_LEN - 1 );
			cur_sec[INI_SEC_LEN - 1] = '\0';
			continue;
		}

		/* Key = value : split at the FIRST '='. */
		char *eq = strchr( p, '=' );
		if ( !eq )
			continue;               /* no '=', skip */

		*eq = '\0';
		char *key = ini_trim( p );
		char *val = ini_trim( eq + 1 );
		if ( *key == '\0' )
			continue;               /* empty key, skip */

		/* Truncate to field limits. */
		if ( strlen( key ) > INI_KEY_LEN - 1 )
			key[INI_KEY_LEN - 1] = '\0';
		if ( strlen( val ) > INI_VAL_LEN - 1 )
			val[INI_VAL_LEN - 1] = '\0';

		/* Find existing entry (same section + key) -> overwrite. */
		uint32_t idx;
		int found = 0;
		for ( idx = 0; idx < cfg->count; idx++ )
		{
			if ( strcmp( cfg->entries[idx].section, cur_sec ) == 0 &&
					strcmp( cfg->entries[idx].key, key ) == 0 )
			{
				found = 1;
				break;
			}
		}

		if ( !found )
		{
			if ( cfg->count >= INI_ENTRY_MAX )
				return INI_ERR_FULL;

			idx = cfg->count++;
			strncpy( cfg->entries[idx].section, cur_sec, INI_SEC_LEN - 1 );
			cfg->entries[idx].section[INI_SEC_LEN - 1] = '\0';
			strncpy( cfg->entries[idx].key, key, INI_KEY_LEN - 1 );
			cfg->entries[idx].key[INI_KEY_LEN - 1] = '\0';
		}

		strncpy( cfg->entries[idx].value, val, INI_VAL_LEN - 1 );
		cfg->entries[idx].value[INI_VAL_LEN - 1] = '\0';
	}

	return INI_OK;
}





/**
 * @brief  Serialize an ini_t structure into a text buffer (core, storage
 *         independent). Section headers are emitted when the section
 *         changes; the global section has no header.
 * @param  buf     Output text buffer.
 * @param  bufLen  Size of the output buffer.
 * @param  olen    [out] Number of bytes written.
 * @param  cfg     Configuration structure to serialize.
 * @retval INI_OK on success, INI_ERR_SERIALIZE if the buffer overflows.
 */
int8_t 
ini_serialize( char *buf, uint32_t bufLen, uint32_t *olen, const ini_t *cfg )
{
	if ( !buf || !olen || !cfg )
			return INI_ERR_PARAM;

	uint32_t used = 0;
	char cur_sec[INI_SEC_LEN] = { 0 };

	for ( uint32_t i = 0; i < cfg->count; i++ )
	{
		const ini_entry_t *e = &cfg->entries[i];

		/* Section changed: emit a header line (skip global section). */
		if ( strcmp( e->section, cur_sec ) != 0 )
		{
			strncpy( cur_sec, e->section, INI_SEC_LEN - 1 );
			cur_sec[INI_SEC_LEN - 1] = '\0';

			if ( cur_sec[0] != '\0' )
			{
				int n = snprintf( buf + used, bufLen - used, "[%s]\r\n", cur_sec );
				if ( n < 0 || (uint32_t)n >= bufLen - used )
						return INI_ERR_SERIALIZE;
				used += (uint32_t)n;
			}
		}

		int n = snprintf( buf + used, bufLen - used, "%s=%s\r\n", e->key, e->value );
		if ( n < 0 || (uint32_t)n >= bufLen - used )
			return INI_ERR_SERIALIZE;
		used += (uint32_t)n;
	}

	*olen = used;
	return INI_OK;
}


/**
 * @brief  Look up a value by section and key.
 * @param  cfg  Configuration structure.
 * @param  sec  Section name; empty string means the global section.
 * @param  key  Key name.
 * @retval Pointer to the stored value string, or NULL if not found.
 */
const char *
ini_get_str( const ini_t *cfg, const char *sec, const char *key )
{
	if ( !cfg || !sec || !key )
		return NULL;

	for( uint8_t idx = 0; idx < cfg->count; idx++ )
	{
		const ini_entry_t *e = &cfg->entries[idx];
		if ( strcmp( e->section, sec ) == 0 && 
				strcmp( e->key, key ) == 0 )
		{
			/* Successfully find relavent entry. */
			return (const char *)e->value;
		}
	}

	return NULL;
}


/**
 * @brief  Strictly convert a decimal string to uint64_t. The whole
 *         string must consist of digits, otherwise the conversion fails
 *         (no partial parse, no sign support).
 * @param  str  Input string, e.g. "4578124".
 * @param  out  [out] Parsed value on success.
 * @retval INI_OK on success, INI_ERR_INVALID on failure.
 */
int8_t 
ini_str_2_int( const char *str, uint64_t *out )
{
	if ( !str || !out )
		return INI_ERR_PARAM;

	const char *p = str;
	uint64_t num = 0;

	if ( *p == '\0' )                      /* empty string */
		return INI_ERR_INVALID;

	while ( *p >= '0' && *p <= '9' )
	{
		uint64_t new_num = num * 10 + (uint64_t)( *p - '0' );
		if ( new_num < num )            /* overflow */
			return INI_ERR_INVALID;
		num = new_num;
		p++;
	}

	if ( *p != '\0' )                      /* trailing non-digit */
		return INI_ERR_INVALID;

	*out = num;
	return INI_OK;
}


