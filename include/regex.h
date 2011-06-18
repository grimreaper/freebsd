/*
  tre.h - TRE public API definitions

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.

*/

#ifndef REGEX_H
#define REGEX_H 1

#include <sys/types.h>

#define TRE_WCHAR 1
#define TRE_APPROX 1

#ifdef __cplusplus
extern "C" {
#endif

#define tre_regcomp     regcomp
#define tre_regerror    regerror
#define tre_regexec     regexec
#define tre_regfree     regfree

#define tre_regacomp    regacomp
#define tre_regaexec    regaexec
#define tre_regancomp   regancomp
#define tre_reganexec   reganexec
#define tre_regawncomp  regawncomp
#define tre_regawnexec  regawnexec
#define tre_regncomp    regncomp
#define tre_regnexec    regnexec
#define tre_regwcomp    regwcomp
#define tre_regwexec    regwexec
#define tre_regwncomp   regwncomp
#define tre_regwnexec   regwnexec

typedef int regoff_t;
typedef struct {
  size_t re_nsub;  /* Number of parenthesized subexpressions. */
  void *value;	   /* For internal use only. */
  const char *re_endp;
} regex_t;

typedef struct {
  regoff_t rm_so;
  regoff_t rm_eo;
} regmatch_t;


typedef enum {
  REG_OK = 0,		/* No error. */
  /* POSIX tre_regcomp() return error codes.  (In the order listed in the
     standard.)	 */
  REG_NOMATCH,		/* No match. */
  REG_BADPAT,		/* Invalid regexp. */
  REG_ECOLLATE,		/* Unknown collating element. */
  REG_ECTYPE,		/* Unknown character class name. */
  REG_EESCAPE,		/* Trailing backslash. */
  REG_ESUBREG,		/* Invalid back reference. */
  REG_EBRACK,		/* "[]" imbalance */
  REG_EPAREN,		/* "\(\)" or "()" imbalance */
  REG_EBRACE,		/* "\{\}" or "{}" imbalance */
  REG_BADBR,		/* Invalid content of {} */
  REG_ERANGE,		/* Invalid use of range operator */
  REG_ESPACE,		/* Out of memory.  */
  REG_BADRPT            /* Invalid use of repetition operators. */
} reg_errcode_t;

/* POSIX tre_regcomp() flags. */
#define REG_EXTENDED	1
#define REG_ICASE	(REG_EXTENDED << 1)
#define REG_NEWLINE	(REG_ICASE << 1)
#define REG_NOSUB	(REG_NEWLINE << 1)

/* Extra tre_regcomp() flags. */
#define REG_BASIC	0
#define REG_LITERAL	(REG_NOSUB << 1)
#define REG_RIGHT_ASSOC (REG_LITERAL << 1)
#define REG_UNGREEDY    (REG_RIGHT_ASSOC << 1)
#define REG_PEND	(REG_UNGREEDY << 1)

/* POSIX tre_regexec() flags. */
#define REG_NOTBOL 1
#define REG_NOTEOL (REG_NOTBOL << 1)

/* Extra tre_regexec() flags. */
#define REG_APPROX_MATCHER	 (REG_NOTEOL << 1)
#define REG_BACKTRACKING_MATCHER (REG_APPROX_MATCHER << 1)
#define REG_STARTEND		 (REG_BACKTRACKING_MATCHER << 1)

/* REG_NOSPEC and REG_LITERAL mean the same thing. */
#if defined(REG_LITERAL) && !defined(REG_NOSPEC)
#define REG_NOSPEC	REG_LITERAL
#elif defined(REG_NOSPEC) && !defined(REG_LITERAL)
#define REG_LITERAL	REG_NOSPEC
#endif /* defined(REG_NOSPEC) */

/* The maximum number of iterations in a bound expression. */
#undef RE_DUP_MAX
#define RE_DUP_MAX 255

/* The POSIX.2 regexp functions */
extern int
regcomp(regex_t *preg, const char *regex, int cflags);

extern int
regexec(const regex_t *preg, const char *string, size_t nmatch,
	regmatch_t pmatch[], int eflags);

extern size_t
regerror(int errcode, const regex_t *preg, char *errbuf,
	 size_t errbuf_size);

extern void
regfree(regex_t *preg);

#ifdef TRE_WCHAR
#include <wchar.h>

/* Wide character versions (not in POSIX.2). */
extern int
regwcomp(regex_t *preg, const wchar_t *regex, int cflags);

extern int
regwexec(const regex_t *preg, const wchar_t *string,
	 size_t nmatch, regmatch_t pmatch[], int eflags);
#endif /* TRE_WCHAR */

/* Versions with a maximum length argument and therefore the capability to
   handle null characters in the middle of the strings (not in POSIX.2). */
extern int
regncomp(regex_t *preg, const char *regex, size_t len, int cflags);

extern int
regnexec(const regex_t *preg, const char *string, size_t len,
	 size_t nmatch, regmatch_t pmatch[], int eflags);

#ifdef TRE_WCHAR
extern int
regwncomp(regex_t *preg, const wchar_t *regex, size_t len, int cflags);

extern int
regwnexec(const regex_t *preg, const wchar_t *string, size_t len,
	  size_t nmatch, regmatch_t pmatch[], int eflags);
#endif /* TRE_WCHAR */

#ifdef TRE_APPROX

/* Approximate matching parameter struct. */
typedef struct {
  int cost_ins;	       /* Default cost of an inserted character. */
  int cost_del;	       /* Default cost of a deleted character. */
  int cost_subst;      /* Default cost of a substituted character. */
  int max_cost;	       /* Maximum allowed cost of a match. */

  int max_ins;	       /* Maximum allowed number of inserts. */
  int max_del;	       /* Maximum allowed number of deletes. */
  int max_subst;       /* Maximum allowed number of substitutes. */
  int max_err;	       /* Maximum allowed number of errors total. */
} regaparams_t;

/* Approximate matching result struct. */
typedef struct {
  size_t nmatch;       /* Length of pmatch[] array. */
  regmatch_t *pmatch;  /* Submatch data. */
  int cost;	       /* Cost of the match. */
  int num_ins;	       /* Number of inserts in the match. */
  int num_del;	       /* Number of deletes in the match. */
  int num_subst;       /* Number of substitutes in the match. */
} regamatch_t;


/* Approximate matching functions. */
extern int
regaexec(const regex_t *preg, const char *string,
	 regamatch_t *match, regaparams_t params, int eflags);

extern int
reganexec(const regex_t *preg, const char *string, size_t len,
	  regamatch_t *match, regaparams_t params, int eflags);
#ifdef TRE_WCHAR
/* Wide character approximate matching. */
extern int
regawexec(const regex_t *preg, const wchar_t *string,
	  regamatch_t *match, regaparams_t params, int eflags);

extern int
regawnexec(const regex_t *preg, const wchar_t *string, size_t len,
	   regamatch_t *match, regaparams_t params, int eflags);
#endif /* TRE_WCHAR */

/* Sets the parameters to default values. */
extern void
tre_regaparams_default(regaparams_t *params);
#endif /* TRE_APPROX */

#ifdef TRE_WCHAR
typedef wchar_t tre_char_t;
#else /* !TRE_WCHAR */
typedef unsigned char tre_char_t;
#endif /* !TRE_WCHAR */

typedef struct {
  int (*get_next_char)(tre_char_t *c, unsigned int *pos_add, void *context);
  void (*rewind)(size_t pos, void *context);
  int (*compare)(size_t pos1, size_t pos2, size_t len, void *context);
  void *context;
} tre_str_source;

extern int
reguexec(const regex_t *preg, const tre_str_source *string,
	 size_t nmatch, regmatch_t pmatch[], int eflags);

/* Returns the version string.	The returned string is static. */
extern char *
tre_version(void);

/* Returns the value for a config parameter.  The type to which `result'
   must point to depends of the value of `query', see documentation for
   more details. */
extern int
tre_config(int query, void *result);

enum {
  TRE_CONFIG_APPROX,
  TRE_CONFIG_WCHAR,
  TRE_CONFIG_MULTIBYTE,
  TRE_CONFIG_SYSTEM_ABI,
  TRE_CONFIG_VERSION
};

/* Returns 1 if the compiled pattern has back references, 0 if not. */
extern int
tre_have_backrefs(const regex_t *preg);

/* Returns 1 if the compiled pattern uses approximate matching features,
   0 if not. */
extern int
tre_have_approx(const regex_t *preg);

#ifdef __cplusplus
}
#endif
#endif				/* REGEX_H */

/* EOF */
