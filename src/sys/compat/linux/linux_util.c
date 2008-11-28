/*-
 * Copyright (c) 1994 Christos Zoulas
 * Copyright (c) 1995 Frank van der Linden
 * Copyright (c) 1995 Scott Bartram
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
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission
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
 *
 *	from: svr4_util.c,v 1.5 1995/01/22 23:44:50 christos Exp
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_compat.h"
#include "opt_kdtrace.h"

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/fcntl.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/linker_set.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/sdt.h>
#include <sys/syscallsubr.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include <machine/stdarg.h>

#include <compat/linux/linux_util.h>
#ifdef COMPAT_LINUX32
#include <machine/../linux32/linux.h>
#else
#include <machine/../linux/linux.h>
#endif

#include <compat/linux/linux_dtrace.h>

const char      linux_emul_path[] = "/compat/linux";

LIN_SDT_PROVIDER_DECLARE(LINUX_DTRACE);
LIN_SDT_PROBE_DEFINE(util, linux_emul_convpath, entry);
LIN_SDT_PROBE_ARGTYPE(util, linux_emul_convpath, entry, 0, "const char *");
LIN_SDT_PROBE_ARGTYPE(util, linux_emul_convpath, entry, 1, "enum uio_seg");
LIN_SDT_PROBE_ARGTYPE(util, linux_emul_convpath, entry, 2, "char **");
LIN_SDT_PROBE_ARGTYPE(util, linux_emul_convpath, entry, 3, "int");
LIN_SDT_PROBE_ARGTYPE(util, linux_emul_convpath, entry, 4, "int");
LIN_SDT_PROBE_DEFINE(util, linux_emul_convpath, return);
LIN_SDT_PROBE_ARGTYPE(util, linux_emul_convpath, return, 0, "int");
LIN_SDT_PROBE_DEFINE(util, linux_msg, entry);
LIN_SDT_PROBE_ARGTYPE(util, linux_msg, entry, 0, "const char *");
LIN_SDT_PROBE_DEFINE(util, linux_msg, return);
LIN_SDT_PROBE_DEFINE(util, linux_driver_get_name_dev, entry);
LIN_SDT_PROBE_ARGTYPE(util, linux_driver_get_name_dev, entry, 0, "device_t");
LIN_SDT_PROBE_ARGTYPE(util, linux_driver_get_name_dev, entry, 1,
    "const char *");
LIN_SDT_PROBE_DEFINE(util, linux_driver_get_name_dev, nullcall);
LIN_SDT_PROBE_DEFINE(util, linux_driver_get_name_dev, return);
LIN_SDT_PROBE_ARGTYPE(util, linux_driver_get_name_dev, return, 0, "char *");
LIN_SDT_PROBE_DEFINE(util, linux_driver_get_major_minor, entry);
LIN_SDT_PROBE_ARGTYPE(util, linux_driver_get_major_minor, entry, 0, "char *");
LIN_SDT_PROBE_ARGTYPE(util, linux_driver_get_major_minor, entry, 1, "int *");
LIN_SDT_PROBE_ARGTYPE(util, linux_driver_get_major_minor, entry, 2, "int *");
LIN_SDT_PROBE_DEFINE(util, linux_driver_get_major_minor, nullcall);
LIN_SDT_PROBE_DEFINE(util, linux_driver_get_major_minor, notfound);
LIN_SDT_PROBE_ARGTYPE(util, linux_driver_get_major_minor, notfound, 0,
    "char *");
LIN_SDT_PROBE_DEFINE(util, linux_driver_get_major_minor, return);
LIN_SDT_PROBE_ARGTYPE(util, linux_driver_get_major_minor, return, 0, "int");
LIN_SDT_PROBE_ARGTYPE(util, linux_driver_get_major_minor, return, 1, "int");
LIN_SDT_PROBE_ARGTYPE(util, linux_driver_get_major_minor, return, 2, "int");
LIN_SDT_PROBE_DEFINE(util, linux_get_char_devices, entry);
LIN_SDT_PROBE_DEFINE(util, linux_get_char_devices, return);
LIN_SDT_PROBE_ARGTYPE(util, linux_get_char_devices, return, 0, "char *");
LIN_SDT_PROBE_DEFINE(util, linux_free_get_char_devices, entry);
LIN_SDT_PROBE_ARGTYPE(util, linux_free_get_char_devices, entry, 0, "char *");
LIN_SDT_PROBE_DEFINE(util, linux_free_get_char_devices, return);
LIN_SDT_PROBE_DEFINE(util, linux_device_register_handler, entry);
LIN_SDT_PROBE_ARGTYPE(util, linux_device_register_handler, entry, 0,
    "struct linux_device_handler *");
LIN_SDT_PROBE_DEFINE(util, linux_device_register_handler, return);
LIN_SDT_PROBE_ARGTYPE(util, linux_device_register_handler, return, 0, "int");
LIN_SDT_PROBE_DEFINE(util, linux_device_unregister_handler, entry);
LIN_SDT_PROBE_ARGTYPE(util, linux_device_unregister_handler, entry, 0,
    "struct linux_device_handler *");
LIN_SDT_PROBE_DEFINE(util, linux_device_unregister_handler, return);
LIN_SDT_PROBE_ARGTYPE(util, linux_device_unregister_handler, return, 0, "int");

/*
 * Search an alternate path before passing pathname arguments on to
 * system calls. Useful for keeping a separate 'emulation tree'.
 *
 * If cflag is set, we check if an attempt can be made to create the
 * named file, i.e. we check if the directory it should be in exists.
 */
int
linux_emul_convpath(struct thread *td, const char *path, enum uio_seg pathseg,
    char **pbuf, int cflag, int dfd)
{
	int retval;

	LIN_SDT_PROBE(util, linux_emul_convpath, entry, path, pathseg, pbuf,
	    cflag, dfd);

	retval = kern_alternate_path(td, linux_emul_path, path, pathseg, pbuf,
	    cflag, dfd);

	LIN_SDT_PROBE(util, linux_emul_convpath, return, retval, 0, 0, 0, 0);

	return (retval);
}

void
linux_msg(const struct thread *td, const char *fmt, ...)
{
	va_list ap;
	struct proc *p;

	LIN_SDT_PROBE(util, linux_msg, entry, fmt, 0, 0, 0, 0);

	p = td->td_proc;
	printf("linux: pid %d (%s): ", (int)p->p_pid, p->p_comm);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");

	LIN_SDT_PROBE(util, linux_msg, return, 0, 0, 0, 0, 0);
}

struct device_element
{
	TAILQ_ENTRY(device_element) list;
	struct linux_device_handler entry;
};

static TAILQ_HEAD(, device_element) devices =
	TAILQ_HEAD_INITIALIZER(devices);

static struct linux_device_handler null_handler =
	{ "mem", "mem", "null", "null", 1, 3, 1};

DATA_SET(linux_device_handler_set, null_handler);

char *
linux_driver_get_name_dev(device_t dev)
{
	struct device_element *de;
	const char *device_name = device_get_name(dev);

	LIN_SDT_PROBE(util, linux_driver_get_name_dev, entry, dev,
	    device_name, 0, 0, 0);

	if (device_name == NULL) {
		LIN_SDT_PROBE(util, linux_driver_get_name_dev, nullcall, 0, 0,
		    0, 0, 0);
		LIN_SDT_PROBE(util, linux_driver_get_name_dev, return, NULL,
		    0, 0, 0, 0);
		return NULL;
	}
	TAILQ_FOREACH(de, &devices, list) {
		if (strcmp(device_name, de->entry.bsd_driver_name) == 0) {
			LIN_SDT_PROBE(util, linux_driver_get_name_dev, return,
			    de->entry.linux_driver_name, 0, 0, 0, 0);
			return (de->entry.linux_driver_name);
		}
	}

	LIN_SDT_PROBE(util, linux_driver_get_name_dev, return, NULL, 0, 0, 0,
	    0);
	return NULL;
}

int
linux_driver_get_major_minor(char *node, int *major, int *minor)
{
	struct device_element *de;

	LIN_SDT_PROBE(util, linux_driver_get_major_minor, entry, node, major,
	    minor, 0, 0);

	if (node == NULL || major == NULL || minor == NULL) {
		LIN_SDT_PROBE(util, linux_driver_get_major_minor, nullcall, 0,
		    0, 0, 0, 0);
		LIN_SDT_PROBE(util, linux_driver_get_major_minor, return, 1,
		    *major, *minor, 0, 0);
		return 1;
	}

	if (strlen(node) > strlen("pts/") &&
	    strncmp(node, "pts/", strlen("pts/")) == 0) {
		unsigned long devno;

		/*
		 * Linux checks major and minors of the slave device
		 * to make sure it's a pty device, so let's make him
		 * believe it is.
		 */
		devno = strtoul(node + strlen("pts/"), NULL, 10);
		*major = 136 + (devno / 256);
		*minor = devno % 256;

		LIN_SDT_PROBE(util, linux_driver_get_major_minor, return, 0,
		    *major, *minor, 0, 0);
		return 0;
	}

	TAILQ_FOREACH(de, &devices, list) {
		if (strcmp(node, de->entry.bsd_device_name) == 0) {
			*major = de->entry.linux_major;
			*minor = de->entry.linux_minor;

			LIN_SDT_PROBE(util, linux_driver_get_major_minor,
			    return, 0, *major, *minor, 0, 0);
			return 0;
		}
	}

	LIN_SDT_PROBE(util, linux_driver_get_major_minor, notfound, node, 0, 0,
	    0, 0);
	LIN_SDT_PROBE(util, linux_driver_get_major_minor, return, 1, *major,
	    *minor, 0, 0);
	return 1;
}

char *
linux_get_char_devices(void)
{
	struct device_element *de;
	char *temp, *string, *last;
	char formated[256];
	int current_size = 0, string_size = 1024;

	LIN_SDT_PROBE(util, linux_get_char_devices, entry, 0, 0, 0, 0, 0);

	string = malloc(string_size, M_LINUX, M_WAITOK);
	string[0] = '\000';
	last = "";
	TAILQ_FOREACH(de, &devices, list) {
		if (!de->entry.linux_char_device)
			continue;
		temp = string;
		if (strcmp(last, de->entry.bsd_driver_name) != 0) {
			last = de->entry.bsd_driver_name;

			snprintf(formated, sizeof(formated), "%3d %s\n",
				 de->entry.linux_major,
				 de->entry.linux_device_name);
			if (strlen(formated) + current_size
			    >= string_size) {
				string_size *= 2;
				string = malloc(string_size,
				    M_LINUX, M_WAITOK);
				bcopy(temp, string, current_size);
				free(temp, M_LINUX);
			}
			strcat(string, formated);
			current_size = strlen(string);
		}
	}

	LIN_SDT_PROBE(util, linux_get_char_devices, return, string, 0, 0, 0, 0);
	return string;
}

void
linux_free_get_char_devices(char *string)
{

	LIN_SDT_PROBE(util, linux_get_char_devices, entry, string, 0, 0, 0, 0);

	free(string, M_LINUX);

	LIN_SDT_PROBE(util, linux_get_char_devices, return, 0, 0, 0, 0, 0);
}

static int linux_major_starting = 200;

int
linux_device_register_handler(struct linux_device_handler *d)
{
	struct device_element *de;

	LIN_SDT_PROBE(util, linux_device_register_handler, entry, d, 0, 0, 0,
	    0);

	if (d == NULL) {
		LIN_SDT_PROBE(util, linux_device_register_handler, return,
		    EINVAL, 0, 0, 0, 0);
		return (EINVAL);
	}

	de = malloc(sizeof(*de), M_LINUX, M_WAITOK);
	if (d->linux_major < 0) {
		d->linux_major = linux_major_starting++;
	}
	bcopy(d, &de->entry, sizeof(*d));

	/* Add the element to the list, sorted on span. */
	TAILQ_INSERT_TAIL(&devices, de, list);

	LIN_SDT_PROBE(util, linux_device_register_handler, return, 0, 0, 0, 0,
	   0);
	return (0);
}

int
linux_device_unregister_handler(struct linux_device_handler *d)
{
	struct device_element *de;

	LIN_SDT_PROBE(util, linux_device_unregister_handler, entry, d, 0, 0, 0,
	    0);

	if (d == NULL) {
		LIN_SDT_PROBE(util, linux_device_unregister_handler, return,
		    EINVAL, 0, 0, 0, 0);
		return (EINVAL);
	}

	TAILQ_FOREACH(de, &devices, list) {
		if (bcmp(d, &de->entry, sizeof(*d)) == 0) {
			TAILQ_REMOVE(&devices, de, list);
			free(de, M_LINUX);

			LIN_SDT_PROBE(util, linux_device_unregister_handler,
			    return, 0, 0, 0, 0, 0);
			return (0);
		}
	}

	LIN_SDT_PROBE(util, linux_device_unregister_handler, return, EINVAL, 0,
	    0, 0, 0);
	return (EINVAL);
}
