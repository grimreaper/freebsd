#	$Id: bsd.kern.mk,v 1.8 1997/10/21 10:39:27 bde Exp $

#
# Warning flags for compiling the kernel and components of the kernel.
#
CWARNFLAGS?=	-Wreturn-type -Wcomment -Wredundant-decls -Wimplicit \
		-Wnested-externs -Wstrict-prototypes -Wmissing-prototypes \
		-Wpointer-arith -Winline -Wuninitialized -ansi
#
# The following flags are next up for working on:
#	-W -Wunused -Wcast-qual -Wformat -Wall
#
# When working on removing warnings from code, the `-Werror' flag should be
# of material assistance.
#
