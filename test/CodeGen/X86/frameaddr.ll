; RUN: llc < %s -march=x86                                | FileCheck %s --check-prefix=CHECK-32
; RUN: llc < %s -march=x86    -fast-isel -fast-isel-abort=1 | FileCheck %s --check-prefix=CHECK-32
; RUN: llc < %s -mtriple=x86_64-pc-win32 -fast-isel | FileCheck %s --check-prefix=CHECK-W64
; RUN: llc < %s -mtriple=x86_64-unknown                             | FileCheck %s --check-prefix=CHECK-64
; RUN: llc < %s -mtriple=x86_64-unknown -fast-isel -fast-isel-abort=1 | FileCheck %s --check-prefix=CHECK-64
; RUN: llc < %s -mtriple=x86_64-gnux32                    | FileCheck %s --check-prefix=CHECK-X32ABI
; RUN: llc < %s -mtriple=x86_64-gnux32 -fast-isel -fast-isel-abort=1 | FileCheck %s --check-prefix=CHECK-X32ABI
; RUN: llc < %s -mtriple=x86_64-nacl                    | FileCheck %s --check-prefix=CHECK-NACL64
; RUN: llc < %s -mtriple=x86_64-nacl -fast-isel -fast-isel-abort=1 | FileCheck %s --check-prefix=CHECK-NACL64

define i8* @test1() nounwind {
entry:
; CHECK-32-LABEL: test1
; CHECK-32:       push
; CHECK-32-NEXT:  movl %esp, %ebp
; CHECK-32-NEXT:  movl %ebp, %eax
; CHECK-32-NEXT:  pop
; CHECK-32-NEXT:  ret
; CHECK-W64-LABEL: test1
; CHECK-W64:       push
; CHECK-W64-NEXT:  movq %rsp, %rbp
; CHECK-W64-NEXT:  leaq (%rbp), %rax
; CHECK-W64-NEXT:  pop
; CHECK-W64-NEXT:  ret
; CHECK-64-LABEL: test1
; CHECK-64:       push
; CHECK-64-NEXT:  movq %rsp, %rbp
; CHECK-64-NEXT:  movq %rbp, %rax
; CHECK-64-NEXT:  pop
; CHECK-64-NEXT:  ret
; CHECK-X32ABI-LABEL: test1
; CHECK-X32ABI:       pushq %rbp
; CHECK-X32ABI-NEXT:  movl %esp, %ebp
; CHECK-X32ABI-NEXT:  movl %ebp, %eax
; CHECK-X32ABI-NEXT:  popq %rbp
; CHECK-X32ABI-NEXT:  ret
; CHECK-NACL64-LABEL: test1
; @LOCALMOD: base address hiding avoids pushing the full rbp onto the stack.
; CHECK-NACL64:       movl %ebp, %eax
; CHECK-NACL64-NEXT:  pushq %rax
; CHECK-NACL64-NEXT:  movq %rsp, %rbp
; CHECK-NACL64-NEXT:  movl %ebp, %eax
  %0 = tail call i8* @llvm.frameaddress(i32 0)
  ret i8* %0
}

define i8* @test2() nounwind {
entry:
; CHECK-32-LABEL: test2
; CHECK-32:       push
; CHECK-32-NEXT:  movl %esp, %ebp
; CHECK-32-NEXT:  movl (%ebp), %eax
; CHECK-32-NEXT:  movl (%eax), %eax
; CHECK-32-NEXT:  pop
; CHECK-32-NEXT:  ret
; CHECK-W64-LABEL: test2
; CHECK-W64:       push
; CHECK-W64-NEXT:  movq %rsp, %rbp
; CHECK-W64-NEXT:  leaq (%rbp), %rax
; CHECK-W64-NEXT:  pop
; CHECK-W64-NEXT:  ret
; CHECK-64-LABEL: test2
; CHECK-64:       push
; CHECK-64-NEXT:  movq %rsp, %rbp
; CHECK-64-NEXT:  movq (%rbp), %rax
; CHECK-64-NEXT:  movq (%rax), %rax
; CHECK-64-NEXT:  pop
; CHECK-64-NEXT:  ret
; CHECK-X32ABI-LABEL: test2
; CHECK-X32ABI:       pushq %rbp
; CHECK-X32ABI-NEXT:  movl %esp, %ebp
; CHECK-X32ABI-NEXT:  movl (%ebp), %eax
; CHECK-X32ABI-NEXT:  movl (%eax), %eax
; CHECK-X32ABI-NEXT:  popq %rbp
; CHECK-X32ABI-NEXT:  ret
; CHECK-NACL64-LABEL: test2
; @LOCALMOD base-address hiding.
; CHECK-NACL64:       movl %ebp, %eax
; CHECK-NACL64-NEXT:  pushq %rax
; CHECK-NACL64-NEXT:  movq %rsp, %rbp
; -fast-isel and -O2 are a bit different in how they load from rbp
; (we can use NACL64-NEXT if we ever make the sequences the same).
; CHECK-NACL64:       movl {{.*}}(%{{.*}}), %eax
; CHECK-NACL64-NEXT:  movl %nacl:(%r15,%rax), %eax
  %0 = tail call i8* @llvm.frameaddress(i32 2)
  ret i8* %0
}

declare i8* @llvm.frameaddress(i32) nounwind readnone
