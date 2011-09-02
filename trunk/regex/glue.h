/* $FreeBSD$ */

#ifndef GLUE_H
#define GLUE_H

#include <limits.h>
#undef RE_DUP_MAX
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>

#define TRE_WCHAR			1
#define TRE_MULTIBYTE			1

#define TRE_CHAR(n) L##n

#define tre_char_t			wchar_t
#define tre_mbrtowc(pwc, s, n, ps)	(mbrtowc((pwc), (s), (n), (ps)))
#define tre_strlen			wcslen
#define tre_isspace			iswspace
#define tre_isalnum			iswalnum

#define REG_LITERAL			0020
#define REG_WORD			0100
#define REG_GNU				0400
#define _REG_HEUR			01000

#define REG_OK				0

#define TRE_MB_CUR_MAX			MB_CUR_MAX

#ifndef _GREP_DEBUG
#define DPRINT(msg)
#else			
#define DPRINT(msg) do {printf msg; fflush(stdout);} while(/*CONSTCOND*/0)
#endif

#define MIN(a,b)			((a > b) ? (b) : (a))
#define MAX(a,b)			((a > b) ? (a) : (b))

typedef enum { STR_WIDE, STR_BYTE, STR_MBS, STR_USER } tre_str_type_t;
#endif
