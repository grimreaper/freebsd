// RUN: llvm-mc -triple i386-unknown-unknown --show-encoding %s | FileCheck %s

	lfence
// CHECK: lfence
// CHECK: encoding: [0x0f,0xae,0xe8]
	mfence
// CHECK: mfence
// CHECK: encoding: [0x0f,0xae,0xf0]
	monitor
// CHECK: monitor
// CHECK: encoding: [0x0f,0x01,0xc8]
	mwait
// CHECK: mwait
// CHECK: encoding: [0x0f,0x01,0xc9]

	vmcall
// CHECK: vmcall
// CHECK: encoding: [0x0f,0x01,0xc1]
	vmlaunch
// CHECK: vmlaunch
// CHECK: encoding: [0x0f,0x01,0xc2]
	vmresume
// CHECK: vmresume
// CHECK: encoding: [0x0f,0x01,0xc3]
	vmxoff
// CHECK: vmxoff
// CHECK: encoding: [0x0f,0x01,0xc4]
	swapgs
// CHECK: swapgs
// CHECK: encoding: [0x0f,0x01,0xf8]

rdtscp
// CHECK: rdtscp
// CHECK:  encoding: [0x0f,0x01,0xf9]


// CHECK: movl	%eax, 16(%ebp)          # encoding: [0x89,0x45,0x10]
	movl	%eax, 16(%ebp)
// CHECK: movl	%eax, -16(%ebp)          # encoding: [0x89,0x45,0xf0]
	movl	%eax, -16(%ebp)

// CHECK: testb	%bl, %cl                # encoding: [0x84,0xcb]
        testb %bl, %cl

// CHECK: cmpl	%eax, %ebx              # encoding: [0x39,0xc3]
        cmpl %eax, %ebx

// CHECK: addw	%ax, %ax                # encoding: [0x66,0x01,0xc0]
        addw %ax, %ax

// CHECK: shrl	%eax                    # encoding: [0xd1,0xe8]
        shrl $1, %eax
