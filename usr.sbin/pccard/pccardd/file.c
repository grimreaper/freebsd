/*
 * Copyright (c) 1995 Andrew McRae.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef lint
static const char rcsid[] =
	"$Id: file.c,v 1.17 1999/06/17 21:07:58 markm Exp $";
#endif /* not lint */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "cardd.h"

static FILE *in;
static int pushc, pusht;
static int lineno;
static char *filename;

static char *keys[] = {
	"__EOF__",		/* 1 */
	"io",			/* 2 */
	"irq",			/* 3 */
	"memory",		/* 4 */
	"card",			/* 5 */
	"device",		/* 6 */
	"config",		/* 7 */
	"reset",		/* 8 */
	"ether",		/* 9 */
	"insert",		/* 10 */
	"remove",		/* 11 */
	0
};

#define KWD_EOF			1
#define KWD_IO			2
#define KWD_IRQ			3
#define KWD_MEMORY		4
#define KWD_CARD		5
#define KWD_DEVICE		6
#define KWD_CONFIG		7
#define KWD_RESET		8
#define KWD_ETHER		9
#define KWD_INSERT		10
#define KWD_REMOVE		11

struct flags {
	char   *name;
	int     mask;
};

static void    parsefile(void);
static char   *getline(void);
static char   *next_tok(void);
static int     num_tok(void);
static void    error(char *);
static int     keyword(char *);
static int     irq_tok(int);
static struct allocblk *ioblk_tok(int);
static struct allocblk *memblk_tok(int);
static struct driver *new_driver(char *);

static void    addcmd(struct cmd **);
static void    parse_card(void);

/*
 * Read a file and parse the pcmcia configuration data.
 * After parsing, verify the links.
 */
void
readfile(char *name)
{
	struct card *cp;

	in = fopen(name, "r");
	if (in == 0) {
		logerr(name);
		die("readfile");
	}
	parsefile();
	for (cp = cards; cp; cp = cp->next) {
		if (cp->config == 0)
			logmsg("warning: card %s(%s) has no valid configuration\n",
			    cp->manuf, cp->version);
	}
}

static void
parsefile(void)
{
	int     i;
	int     irq_init = 0;
	struct allocblk *bp;

	pushc = 0;
	lineno = 1;
	for (i = 0; i < 16 ; i++) 
		if (pool_irq[i]) {
			irq_init = 1;
			break;
		}
	for (;;)
		switch (keyword(next_tok())) {
		case KWD_EOF:
			/* EOF */
			return;
		case KWD_IO:
			/* reserved I/O blocks */
			while ((bp = ioblk_tok(0)) != 0) {
				if (bp->size == 0 || bp->addr == 0) {
					free(bp);
					continue;
				}
				bit_nset(io_avail, bp->addr,
					 bp->addr + bp->size - 1);
				bp->next = pool_ioblks;
				pool_ioblks = bp;
			}
			pusht = 1;
			break;
		case KWD_IRQ:
			/* reserved irqs */
			while ((i = irq_tok(0)) > 0)
				if (!irq_init)
					pool_irq[i] = 1;
			pusht = 1;
			break;
		case KWD_MEMORY:
			/* reserved memory blocks. */
			while ((bp = memblk_tok(0)) != 0) {
				if (bp->size == 0 || bp->addr == 0) {
					free(bp);
					continue;
				}
				bit_nset(mem_avail, MEM2BIT(bp->addr),
				    MEM2BIT(bp->addr + bp->size) - 1);
				bp->next = pool_mem;
				pool_mem = bp;
			}
			pusht = 1;
			break;
		case KWD_CARD:
			/* Card definition. */
			parse_card();
			break;
		default:
			error("syntax error");
			pusht = 0;
			break;
		}
}

/*
 *	Parse a card definition.
 */
static void
parse_card(void)
{
	char   *man, *vers;
	struct card *cp;
	int     i;
	struct card_config *confp, *lastp;

	man = newstr(next_tok());
	vers = newstr(next_tok());
	cp = xmalloc(sizeof(*cp));
	cp->manuf = man;
	cp->version = vers;
	cp->reset_time = 50;
	cp->next = cards;
	cards = cp;
	for (;;) {
		switch (keyword(next_tok())) {
		case KWD_CONFIG:
			/* config */
			i = num_tok();
			if (i == -1) {
				error("illegal card config index");
				break;
			}
			confp = xmalloc(sizeof(*confp));
			man = next_tok();
			confp->driver = new_driver(man);
			confp->irq = irq_tok(1);
			confp->flags = num_tok();
			if (confp->flags == -1) {
				pusht = 1;
				confp->flags = 0;
			}
			if (confp->irq < 0 || confp->irq > 15) {
				error("illegal card IRQ value");
				break;
			}
			confp->index = i & 0x3F;

			/*
			 * If no valid driver for this config, then do not save
			 * this configuration entry.
			 */
			if (confp->driver) {
				if (cp->config == 0)
					cp->config = confp;
				else {
					for (lastp = cp->config; lastp->next;
					    lastp = lastp->next);
					lastp->next = confp;
				}
			} else
				free(confp);
			break;
		case KWD_RESET:
			/* reset */
			i = num_tok();
			if (i == -1) {
				error("illegal card reset time");
				break;
			}
			cp->reset_time = i;
			break;
		case KWD_ETHER:
			/* ether */
			cp->ether = num_tok();
			if (cp->ether == -1) {
				error("illegal ether address offset");
				cp->ether = 0;
			}
			break;
		case KWD_INSERT:
			/* insert */
			addcmd(&cp->insert);
			break;
		case KWD_REMOVE:
			/* remove */
			addcmd(&cp->remove);
			break;
		default:
			pusht = 1;
			return;
		}
	}
}

/*
 *	Generate a new driver structure. If one exists, use
 *	that one after confirming the correct class.
 */
static struct driver *
new_driver(char *name)
{
	struct driver *drvp;
	char   *p;

	for (drvp = drivers; drvp; drvp = drvp->next)
		if (strcmp(drvp->name, name) == 0)
			return (drvp);
	drvp = xmalloc(sizeof(*drvp));
	drvp->next = drivers;
	drivers = drvp;
	drvp->name = newstr(name);
	drvp->kernel = newstr(name);
	p = drvp->kernel;
	while (*p++)
		if (*p >= '0' && *p <= '9') {
			drvp->unit = atoi(p);
			*p = 0;
			break;
		}
#ifdef	DEBUG
	printf("Drv %s%d created\n", drvp->kernel, drvp->unit);
#endif
	return (drvp);
}


/*
 *	Parse one I/O block.
 */
static struct allocblk *
ioblk_tok(int force)
{
	struct allocblk *io;
	int     i, j;

	if ((i = num_tok()) >= 0) {
		if (strcmp("-", next_tok()) || (j = num_tok()) < 0 || j < i) {
			error("I/O block format error");
			return (0);
		}
		io = xmalloc(sizeof(*io));
		io->addr = i;
		io->size = j - i + 1;
		if (j > IOPORTS) {
			error("I/O port out of range");
			if (force) {
				free(io);
				io = 0;
			} else
				io->addr = io->size = 0;
		}
		return (io);
	}
	if (force)
		error("illegal or missing I/O block spec");
	return (0);
}

/*
 *	Parse a memory block.
 */
static struct allocblk *
memblk_tok(int force)
{
	struct allocblk *mem;
	int     i, j;

	if ((i = num_tok()) >= 0) {
		if ((j = num_tok()) < 0)
			error("illegal memory block");
		else {
			mem = xmalloc(sizeof(*mem));
			mem->addr = i & ~(MEMUNIT - 1);
			mem->size = (j + MEMUNIT - 1) & ~(MEMUNIT - 1);
			if (i < MEMSTART || (i + j) > MEMEND) {
				error("memory address out of range");
				if (force) {
					free(mem);
					mem = 0;
				} else
					mem->addr = mem->size = 0;
			}
			return (mem);
		}
	}
	if (force)
		error("illegal or missing memory block spec");
	return (0);
}

/*
 *	IRQ token. Must be number > 0 && < 16.
 *	If force is set, IRQ must exist, and can also be '?'.
 */
static int
irq_tok(int force)
{
	int     i;

	if (strcmp("?", next_tok()) == 0 && force)
		return (0);
	pusht = 1;
	i = num_tok();
	if (i > 0 && i < 16)
		return (i);
	if (force)
		error("illegal IRQ value");
	return (-1);
}

/*
 *	search the table for a match.
 */
static int
keyword(char *str)
{
	char  **s;
	int     i = 1;

	for (s = keys; *s; s++, i++)
		if (strcmp(*s, str) == 0)
			return (i);
	return (0);
}

/*
 *	addcmd - Append the command line to the list of
 *	commands.
 */
static void
addcmd(struct cmd **cp)
{
	struct cmd *ncp;
	char   *s = getline();

	if (*s) {
		ncp = xmalloc(sizeof(*ncp));
		ncp->line = s;
		while (*cp)
			cp = &(*cp)->next;
		*cp = ncp;
	}

}

static void
error(char *msg)
{
	pusht = 1;
	logmsg("%s: %s at line %d, near %s\n",
	    filename, msg, lineno, next_tok());
	pusht = 1;
}

static int     last_char;

static int
get(void)
{
	int     c;

	if (pushc)
		c = pushc;
	else
		c = getc(in);
	pushc = 0;
	while (c == '\\') {
		c = getc(in);
		switch (c) {
		case '#':
			return (last_char = c);
		case '\n':
			lineno++;
			c = getc(in);
			continue;
		}
		pushc = c;
		return ('\\');
	}
	if (c == '\n')
		lineno++;
	if (c == '#') {
		while (get() != '\n');
		return (last_char = '\n');
	}
	return (last_char = c);
}

/*
 *	num_tok - expecting a number token. If not a number,
 *	return -1.
 *	Handles octal (who uses octal anymore?)
 *		hex
 *		decimal
 *	Looks for a 'k' at the end of decimal numbers
 *	and multiplies by 1024.
 */
static int
num_tok(void)
{
	char   *s = next_tok(), c;
	int     val = 0, base;

	base = 10;
	c = *s++;
	if (c == '0') {
		base = 8;
		c = *s++;
		if (c == '\0') return 0; 
		else if (c == 'x' || c == 'X') {
			c = *s++;
			base = 16;
		}
	}
	do {
		switch (c) {
		case 'k':
		case 'K':
			if (val && base == 10 && *s == 0)
				return (val * 1024);
			return (-1);
		default:
			return (-1);
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
			val = val * base + c - '0';
			break;

		case '8':
		case '9':
			if (base == 8)
				return (-1);
			else
				val = val * base + c - '0';
			break;
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f':
			if (base == 16)
				val = val * base + c - 'a' + 10;
			else
				return (-1);
			break;
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
			if (base == 16)
				val = val * base + c - 'A' + 10;
			else
				return (-1);
			break;
		}
	} while ((c = *s++) != 0);
	return (val);
}

static char   *_next_tok(void);

static char *
next_tok(void)
{
	char   *s = _next_tok();
#if 0
	printf("Tok = %s\n", s);
#endif
	return (s);
}

/*
 *	get one token. Handles string quoting etc.
 */
static char *
_next_tok(void)
{
	static char buf[1024];
	char   *p = buf, instr = 0;
	int     c;

	if (pusht) {
		pusht = 0;
		return (buf);
	}
	for (;;) {
		c = get();
		switch (c) {
		default:
			*p++ = c;
			break;
		case '"':
			if (instr) {
				*p++ = 0;
				return (buf);
			}
			instr = 1;
			break;
		case '\n':
			if (instr) {
				error("unterminated string");
				break;
			}
		case ' ':
		case '\t':
			/* Eat whitespace unless in a string. */
			if (!instr) {
				if (p != buf) {
					*p++ = 0;
					return (buf);
				}
			} else
				*p++ = c;
			break;
		case '-':
		case '?':
		case '*':
			/* Special characters that are tokens on their own. */
			if (instr)
				*p++ = c;
			else {
				if (p != buf)
					pushc = c;
				else
					*p++ = c;
				*p++ = 0;
				return (buf);
			}
			break;
		case EOF:
			if (p != buf) {
				*p++ = 0;
				return (buf);
			}
			strcpy(buf, "__EOF__");
			return (buf);
		}
	}
}

/*
 *	get the rest of the line. If the
 *	last character scanned was a newline, then
 *	return an empty line. If this isn't checked, then
 *	a getline may incorrectly return the next line.
 */
static char *
getline(void)
{
	char    buf[1024], *p = buf;
	int     c, i = 0;

	if (last_char == '\n')
		return (newstr(""));
	do {
		c = get();
	} while (c == ' ' || c == '\t');
	for (; c != '\n' && c != EOF; c = get())
		if (i++ < sizeof(buf) - 10)
			*p++ = c;
	*p = 0;
	return (newstr(buf));
}
