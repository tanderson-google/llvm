; RUN: pnacl-llc -mtriple=armv7-unknown-nacl -filetype=obj %s -o - \
; RUN:  | llvm-objdump -disassemble -triple armv7 - | FileCheck %s

define i32 @foo() nounwind {
entry:
  ret i32 42
; CHECK: bic	lr, lr, #-1073741809
; CHECK-NEXT: bx lr
}

