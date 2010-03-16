// Validate that we can assemble this file exactly like the platform
// assembler.
//
// XFAIL: *
// RUN: llvm-mc -filetype=obj -triple i386-unknown-unknown -o %t.mc.o %s
// RUN: as -arch i386 -o %t.as.o %s
// RUN: diff %t.mc.o %t.as.o

        	movb	$0x7f,0xdeadbeef(%ebx,%ecx,8)
        	movw	$0x7ace,0xdeadbeef(%ebx,%ecx,8)
        	movl	$0x7afebabe,0xdeadbeef(%ebx,%ecx,8)
        	movl	$0x13572468,0xdeadbeef(%ebx,%ecx,8)
        	movsbl	0xdeadbeef(%ebx,%ecx,8),%ecx
        	movswl	0xdeadbeef(%ebx,%ecx,8),%ecx
        	movzbl	0xdeadbeef(%ebx,%ecx,8),%ecx
        	movzwl	0xdeadbeef(%ebx,%ecx,8),%ecx
        	pushl	0xdeadbeef(%ebx,%ecx,8)
        	popl	0xdeadbeef(%ebx,%ecx,8)
        	lahf
        	sahf
        	addb	$0xfe,0xdeadbeef(%ebx,%ecx,8)
        	addb	$0x7f,0xdeadbeef(%ebx,%ecx,8)
        	addw	$0x7ace,0xdeadbeef(%ebx,%ecx,8)
        	addl	$0x7afebabe,0xdeadbeef(%ebx,%ecx,8)
        	addl	$0x13572468,0xdeadbeef(%ebx,%ecx,8)
        	incl	0xdeadbeef(%ebx,%ecx,8)
        	subb	$0xfe,0xdeadbeef(%ebx,%ecx,8)
        	subb	$0x7f,0xdeadbeef(%ebx,%ecx,8)
        	subw	$0x7ace,0xdeadbeef(%ebx,%ecx,8)
        	subl	$0x7afebabe,0xdeadbeef(%ebx,%ecx,8)
        	subl	$0x13572468,0xdeadbeef(%ebx,%ecx,8)
        	decl	0xdeadbeef(%ebx,%ecx,8)
        	sbbw	$0x7ace,0xdeadbeef(%ebx,%ecx,8)
        	sbbl	$0x7afebabe,0xdeadbeef(%ebx,%ecx,8)
        	sbbl	$0x13572468,0xdeadbeef(%ebx,%ecx,8)
        	cmpb	$0xfe,0xdeadbeef(%ebx,%ecx,8)
        	cmpb	$0x7f,0xdeadbeef(%ebx,%ecx,8)
        	cmpw	$0x7ace,0xdeadbeef(%ebx,%ecx,8)
        	cmpl	$0x7afebabe,0xdeadbeef(%ebx,%ecx,8)
        	cmpl	$0x13572468,0xdeadbeef(%ebx,%ecx,8)
        	testb	$0x7f,0xdeadbeef(%ebx,%ecx,8)
        	testw	$0x7ace,0xdeadbeef(%ebx,%ecx,8)
        	testl	$0x7afebabe,0xdeadbeef(%ebx,%ecx,8)
        	testl	$0x13572468,0xdeadbeef(%ebx,%ecx,8)
        	andb	$0xfe,0xdeadbeef(%ebx,%ecx,8)
        	andb	$0x7f,0xdeadbeef(%ebx,%ecx,8)
        	andw	$0x7ace,0xdeadbeef(%ebx,%ecx,8)
        	andl	$0x7afebabe,0xdeadbeef(%ebx,%ecx,8)
        	andl	$0x13572468,0xdeadbeef(%ebx,%ecx,8)
        	orb	$0xfe,0xdeadbeef(%ebx,%ecx,8)
        	orb	$0x7f,0xdeadbeef(%ebx,%ecx,8)
        	orw	$0x7ace,0xdeadbeef(%ebx,%ecx,8)
        	orl	$0x7afebabe,0xdeadbeef(%ebx,%ecx,8)
        	orl	$0x13572468,0xdeadbeef(%ebx,%ecx,8)
        	xorb	$0xfe,0xdeadbeef(%ebx,%ecx,8)
        	xorb	$0x7f,0xdeadbeef(%ebx,%ecx,8)
        	xorw	$0x7ace,0xdeadbeef(%ebx,%ecx,8)
        	xorl	$0x7afebabe,0xdeadbeef(%ebx,%ecx,8)
        	xorl	$0x13572468,0xdeadbeef(%ebx,%ecx,8)
        	adcb	$0xfe,0xdeadbeef(%ebx,%ecx,8)
        	adcb	$0x7f,0xdeadbeef(%ebx,%ecx,8)
        	adcw	$0x7ace,0xdeadbeef(%ebx,%ecx,8)
        	adcl	$0x7afebabe,0xdeadbeef(%ebx,%ecx,8)
        	adcl	$0x13572468,0xdeadbeef(%ebx,%ecx,8)
        	negl	0xdeadbeef(%ebx,%ecx,8)
        	notl	0xdeadbeef(%ebx,%ecx,8)
        	cbtw
        	cwtl
        	cwtd
        	cltd
        	mull	0xdeadbeef(%ebx,%ecx,8)
        	imull	0xdeadbeef(%ebx,%ecx,8)
        	divl	0xdeadbeef(%ebx,%ecx,8)
        	idivl	0xdeadbeef(%ebx,%ecx,8)
        	roll	$0,0xdeadbeef(%ebx,%ecx,8)
        	rolb	$0x7f,0xdeadbeef(%ebx,%ecx,8)
        	roll	0xdeadbeef(%ebx,%ecx,8)
        	rorl	$0,0xdeadbeef(%ebx,%ecx,8)
        	rorb	$0x7f,0xdeadbeef(%ebx,%ecx,8)
        	rorl	0xdeadbeef(%ebx,%ecx,8)
        	shll	$0,0xdeadbeef(%ebx,%ecx,8)
        	shlb	$0x7f,0xdeadbeef(%ebx,%ecx,8)
        	shll	0xdeadbeef(%ebx,%ecx,8)
        	shrl	$0,0xdeadbeef(%ebx,%ecx,8)
        	shrb	$0x7f,0xdeadbeef(%ebx,%ecx,8)
        	shrl	0xdeadbeef(%ebx,%ecx,8)
        	sarl	$0,0xdeadbeef(%ebx,%ecx,8)
        	sarb	$0x7f,0xdeadbeef(%ebx,%ecx,8)
        	sarl	0xdeadbeef(%ebx,%ecx,8)
        	call	*%ecx
        	call	*0xdeadbeef(%ebx,%ecx,8)
        	call	*0xdeadbeef(%ebx,%ecx,8)
        	jmp	*0xdeadbeef(%ebx,%ecx,8)
        	jmp	*0xdeadbeef(%ebx,%ecx,8)
        	ljmpl	*0xdeadbeef(%ebx,%ecx,8)
        	lret
        	leave
        	seto	%bl
        	seto	0xdeadbeef(%ebx,%ecx,8)
        	setno	%bl
        	setno	0xdeadbeef(%ebx,%ecx,8)
        	setb	%bl
        	setb	0xdeadbeef(%ebx,%ecx,8)
        	setae	%bl
        	setae	0xdeadbeef(%ebx,%ecx,8)
        	sete	%bl
        	sete	0xdeadbeef(%ebx,%ecx,8)
        	setne	%bl
        	setne	0xdeadbeef(%ebx,%ecx,8)
        	setbe	%bl
        	setbe	0xdeadbeef(%ebx,%ecx,8)
        	seta	%bl
        	seta	0xdeadbeef(%ebx,%ecx,8)
        	sets	%bl
        	sets	0xdeadbeef(%ebx,%ecx,8)
        	setns	%bl
        	setns	0xdeadbeef(%ebx,%ecx,8)
        	setp	%bl
        	setp	0xdeadbeef(%ebx,%ecx,8)
        	setnp	%bl
        	setnp	0xdeadbeef(%ebx,%ecx,8)
        	setl	%bl
        	setl	0xdeadbeef(%ebx,%ecx,8)
        	setge	%bl
        	setge	0xdeadbeef(%ebx,%ecx,8)
        	setle	%bl
        	setle	0xdeadbeef(%ebx,%ecx,8)
        	setg	%bl
        	setg	0xdeadbeef(%ebx,%ecx,8)
        	nopl	0xdeadbeef(%ebx,%ecx,8)
        	nop
        	fldl	0xdeadbeef(%ebx,%ecx,8)
        	fildl	0xdeadbeef(%ebx,%ecx,8)
        	fildll	0xdeadbeef(%ebx,%ecx,8)
        	fldt	0xdeadbeef(%ebx,%ecx,8)
        	fbld	0xdeadbeef(%ebx,%ecx,8)
        	fstl	0xdeadbeef(%ebx,%ecx,8)
        	fistl	0xdeadbeef(%ebx,%ecx,8)
        	fstpl	0xdeadbeef(%ebx,%ecx,8)
        	fistpl	0xdeadbeef(%ebx,%ecx,8)
        	fistpll	0xdeadbeef(%ebx,%ecx,8)
        	fstpt	0xdeadbeef(%ebx,%ecx,8)
        	fbstp	0xdeadbeef(%ebx,%ecx,8)
        	ficoml	0xdeadbeef(%ebx,%ecx,8)
        	ficompl	0xdeadbeef(%ebx,%ecx,8)
        	fucompp
        	ftst
        	fld1
        	fldz
        	faddl	0xdeadbeef(%ebx,%ecx,8)
        	fiaddl	0xdeadbeef(%ebx,%ecx,8)
        	fsubl	0xdeadbeef(%ebx,%ecx,8)
        	fisubl	0xdeadbeef(%ebx,%ecx,8)
        	fsubrl	0xdeadbeef(%ebx,%ecx,8)
        	fisubrl	0xdeadbeef(%ebx,%ecx,8)
        	fmull	0xdeadbeef(%ebx,%ecx,8)
        	fimull	0xdeadbeef(%ebx,%ecx,8)
        	fdivl	0xdeadbeef(%ebx,%ecx,8)
        	fidivl	0xdeadbeef(%ebx,%ecx,8)
        	fdivrl	0xdeadbeef(%ebx,%ecx,8)
        	fidivrl	0xdeadbeef(%ebx,%ecx,8)
        	fsqrt
        	fsin
        	fcos
        	fchs
        	fabs
        	fldcw	0xdeadbeef(%ebx,%ecx,8)
        	fnstcw	0xdeadbeef(%ebx,%ecx,8)
        	rdtsc
        	sysenter
        	sysexit
        	ud2
        	movnti	%ecx,0xdeadbeef(%ebx,%ecx,8)
        	clflush	0xdeadbeef(%ebx,%ecx,8)
        	emms
        	movd	%ecx,%mm3
        	movd	0xdeadbeef(%ebx,%ecx,8),%mm3
        	movd	%ecx,%xmm5
        	movd	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	movd	%xmm5,%ecx
        	movd	%xmm5,0xdeadbeef(%ebx,%ecx,8)
        	movq	0xdeadbeef(%ebx,%ecx,8),%mm3
        	movq	%mm3,%mm3
        	movq	%mm3,%mm3
        	movq	%xmm5,%xmm5
        	movq	%xmm5,%xmm5
        	packssdw	%mm3,%mm3
        	packssdw	%xmm5,%xmm5
        	packsswb	%mm3,%mm3
        	packsswb	%xmm5,%xmm5
        	packuswb	%mm3,%mm3
        	packuswb	%xmm5,%xmm5
        	paddb	%mm3,%mm3
        	paddb	%xmm5,%xmm5
        	paddw	%mm3,%mm3
        	paddw	%xmm5,%xmm5
        	paddd	%mm3,%mm3
        	paddd	%xmm5,%xmm5
        	paddq	%mm3,%mm3
        	paddq	%xmm5,%xmm5
        	paddsb	%mm3,%mm3
        	paddsb	%xmm5,%xmm5
        	paddsw	%mm3,%mm3
        	paddsw	%xmm5,%xmm5
        	paddusb	%mm3,%mm3
        	paddusb	%xmm5,%xmm5
        	paddusw	%mm3,%mm3
        	paddusw	%xmm5,%xmm5
        	pand	%mm3,%mm3
        	pand	%xmm5,%xmm5
        	pandn	%mm3,%mm3
        	pandn	%xmm5,%xmm5
        	pcmpeqb	%mm3,%mm3
        	pcmpeqb	%xmm5,%xmm5
        	pcmpeqw	%mm3,%mm3
        	pcmpeqw	%xmm5,%xmm5
        	pcmpeqd	%mm3,%mm3
        	pcmpeqd	%xmm5,%xmm5
        	pcmpgtb	%mm3,%mm3
        	pcmpgtb	%xmm5,%xmm5
        	pcmpgtw	%mm3,%mm3
        	pcmpgtw	%xmm5,%xmm5
        	pcmpgtd	%mm3,%mm3
        	pcmpgtd	%xmm5,%xmm5
        	pmaddwd	%mm3,%mm3
        	pmaddwd	%xmm5,%xmm5
        	pmulhw	%mm3,%mm3
        	pmulhw	%xmm5,%xmm5
        	pmullw	%mm3,%mm3
        	pmullw	%xmm5,%xmm5
        	por	%mm3,%mm3
        	por	%xmm5,%xmm5
        	psllw	%mm3,%mm3
        	psllw	%xmm5,%xmm5
        	psllw	$0x7f,%mm3
        	psllw	$0x7f,%xmm5
        	pslld	%mm3,%mm3
        	pslld	%xmm5,%xmm5
        	pslld	$0x7f,%mm3
        	pslld	$0x7f,%xmm5
        	psllq	%mm3,%mm3
        	psllq	%xmm5,%xmm5
        	psllq	$0x7f,%mm3
        	psllq	$0x7f,%xmm5
        	psraw	%mm3,%mm3
        	psraw	%xmm5,%xmm5
        	psraw	$0x7f,%mm3
        	psraw	$0x7f,%xmm5
        	psrad	%mm3,%mm3
        	psrad	%xmm5,%xmm5
        	psrad	$0x7f,%mm3
        	psrad	$0x7f,%xmm5
        	psrlw	%mm3,%mm3
        	psrlw	%xmm5,%xmm5
        	psrlw	$0x7f,%mm3
        	psrlw	$0x7f,%xmm5
        	psrld	%mm3,%mm3
        	psrld	%xmm5,%xmm5
        	psrld	$0x7f,%mm3
        	psrld	$0x7f,%xmm5
        	psrlq	%mm3,%mm3
        	psrlq	%xmm5,%xmm5
        	psrlq	$0x7f,%mm3
        	psrlq	$0x7f,%xmm5
        	psubb	%mm3,%mm3
        	psubb	%xmm5,%xmm5
        	psubw	%mm3,%mm3
        	psubw	%xmm5,%xmm5
        	psubd	%mm3,%mm3
        	psubd	%xmm5,%xmm5
        	psubq	%mm3,%mm3
        	psubq	%xmm5,%xmm5
        	psubsb	%mm3,%mm3
        	psubsb	%xmm5,%xmm5
        	psubsw	%mm3,%mm3
        	psubsw	%xmm5,%xmm5
        	psubusb	%mm3,%mm3
        	psubusb	%xmm5,%xmm5
        	psubusw	%mm3,%mm3
        	psubusw	%xmm5,%xmm5
        	punpckhbw	%mm3,%mm3
        	punpckhbw	%xmm5,%xmm5
        	punpckhwd	%mm3,%mm3
        	punpckhwd	%xmm5,%xmm5
        	punpckhdq	%mm3,%mm3
        	punpckhdq	%xmm5,%xmm5
        	punpcklbw	%mm3,%mm3
        	punpcklbw	%xmm5,%xmm5
        	punpcklwd	%mm3,%mm3
        	punpcklwd	%xmm5,%xmm5
        	punpckldq	%mm3,%mm3
        	punpckldq	%xmm5,%xmm5
        	pxor	%mm3,%mm3
        	pxor	%xmm5,%xmm5
        	addps	%xmm5,%xmm5
        	addss	%xmm5,%xmm5
        	andnps	%xmm5,%xmm5
        	andps	%xmm5,%xmm5
        	cvtpi2ps	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	cvtpi2ps	%mm3,%xmm5
        	cvtps2pi	0xdeadbeef(%ebx,%ecx,8),%mm3
        	cvtps2pi	%xmm5,%mm3
        	cvtsi2ss	%ecx,%xmm5
        	cvtsi2ss	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	cvttps2pi	0xdeadbeef(%ebx,%ecx,8),%mm3
        	cvttps2pi	%xmm5,%mm3
        	cvttss2si	0xdeadbeef(%ebx,%ecx,8),%ecx
        	cvttss2si	%xmm5,%ecx
        	divps	%xmm5,%xmm5
        	divss	%xmm5,%xmm5
        	ldmxcsr	0xdeadbeef(%ebx,%ecx,8)
        	maskmovq	%mm3,%mm3
        	maxps	%xmm5,%xmm5
        	maxss	%xmm5,%xmm5
        	minps	%xmm5,%xmm5
        	minss	%xmm5,%xmm5
        	movaps	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	movaps	%xmm5,%xmm5
        	movaps	%xmm5,0xdeadbeef(%ebx,%ecx,8)
        	movaps	%xmm5,%xmm5
        	movhlps	%xmm5,%xmm5
        	movhps	%xmm5,0xdeadbeef(%ebx,%ecx,8)
        	movlhps	%xmm5,%xmm5
        	movlps	%xmm5,0xdeadbeef(%ebx,%ecx,8)
        	movmskps	%xmm5,%ecx
        	movntps	%xmm5,0xdeadbeef(%ebx,%ecx,8)
        	movntq	%mm3,0xdeadbeef(%ebx,%ecx,8)
        	movntdq	%xmm5,0xdeadbeef(%ebx,%ecx,8)
        	movss	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	movss	%xmm5,%xmm5
        	movss	%xmm5,0xdeadbeef(%ebx,%ecx,8)
        	movss	%xmm5,%xmm5
        	movups	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	movups	%xmm5,%xmm5
        	movups	%xmm5,0xdeadbeef(%ebx,%ecx,8)
        	movups	%xmm5,%xmm5
        	mulps	%xmm5,%xmm5
        	mulss	%xmm5,%xmm5
        	orps	%xmm5,%xmm5
        	pavgb	%mm3,%mm3
        	pavgb	%xmm5,%xmm5
        	pavgw	%mm3,%mm3
        	pavgw	%xmm5,%xmm5
        	pmaxsw	%mm3,%mm3
        	pmaxsw	%xmm5,%xmm5
        	pmaxub	%mm3,%mm3
        	pmaxub	%xmm5,%xmm5
        	pminsw	%mm3,%mm3
        	pminsw	%xmm5,%xmm5
        	pminub	%mm3,%mm3
        	pminub	%xmm5,%xmm5
        	pmovmskb	%mm3,%ecx
        	pmovmskb	%xmm5,%ecx
        	pmulhuw	%mm3,%mm3
        	pmulhuw	%xmm5,%xmm5
        	prefetchnta	0xdeadbeef(%ebx,%ecx,8)
        	prefetcht0	0xdeadbeef(%ebx,%ecx,8)
        	prefetcht1	0xdeadbeef(%ebx,%ecx,8)
        	prefetcht2	0xdeadbeef(%ebx,%ecx,8)
        	psadbw	%mm3,%mm3
        	psadbw	%xmm5,%xmm5
        	rcpps	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	rcpps	%xmm5,%xmm5
        	rcpss	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	rcpss	%xmm5,%xmm5
        	rsqrtps	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	rsqrtps	%xmm5,%xmm5
        	rsqrtss	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	rsqrtss	%xmm5,%xmm5
        	sqrtps	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	sqrtps	%xmm5,%xmm5
        	sqrtss	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	sqrtss	%xmm5,%xmm5
        	stmxcsr	0xdeadbeef(%ebx,%ecx,8)
        	subps	%xmm5,%xmm5
        	subss	%xmm5,%xmm5
        	ucomiss	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	ucomiss	%xmm5,%xmm5
        	unpckhps	%xmm5,%xmm5
        	unpcklps	%xmm5,%xmm5
        	xorps	%xmm5,%xmm5
        	addpd	%xmm5,%xmm5
        	addsd	%xmm5,%xmm5
        	andnpd	%xmm5,%xmm5
        	andpd	%xmm5,%xmm5
        	comisd	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	comisd	%xmm5,%xmm5
        	cvtpi2pd	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	cvtpi2pd	%mm3,%xmm5
        	cvtsi2sd	%ecx,%xmm5
        	cvtsi2sd	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	divpd	%xmm5,%xmm5
        	divsd	%xmm5,%xmm5
        	maxpd	%xmm5,%xmm5
        	maxsd	%xmm5,%xmm5
        	minpd	%xmm5,%xmm5
        	minsd	%xmm5,%xmm5
        	movapd	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	movapd	%xmm5,%xmm5
        	movapd	%xmm5,0xdeadbeef(%ebx,%ecx,8)
        	movapd	%xmm5,%xmm5
        	movhpd	%xmm5,0xdeadbeef(%ebx,%ecx,8)
        	movlpd	%xmm5,0xdeadbeef(%ebx,%ecx,8)
        	movmskpd	%xmm5,%ecx
        	movntpd	%xmm5,0xdeadbeef(%ebx,%ecx,8)
        	movsd	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	movsd	%xmm5,%xmm5
        	movsd	%xmm5,0xdeadbeef(%ebx,%ecx,8)
        	movsd	%xmm5,%xmm5
        	movupd	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	movupd	%xmm5,%xmm5
        	movupd	%xmm5,0xdeadbeef(%ebx,%ecx,8)
        	movupd	%xmm5,%xmm5
        	mulpd	%xmm5,%xmm5
        	mulsd	%xmm5,%xmm5
        	orpd	%xmm5,%xmm5
        	sqrtpd	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	sqrtpd	%xmm5,%xmm5
        	sqrtsd	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	sqrtsd	%xmm5,%xmm5
        	subpd	%xmm5,%xmm5
        	subsd	%xmm5,%xmm5
        	ucomisd	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	ucomisd	%xmm5,%xmm5
        	unpckhpd	%xmm5,%xmm5
        	unpcklpd	%xmm5,%xmm5
        	xorpd	%xmm5,%xmm5
        	cvtdq2pd	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	cvtdq2pd	%xmm5,%xmm5
        	cvtpd2dq	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	cvtpd2dq	%xmm5,%xmm5
        	cvtdq2ps	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	cvtdq2ps	%xmm5,%xmm5
        	cvtpd2pi	0xdeadbeef(%ebx,%ecx,8),%mm3
        	cvtpd2pi	%xmm5,%mm3
        	cvtps2dq	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	cvtps2dq	%xmm5,%xmm5
        	cvtsd2ss	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	cvtsd2ss	%xmm5,%xmm5
        	cvtss2sd	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	cvtss2sd	%xmm5,%xmm5
        	cvttpd2pi	0xdeadbeef(%ebx,%ecx,8),%mm3
        	cvttpd2pi	%xmm5,%mm3
        	cvttsd2si	0xdeadbeef(%ebx,%ecx,8),%ecx
        	cvttsd2si	%xmm5,%ecx
        	maskmovdqu	%xmm5,%xmm5
        	movdqa	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	movdqa	%xmm5,%xmm5
        	movdqa	%xmm5,0xdeadbeef(%ebx,%ecx,8)
        	movdqa	%xmm5,%xmm5
        	movdqu	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	movdqu	%xmm5,0xdeadbeef(%ebx,%ecx,8)
        	movdq2q	%xmm5,%mm3
        	movq2dq	%mm3,%xmm5
        	pmuludq	%mm3,%mm3
        	pmuludq	%xmm5,%xmm5
        	pslldq	$0x7f,%xmm5
        	psrldq	$0x7f,%xmm5
        	punpckhqdq	%xmm5,%xmm5
        	punpcklqdq	%xmm5,%xmm5
        	addsubpd	%xmm5,%xmm5
        	addsubps	%xmm5,%xmm5
        	haddpd	%xmm5,%xmm5
        	haddps	%xmm5,%xmm5
        	hsubpd	%xmm5,%xmm5
        	hsubps	%xmm5,%xmm5
        	lddqu	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	movddup	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	movddup	%xmm5,%xmm5
        	movshdup	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	movshdup	%xmm5,%xmm5
        	movsldup	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	movsldup	%xmm5,%xmm5
        	phaddw	%mm3,%mm3
        	phaddw	%xmm5,%xmm5
        	phaddd	%mm3,%mm3
        	phaddd	%xmm5,%xmm5
        	phaddsw	%mm3,%mm3
        	phaddsw	%xmm5,%xmm5
        	phsubw	%mm3,%mm3
        	phsubw	%xmm5,%xmm5
        	phsubd	%mm3,%mm3
        	phsubd	%xmm5,%xmm5
        	phsubsw	%mm3,%mm3
        	phsubsw	%xmm5,%xmm5
        	pmaddubsw	%mm3,%mm3
        	pmaddubsw	%xmm5,%xmm5
        	pmulhrsw	%mm3,%mm3
        	pmulhrsw	%xmm5,%xmm5
        	pshufb	%mm3,%mm3
        	pshufb	%xmm5,%xmm5
        	psignb	%mm3,%mm3
        	psignb	%xmm5,%xmm5
        	psignw	%mm3,%mm3
        	psignw	%xmm5,%xmm5
        	psignd	%mm3,%mm3
        	psignd	%xmm5,%xmm5
        	pabsb	0xdeadbeef(%ebx,%ecx,8),%mm3
        	pabsb	%mm3,%mm3
        	pabsb	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	pabsb	%xmm5,%xmm5
        	pabsw	0xdeadbeef(%ebx,%ecx,8),%mm3
        	pabsw	%mm3,%mm3
        	pabsw	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	pabsw	%xmm5,%xmm5
        	pabsd	0xdeadbeef(%ebx,%ecx,8),%mm3
        	pabsd	%mm3,%mm3
        	pabsd	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	pabsd	%xmm5,%xmm5
        	femms
        	packusdw	%xmm5,%xmm5
        	pcmpeqq	%xmm5,%xmm5
        	phminposuw	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	phminposuw	%xmm5,%xmm5
        	pmaxsb	%xmm5,%xmm5
        	pmaxsd	%xmm5,%xmm5
        	pmaxud	%xmm5,%xmm5
        	pmaxuw	%xmm5,%xmm5
        	pminsb	%xmm5,%xmm5
        	pminsd	%xmm5,%xmm5
        	pminud	%xmm5,%xmm5
        	pminuw	%xmm5,%xmm5
        	pmovsxbw	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	pmovsxbw	%xmm5,%xmm5
        	pmovsxbd	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	pmovsxbd	%xmm5,%xmm5
        	pmovsxbq	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	pmovsxbq	%xmm5,%xmm5
        	pmovsxwd	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	pmovsxwd	%xmm5,%xmm5
        	pmovsxwq	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	pmovsxwq	%xmm5,%xmm5
        	pmovsxdq	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	pmovsxdq	%xmm5,%xmm5
        	pmovzxbw	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	pmovzxbw	%xmm5,%xmm5
        	pmovzxbd	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	pmovzxbd	%xmm5,%xmm5
        	pmovzxbq	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	pmovzxbq	%xmm5,%xmm5
        	pmovzxwd	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	pmovzxwd	%xmm5,%xmm5
        	pmovzxwq	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	pmovzxwq	%xmm5,%xmm5
        	pmovzxdq	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	pmovzxdq	%xmm5,%xmm5
        	pmuldq	%xmm5,%xmm5
        	pmulld	%xmm5,%xmm5
        	ptest	0xdeadbeef(%ebx,%ecx,8),%xmm5
        	ptest	%xmm5,%xmm5
        	pcmpgtq	%xmm5,%xmm5
