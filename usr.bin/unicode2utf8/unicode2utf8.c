/*-
 * Copyright (c) 2009 Edwin Groothuis <edwin@FreeBSD.org>
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

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>

#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>

#define MAXBUF	512

struct utf8map {
	char *uniname;
	char *utf8char;
	int utf8len;
	struct utf8map *next;
};

struct utf8map *utf8map_head[256];

void		 usage(void);
struct utf8map	*get_utf8map(char *dir);
struct utf8map	*find_utf8map(char *unidata);
void		 translate(char *file_in, char *file_out);

int debug = 0;

int
main(int argc, char *argv[])
{
	char *cldr = NULL, *file_in = NULL, *file_out = NULL;
	char ch;

	static struct option longopts[] = {
		{ "cldr",	required_argument,	NULL,	1 },
		{ "input",	required_argument,	NULL,	2 },
		{ "output",	required_argument,	NULL,	3 },
		{ "debug",	no_argument,		NULL,	4 },
		{ NULL,		0,			NULL,	0 }
	};

	while ((ch = getopt_long_only(argc, argv, "", longopts, NULL)) != -1) {
		switch (ch) {
		case 1:
			cldr = optarg;
			break;
		case 2:
			file_in = optarg;
			break;
		case 3:
			file_out = optarg;
			break;
		case 4:
			debug++;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (cldr == NULL || file_in == NULL || file_out == NULL)
		usage();

	get_utf8map(cldr);
	translate(file_in, file_out);
}

void
translate(char *file_in, char *file_out)
{
	struct utf8map *map;
	FILE *fin, *fout;
	char *p, *q1, *q2, line[MAXBUF];

	if ((fin = fopen(file_in, "r")) == NULL)
		errx(EX_DATAERR, "Cannot open %s for reading.", file_in);
	if ((fout = fopen(file_out, "w")) == NULL)
		errx(EX_DATAERR, "Cannot open %s for writing.", file_out);

	while (!feof(fin)) {
		if (fgets(line, sizeof(line), fin) != NULL) {
			if (line[0] == '#') {
				fprintf(fout, "%s", line);
				continue;
			}

			p = line;
			while (*p != '\0') {
				if (*p != '<') {
					fputc(*p, fout);
					p++;
					continue;
				}
				q1 = strchr(p + 1, '>');
				q2 = strchr(p + 1, '<');
				if (q2 != NULL && q2 < q1)
					errx(EX_DATAERR,
					    "Unexpected < in line %s after %s",
					    line, p);
				*q1 = '\0';
				if ((map = find_utf8map(p + 1)) ==NULL)
					errx(EX_DATAERR,
					    "Cannot find translation for '%s'",
					    p + 1);

				*q1 = '>';
				p = q1 + 1;
				fwrite(map->utf8char, map->utf8len, 1, fout);
			}

		}
	}

	fclose(fin);
	fclose(fout);
}

struct utf8map *
find_utf8map(char *uniname)
{
	struct utf8map *p;
	int hashindex = uniname[strlen(uniname) - 1];

	p = utf8map_head[hashindex];
	while (p != NULL) {
		if (strcmp(p->uniname, uniname) == 0)
			return p;
		if (debug)
			printf("'%s' - '%s'\n", p->uniname, uniname);
		p = p->next;
	}

	return NULL;
}

struct utf8map *
get_utf8map(char *dir)
{
	struct utf8map *new;
	FILE *fin;
	int len, i, hashindex;
	char filename[MAXPATHLEN], uniname[MAXBUF], utf8char[MAXBUF], *p;

	sprintf(filename, "%s/posix/UTF-8.cm", dir);

	if ((fin = fopen(filename, "r")) == NULL)
		errx(EX_DATAERR, "Cannot open UTF-8 in %s/posix", filename);

	while (!feof(fin)) {
		if (fgets(uniname, sizeof(uniname), fin) != NULL)
			if (strncmp(uniname, "CHARMAP", 7) == 0)
				break;
	}
	if (feof(fin))
		errx(EX_DATAERR, "Didn't find initial CHARMAP magic cookie.\n");

	while (!feof(fin)) {
		if (fscanf(fin, "%s %s", uniname, utf8char) == 2) {
			/* ^END CHARMAP$ */
			if (strcmp(uniname, "END") == 0
			 && strcmp(utf8char, "CHARMAP") == 0)
				break;

			/* Get rid of the _'s in the name. */
			while ((p = strchr(uniname, '_')) != NULL)
				*p = ' ';
			if ((p = strchr(uniname, '>')) == NULL)
				errx(EX_DATAERR, "No trailing '>' for %s",
				    uniname);
			hashindex = p[-1];
			*p = '\0';
			if (uniname[0] != '<')
				errx(EX_DATAERR, "No leading '<' for %s",
				    uniname);

			/* Translate hex strings into ascii-strings. */
			len = strlen(utf8char);
			if (len % 4 != 0)
				errx(EX_DATAERR, "Wrong length: '%s'",
				    utf8char);
			len /= 4;
			for (i = 0; i < len; i++) {
				/*
				 * Not setting will produce wrong results for
				 * the unicode string NULL.
				 */
				errno = 0;

				/* "\xAA" -> "AA" -> chr(hextodec("AA")) */
				utf8char[i] = strtol(utf8char + 4 * i + 2, NULL,
				    16);
				if (utf8char[i] == 0 && errno != 0)
					errx(errno,
					    "'%s' isn't a hex digit (%d)",
					    utf8char + 4 * i + 2, errno);
				utf8char[len] = 0;
			}

			if (debug)
				printf("-%s-%s-\n", uniname, utf8char);

			new = (struct utf8map *)malloc(sizeof(struct utf8map));
			new->next = utf8map_head[hashindex];
			new->uniname = strdup(uniname + 1);
			new->utf8char = strdup(utf8char);
			new->utf8len = len;
			utf8map_head[hashindex] = new;
		}
	}

	if (feof(fin))
		errx(EX_DATAERR, "Didn't find final CHARMAP magic cookie.\n");
	fclose(fin);

	return NULL;
}

void
usage(void)
{

	printf("Usage: unicode2utf8 --cldr=dir --input=file --output=file\n");
	exit(EX_USAGE);
}

