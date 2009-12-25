#include <stdio.h>
#include <stdlib.h>
#include <err.h>
#include <time.h>

#include "calendar.h"

struct cal_year {
	int year;	/* 19xx, 20xx, 21xx */
	int easter;	/* Julian day */
	int paskha;	/* Julian day */
	int cny;	/* Julian day */
	int firstdayofweek; /* 0 .. 6 */
	struct cal_month *months;
	struct cal_year	*nextyear;
} cal_year;

struct cal_month {
	int month;			/* 01 .. 12 */
	int firstdayjulian;		/* 000 .. 366 */
	int firstdayofweek;		/* 0 .. 6 */
	struct cal_year *year;		/* points back */
	struct cal_day *days;
	struct cal_month *nextmonth;
} cal_month;

struct cal_day {
	int dayofmonth;			/* 01 .. 31 */
	int julianday;			/* 000 .. 366 */
	int dayofweek;			/* 0 .. 6 */
	struct cal_day *nextday;
	struct cal_month *month;
	struct cal_year	*year;
	struct event *events;
} cal_day;

struct cal_year	*hyear = NULL;

/* 1-based month, 0-based days, cumulative */
int *cumdays;
int	cumdaytab[][14] = {
	{0, -1, 30, 58, 89, 119, 150, 180, 211, 242, 272, 303, 333, 364},
	{0, -1, 30, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
};
/* 1-based month, individual */
int *mondays;
int	mondaytab[][14] = {
	{0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 30},
	{0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 30},
};

static void
createdate(int y, int m, int d)
{
	struct cal_year *py, *pyp;
	struct cal_month *pm, *pmp;
	struct cal_day *pd, *pdp;
	int *cumday;

	pyp = NULL;
	py = hyear;
	while (py != NULL) {
		if (py->year == y + 1900)
			break;
		pyp = py;
		py = py->nextyear;
	}

	if (py == NULL) {
		struct tm td;
		time_t t;
		py = (struct cal_year *)calloc(1, sizeof(struct cal_year));
		py->year = y + 1900;
		py->easter = easter(y);
		py->paskha = paskha(y);

		td = tm0;
		td.tm_year = y;
		td.tm_mday = 1;
		t = mktime(&td);
		localtime_r(&t, &td);
		py->firstdayofweek = td.tm_wday;

		if (pyp != NULL)
			pyp->nextyear = py;
	}
	if (pyp == NULL) {
		/* The very very very first one */
		hyear = py;
	}

	pmp = NULL;
	pm = py->months;
	while (pm != NULL) {
		if (pm->month == m)
			break;
		pmp = pm;
		pm = pm->nextmonth;
	}

	if (pm == NULL) {
		pm = (struct cal_month *)calloc(1, sizeof(struct cal_month));
		pm->year = py;
		pm->month = m;
		cumday = cumdaytab[isleap(y)];
		pm->firstdayjulian = cumday[m] + 2;
		pm->firstdayofweek =
		    (py->firstdayofweek + pm->firstdayjulian -1) % 7;
		if (pmp != NULL)
			pmp->nextmonth = pm;
	}
	if (pmp == NULL)
		py->months = pm;

	pdp = NULL;
	pd = pm->days;
	while (pd != NULL) {
		pdp = pd;
		pd = pd->nextday;
	}

	if (pd == NULL) {	/* Always true */
		pd = (struct cal_day *)calloc(1, sizeof(struct cal_day));
		pd->month = pm;
		pd->year = py;
		pd->dayofmonth = d;
		pd->julianday = pm->firstdayjulian + d - 1;
		pd->dayofweek = (pm->firstdayofweek + d - 1) % 7;
		if (pdp != NULL)
			pdp->nextday = pd;
	}
	if (pdp == NULL)
		pm->days = pd;
}

void
generatedates(struct tm *tp1, struct tm *tp2)
{
	int y1, m1, d1;
	int y2, m2, d2;
	int y, m, d;
	int *mondays;

	y1 = tp1->tm_year;
	m1 = tp1->tm_mon + 1;
	d1 = tp1->tm_mday;
	y2 = tp2->tm_year;
	m2 = tp2->tm_mon + 1;
	d2 = tp2->tm_mday;

	if (y1 == y2) {
		if (m1 == m2) {
			/* Same year, same month. Easy! */
			for (d = d1; d <= d2; d++)
				createdate(y1, m1, d);
			return;
		}
		/*
		 * Same year, different month.
		 * - Take the leftover days from m1
		 * - Take all days from <m1 .. m2>
		 * - Take the first days from m2
		 */
		mondays = mondaytab[isleap(y1)];
		for (d = d1; d <= mondays[m1]; d++)
			createdate(y1, m1, d);
		for (m = m1 + 1; m < m2; m++)
			for (d = 1; d <= mondays[m]; d++)
				createdate(y1, m, d);
		for (d = 1; d <= d2; d++)
			createdate(y1, m2, d);
		return;
	}
	/*
	 * Different year, different month.
	 * - Take the leftover days from y1-m1
	 * - Take all days from y1-<m1 .. 12]
	 * - Take all days from <y1 .. y2>
	 * - Take all days from y2-[1 .. m2>
	 * - Take the first days of y2-m2
	 */
	mondays = mondaytab[isleap(y1)];
	for (d = d1; d <= mondays[m1]; d++)
		createdate(y1, m1, d);
	for (m = m1 + 1; m <= 12; m++)
		for (d = 1; d <= mondays[m]; d++)
			createdate(y1, m, d);
	for (y = y1 + 1; y < y2; y++) {
		mondays = mondaytab[isleap(y)];
		for (m = 1; m <= 12; m++)
			for (d = 1; d <= mondays[m]; d++)
				createdate(y, m, d);
	}
	mondays = mondaytab[isleap(y2)];
	for (m = 1; m < m2; m++)
		for (d = 1; d <= mondays[m]; d++)
			createdate(y2, m, d);
	for (d = 1; d <= d2; d++)
		createdate(y2, m2, d);
}

void
dumpdates(void)
{
	struct cal_year *y;
	struct cal_month *m;
	struct cal_day *d;

	y = hyear;
	while (y != NULL) {
		printf("%-5d (wday:%d)\n", y->year, y->firstdayofweek);
		m = y->months;
		while (m != NULL) {
			printf("-- %-5d (julian:%d, dow:%d)\n", m->month,
			    m->firstdayjulian, m->firstdayofweek);
			d = m->days;
			while (d != NULL) {
				printf("  -- %-5d (julian:%d, dow:%d)\n",
				    d->dayofmonth, d->julianday, d->dayofweek);
				d = d->nextday;
			}
			m = m->nextmonth;
		}
		y = y->nextyear;
	}
}
