; RUN: opt < %s -instcombine -S | \
; RUN:   grep select | not grep {i32\\*}

; This testcase corresponds to PR362, which notices that this horrible code
; is generated by the C++ front-end and LLVM optimizers, which has lots of
; loads and other stuff that are unneeded.
;
; Instcombine should propagate the load through the select instructions to
; allow elimination of the extra stuff by the mem2reg pass.

define void @_Z5test1RiS_(i32* %x, i32* %y) {
entry:
        %tmp.1.i = load i32* %y         ; <i32> [#uses=1]
        %tmp.3.i = load i32* %x         ; <i32> [#uses=1]
        %tmp.4.i = icmp slt i32 %tmp.1.i, %tmp.3.i              ; <i1> [#uses=1]
        %retval.i = select i1 %tmp.4.i, i32* %y, i32* %x                ; <i32*> [#uses=1]
        %tmp.4 = load i32* %retval.i            ; <i32> [#uses=1]
        store i32 %tmp.4, i32* %x
        ret void
}

define void @_Z5test2RiS_(i32* %x, i32* %y) {
entry:
        %tmp.0 = alloca i32             ; <i32*> [#uses=2]
        %tmp.2 = load i32* %x           ; <i32> [#uses=2]
        store i32 %tmp.2, i32* %tmp.0
        %tmp.3.i = load i32* %y         ; <i32> [#uses=1]
        %tmp.4.i = icmp slt i32 %tmp.2, %tmp.3.i                ; <i1> [#uses=1]
        %retval.i = select i1 %tmp.4.i, i32* %y, i32* %tmp.0            ; <i32*> [#uses=1]
        %tmp.6 = load i32* %retval.i            ; <i32> [#uses=1]
        store i32 %tmp.6, i32* %y
        ret void
}

