/**
 * @file    ini_parser.h
 * @brief   Public interface of the lightweight INI configuration module.
 *
 *          Defines the storage structures (sections + key/value entries),
 *          the access / file I/O functions and the module error codes.
 */

#ifndef __INI_PARSER_H__
#define __INI_PARSER_H__

#include "ff.h"		/* Your FileSystem. */

#include <stdint.h>

/* *************************************** */
#define INI_SEC_LEN				(16u)
#define INI_KEY_LEN				(16u)
#define INI_VAL_LEN				(24u)
#define INI_ENTRY_MAX			(32u)

#define LOAD_BUF				(2048u)
/* *************************************** */

#define INI_MALLOC(size)  malloc(size)
#define INI_FREE(ptr)     free(ptr)

typedef enum
{
	INI_OK            =  0,
	INI_ERR_PARAM     = -1,
	INI_ERR_OPEN      = -2,
	INI_ERR_MEM       = -3,
	INI_ERR_READ      = -4,
	INI_ERR_WRITE     = -5,
	INI_ERR_SIZE      = -6,
	INI_ERR_PARSE     = -7,
	INI_ERR_SERIALIZE = -8,
	INI_ERR_FULL      = -9,
	INI_ERR_INVALID   = -10,

} ini_err_t;

typedef struct 
{
	char section[INI_SEC_LEN];
	char key[INI_KEY_LEN];
	char value[INI_VAL_LEN];

} ini_entry_t;


typedef struct 
{
	ini_entry_t entries[INI_ENTRY_MAX];
	uint32_t count;

} ini_t;


const char *ini_get_str( const ini_t *cfg, const char *sec, const char *key );
int8_t ini_str_2_int( const char *str, uint64_t *out );

/* ******************** Parse & Output ******************** */
int8_t ini_parse( const char *buf, uint32_t len, ini_t *cfg );
int8_t ini_set_str( ini_t *cfg, const char *sec, const char *key, const char *value );
int8_t ini_serialize( char *buf, uint32_t bufLen, uint32_t *olen, const ini_t *cfg );

/* ******************** FILE ******************** */
int8_t ini_load( const char *path, ini_t *cfg );
int8_t ini_save( const char *path, const ini_t *cfg );


#endif /* __INI_PARSER_H__ */

