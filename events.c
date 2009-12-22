/*
 * Copyright (c) 1989, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD: user/edwin/calendar/io.c 200813 2009-12-21 21:17:59Z edwin $");

#include <sys/time.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pathnames.h"
#include "calendar.h"

struct event *
event_add(struct event *events, int month, int day,
    char *date, int var, char *txt)
{
	struct event *e;

	/*
	 * Creating a new event:
	 * - Create a new event
	 * - Copy the machine readable day and month
	 * - Copy the human readable and language specific date
	 * - Copy the text of the event
	 */
	e = (struct event *)calloc(1, sizeof(struct event));
	if (e == NULL)
		errx(1, "event_add: cannot allocate memory");
	e->month = month;
	e->day = day;
	e->var = var;
	e->date = strdup(date);
	if (e->date == NULL)
		errx(1, "event_add: cannot allocate memory");
	e->text = strdup(txt);
	if (e->text == NULL)
		errx(1, "event_add: cannot allocate memory");
	e->next = events;

	return e;
}

void
event_continue(struct event *e, char *txt)
{
	char *text;

	/*
	 * Adding text to the event:
	 * - Save a copy of the old text (unknown length, so strdup())
	 * - Allocate enough space for old text + \n + new text + 0
	 * - Store the old text + \n + new text
	 * - Destroy the saved copy.
	 */
	text = strdup(e->text);
	if (text == NULL)
		errx(1, "event_continue: cannot allocate memory");

	free(e->text);
	e->text = (char *)malloc(strlen(text) + strlen(txt) + 3);
	if (e->text == NULL)
		errx(1, "event_continue: cannot allocate memory");
	strcpy(e->text, text);
	strcat(e->text, "\n");
	strcat(e->text, txt);
	free(text);

	return;
}

void
event_print_all(FILE *fp, struct event *events)
{
	struct event *e, *e_next;
	int daycounter;
	int day, month;

	/*
	 * Print all events:
	 * - We know the number of days to be counted (f_dayAfter + f_dayBefore)
	 * - We know the current day of the year ("now" - f_dayBefore + counter)
	 * - We know the number of days in the year (yrdays, set in settime())
	 * - So we know the date on which the current daycounter is on the
	 *   calendar in days and months.
	 * - Go through the list of events, and print all matching dates
	 */
	for (daycounter = 0; daycounter <= f_dayAfter + f_dayBefore;
	    daycounter++) {
		day = tp1.tm_yday - f_dayBefore + daycounter;
		if (day < 0)
			day += yrdays;
		if (day >= yrdays)
			day -= yrdays;

		/*
		 * When we know the day of the year, we can determine the day
		 * of the month and the month.
		 */
		month = 1;
		while (month <= 12) {
			if (day <= cumdays[month])
				break;
			month++;
		}
		month--;
		day -= cumdays[month];

#ifdef DEBUG
		fprintf(stderr, "event_print_allmonth: %d, day: %d\n",
		    month, day);
#endif

		/*
		 * Go through all events and print the text of the matching
		 * dates
		 */
		for (e = events; e != NULL; e = e_next) {
			e_next = e->next;

			if (month != e->month || day != e->day)
				continue;

			(void)fprintf(fp, "%s%c%s\n", e->date,
			    e->var ? '*' : ' ', e->text);
		}
	}
}
