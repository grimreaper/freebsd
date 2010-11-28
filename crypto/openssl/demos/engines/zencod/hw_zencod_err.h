/* ====================================================================
 * Copyright (c) 2001-2002 The OpenSSL Project.  All rights reserved.
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
 *    for use in the OpenSSL Toolkit. (http://www.openssl.org/)"
 *
 * 4. The names "OpenSSL Toolkit" and "OpenSSL Project" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    openssl-core@openssl.org.
 *
 * 5. Products derived from this software may not be called "OpenSSL"
 *    nor may "OpenSSL" appear in their names without prior written
 *    permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit (http://www.openssl.org/)"
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

#ifndef HEADER_ZENCOD_ERR_H
#define HEADER_ZENCOD_ERR_H

#ifdef  __cplusplus
extern "C" {
#endif

/* BEGIN ERROR CODES */
/* The following lines are auto generated by the script mkerr.pl. Any changes
 * made after this point may be overwritten when the script is next run.
 */
static void ERR_load_ZENCOD_strings(void);
static void ERR_unload_ZENCOD_strings(void);
static void ERR_ZENCOD_error(int function, int reason, char *file, int line);
#define ZENCODerr(f,r) ERR_ZENCOD_error((f),(r),__FILE__,__LINE__)

/* Error codes for the ZENCOD functions. */

/* Function codes. */
#define ZENCOD_F_ZENCOD_BN_MOD_EXP			 100
#define ZENCOD_F_ZENCOD_CTRL				 101
#define ZENCOD_F_ZENCOD_DH_COMPUTE			 102
#define ZENCOD_F_ZENCOD_DH_GENERATE			 103
#define ZENCOD_F_ZENCOD_DSA_DO_SIGN			 104
#define ZENCOD_F_ZENCOD_DSA_DO_VERIFY			 105
#define ZENCOD_F_ZENCOD_FINISH				 106
#define ZENCOD_F_ZENCOD_INIT				 107
#define ZENCOD_F_ZENCOD_RAND				 108
#define ZENCOD_F_ZENCOD_RSA_MOD_EXP			 109
#define ZENCOD_F_ZENCOD_RSA_MOD_EXP_CRT			 110

/* Reason codes. */
#define ZENCOD_R_ALREADY_LOADED				 100
#define ZENCOD_R_BAD_KEY_COMPONENTS			 101
#define ZENCOD_R_BN_EXPAND_FAIL				 102
#define ZENCOD_R_CTRL_COMMAND_NOT_IMPLEMENTED		 103
#define ZENCOD_R_DSO_FAILURE				 104
#define ZENCOD_R_NOT_LOADED				 105
#define ZENCOD_R_REQUEST_FAILED				 106
#define ZENCOD_R_UNIT_FAILURE				 107

#ifdef  __cplusplus
}
#endif
#endif
