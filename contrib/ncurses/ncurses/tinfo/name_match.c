/****************************************************************************
 * Copyright (c) 1999,2000,2001 Free Software Foundation, Inc.              *
 *                                                                          *
 * Permission is hereby granted, free of charge, to any person obtaining a  *
 * copy of this software and associated documentation files (the            *
 * "Software"), to deal in the Software without restriction, including      *
 * without limitation the rights to use, copy, modify, merge, publish,      *
 * distribute, distribute with modifications, sublicense, and/or sell       *
 * copies of the Software, and to permit persons to whom the Software is    *
 * furnished to do so, subject to the following conditions:                 *
 *                                                                          *
 * The above copyright notice and this permission notice shall be included  *
 * in all copies or substantial portions of the Software.                   *
 *                                                                          *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS  *
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF               *
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.   *
 * IN NO EVENT SHALL THE ABOVE COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,   *
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR    *
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR    *
 * THE USE OR OTHER DEALINGS IN THE SOFTWARE.                               *
 *                                                                          *
 * Except as contained in this notice, the name(s) of the above copyright   *
 * holders shall not be used in advertising or otherwise to promote the     *
 * sale, use or other dealings in this Software without prior written       *
 * authorization.                                                           *
 ****************************************************************************/

/****************************************************************************
 *  Author: Thomas E. Dickey <dickey@clark.net> 1999                        *
 ****************************************************************************/

#include <curses.priv.h>
#include <term.h>
#include <tic.h>

MODULE_ID("$Id: name_match.c,v 1.11 2001/09/22 21:11:34 tom Exp $")

/*
 *	_nc_first_name(char *names)
 *
 *	Extract the primary name from a compiled entry.
 */

NCURSES_EXPORT(char *)
_nc_first_name(const char *const sp)
/* get the first name from the given name list */
{
    static char *buf;
    register unsigned n;

    if (buf == 0)
	buf = _nc_doalloc(buf, MAX_NAME_SIZE + 1);
    for (n = 0; n < MAX_NAME_SIZE; n++) {
	if ((buf[n] = sp[n]) == '\0'
	    || (buf[n] == '|'))
	    break;
    }
    buf[n] = '\0';
    return (buf);
}

/*
 *	int _nc_name_match(namelist, name, delim)
 *
 *	Is the given name matched in namelist?
 */

NCURSES_EXPORT(int)
_nc_name_match
(const char *const namelst, const char *const name, const char *const delim)
{
    const char *s, *d, *t;
    int code, found;

    if ((s = namelst) != 0) {
	while (*s != '\0') {
	    for (d = name; *d != '\0'; d++) {
		if (*s != *d)
		    break;
		s++;
	    }
	    found = FALSE;
	    for (code = TRUE; *s != '\0'; code = FALSE, s++) {
		for (t = delim; *t != '\0'; t++) {
		    if (*s == *t) {
			found = TRUE;
			break;
		    }
		}
		if (found)
		    break;
	    }
	    if (code && *d == '\0')
		return code;
	    if (*s++ == 0)
		break;
	}
    }
    return FALSE;
}
