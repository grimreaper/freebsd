/*-
 * Copyright (c) 2007 Fabio Checconi <fabio@FreeBSD.org>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>

#include <stdio.h>
#include <stdint.h>
#include <libgeom.h>

#include "core/geom.h"

#define	G_SCHED_VERSION	0

uint32_t lib_version = G_LIB_VERSION;
uint32_t version = G_SCHED_VERSION;

static char sched[] = "as";

struct g_command class_commands[] = {
	{ "create", G_FLAG_VERBOSE | G_FLAG_LOADKLD, NULL,
	    {
		{ 's', "sched", sched, G_TYPE_STRING },
		G_OPT_SENTINEL
	    },
	    NULL, "[-v] [-s sched] dev ..."
	},
	{ "configure", G_FLAG_VERBOSE, NULL,
	    {
		G_OPT_SENTINEL
	    },
	    NULL, "[-v] prov ..."
	},
	{ "destroy", G_FLAG_VERBOSE, NULL,
	    {
		{ 'f', "force", NULL, G_TYPE_BOOL },
		G_OPT_SENTINEL
	    },
	    NULL, "[-fv] prov ..."
	},
	{ "reset", G_FLAG_VERBOSE, NULL, G_NULL_OPTS, NULL,
	    "[-v] prov ..."
	},
	G_CMD_SENTINEL
};
