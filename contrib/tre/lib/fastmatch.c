/*-
 * Copyright (c) 1999 James Howard and Dag-Erling Coïdan Smørgrav
 * Copyright (C) 2008-2011 Gabor Kovesdan <gabor@FreeBSD.org>
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "fastmatch.h"
#include "hashtable.h"
#include "tre.h"
#include "tre-internal.h"
#include "xmalloc.h"

static int	fastcmp(const void *, const void *, size_t,
			tre_str_type_t, bool);

/*
 * We will work with wide characters if they are supported
 */
#ifdef TRE_WCHAR
#define TRE_CHAR(n)	L##n
#else
#define TRE_CHAR(n)	n
#endif

/*
 * Skips n characters in the input string and assigns the start
 * address to startptr. Note: as per IEEE Std 1003.1-2008
 * matching is based on bit pattern not character representations
 * so we can handle MB strings as byte sequences just like
 * SB strings.
 */
#define SKIP_CHARS(n)						\
  do {								\
    switch (type)						\
      {								\
	case STR_WIDE:						\
	  startptr = str_wide + n;				\
	  break;						\
	default:						\
	  startptr = str_byte + n;				\
      }								\
  } while (0);							\

/*
 * Converts the wide string pattern to SB/MB string and stores
 * it in fg->pattern. Sets fg->len to the byte length of the
 * converted string.
 */
#define STORE_MBS_PAT						\
  do {								\
    size_t siz;							\
								\
    siz = wcstombs(NULL, fg->wpattern, 0);			\
    if (siz == (size_t)-1)					\
      return REG_BADPAT;					\
    fg->len = siz;						\
    fg->pattern = xmalloc(siz + 1);				\
    if (fg->pattern == NULL)					\
      return REG_ESPACE;					\
    wcstombs(fg->pattern, fg->wpattern, siz);			\
    fg->pattern[siz] = '\0';					\
  } while (0);							\

/*
 * Compares the pattern to the input string at the position
 * stored in startptr.
 */
#define COMPARE								\
  switch (type)								\
    {									\
      case STR_WIDE:							\
	mismatch = fastcmp(fg->wpattern, startptr, fg->wlen, type,	\
			   fg->icase);					\
	break;								\
      default:								\
	mismatch = fastcmp(fg->pattern, startptr, fg->len, type,	\
			   fg->icase);					\
      }									\

#define IS_OUT_OF_BOUNDS						\
  ((type == STR_WIDE) ? ((j + fg->wlen) > len) : ((j + fg->len) > len))

/*
 * Checks whether the new position after shifting in the input string
 * is out of the bounds and break out from the loop if so.
 */
#define CHECKBOUNDS							\
  if (IS_OUT_OF_BOUNDS)							\
    break;								\

/*
 * Shifts in the input string after a mismatch. The position of the
 * mismatch is stored in the mismatch variable.
 */
#define SHIFT								\
  CHECKBOUNDS;								\
									\
  {									\
    int bc = 0, gs = 0, ts, r = -1;					\
									\
    switch (type)							\
      {									\
	case STR_WIDE:							\
	  if (!fg->hasdot)						\
	    {								\
	      if (u != 0 && mismatch == fg->wlen - 1 - shift)		\
		mismatch -= u;						\
	      v = fg->wlen - 1 - mismatch;				\
	      r = hashtable_get(fg->qsBc_table,				\
		&((wchar_t *)startptr)[mismatch + 1], &bc);		\
	      gs = fg->bmGs[mismatch];					\
	    }								\
	    bc = (r == 0) ? bc : fg->defBc;				\
            break;							\
	default:							\
	  if (!fg->hasdot)						\
	    {								\
	      if (u != 0 && mismatch == fg->len - 1 - shift)		\
		mismatch -= u;						\
	      v = fg->len - 1 - mismatch;				\
	      gs = fg->sbmGs[mismatch];					\
	    }								\
	  bc = fg->qsBc[((unsigned char *)startptr)[mismatch + 1]];	\
      }									\
    if (fg->hasdot)							\
      shift = bc;							\
    else								\
      {									\
	ts = u - v;							\
	shift = MAX(ts, bc);						\
	shift = MAX(shift, gs);						\
	if (shift == gs)						\
	  u = MIN((type == STR_WIDE ? fg->wlen : fg->len) - shift, v);	\
	else								\
	  {								\
	    if (ts < bc)						\
	      shift = MAX(shift, u + 1);				\
	    u = 0;							\
	  }								\
      }									\
      j += shift;							\
  }

/*
 * Normal Quick Search would require a shift based on the position the
 * next character after the comparison is within the pattern.  With
 * wildcards, the position of the last dot effects the maximum shift
 * distance.
 * The closer to the end the wild card is the slower the search.
 *
 * Examples:
 * Pattern    Max shift
 * -------    ---------
 * this               5
 * .his               4
 * t.is               3
 * th.s               2
 * thi.               1
 */

/*
 * Fills in the bad character shift array for SB/MB strings.
 */
#define FILL_QSBC							\
  for (unsigned int i = 0; i <= UCHAR_MAX; i++)				\
    fg->qsBc[i] = fg->len - fg->hasdot;					\
  for (int i = fg->hasdot + 1; i < fg->len; i++)			\
    {									\
      fg->qsBc[(unsigned)fg->pattern[i]] = fg->len - i;			\
      if (fg->icase)							\
        {								\
          char c = islower(fg->pattern[i]) ? toupper(fg->pattern[i])	\
            : tolower(fg->pattern[i]);					\
          fg->qsBc[(unsigned)c] = fg->len - i;				\
        }								\
    }

/*
 * Fills in the bad character shifts into a hastable for wide strings.
 * With wide characters it is not possible any more to use a normal
 * array because there are too many characters and we could not
 * provide enough memory. Fortunately, we only have to store distinct
 * values for so many characters as the number of distinct characters
 * in the pattern, so we can store them in a hashtable and store a
 * default shift value for the rest.
 */
#define FILL_QSBC_WIDE							\
  /* Adjust the shift based on location of the last dot ('.'). */	\
  fg->defBc = fg->wlen - fg->hasdot;					\
									\
  /* Preprocess pattern. */						\
  fg->qsBc_table = hashtable_init(fg->wlen * 4, sizeof(tre_char_t),	\
    sizeof(int));							\
  for (unsigned int i = fg->hasdot + 1; i < fg->wlen; i++)		\
  {									\
    int k = fg->wlen - i;						\
    hashtable_put(fg->qsBc_table, &fg->wpattern[i], &k);		\
    if (fg->icase)							\
      {									\
	wint_t wc = iswlower(fg->wpattern[i]) ?				\
	  towupper(fg->wpattern[i]) : towlower(fg->wpattern[i]);	\
	hashtable_put(fg->qsBc_table, &wc, &k);				\
      }									\
  }									\

/*
 * Fills in the good suffix table for SB/MB strings.
 */
#define FILL_BMGS							\
  if (!fg->hasdot)							\
    _FILL_BMGS(fg->sbmGs, fg->pattern, fg->len, false);

/*
 * Fills in the good suffix table for wide strings.
 */
#define FILL_BMGS_WIDE							\
  if (!fg->hasdot)							\
    _FILL_BMGS(fg->bmGs, fg->wpattern, fg->wlen, true);

#define _FILL_BMGS(arr, pat, plen, wide)				\
  {									\
    char *p;								\
    wchar_t *wp;							\
									\
    if (wide)								\
      {									\
	if (fg->icase)							\
	  {								\
	    wp = alloca(plen * sizeof(wint_t));				\
	    for (int i = 0; i < plen; i++)				\
	      wp[i] = towlower(pat[i]);					\
	    _CALC_BMGS(arr, wp, plen);					\
	  }								\
	else								\
	  _CALC_BMGS(arr, pat, plen);					\
      }									\
    else								\
      {									\
	if (fg->icase)							\
	  {								\
	    p = alloca(plen);						\
	    for (int i = 0; i < plen; i++)				\
	      p[i] = tolower(pat[i]);					\
	    _CALC_BMGS(arr, p, plen);					\
	  }								\
	else								\
	  _CALC_BMGS(arr, pat, plen);					\
      }									\
  }

#define _CALC_BMGS(arr, pat, plen)					\
  {									\
    int f, g;								\
									\
    int *suff = xmalloc(plen * sizeof(int));				\
    if (suff == NULL)							\
      return REG_ESPACE;						\
									\
    suff[plen - 1] = plen;						\
    g = plen - 1;							\
    for (int i = plen - 2; i >= 0; i--)					\
      {									\
	if (i > g && suff[i + plen - 1 - f] < i - g)			\
	  suff[i] = suff[i + plen - 1 - f];				\
	else								\
	  {								\
	    if (i < g)							\
	      g = i;							\
	    f = i;							\
	    while (g >= 0 && pat[g] == pat[g + plen - 1 - f])		\
	      g--;							\
	    suff[i] = f - g;						\
	  }								\
      }									\
									\
    for (int i = 0; i < plen; i++)					\
      arr[i] = plen;							\
    g = 0;								\
    for (int i = plen - 1; i >= 0; i--)					\
      if (suff[i] == i + 1)						\
	for(; g < plen - 1 - i; g++)					\
	  if (arr[g] == plen)						\
	    arr[g] = plen - 1 - i;					\
    for (int i = 0; i <= plen - 2; i++)					\
      arr[plen - 1 - suff[i]] = plen - 1 - i;				\
									\
    free(suff);								\
  }

#define SAVE_PATTERN(p, l)						\
  l = (n == 0) ? tre_strlen(pat) : n;					\
  p = xmalloc((l + 1) * sizeof(tre_char_t));				\
  if (p == NULL)							\
    return REG_ESPACE;							\
  memcpy(p, pat, l * sizeof(tre_char_t));				\
  p[l] = TRE_CHAR('\0');


/*
 * Returns: REG_OK on success, error code otherwise
 */
int
tre_fastcomp_literal(fastmatch_t *fg, const tre_char_t *pat, size_t n,
		     int cflags)
{
  /* Initialize. */
  memset(fg, 0, sizeof(*fg));
  fg->icase = (cflags & REG_ICASE);

  /* Cannot handle REG_ICASE with MB string */
  if (fg->icase && (MB_CUR_MAX > 1))
    return REG_BADPAT;

#ifdef TRE_WCHAR
  SAVE_PATTERN(fg->wpattern, fg->wlen);
  STORE_MBS_PAT;
#else
  SAVE_PATTERN(fg->pattern, fg->len);
#endif

  FILL_QSBC;
  FILL_BMGS;
#ifdef TRE_WCHAR
  FILL_QSBC_WIDE;
  FILL_BMGS_WIDE;
#endif

  return REG_OK;
}

/*
 * Returns: REG_OK on success, error code otherwise
 */
int
tre_fastcomp(fastmatch_t *fg, const tre_char_t *pat, size_t n,
	     int cflags)
{
  /* Initialize. */
  memset(fg, 0, sizeof(*fg));
  fg->icase = (cflags & REG_ICASE);

  /* Cannot handle REG_ICASE with MB string */
  if (fg->icase && (MB_CUR_MAX > 1))
    return REG_BADPAT;

  fg->wlen = (n == 0) ? tre_strlen(pat) : n;

  /* Remove end-of-line character ('$'). */
  if ((fg->wlen > 0) && (pat[fg->wlen - 1] == TRE_CHAR('$')))
  {
    fg->eol = true;
    fg->wlen--;
  }

  /* Remove beginning-of-line character ('^'). */
  if (pat[0] == TRE_CHAR('^'))
  {
    fg->bol = true;
    fg->wlen--;
    pat++;
  }

  if ((fg->wlen >= 14) &&
      (memcmp(pat, TRE_CHAR("[[:<:]]"), 7 * sizeof(tre_char_t)) == 0) &&
      (memcmp(pat + fg->wlen - 7, TRE_CHAR("[[:>:]]"),
	      7 * sizeof(tre_char_t)) == 0))
  {
    fg->wlen -= 14;
    pat += 7;
    fg->word = true;
  }

  /*
   * pat has been adjusted earlier to not include '^', '$' or
   * the word match character classes at the beginning and ending
   * of the string respectively.
   */
  SAVE_PATTERN(fg->wpattern, fg->wlen);

  /* Look for ways to cheat...er...avoid the full regex engine. */
  for (unsigned int i = 0; i < fg->wlen; i++) {
    /* Can still cheat? */
    if ((tre_isalnum(fg->wpattern[i])) || tre_isspace(fg->wpattern[i]) ||
      (fg->wpattern[i] == TRE_CHAR('_')) || (fg->wpattern[i] == TRE_CHAR(',')) ||
      (fg->wpattern[i] == TRE_CHAR('=')) || (fg->wpattern[i] == TRE_CHAR('-')) ||
      (fg->wpattern[i] == TRE_CHAR(':')) || (fg->wpattern[i] == TRE_CHAR('/'))) {
	continue;
    } else if (fg->wpattern[i] == TRE_CHAR('.'))
      fg->hasdot = i;
    else {
	/* Free memory and let others know this is empty. */
	free(fg->wpattern);
	fg->wpattern = NULL;
	return REG_BADPAT;
    }
  }

#ifdef TRE_WCHAR
  STORE_MBS_PAT;
#else
  fg->len = fg->wlen;
  fg->patter = fg->wpattern;
#endif

  FILL_QSBC;
  FILL_BMGS;
#ifdef TRE_WCHAR
  FILL_QSBC_WIDE;
  FILL_BMGS_WIDE;
#endif

  return REG_OK;
}

int
tre_fastexec(const fastmatch_t *fg, const void *data, size_t len,
    tre_str_type_t type, int nmatch, regmatch_t pmatch[])
{
  unsigned int j;
  int ret = REG_NOMATCH;
  int mismatch, shift, u = 0, v;
  const char *str_byte = data;
  const void *startptr = NULL;
#ifdef TRE_WCHAR
  const wchar_t *str_wide = data;
#endif

  if (len == (unsigned)-1)
    switch (type)
      {
	case STR_WIDE:
	  len = wcslen(str_wide);
	  break;
	default:
	  len = strlen(str_byte);
	  break;
      }

  /* No point in going farther if we do not have enough data. */
  switch (type)
    {
      case STR_WIDE:
	if (len < fg->wlen)
	  return ret;
	shift = fg->wlen;
	break;
      default:
	if (len < fg->len)
	  return ret;
	shift = fg->len;
    }

  /* XXX: make wchar-clean */
  /* Only try once at the beginning or ending of the line. */
  if (fg->bol || fg->eol) {
    /* Simple text comparison. */
    if (!((fg->bol && fg->eol) && (len != fg->len))) {
      /* Determine where in data to start search at. */
      j = fg->eol ? len - fg->len : 0;
      SKIP_CHARS(j);
      COMPARE;
      if (mismatch == REG_OK) {
	pmatch[0].rm_so = j;
	pmatch[0].rm_eo = j + fg->len;
	return REG_OK;
      }
    }
  } else {
    /* Quick Search algorithm. */
    j = 0;
    do {
      SKIP_CHARS(j);
      COMPARE;
      if (mismatch == REG_OK) {
	pmatch[0].rm_so = j;
	pmatch[0].rm_eo = j + ((type == STR_WIDE) ? fg->wlen : fg->len);
	return REG_OK;
      } else if (mismatch > 0)
        return mismatch;
      mismatch = -mismatch - 1;
      SHIFT;
    } while (!IS_OUT_OF_BOUNDS);
  }
  return ret;
}

void
tre_fastfree(fastmatch_t *fg)
{

#ifdef TRE_WCHAR
  hashtable_free(fg->qsBc_table);
  free(fg->wpattern);
#endif
  free(fg->pattern);
}

/*
 * Returns:	-(i + 1) on failure (position that it failed with minus sign)
 *		error code on error
 *		REG_OK on success
 */
static inline int
fastcmp(const void *pat, const void *data, size_t len,
	tre_str_type_t type, bool icase)
{
  const char *str_byte = data;
  const char *pat_byte = pat;
  int ret = REG_OK;
#ifdef TRE_WCHAR
  const wchar_t *str_wide = data;
  const wchar_t *pat_wide = pat;
#endif

  for (int i = len - 1; i >= 0; i--) {
    switch (type)
      {
	case STR_WIDE:
	  if (pat_wide[i] == L'.')
	    continue;
	  if (icase ? (towlower(pat_wide[i]) == towlower(str_wide[i]))
		    : (pat_wide[i] == str_wide[i]))
	    continue;
	  break;
	default:
	  if (pat_byte[i] == '.')
	    continue;
	  if (icase ? (tolower(pat_byte[i]) == tolower(str_byte[i]))
		    : (pat_byte[i] == str_byte[i]))
	  continue;
      }
    ret = -(i + 1);
    break;
  }

  return ret;
}
