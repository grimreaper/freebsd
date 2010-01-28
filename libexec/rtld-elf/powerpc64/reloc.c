/*      $NetBSD: ppc_reloc.c,v 1.10 2001/09/10 06:09:41 mycroft Exp $   */

/*-
 * Copyright (C) 1998   Tsubai Masanari
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
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/param.h>
#include <sys/mman.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <machine/cpu.h>
#include <machine/md_var.h>

#include "debug.h"
#include "rtld.h"

struct funcdesc {
	uint64_t addr;
	uint64_t toc;
	uint64_t env;
};

/*
 * Process the R_PPC_COPY relocations
 */
int
do_copy_relocations(Obj_Entry *dstobj)
{
	const Elf_Rela *relalim;
	const Elf_Rela *rela;

	/*
	 * COPY relocs are invalid outside of the main program
	 */
	assert(dstobj->mainprog);

	relalim = (const Elf_Rela *) ((caddr_t) dstobj->rela +
	    dstobj->relasize);
	for (rela = dstobj->rela;  rela < relalim;  rela++) {
		void *dstaddr;
		const Elf_Sym *dstsym;
		const char *name;
		unsigned long hash;
		size_t size;
		const void *srcaddr;
		const Elf_Sym *srcsym = NULL;
		Obj_Entry *srcobj;
		const Ver_Entry *ve;

		if (ELF_R_TYPE(rela->r_info) != R_PPC_COPY) {
			continue;
		}

		dstaddr = (void *) (dstobj->relocbase + rela->r_offset);
		dstsym = dstobj->symtab + ELF_R_SYM(rela->r_info);
		name = dstobj->strtab + dstsym->st_name;
		hash = elf_hash(name);
		size = dstsym->st_size;
		ve = fetch_ventry(dstobj, ELF_R_SYM(rela->r_info));

		for (srcobj = dstobj->next;  srcobj != NULL;
		     srcobj = srcobj->next) {
			if ((srcsym = symlook_obj(name, hash, srcobj, ve, 0))
			    != NULL) {
				break;
			}
		}

		if (srcobj == NULL) {
			_rtld_error("Undefined symbol \"%s\" "
				    " referenced from COPY"
				    " relocation in %s", name, dstobj->path);
			return (-1);
		}

		srcaddr = (const void *) (srcobj->relocbase+srcsym->st_value);
		memcpy(dstaddr, srcaddr, size);
		dbg("copy_reloc: src=%p,dst=%p,size=%d\n",srcaddr,dstaddr,size);
	}

	return (0);
}


/*
 * Perform early relocation of the run-time linker image
 */
void
reloc_non_plt_self(Elf_Dyn *dynp, Elf_Addr relocbase)
{
	const Elf_Rela *rela = 0, *relalim;
	Elf_Addr relasz = 0;
	Elf_Addr *where;

	/*
	 * Extract the rela/relasz values from the dynamic section
	 */
	for (; dynp->d_tag != DT_NULL; dynp++) {
		switch (dynp->d_tag) {
		case DT_RELA:
			rela = (const Elf_Rela *)(relocbase+dynp->d_un.d_ptr);
			break;
		case DT_RELASZ:
			relasz = dynp->d_un.d_val;
			break;
		}
	}

	/*
	 * Relocate these values
	 */
	relalim = (const Elf_Rela *)((caddr_t)rela + relasz);
	for (; rela < relalim; rela++) {
		where = (Elf_Addr *)(relocbase + rela->r_offset);
		*where = (Elf_Addr)(relocbase + rela->r_addend);
	}
}


/*
 * Relocate a non-PLT object with addend.
 */
static int
reloc_nonplt_object(Obj_Entry *obj_rtld, Obj_Entry *obj, const Elf_Rela *rela,
		    SymCache *cache)
{
	Elf_Addr        *where = (Elf_Addr *)(obj->relocbase + rela->r_offset);
	const Elf_Sym   *def;
	const Obj_Entry *defobj;
	Elf_Addr         tmp;

	switch (ELF_R_TYPE(rela->r_info)) {

	case R_PPC_NONE:
		break;

        case R_PPC_ADDR32:    /* word32 S + A */
        case R_PPC_GLOB_DAT:  /* word32 S + A */
		def = find_symdef(ELF_R_SYM(rela->r_info), obj, &defobj,
				  false, cache);
		if (def == NULL) {
			return (-1);
		}

                tmp = (Elf_Addr)(defobj->relocbase + def->st_value +
                    rela->r_addend);

		/* Don't issue write if unnecessary; avoid COW page fault */
                if (*where != tmp) {
                        *where = tmp;
		}
                break;

        case R_PPC_RELATIVE:  /* word32 B + A */
		tmp = (Elf_Addr)(obj->relocbase + rela->r_addend);

		/* As above, don't issue write unnecessarily */
		if (*where != tmp) {
			*where = tmp;
		}
		break;

	case R_PPC_COPY:
		/*
		 * These are deferred until all other relocations
		 * have been done.  All we do here is make sure
		 * that the COPY relocation is not in a shared
		 * library.  They are allowed only in executable
		 * files.
		 */
		if (!obj->mainprog) {
			_rtld_error("%s: Unexpected R_COPY "
				    " relocation in shared library",
				    obj->path);
			return (-1);
		}
		break;

	case R_PPC_JMP_SLOT:
		/*
		 * These will be handled by the plt/jmpslot routines
		 */
		break;

	case R_PPC_DTPMOD32:
		def = find_symdef(ELF_R_SYM(rela->r_info), obj, &defobj,
		    false, cache);

		if (def == NULL)
			return (-1);

		*where = (Elf_Addr) defobj->tlsindex;

		break;

	case R_PPC_TPREL32:
		def = find_symdef(ELF_R_SYM(rela->r_info), obj, &defobj,
		    false, cache);

		if (def == NULL)
			return (-1);

		/*
		 * We lazily allocate offsets for static TLS as we
		 * see the first relocation that references the
		 * TLS block. This allows us to support (small
		 * amounts of) static TLS in dynamically loaded
		 * modules. If we run out of space, we generate an
		 * error.
		 */
		if (!defobj->tls_done) {
			if (!allocate_tls_offset((Obj_Entry*) defobj)) {
				_rtld_error("%s: No space available for static "
				    "Thread Local Storage", obj->path);
				return (-1);
			}
		}

		*(Elf_Addr **)where = *where * sizeof(Elf_Addr)
		    + (Elf_Addr *)(def->st_value + rela->r_addend 
		    + defobj->tlsoffset - TLS_TP_OFFSET);
		
		break;
		
	case R_PPC_DTPREL32:
		def = find_symdef(ELF_R_SYM(rela->r_info), obj, &defobj,
		    false, cache);

		if (def == NULL)
			return (-1);

		*where += (Elf_Addr)(def->st_value + rela->r_addend 
		    - TLS_DTV_OFFSET);

		break;
		
	default:
		_rtld_error("%s: Unsupported relocation type %ld"
			    " in non-PLT relocations\n", obj->path,
			    ELF_R_TYPE(rela->r_info));
		return (-1);
        }
	return (0);
}


/*
 * Process non-PLT relocations
 */
int
reloc_non_plt(Obj_Entry *obj, Obj_Entry *obj_rtld)
{
	const Elf_Rela *relalim;
	const Elf_Rela *rela;
	SymCache *cache;
	int bytes = obj->nchains * sizeof(SymCache);
	int r = -1;

	/*
	 * The dynamic loader may be called from a thread, we have
	 * limited amounts of stack available so we cannot use alloca().
	 */
	if (obj != obj_rtld) {
		cache = mmap(NULL, bytes, PROT_READ|PROT_WRITE, MAP_ANON,
		    -1, 0);
		if (cache == MAP_FAILED)
			cache = NULL;
	} else
		cache = NULL;

	/*
	 * From the SVR4 PPC ABI:
	 * "The PowerPC family uses only the Elf32_Rela relocation
	 *  entries with explicit addends."
	 */
	relalim = (const Elf_Rela *)((caddr_t)obj->rela + obj->relasize);
	for (rela = obj->rela; rela < relalim; rela++) {
		if (reloc_nonplt_object(obj_rtld, obj, rela, cache) < 0)
			goto done;
	}
	r = 0;
done:
	if (cache) {
		munmap(cache, bytes);
	}
	return (r);
}


/*
 * Initialise a PLT slot to the resolving trampoline
 */
static int
reloc_plt_object(Obj_Entry *obj, const Elf_Rela *rela)
{
	Elf_Word *where = (Elf_Word *)(obj->relocbase + rela->r_offset);
	int reloff;

	reloff = rela - obj->pltrela;

	if ((reloff < 0) || (reloff >= 0x8000)) {
		return (-1);
	}

	dbg(" reloc_plt_object: where=%p,pltres=%p,reloff=%x,distance=%x",
	    (void *)where, (void *)pltresolve, reloff, distance);

	((struct funcdesc *)(where))->addr =
	    (uint64_t)_rtld_powerpc64_pltresolve;
	((struct funcdesc *)(where))->toc = reloff;
	((struct funcdesc *)(where))->env = (uint64_t)obj;

	return (0);
}


/*
 * Process the PLT relocations.
 */
int
reloc_plt(Obj_Entry *obj)
{
	const Elf_Rela *relalim;
	const Elf_Rela *rela;

	if (obj->pltrelasize != 0) {

		relalim = (const Elf_Rela *)((char *)obj->pltrela +
		    obj->pltrelasize);
		for (rela = obj->pltrela;  rela < relalim;  rela++) {
			assert(ELF_R_TYPE(rela->r_info) == R_PPC_JMP_SLOT);

			if (reloc_plt_object(obj, rela) < 0) {
				return (-1);
			}
		}
	}

	return (0);
}


/*
 * LD_BIND_NOW was set - force relocation for all jump slots
 */
int
reloc_jmpslots(Obj_Entry *obj)
{
	const Obj_Entry *defobj;
	const Elf_Rela *relalim;
	const Elf_Rela *rela;
	const Elf_Sym *def;
	Elf_Addr *where;
	Elf_Addr target;

	relalim = (const Elf_Rela *)((char *)obj->pltrela + obj->pltrelasize);
	for (rela = obj->pltrela; rela < relalim; rela++) {
		assert(ELF_R_TYPE(rela->r_info) == R_PPC_JMP_SLOT);
		where = (Elf_Addr *)(obj->relocbase + rela->r_offset);
		def = find_symdef(ELF_R_SYM(rela->r_info), obj, &defobj,
		   true, NULL);
		if (def == NULL) {
			dbg("reloc_jmpslots: sym not found");
			return (-1);
		}

		target = (Elf_Addr)(defobj->relocbase + def->st_value);

#if 0
		/* PG XXX */
		dbg("\"%s\" in \"%s\" --> %p in \"%s\"",
		    defobj->strtab + def->st_name, basename(obj->path),
		    (void *)target, basename(defobj->path));
#endif

		reloc_jmpslot(where, target, defobj, obj,
		    (const Elf_Rel *) rela);
	}

	obj->jmpslots_done = true;

	return (0);
}


/*
 * Update the value of a PLT jump slot. Branch directly to the target if
 * it is within +/- 32Mb, otherwise go indirectly via the pltcall
 * trampoline call and jump table.
 */
Elf_Addr
reloc_jmpslot(Elf_Addr *wherep, Elf_Addr target, const Obj_Entry *defobj,
	      const Obj_Entry *obj, const Elf_Rel *rel)
{
	dbg(" reloc_jmpslot: where=%p, target=%p",
	    (void *)wherep, (void *)target);

	/*
	 * At the PLT entry pointed at by `wherep', construct
	 * a direct transfer to the now fully resolved function
	 * address.
	 */

	memcpy(wherep, (void *)target, sizeof(struct funcdesc));

	return (target);
}

void
init_pltgot(Obj_Entry *obj)
{
	struct funcdesc *pltcall;
	int N = obj->pltrelasize / sizeof(Elf_Rela);

	pltcall = (struct funcdesc *)obj->pltgot;

	if (pltcall == NULL) {
		return;
	}

	/*
	 * Copy the function description into the PLT slot
	 */
	memcpy(pltcall, _rtld_powerpc64_pltresolve, sizeof(*pltcall));

	/*
	 * Now fake the two arguments we get in the descriptor to
	 * pass information to the resolver.
	 */
	pltcall->toc = N;
	pltcall->env = (uint64_t)obj;
}

void
allocate_initial_tls(Obj_Entry *list)
{
	register Elf_Addr **tp __asm__("r2");
	Elf_Addr **_tp;

	/*
	* Fix the size of the static TLS block by using the maximum
	* offset allocated so far and adding a bit for dynamic modules to
	* use.
	*/

	tls_static_space = tls_last_offset + tls_last_size + RTLD_STATIC_TLS_EXTRA;

	_tp = (Elf_Addr **) ((char *) allocate_tls(list, NULL, TLS_TCB_SIZE, 8) 
	    + TLS_TP_OFFSET + TLS_TCB_SIZE);

	/*
	 * XXX gcc seems to ignore 'tp = _tp;' 
	 */
	 
	__asm __volatile("mr %0,%1" : "=r"(tp) : "r"(_tp));
}

void*
__tls_get_addr(tls_index* ti)
{
	register Elf_Addr **tp __asm__("r2");
	char *p;

	p = tls_get_addr_common((Elf_Addr**)((Elf_Addr)tp - TLS_TP_OFFSET 
	    - TLS_TCB_SIZE), ti->ti_module, ti->ti_offset);

	return (p + TLS_DTV_OFFSET);
}
