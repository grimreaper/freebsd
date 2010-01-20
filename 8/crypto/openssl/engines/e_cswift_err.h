/* ====================================================================
 * Copyright (c) 2001 The OpenSSL Project.  All rights reserved.
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

#ifndef HEADER_CSWIFT_ERR_H
#define HEADER_CSWIFT_ERR_H

#ifdef  __cplusplus
extern "C" {
#endif

/* BEGIN ERROR CODES */
/* The following lines are auto generated by the script mkerr.pl. Any changes
 * made after this point may be overwritten when the script is next run.
 */
static void ERR_load_CSWIFT_strings(void);
static void ERR_unload_CSWIFT_strings(void);
static void ERR_CSWIFT_error(int function, int reason, char *file, int line);
#define CSWIFTerr(f,r) ERR_CSWIFT_error((f),(r),__FILE__,__LINE__)

/* Error codes for the CSWIFT functions. */

/* Function codes. */
#define CSWIFT_F_CSWIFT_CTRL				 100
#define CSWIFT_F_CSWIFT_DSA_SIGN			 101
#define CSWIFT_F_CSWIFT_DSA_VERIFY			 102
#define CSWIFT_F_CSWIFT_FINISH				 103
#define CSWIFT_F_CSWIFT_INIT				 104
#define CSWIFT_F_CSWIFT_MOD_EXP				 105
#define CSWIFT_F_CSWIFT_MOD_EXP_CRT			 106
#define CSWIFT_F_CSWIFT_RAND_BYTES			 108
#define CSWIFT_F_CSWIFT_RSA_MOD_EXP			 107

/* Reason codes. */
#define CSWIFT_R_ALREADY_LOADED				 100
#define CSWIFT_R_BAD_KEY_SIZE				 101
#define CSWIFT_R_BN_CTX_FULL				 102
#define CSWIFT_R_BN_EXPAND_FAIL				 103
#define CSWIFT_R_CTRL_COMMAND_NOT_IMPLEMENTED		 104
#define CSWIFT_R_MISSING_KEY_COMPONENTS			 105
#define CSWIFT_R_NOT_LOADED				 106
#define CSWIFT_R_REQUEST_FAILED				 107
#define CSWIFT_R_UNIT_FAILURE				 108

#ifdef  __cplusplus
}
#endif
#endif
