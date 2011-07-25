/*-
 * Copyright (c) 1999 James Howard and Dag-Erling Coïdan Smørgrav
 * Copyright (c) 2008-2011 Gabor Kovesdan <gabor@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef FASTMATCH_H
#define FASTMATCH_H 1

#include <limits.h>
#include <stdbool.h>

#include "hashtable.h"
#include "tre.h"
#include "tre-internal.h"

typedef struct {
  size_t wlen;
  size_t len;
  tre_char_t *wpattern;
  char *pattern;
#ifdef TRE_WCHAR
  int defBc;
  hashtable *qsBc_table;
#endif
  int qsBc[UCHAR_MAX + 1];
  /* flags */
  bool bol;
  bool eol;
  bool reversed;
  bool word;
} fastmatch_t;

int	tre_fastcomp_literal(fastmatch_t *preg, const tre_char_t *regex,
	    size_t);
int	tre_fastcomp(fastmatch_t *preg, const tre_char_t *regex, size_t);
int	tre_fastexec(const fastmatch_t *fg, const void *data, size_t len,
	    tre_str_type_t type, int nmatch, regmatch_t pmatch[]);
void	tre_fastfree(fastmatch_t *preg);

#endif		/* FASTMATCH_H */
