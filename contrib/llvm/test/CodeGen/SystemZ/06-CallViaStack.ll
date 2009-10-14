; RUN: llc < %s | grep 168 | count 1
; RUN: llc < %s | grep 160 | count 3
; RUN: llc < %s | grep 328 | count 1

target datalayout = "E-p:64:64:64-i1:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-f128:128:128"
target triple = "s390x-unknown-linux-gnu"

define i64 @foo(i64 %b, i64 %c, i64 %d, i64 %e, i64 %f, i64 %g) nounwind {
entry:
	%a = alloca i64, align 8		; <i64*> [#uses=3]
	store i64 %g, i64* %a
	call void @bar(i64* %a) nounwind
	%tmp1 = load i64* %a		; <i64> [#uses=1]
	ret i64 %tmp1
}

declare void @bar(i64*)
