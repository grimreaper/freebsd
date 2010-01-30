/*-
 * Copyright 2000 David E. O'Brien, John D. Polstra.
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

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <unistd.h>

#define ABI_VENDOR	"FreeBSD"
#define ABI_SECTION	".note.ABI-tag"
#define ABI_NOTETYPE	1

/*
 * Special ".note" entry specifying the ABI version.  See
 * http://www.netbsd.org/Documentation/kernel/elf-notes.html
 * for more information.
 */
static const struct {
    int32_t	namesz;
    int32_t	descsz;
    int32_t	type;
    char	name[sizeof ABI_VENDOR];
    int32_t	desc;
} abitag __attribute__ ((section (ABI_SECTION), aligned(4))) __used = {
    sizeof ABI_VENDOR,
    sizeof(int32_t),
    ABI_NOTETYPE,
    ABI_VENDOR,
    __FreeBSD_version
};

int cap_main(int argc, char **argv, char **env)
{
	const char warning[] =
		"ERROR: attempting to run a regular binary in capability mode.\n\nIf you wish to run a binary in a sandbox, you must provide a cap_main() function which takes the same arguments as main().\n";

	write(2, warning, sizeof(warning));
	return 1;
}

