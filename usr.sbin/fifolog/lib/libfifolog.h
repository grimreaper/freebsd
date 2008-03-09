/*-
 * Copyright (c) 2005-2008 Poul-Henning Kamp
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
 *
 * $FreeBSD$
 */

#include <sys/stdint.h>

/* CREATORS */
const char *fifolog_create(const char *fn, off_t size, unsigned recsize);


/* WRITERS */

struct fifolog_writer;
struct fifolog_writer *fifolog_write_new(void);
const char *fifolog_write_open(struct fifolog_writer *f, const char *fn, unsigned writerate, unsigned syncrate, int compression);
int fifolog_write_bytes(struct fifolog_writer *f, uint32_t id, time_t now, const void *ptr, unsigned len);
void fifolog_write_bytes_poll(struct fifolog_writer *f, uint32_t id, time_t now, const void *ptr, unsigned len);
int fifolog_write_poll(struct fifolog_writer *f, time_t now);
void fifolog_write_close(struct fifolog_writer *f);
void fifolog_write_destroy(struct fifolog_writer *f);
int fifolog_write_flush(struct fifolog_writer *f);
extern const char *fifolog_write_statnames[];

/* READERS */

typedef void fifolog_reader_render_t(void *priv, time_t when, unsigned flag, const unsigned char *p, unsigned l);
struct fifolog_reader;
struct fifolog_reader *fifolog_reader_open(const char *fname);
off_t fifolog_reader_seek(const struct fifolog_reader *fl, time_t t0);
void fifolog_reader_process(struct fifolog_reader *fl, off_t from, fifolog_reader_render_t *func, void *priv, time_t end);

/* UTILS */
time_t get_date(char *p);

#if (__FreeBSD__ < 7)
int     expand_number(char *_buf, int64_t *_num);
#endif

