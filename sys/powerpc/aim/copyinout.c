/*-
 * Copyright (C) 2002 Benno Rice
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
 * THIS SOFTWARE IS PROVIDED BY Benno Rice ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL TOOLS GMBH BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
/*-
 * Copyright (C) 1993 Wolfgang Solfrank.
 * Copyright (C) 1993 TooLs GmbH.
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by TooLs GmbH.
 * 4. The name of TooLs GmbH may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY TOOLS GMBH ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL TOOLS GMBH BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>

#include <machine/pcb.h>
#include <machine/sr.h>

int	setfault(faultbuf);	/* defined in locore.S */

/*
 * Makes sure that the right segment of userspace is mapped in.
 */
static __inline register_t
va_to_vsid(pmap_t pm, const volatile void *va)
{
        #ifdef __powerpc64__
        return (((uint64_t)pm->pm_context << 17) |
            ((uintptr_t)va >> ADDR_SR_SHFT));
        #else
        return ((pm->pm_sr[(uintptr_t)va >> ADDR_SR_SHFT]) & SR_VSID_MASK);
        #endif
}

#ifdef __powerpc64__
static __inline void
set_user_sr(register_t vsid)
{
	register_t esid, slb1, slb2;

	esid = USER_SR;

	slb1 = vsid << 12;
	slb2 = (((esid << 1) | 1UL) << 27) | USER_SR;

	__asm __volatile ("slbie %0; slbmte %1, %2" :: "r"(esid << 28),
	    "r"(slb1), "r"(slb2));
	isync();
}
#else
static __inline void
set_user_sr(register_t vsid)
{

	isync();
	__asm __volatile ("mtsr %0,%1" :: "n"(USER_SR), "r"(vsid));
	isync();
}
#endif

int
copyout(const void *kaddr, void *udaddr, size_t len)
{
	struct		thread *td;
	pmap_t		pm;
	faultbuf	env;
	const char	*kp;
	char		*up, *p;
	size_t		l;

	td = PCPU_GET(curthread);
	pm = &td->td_proc->p_vmspace->vm_pmap;

	if (setfault(env)) {
		td->td_pcb->pcb_onfault = NULL;
		return (EFAULT);
	}

	kp = kaddr;
	up = udaddr;

	while (len > 0) {
		p = (char *)USER_ADDR + ((uintptr_t)up & ~SEGMENT_MASK);

		l = ((char *)USER_ADDR + SEGMENT_LENGTH) - p;
		if (l > len)
			l = len;

		set_user_sr(va_to_vsid(pm,up));

		bcopy(kp, p, l);

		up += l;
		kp += l;
		len -= l;
	}

	td->td_pcb->pcb_onfault = NULL;
	return (0);
}

int
copyin(const void *udaddr, void *kaddr, size_t len)
{
	struct		thread *td;
	pmap_t		pm;
	faultbuf	env;
	const char	*up;
	char		*kp, *p;
	size_t		l;

	td = PCPU_GET(curthread);
	pm = &td->td_proc->p_vmspace->vm_pmap;

	if (setfault(env)) {
		td->td_pcb->pcb_onfault = NULL;
		return (EFAULT);
	}

	kp = kaddr;
	up = udaddr;

	while (len > 0) {
		p = (char *)USER_ADDR + ((uintptr_t)up & ~SEGMENT_MASK);

		l = ((char *)USER_ADDR + SEGMENT_LENGTH) - p;
		if (l > len)
			l = len;

		set_user_sr(va_to_vsid(pm,up));

		bcopy(p, kp, l);

		up += l;
		kp += l;
		len -= l;
	}

	td->td_pcb->pcb_onfault = NULL;
	return (0);
}

int
copyinstr(const void *udaddr, void *kaddr, size_t len, size_t *done)
{
	struct		thread *td;
	pmap_t		pm;
	faultbuf	env;
	const char	*up;
	char		*kp;
	size_t		l;
	int		rv, c;

	td = PCPU_GET(curthread);
	pm = &td->td_proc->p_vmspace->vm_pmap;

	if (setfault(env)) {
		td->td_pcb->pcb_onfault = NULL;
		return (EFAULT);
	}

	kp = kaddr;
	up = udaddr;

	rv = ENAMETOOLONG;

	for (l = 0; len-- > 0; l++) {
		if ((c = fubyte(up++)) < 0) {
			rv = EFAULT;
			break;
		}

		if (!(*kp++ = c)) {
			l++;
			rv = 0;
			break;
		}
	}

	if (done != NULL) {
		*done = l;
	}

	td->td_pcb->pcb_onfault = NULL;
	return (rv);
}

int
subyte(void *addr, int byte)
{
	struct		thread *td;
	pmap_t		pm;
	faultbuf	env;
	char		*p;

	td = PCPU_GET(curthread);
	pm = &td->td_proc->p_vmspace->vm_pmap;
	p = (char *)((uintptr_t)USER_ADDR + ((uintptr_t)addr & ~SEGMENT_MASK));

	if (setfault(env)) {
		td->td_pcb->pcb_onfault = NULL;
		return (-1);
	}

	set_user_sr(va_to_vsid(pm,addr));

	*p = (char)byte;

	td->td_pcb->pcb_onfault = NULL;
	return (0);
}

#ifdef __powerpc64__
int
suword32(void *addr, int word)
{
	struct		thread *td;
	pmap_t		pm;
	faultbuf	env;
	int		*p;

	td = PCPU_GET(curthread);
	pm = &td->td_proc->p_vmspace->vm_pmap;
	p = (int *)((uintptr_t)USER_ADDR + ((uintptr_t)addr & ~SEGMENT_MASK));

	if (setfault(env)) {
		td->td_pcb->pcb_onfault = NULL;
		return (-1);
	}

	set_user_sr(va_to_vsid(pm,addr));

	*p = word;

	td->td_pcb->pcb_onfault = NULL;
	return (0);
}
#endif

int
suword(void *addr, long word)
{
	struct		thread *td;
	pmap_t		pm;
	faultbuf	env;
	long		*p;

	td = PCPU_GET(curthread);
	pm = &td->td_proc->p_vmspace->vm_pmap;
	p = (long *)((uintptr_t)USER_ADDR + ((uintptr_t)addr & ~SEGMENT_MASK));

	if (setfault(env)) {
		td->td_pcb->pcb_onfault = NULL;
		return (-1);
	}

	set_user_sr(va_to_vsid(pm,addr));

	*p = word;

	td->td_pcb->pcb_onfault = NULL;
	return (0);
}

#ifdef __powerpc64__
int
suword64(void *addr, int64_t word)
{
	return (suword(addr, (long)word));
}
#else
int
suword32(void *addr, int32_t word)
{
	return (suword(addr, (long)word));
}
#endif

int
fubyte(const void *addr)
{
	struct		thread *td;
	pmap_t		pm;
	faultbuf	env;
	u_char		*p;
	int		val;

	td = PCPU_GET(curthread);
	pm = &td->td_proc->p_vmspace->vm_pmap;
	p = (u_char *)((uintptr_t)USER_ADDR +
	    ((uintptr_t)addr & ~SEGMENT_MASK));

	if (setfault(env)) {
		td->td_pcb->pcb_onfault = NULL;
		return (-1);
	}

	set_user_sr(va_to_vsid(pm,addr));

	val = *p;

	td->td_pcb->pcb_onfault = NULL;
	return (val);
}

long
fuword(const void *addr)
{
	struct		thread *td;
	pmap_t		pm;
	faultbuf	env;
	long		*p, val;

	td = PCPU_GET(curthread);
	pm = &td->td_proc->p_vmspace->vm_pmap;
	p = (long *)((uintptr_t)USER_ADDR + ((uintptr_t)addr & ~SEGMENT_MASK));

	if (setfault(env)) {
		td->td_pcb->pcb_onfault = NULL;
		return (-1);
	}

	set_user_sr(va_to_vsid(pm,addr));

	val = *p;

	td->td_pcb->pcb_onfault = NULL;
	return (val);
}

int32_t
fuword32(const void *addr)
{
	return ((int32_t)fuword(addr));
}

uint32_t
casuword32(volatile uint32_t *base, uint32_t oldval, uint32_t newval)
{
	return (casuword((volatile u_long *)base, oldval, newval));
}

u_long
casuword(volatile u_long *addr, u_long old, u_long new)
{
	struct thread *td;
	pmap_t pm;
	faultbuf env;
	u_long *p, val;

	td = PCPU_GET(curthread);
	pm = &td->td_proc->p_vmspace->vm_pmap;
	p = (u_long *)((uintptr_t)USER_ADDR +
	    ((uintptr_t)addr & ~SEGMENT_MASK));

	set_user_sr(va_to_vsid(pm,addr));

	if (setfault(env)) {
		td->td_pcb->pcb_onfault = NULL;
		return (-1);
	}

	val = *p;
	(void) atomic_cmpset_32((volatile uint32_t *)p, old, new);

	td->td_pcb->pcb_onfault = NULL;

	return (val);
}
