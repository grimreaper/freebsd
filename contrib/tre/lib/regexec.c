/*
  tre_regexec.c - TRE POSIX compatible matching functions (and more).

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.

*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#ifdef TRE_USE_ALLOCA
/* AIX requires this to be the first thing in the file.	 */
#ifndef __GNUC__
# if HAVE_ALLOCA_H
#  include <alloca.h>
# else
#  ifdef _AIX
 #pragma alloca
#  else
#   ifndef alloca /* predefined by HP cc +Olibcalls */
char *alloca ();
#   endif
#  endif
# endif
#endif
#endif /* TRE_USE_ALLOCA */

#include <assert.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_WCHAR_H
#include <wchar.h>
#endif /* HAVE_WCHAR_H */
#ifdef HAVE_WCTYPE_H
#include <wctype.h>
#endif /* HAVE_WCTYPE_H */
#ifndef TRE_WCHAR
#include <ctype.h>
#endif /* !TRE_WCHAR */
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif /* HAVE_MALLOC_H */
#include <limits.h>

#include "tre-fastmatch.h"
#include "tre-heuristic.h"
#include "tre-internal.h"
#include "xmalloc.h"

#ifdef TRE_LIBC_BUILD
__weak_reference(tre_regexec, regexec);
__weak_reference(tre_regnexec, regnexec);
__weak_reference(tre_regwexec, regwexec);
__weak_reference(tre_regwnexec, regwnexec);
__weak_reference(tre_reguexec, reguexec);
__weak_reference(tre_regaexec, regaexec);
__weak_reference(tre_reganexec, reganexec);
__weak_reference(tre_regawexec, regawexec);
__weak_reference(tre_regawnexec, regawnexec);
#endif

static int tre_match_heur(const tre_tnfa_t *tnfa, heur_t *heur,
			  const void *string, size_t len,
			  tre_str_type_t type, size_t nmatch,
			  regmatch_t pmatch[], int eflags);
static int tre_match_nfa(const tre_tnfa_t *tnfa, const void *string,
			 size_t len, tre_str_type_t type, size_t nmatch,
			 regmatch_t pmatch[], int eflags);

/* Fills the POSIX.2 regmatch_t array according to the TNFA tag and match
   endpoint values. */
void
tre_fill_pmatch(size_t nmatch, regmatch_t pmatch[], int cflags,
		const tre_tnfa_t *tnfa, int *tags, int match_eo)
{
  tre_submatch_data_t *submatch_data;
  unsigned int i, j;
  int *parents;

  i = 0;
  if (match_eo >= 0 && !(cflags & REG_NOSUB))
    {
      /* Construct submatch offsets from the tags. */
      DPRINT(("end tag = t%d = %d\n", tnfa->end_tag, match_eo));
      submatch_data = tnfa->submatch_data;
      while (i < tnfa->num_submatches && i < nmatch)
	{
	  if (submatch_data[i].so_tag == tnfa->end_tag)
	    pmatch[i].rm_so = match_eo;
	  else
	    pmatch[i].rm_so = tags[submatch_data[i].so_tag];

	  if (submatch_data[i].eo_tag == tnfa->end_tag)
	    pmatch[i].rm_eo = match_eo;
	  else
	    pmatch[i].rm_eo = tags[submatch_data[i].eo_tag];

	  /* If either of the endpoints were not used, this submatch
	     was not part of the match. */
	  if (pmatch[i].rm_so == -1 || pmatch[i].rm_eo == -1)
	    pmatch[i].rm_so = pmatch[i].rm_eo = -1;

	  DPRINT(("pmatch[%d] = {t%d = %d, t%d = %d}\n", i,
		  submatch_data[i].so_tag, pmatch[i].rm_so,
		  submatch_data[i].eo_tag, pmatch[i].rm_eo));
	  i++;
	}
      /* Reset all submatches that are not within all of their parent
	 submatches. */
      i = 0;
      while (i < tnfa->num_submatches && i < nmatch)
	{
	  if (pmatch[i].rm_eo == -1)
	    assert(pmatch[i].rm_so == -1);
	  assert(pmatch[i].rm_so <= pmatch[i].rm_eo);

	  parents = submatch_data[i].parents;
	  if (parents != NULL)
	    for (j = 0; parents[j] >= 0; j++)
	      {
		DPRINT(("pmatch[%d] parent %d\n", i, parents[j]));
		if (pmatch[i].rm_so < pmatch[parents[j]].rm_so
		    || pmatch[i].rm_eo > pmatch[parents[j]].rm_eo)
		  pmatch[i].rm_so = pmatch[i].rm_eo = -1;
	      }
	  i++;
	}
    }

  while (i < nmatch)
    {
      pmatch[i].rm_so = -1;
      pmatch[i].rm_eo = -1;
      i++;
    }
}


/*
  Wrapper functions for POSIX compatible regexp matching.
*/

int
tre_have_backrefs(const regex_t *preg)
{
  tre_tnfa_t *tnfa = (void *)preg->TRE_REGEX_T_FIELD;
  return tnfa->have_backrefs;
}

int
tre_have_approx(const regex_t *preg)
{
  tre_tnfa_t *tnfa = (void *)preg->TRE_REGEX_T_FIELD;
  return tnfa->have_approx;
}

int
tre_match(const regex_t *preg, const void *string, size_t len,
	  tre_str_type_t type, size_t nmatch, regmatch_t pmatch[],
	  int eflags)
{
  tre_tnfa_t *tnfa = (void *)preg->TRE_REGEX_T_FIELD;
  fastmatch_t *shortcut = preg->shortcut;
  heur_t *heur = preg->heur;

  if ((shortcut != NULL) && (type != STR_USER))
    tre_match_fast(shortcut, string, len, type, nmatch,
      pmatch, eflags);
  else if ((heur != NULL) && (type != STR_USER))
    return tre_match_heur(tnfa, heur, string, len, type, nmatch,
			  pmatch, eflags);

  return tre_match_nfa(tnfa, string, len, type, nmatch, pmatch, eflags);
}

static int
tre_match_heur(const tre_tnfa_t *tnfa, heur_t *heur, const void *string,
	       size_t len, tre_str_type_t type, size_t nmatch,
	       regmatch_t pmatch[], int eflags)
{
  int ret;
  size_t st = 0, i = 1, n;
  const char *data_byte = string;
  const tre_char_t *data_wide = string;

#define FIX_OFFSETS(adj)						\
  if (ret == REG_NOMATCH)						\
    {									\
      adj;								\
      continue;								\
    }									\
  else if ((ret == REG_OK) && !(tnfa->cflags & REG_NOSUB))		\
    for (int i = 0; i < nmatch; i++)					\
      {									\
	pmatch[i].rm_so += st;						\
	pmatch[i].rm_eo += st;						\
      }									\
  return ret;

#define SEEK_TO(off)							\
  string = (type == STR_WIDE) ? (void *)&data_wide[off] :		\
    (void *)&data_byte[off];

  /*
   * REG_NEWLINE: looking for the longest fragment and then
   * isolate the line and run the automaton.
   */
  if (heur->type == HEUR_LONGEST)
    {
      while (st < len)
	{
	  size_t eo, so;

	  SEEK_TO(st);

	  /* Match for heuristic */
	   ret = tre_match_fast(heur->heurs[0], string, len - st, type, nmatch,
				pmatch, eflags);
	   if (ret != REG_OK)
	     return ret;

	   /*
	    * If we do not know the length of the possibly matching part,
	    * look for newlines.
	    */
	   if (heur->tlen == -1)
	     {
		for (so = st + pmatch[0].rm_so - 1; ; so--)
		  {
		    if ((type == STR_WIDE) ? (data_wide[so] == TRE_CHAR('\n')) :
		       (data_byte[so] == '\n'))
		      break;
		    if (so == 0)
		      break;
		  }

		for (eo = st + pmatch[0].rm_eo; st + eo < len; eo++)
		  {
		    if ((type == STR_WIDE) ? (data_wide[eo] == TRE_CHAR('\n')) :
		       (data_byte[eo] == '\n'))
		      break;
		  }
	      }

	    /*
	     * If we know the possible match length, just check the narrowest
	     * context that we can, without looking for explicit newlines.
	     */
	    else
	      {
		size_t rem = heur->tlen - (pmatch[0].rm_eo - pmatch[0].rm_so);

		so = st + pmatch[0].rm_so <= rem ? 0 : st + pmatch[0].rm_so - rem;
		eo = st + pmatch[0].rm_eo + rem >= len ? len : st + pmatch[0].rm_eo + rem;
	      }

	    SEEK_TO(so);
	    ret = tre_match_nfa(tnfa, string, eo - so, type, nmatch, pmatch, eflags);
	    FIX_OFFSETS(st = eo);
	}
	return REG_NOMATCH;
    }

  /*
   * General case when REG_NEWLINE is not set.  Look for prefix,
   * intermediate and suffix heuristics is available, to determine
   * the context where the automaton will be invoked.  The start
   * of the context is st and the relative end offset from st is
   * stored in n.
   */
  else
    {
      while (st < len)
	{
	  SEEK_TO(st);

	  /* Prefix heuristic */
	  ret = tre_match_fast(heur->heurs[0], string, len - st,
			       type, nmatch, pmatch, eflags);
	  if (ret != REG_OK)
	    return ret;
	  st += pmatch[0].rm_so;
	  n = pmatch[0].rm_eo - pmatch[0].rm_so;

	  /* Intermediate heuristics (if any) */
	  while (!(heur->heurs[i] == NULL) &&
		((heur->heurs[i + 1] != NULL) ||
		((heur->heurs[i + 1] == NULL) && (heur->type == HEUR_PREFIX_ARRAY))))
	    {
	      SEEK_TO(st + n);
	      if (len <= st + n)
		return REG_NOMATCH;
	      ret = tre_match_fast(heur->heurs[i], string, len - st - n,
				   type, nmatch, pmatch, eflags);
	      if (ret != REG_OK)
		return ret;
	      n += pmatch[0].rm_eo;
	      i++;
	    }

	/* Suffix heuristic available */
	if ((heur->type == HEUR_ARRAY) && heur->heurs[i] != NULL)
	  {
	    SEEK_TO(st + n);
	    if (len <= st + n)
	      return REG_NOMATCH;
	    ret = tre_match_fast(heur->heurs[i], string, len - st - n,
				 type, nmatch, pmatch, eflags);
	    if (ret != REG_OK)
	      return ret;
	    n += pmatch[0].rm_eo;

	    SEEK_TO(st);
	    ret = tre_match_nfa(tnfa, string, n, type, nmatch, pmatch,
				eflags);
	    FIX_OFFSETS(st += n);
	  }

        /* Suffix heuristic not available */
	else
	  {
	    size_t l = (heur->tlen == -1) ? len - st : heur->tlen;

	    if (l > len - st)
	      return REG_NOMATCH;
	    SEEK_TO(st);
	    ret = tre_match_nfa(tnfa, string, l, type, nmatch,
				pmatch, eflags);
	    FIX_OFFSETS(st += n);
	  }
      }
      return REG_NOMATCH;
    }
}

static int
tre_match_nfa(const tre_tnfa_t *tnfa, const void *string, size_t len,
	      tre_str_type_t type, size_t nmatch, regmatch_t pmatch[],
	      int eflags)
{
  reg_errcode_t status;
  int eo, *tags = NULL;

  if (tnfa->num_tags > 0 && nmatch > 0)
    {
#ifdef TRE_USE_ALLOCA
      tags = alloca(sizeof(*tags) * tnfa->num_tags);
#else /* !TRE_USE_ALLOCA */
      tags = xmalloc(sizeof(*tags) * tnfa->num_tags);
#endif /* !TRE_USE_ALLOCA */
      if (tags == NULL)
	return REG_ESPACE;
    }

  /* Dispatch to the appropriate matcher. */
  if (tnfa->have_backrefs || eflags & REG_BACKTRACKING_MATCHER)
    {
      /* The regex has back references, use the backtracking matcher. */
      if (type == STR_USER)
	{
	  const tre_str_source *source = string;
	  if (source->rewind == NULL || source->compare == NULL)
	    /* The backtracking matcher requires rewind and compare
	       capabilities from the input stream. */
	    return REG_BADPAT;
	}
      status = tre_tnfa_run_backtrack(tnfa, string, (int)len, type,
				      tags, eflags, &eo);
    }
#ifdef TRE_APPROX
  else if (tnfa->have_approx || eflags & REG_APPROX_MATCHER)
    {
      /* The regex uses approximate matching, use the approximate matcher. */
      regamatch_t match;
      regaparams_t params;
      tre_regaparams_default(&params);
      params.max_err = 0;
      params.max_cost = 0;
      status = tre_tnfa_run_approx(tnfa, string, (int)len, type, tags,
				   &match, params, eflags, &eo);
    }
#endif /* TRE_APPROX */
  else
    {
      /* Exact matching, no back references, use the parallel matcher. */
      status = tre_tnfa_run_parallel(tnfa, string, (int)len, type,
				     tags, eflags, &eo);
    }

  if (status == REG_OK)
    /* A match was found, so fill the submatch registers. */
    tre_fill_pmatch(nmatch, pmatch, tnfa->cflags, tnfa, tags, eo);
#ifndef TRE_USE_ALLOCA
  if (tags)
    xfree(tags);
#endif /* !TRE_USE_ALLOCA */
  return status;
}

int
tre_regnexec(const regex_t *preg, const char *str, size_t len,
	 size_t nmatch, regmatch_t pmatch[], int eflags)
{
  tre_str_type_t type = (TRE_MB_CUR_MAX == 1) ? STR_BYTE : STR_MBS;

  if (eflags & REG_STARTEND)
    CALL_WITH_OFFSET(tre_match(preg, &str[offset], slen, type, nmatch,
		     pmatch, eflags));
  else
    return tre_match(preg, str, len == (unsigned)-1 ? strlen(str) : len,
		     type, nmatch, pmatch, eflags);
}

int
tre_regexec(const regex_t *preg, const char *str,
	size_t nmatch, regmatch_t pmatch[], int eflags)
{
  return tre_regnexec(preg, str, (unsigned)-1, nmatch, pmatch, eflags);
}


#ifdef TRE_WCHAR

int
tre_regwnexec(const regex_t *preg, const wchar_t *str, size_t len,
	  size_t nmatch, regmatch_t pmatch[], int eflags)
{
  tre_str_type_t type = STR_WIDE;

  if (eflags & REG_STARTEND)
    CALL_WITH_OFFSET(tre_match(preg, &str[offset], slen, type, nmatch,
		     pmatch, eflags));
  else
    return tre_match(preg, str, len == (unsigned)-1 ? strlen(str) : len,
		     STR_WIDE, nmatch, pmatch, eflags);
}

int
tre_regwexec(const regex_t *preg, const wchar_t *str,
	 size_t nmatch, regmatch_t pmatch[], int eflags)
{
  return tre_regwnexec(preg, str, (unsigned)-1, nmatch, pmatch, eflags);
}

#endif /* TRE_WCHAR */

int
tre_reguexec(const regex_t *preg, const tre_str_source *str,
	 size_t nmatch, regmatch_t pmatch[], int eflags)
{
  return tre_match(preg, str, (unsigned)-1, STR_USER, nmatch, pmatch, eflags);
}


#ifdef TRE_APPROX

/*
  Wrapper functions for approximate regexp matching.
*/

static int
tre_match_approx(const regex_t *preg, const void *string, size_t len,
		 tre_str_type_t type, regamatch_t *match, regaparams_t params,
		 int eflags)
{
  tre_tnfa_t *tnfa = (void *)preg->TRE_REGEX_T_FIELD;
  reg_errcode_t status;
  int *tags = NULL, eo;

  /* If the regexp does not use approximate matching features, the
     maximum cost is zero, and the approximate matcher isn't forced,
     use the exact matcher instead. */
  if (params.max_cost == 0 && !tnfa->have_approx
      && !(eflags & REG_APPROX_MATCHER))
    return tre_match(preg, string, len, type, match->nmatch, match->pmatch,
		     eflags);

  /* Back references are not supported by the approximate matcher. */
  if (tnfa->have_backrefs)
    return REG_BADPAT;

  if (tnfa->num_tags > 0 && match->nmatch > 0)
    {
#if TRE_USE_ALLOCA
      tags = alloca(sizeof(*tags) * tnfa->num_tags);
#else /* !TRE_USE_ALLOCA */
      tags = xmalloc(sizeof(*tags) * tnfa->num_tags);
#endif /* !TRE_USE_ALLOCA */
      if (tags == NULL)
	return REG_ESPACE;
    }
  status = tre_tnfa_run_approx(tnfa, string, (int)len, type, tags,
			       match, params, eflags, &eo);
  if (status == REG_OK)
    tre_fill_pmatch(match->nmatch, match->pmatch, tnfa->cflags, tnfa, tags, eo);
#ifndef TRE_USE_ALLOCA
  if (tags)
    xfree(tags);
#endif /* !TRE_USE_ALLOCA */
  return status;
}

int
tre_reganexec(const regex_t *preg, const char *str, size_t len,
	  regamatch_t *match, regaparams_t params, int eflags)
{
  tre_str_type_t type = (TRE_MB_CUR_MAX == 1) ? STR_BYTE : STR_MBS;

  return tre_match_approx(preg, str, len, type, match, params, eflags);
}

int
tre_regaexec(const regex_t *preg, const char *str,
	 regamatch_t *match, regaparams_t params, int eflags)
{
  return tre_reganexec(preg, str, (unsigned)-1, match, params, eflags);
}

#ifdef TRE_WCHAR

int
tre_regawnexec(const regex_t *preg, const wchar_t *str, size_t len,
	   regamatch_t *match, regaparams_t params, int eflags)
{
  return tre_match_approx(preg, str, len, STR_WIDE,
			  match, params, eflags);
}

int
tre_regawexec(const regex_t *preg, const wchar_t *str,
	  regamatch_t *match, regaparams_t params, int eflags)
{
  return tre_regawnexec(preg, str, (unsigned)-1, match, params, eflags);
}

#endif /* TRE_WCHAR */

void
tre_regaparams_default(regaparams_t *params)
{
  memset(params, 0, sizeof(*params));
  params->cost_ins = 1;
  params->cost_del = 1;
  params->cost_subst = 1;
  params->max_cost = INT_MAX;
  params->max_ins = INT_MAX;
  params->max_del = INT_MAX;
  params->max_subst = INT_MAX;
  params->max_err = INT_MAX;
}

#endif /* TRE_APPROX */

/* EOF */
