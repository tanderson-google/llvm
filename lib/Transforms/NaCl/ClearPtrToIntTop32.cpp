//===- ClearPtrToIntTop32.cpp - Convert pointer values to integer values--===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// In the NaClDontBreakABI mode, in 64 bit architectures, ptrs are 64 bit
// So when ptrs are converted to ints, they may or may not have the top 32 bits cleared
// This pass clears the top 32 bits for consistency to ensure that operations such as
//    ptr1 - ptr2
// produce the expected result
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/NaCl.h"

using namespace llvm;

namespace {
  class ClearPtrToIntTop32 : public FunctionPass, public InstVisitor<ClearPtrToIntTop32, bool> {
    public:

      static char ID; // Pass identification, replacement for typeid
      ClearPtrToIntTop32() : FunctionPass(ID) {
        initializeClearPtrToIntTop32Pass(*PassRegistry::getPassRegistry());
      }

      virtual bool runOnFunction(Function &F);
      bool visitInstruction(Instruction &I) { return false; }
      bool visitPtrToIntInst(PtrToIntInst &I);

  };
}

char ClearPtrToIntTop32::ID = 0;
INITIALIZE_PASS(ClearPtrToIntTop32, "clear-ptr-to-int-top32",
                "Convert pointer values to integer values",
                false, false)

bool ClearPtrToIntTop32::runOnFunction(Function &F) {
  bool Modified = false;

  for (Function::iterator FI = F.begin(), FE = F.end(); FI != FE; ++FI)
    for (BasicBlock::iterator BI = FI->begin(), BE = FI->end(); BI != BE; ++BI)
      Modified |= visit(&*BI);

  return Modified;
}

bool ClearPtrToIntTop32::visitPtrToIntInst(PtrToIntInst &I) {
  if(I.getType()->isIntegerTy())
  {
    Value* ptrOp = I.getPointerOperand();
    Instruction *newPtrToInt = new PtrToIntInst(ptrOp, I.getType(), I.getName(), &I);

    auto I64 = Type::getInt64Ty(I.getContext());
    Instruction *maskInst = BinaryOperator::CreateAnd(newPtrToInt, ConstantInt::get(I64, 0xffffffff), "ptr.clr", &I);

    I.replaceAllUsesWith(maskInst);
    return true;
  }

  return false;
}

FunctionPass *llvm::createClearPtrToIntTop32Pass() {
  return new ClearPtrToIntTop32();
}
