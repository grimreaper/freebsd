/* crypto/objects/obj_err.c */
/* ====================================================================
 * Copyright (c) 1999-2006 The OpenSSL Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit. (http://www.OpenSSL.org/)"
 *
 * 4. The names "OpenSSL Toolkit" and "OpenSSL Project" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    openssl-core@OpenSSL.org.
 *
 * 5. Products derived from this software may not be called "OpenSSL"
 *    nor may "OpenSSL" appear in their names without prior written
 *    permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit (http://www.OpenSSL.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE OpenSSL PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE OpenSSL PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This product includes cryptographic software written by Eric Young
 * (eay@cryptsoft.com).  This product includes software written by Tim
 * Hudson (tjh@cryptsoft.com).
 *
 */

/* NOTE: this file was auto generated by the mkerr.pl script: any changes
 * made to it will be overwritten when the script next updates this file,
 * only reason strings will be preserved.
 */

#include <stdio.h>
#include <openssl/err.h>
#include <openssl/objects.h>

/* BEGIN ERROR CODES */
#ifndef OPENSSL_NO_ERR

#define ERR_FUNC(func) ERR_PACK(ERR_LIB_OBJ,func,0)
#define ERR_REASON(reason) ERR_PACK(ERR_LIB_OBJ,0,reason)

static ERR_STRING_DATA OBJ_str_functs[]=
	{
{ERR_FUNC(OBJ_F_OBJ_ADD_OBJECT),	"OBJ_add_object"},
{ERR_FUNC(OBJ_F_OBJ_CREATE),	"OBJ_create"},
{ERR_FUNC(OBJ_F_OBJ_DUP),	"OBJ_dup"},
{ERR_FUNC(OBJ_F_OBJ_NAME_NEW_INDEX),	"OBJ_NAME_new_index"},
{ERR_FUNC(OBJ_F_OBJ_NID2LN),	"OBJ_nid2ln"},
{ERR_FUNC(OBJ_F_OBJ_NID2OBJ),	"OBJ_nid2obj"},
{ERR_FUNC(OBJ_F_OBJ_NID2SN),	"OBJ_nid2sn"},
{0,NULL}
	};

static ERR_STRING_DATA OBJ_str_reasons[]=
	{
{ERR_REASON(OBJ_R_MALLOC_FAILURE)        ,"malloc failure"},
{ERR_REASON(OBJ_R_UNKNOWN_NID)           ,"unknown nid"},
{0,NULL}
	};

#endif

void ERR_load_OBJ_strings(void)
	{
#ifndef OPENSSL_NO_ERR

	if (ERR_func_error_string(OBJ_str_functs[0].error) == NULL)
		{
		ERR_load_strings(0,OBJ_str_functs);
		ERR_load_strings(0,OBJ_str_reasons);
		}
#endif
	}
