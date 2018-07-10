//===- PNaClABIVerifyFunctions.cpp - Verify PNaCl ABI rules ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Verify function-level PNaCl ABI requirements.
//
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/NaCl/PNaClABIVerifyFunctions.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Analysis/NaCl.h"
#include "llvm/Analysis/NaCl/PNaClABITypeChecker.h"
#include "llvm/Analysis/NaCl/PNaClAllowedIntrinsics.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

// There's no built-in way to get the name of an MDNode, so use a
// string ostream to print it.
static std::string getMDNodeString(unsigned Kind,
                                   const SmallVectorImpl<StringRef> &MDNames) {
  std::string MDName;
  raw_string_ostream N(MDName);
  if (Kind < MDNames.size()) {
    N << "!" << MDNames[Kind];
  } else {
    N << "!<unknown kind #" << Kind << ">";
  }
  return N.str();
}

PNaClABIVerifyFunctions::~PNaClABIVerifyFunctions() {
  if (ReporterIsOwned)
    delete Reporter;
}

// A valid pointer type is either:
//  * a pointer to a valid PNaCl scalar type (except i1), or
//  * a pointer to a valid PNaCl vector type (except i1), or
//  * a function pointer (with valid argument and return types).
//
// i1 is disallowed so that all loads and stores are a whole number of
// bytes, and so that we do not need to define whether a store of i1
// zero-extends.
static bool isValidPointerType(Type *Ty) {
  if (PointerType *PtrTy = dyn_cast<PointerType>(Ty)) {
    if (PtrTy->getAddressSpace() != 0)
      return false;
    Type *EltTy = PtrTy->getElementType();
    if (PNaClABITypeChecker::isValidScalarType(EltTy) && !EltTy->isIntegerTy(1))
      return true;
    if (PNaClABITypeChecker::isValidVectorType(EltTy) &&
        !cast<VectorType>(EltTy)->getElementType()->isIntegerTy(1))
      return true;
    if (FunctionType *FTy = dyn_cast<FunctionType>(EltTy))
      return PNaClABITypeChecker::isValidFunctionType(FTy);
  }
  return false;
}

static bool isIntrinsicFunc(const Value *Val) {
  if (const Function *F = dyn_cast<Function>(Val))
    return F->isIntrinsic();
  return false;
}

// InherentPtrs may be referenced by casts -- PtrToIntInst and
// BitCastInst -- that produce NormalizedPtrs.
//
// InherentPtrs exclude intrinsic functions in order to prevent taking
// the address of an intrinsic function.  InherentPtrs include
// intrinsic calls because some intrinsics return pointer types
// (e.g. nacl.read.tp returns i8*).
static bool isInherentPtr(const Value *Val) {
  return isa<AllocaInst>(Val) ||
         (isa<GlobalValue>(Val) && !isIntrinsicFunc(Val)) ||
         isa<IntrinsicInst>(Val);
}

// NormalizedPtrs may be used where pointer types are required -- for
// loads, stores, etc.  Note that this excludes ConstantExprs,
// ConstantPointerNull and UndefValue.
static bool isNormalizedPtr(const Value *Val) {
  if (!isValidPointerType(Val->getType()))
    return false;
  // The bitcast must also be a bitcast of an InherentPtr, but we
  // check that when visiting the bitcast instruction.
  return isa<IntToPtrInst>(Val) || isa<BitCastInst>(Val) || isInherentPtr(Val);
}

static bool isValidScalarOperand(const Value *Val) {
  // The types of Instructions and Arguments are checked elsewhere
  // (when visiting the Instruction or the Function).  BasicBlocks are
  // included here because branch instructions have BasicBlock
  // operands.
  if (isa<Instruction>(Val) || isa<Argument>(Val) || isa<BasicBlock>(Val))
    return true;

  // Allow some Constants.  Note that this excludes ConstantExprs.
  return PNaClABITypeChecker::isValidScalarType(Val->getType()) &&
         (isa<ConstantInt>(Val) ||
          isa<ConstantFP>(Val) ||
          isa<UndefValue>(Val));
}

static bool isValidVectorOperand(const Value *Val) {
  // The types of Instructions and Arguments are checked elsewhere.
  if (isa<Instruction>(Val) || isa<Argument>(Val))
    return true;
  // Contrary to scalars, constant vector values aren't allowed on
  // instructions, except undefined. Constant vectors are loaded from
  // constant global memory instead, and can be rematerialized as
  // constants by the backend if need be.
  return PNaClABITypeChecker::isValidVectorType(Val->getType()) &&
         isa<UndefValue>(Val);
}

static bool hasAllowedAtomicRMWOperation(
    const NaCl::AtomicIntrinsics::AtomicIntrinsic *I, const CallInst *Call) {
  for (size_t P = 0; P != I->NumParams; ++P) {
    if (I->ParamType[P] != NaCl::AtomicIntrinsics::RMW)
      continue;

    const Value *Operation = Call->getOperand(P);
    if (!Operation)
      return false;
    const Constant *C = dyn_cast<Constant>(Operation);
    if (!C)
      return false;
    const APInt &I = C->getUniqueInteger();
    if (I.ule(NaCl::AtomicInvalid) || I.uge(NaCl::AtomicNum))
      return false;
  }
  return true;
}

static bool
hasAllowedAtomicMemoryOrder(const NaCl::AtomicIntrinsics::AtomicIntrinsic *I,
                            const CallInst *Call) {
  NaCl::MemoryOrder PreviousOrder = NaCl::MemoryOrderInvalid;

  for (size_t P = 0; P != I->NumParams; ++P) {
    if (I->ParamType[P] != NaCl::AtomicIntrinsics::Mem)
      continue;

    NaCl::MemoryOrder Order = NaCl::MemoryOrderInvalid;
    if (const Value *MemoryOrderOperand = Call->getOperand(P))
      if (const Constant *C = dyn_cast<Constant>(MemoryOrderOperand)) {
        const APInt &I = C->getUniqueInteger();
        if (I.ugt(NaCl::MemoryOrderInvalid) && I.ult(NaCl::MemoryOrderNum))
          Order = static_cast<NaCl::MemoryOrder>(I.getLimitedValue());
      }
    if (Order == NaCl::MemoryOrderInvalid)
      return false;

    // Validate PNaCl restrictions.
    switch (Order) {
    case NaCl::MemoryOrderInvalid:
    case NaCl::MemoryOrderNum:
      llvm_unreachable("Invalid memory order");
    case NaCl::MemoryOrderRelaxed:
    case NaCl::MemoryOrderConsume:
      // TODO(jfb) PNaCl doesn't allow relaxed or consume memory ordering.
      return false;
    case NaCl::MemoryOrderAcquire:
    case NaCl::MemoryOrderRelease:
    case NaCl::MemoryOrderAcquireRelease:
    case NaCl::MemoryOrderSequentiallyConsistent:
      break; // Allowed by PNaCl.
    }

    // Validate conformance to the C++11 memory model.
    switch (I->ID) {
    default:
      llvm_unreachable("unexpected atomic operation");
    case Intrinsic::nacl_atomic_load:
      // C++11 [atomics.types.operations.req]: The order argument shall not be
      // release nor acq_rel.
      if (Order == NaCl::MemoryOrderRelease ||
          Order == NaCl::MemoryOrderAcquireRelease)
        return false;
      break;
    case Intrinsic::nacl_atomic_store:
      // C++11 [atomics.types.operations.req]: The order argument shall not be
      // consume, acquire, nor acq_rel.
      if (Order == NaCl::MemoryOrderConsume ||
          Order == NaCl::MemoryOrderAcquire ||
          Order == NaCl::MemoryOrderAcquireRelease)
        return false;
      break;
    case Intrinsic::nacl_atomic_rmw:
      break; // No restriction.
    case Intrinsic::nacl_atomic_cmpxchg:
      // C++11 [atomics.types.operations.req]: The failure argument shall not be
      // release nor acq_rel. The failure argument shall be no stronger than the
      // success argument.
      // Where the partial ordering is:
      //   relaxed < consume < acquire           < acq_rel < seq_cst
      //   relaxed <                     release < acq_rel < seq_cst
      if (PreviousOrder != NaCl::MemoryOrderInvalid) { // Failure ordering.
        NaCl::MemoryOrder Success = PreviousOrder, Failure = Order;
        if (Failure == NaCl::MemoryOrderRelease ||
            Failure == NaCl::MemoryOrderAcquireRelease)
          return false;
        if ((Success < Failure) || (Success == NaCl::MemoryOrderRelease &&
                                    Failure != NaCl::MemoryOrderRelaxed))
          return false;
      }
      break; // Success ordering has no restriction.
    case Intrinsic::nacl_atomic_fence:
    case Intrinsic::nacl_atomic_fence_all:
      break; // No restrictions.
    }

    PreviousOrder = Order;
  }

  return true;
}

static bool hasAllowedLockFreeByteSize(const CallInst *Call) {
  if (!Call->getType()->isIntegerTy())
    return false;
  const Value *Operation = Call->getOperand(0);
  if (!Operation)
    return false;
  const Constant *C = dyn_cast<Constant>(Operation);
  if (!C)
    return false;
  const APInt &I = C->getUniqueInteger();
  // PNaCl currently only supports atomics of byte size {1,2,4,8} (which
  // may or may not be lock-free). These values coincide with
  // C11/C++11's supported atomic types.
  if (I == 1 || I == 2 || I == 4 || I == 8)
    return true;
  return false;
}

// Check the instruction's opcode and its operands.  The operands may
// require opcode-specific checking.
//
// This returns an error string if the instruction is rejected, or
// NULL if the instruction is allowed.
const char *PNaClABIVerifyFunctions::checkInstruction(const DataLayout *DL,
                                                      const Instruction *Inst) {
  // If the instruction has a single pointer operand, PtrOperandIndex is
  // set to its operand index.
  unsigned PtrOperandIndex = -1;

  // True if we should apply the default operand checks, at the end
  // of this function.
  bool ApplyDefaultOperandTypeChecks = true;

  switch (Inst->getOpcode()) {
    // Disallowed instructions. Default is to disallow.
    // We expand GetElementPtr out into arithmetic.
    case Instruction::GetElementPtr:
    // VAArg is expanded out by ExpandVarArgs.
    case Instruction::VAArg:
    // Zero-cost C++ exception handling is not supported yet.
    case Instruction::Invoke:
    case Instruction::LandingPad:
    case Instruction::Resume:
    // indirectbr may interfere with streaming
    case Instruction::IndirectBr:
    // TODO(jfb) Figure out ShuffleVector.
    case Instruction::ShuffleVector:
    // ExtractValue and InsertValue operate on struct values.
    case Instruction::ExtractValue:
    case Instruction::InsertValue:
    // Atomics should become NaCl intrinsics.
    case Instruction::AtomicCmpXchg:
    case Instruction::AtomicRMW:
    case Instruction::Fence:
      return "bad instruction opcode";
    default:
      return "unknown instruction opcode";

    // Terminator instructions
    case Instruction::Ret:
    case Instruction::Br:
    case Instruction::Unreachable:
    // Binary operations
    case Instruction::FAdd:
    case Instruction::FSub:
    case Instruction::FMul:
    case Instruction::FDiv:
    case Instruction::FRem:
    // Bitwise binary operations
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
    // Conversion operations
    case Instruction::Trunc:
    case Instruction::ZExt:
    case Instruction::SExt:
    case Instruction::FPTrunc:
    case Instruction::FPExt:
    case Instruction::FPToUI:
    case Instruction::FPToSI:
    case Instruction::UIToFP:
    case Instruction::SIToFP:
    // Other operations
    case Instruction::FCmp:
    case Instruction::PHI:
    case Instruction::Select:
      break;

    // The following operations are of dubious usefulness on 1-bit
    // values.  Use of the i1 type is disallowed here so that code
    // generators do not need to support these corner cases.
    case Instruction::ICmp:
    // Binary operations
    case Instruction::Add:
    case Instruction::Sub:
    case Instruction::Mul:
    case Instruction::UDiv:
    case Instruction::SDiv:
    case Instruction::URem:
    case Instruction::SRem:
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr: {
      const Type *Ty = Inst->getOperand(0)->getType();
      if (!PNaClABITypeChecker::isValidIntArithmeticType(
              Inst->getOperand(0)->getType())) {
        if (Ty->isIntegerTy() ||
            (Ty->isVectorTy() && Ty->getVectorElementType()->isIntegerTy())) {
          return "Invalid integer arithmetic type";
        } else {
          return "Expects integer arithmetic type";
        }
      }
      ApplyDefaultOperandTypeChecks = false;
      break;
    }

    // Vector.
    case Instruction::ExtractElement:
    case Instruction::InsertElement: {
      // Insert and extract element are restricted to constant indices
      // that are in range to prevent undefined behavior.
      // TODO(kschimpf) Figure out way to put test into pnacl-bcdis?
      Value *Vec = Inst->getOperand(0);
      Value *Idx = Inst->getOperand(
          Instruction::InsertElement == Inst->getOpcode() ? 2 : 1);
      if (!isa<ConstantInt>(Idx))
        return "non-constant vector insert/extract index";
      if (!PNaClABIProps::isVectorIndexSafe(
              cast<ConstantInt>(Idx)->getValue(),
              cast<VectorType>(Vec->getType())->getNumElements())) {
        return "out of range vector insert/extract index";
      }
      break;
    }

    // Memory accesses.
    case Instruction::Load: {
      const LoadInst *Load = cast<LoadInst>(Inst);
      PtrOperandIndex = Load->getPointerOperandIndex();
      if (Load->isAtomic())
        return "atomic load";
      if (Load->isVolatile())
        return "volatile load";
      if (!isNormalizedPtr(Inst->getOperand(PtrOperandIndex)))
        return "bad pointer";
      if (!PNaClABIProps::
          isAllowedAlignment(DL, Load->getAlignment(), Load->getType()))
        return "bad alignment";
      break;
    }
    case Instruction::Store: {
      const StoreInst *Store = cast<StoreInst>(Inst);
      PtrOperandIndex = Store->getPointerOperandIndex();
      if (Store->isAtomic())
        return "atomic store";
      if (Store->isVolatile())
        return "volatile store";
      if (!isNormalizedPtr(Inst->getOperand(PtrOperandIndex)))
        return "bad pointer";
      if (!PNaClABIProps::
          isAllowedAlignment(DL, Store->getAlignment(),
                             Store->getValueOperand()->getType()))
        return "bad alignment";
      break;
    }

    // Casts.
    case Instruction::BitCast:
      if (Inst->getType()->isPointerTy()) {
        PtrOperandIndex = 0;
        if (!isInherentPtr(Inst->getOperand(PtrOperandIndex)))
          return "operand not InherentPtr";
      }
      break;
    case Instruction::IntToPtr:
      if (!cast<IntToPtrInst>(Inst)->getSrcTy()->isIntegerTy(32))
        return "non-i32 inttoptr";
      break;
    case Instruction::PtrToInt:
      PtrOperandIndex = 0;
      if (!isInherentPtr(Inst->getOperand(PtrOperandIndex)))
        return "operand not InherentPtr";
      if (!Inst->getType()->isIntegerTy(32))
        return "non-i32 ptrtoint";
      break;

    case Instruction::Alloca: {
      const AllocaInst *Alloca = cast<AllocaInst>(Inst);
      if (!PNaClABIProps::isAllocaAllocatedType(Alloca->getAllocatedType()))
        return "non-i8 alloca";
      if (!PNaClABIProps::isAllocaSizeType(Alloca->getArraySize()->getType()))
        return PNaClABIProps::ExpectedAllocaSizeType();
      break;
    }

    case Instruction::Call: {
      const CallInst *Call = cast<CallInst>(Inst);
      if (Call->isInlineAsm())
        return "inline assembly";
      if (!Call->getAttributes().isEmpty())
        return "bad call attributes";
      if (!PNaClABIProps::isValidCallingConv(Call->getCallingConv()))
        return "bad calling convention";

      // Intrinsic calls can have multiple pointer arguments and
      // metadata arguments, so handle them specially.
      // TODO(kschimpf) How can we lift this to pnacl-bcdis.
      if (const IntrinsicInst *Call = dyn_cast<IntrinsicInst>(Inst)) {
        if (PNaClAllowedIntrinsics::isAllowedDebugInfoIntrinsic(
                Call->getIntrinsicID())) {
          // If debug metadata is allowed, always allow calling debug intrinsics
          // and assume they are correct.
          return nullptr;
        }
        for (unsigned ArgNum = 0, E = Call->getNumArgOperands();
             ArgNum < E; ++ArgNum) {
          const Value *Arg = Call->getArgOperand(ArgNum);
          if (!(isValidScalarOperand(Arg) ||
                isValidVectorOperand(Arg) ||
                isNormalizedPtr(Arg)))
            return "bad intrinsic operand";
        }

        // Disallow alignments other than 1 on memcpy() etc., for the
        // same reason that we disallow them on integer loads and
        // stores.
        if (const MemIntrinsic *MemOp = dyn_cast<MemIntrinsic>(Call)) {
          // Avoid the getAlignment() method here because it aborts if
          // the alignment argument is not a Constant.
          Value *AlignArg = MemOp->getArgOperand(3);
          if (!isa<ConstantInt>(AlignArg) ||
              cast<ConstantInt>(AlignArg)->getZExtValue() != 1) {
            return "bad alignment";
          }
        }

        switch (Call->getIntrinsicID()) {
          default: break;  // Other intrinsics don't require checks.
          // Disallow NaCl atomic intrinsics which don't have valid
          // constant NaCl::AtomicOperation and NaCl::MemoryOrder
          // parameters.
          case Intrinsic::nacl_atomic_load:
          case Intrinsic::nacl_atomic_store:
          case Intrinsic::nacl_atomic_rmw:
          case Intrinsic::nacl_atomic_cmpxchg:
          case Intrinsic::nacl_atomic_fence:
          case Intrinsic::nacl_atomic_fence_all: {
            // All overloads have memory order and RMW operation in the
            // same parameter, arbitrarily use the I32 overload.
            Type *T = Type::getInt32Ty(
                Inst->getParent()->getParent()->getContext());
            const NaCl::AtomicIntrinsics::AtomicIntrinsic *I =
                AtomicIntrinsics->find(Call->getIntrinsicID(), T);
            if (!I)
              // All intrinsics have an I32 overload. Failure here means there
              // is no such intrinsic.
              return "invalid atomic intrinsic";
            if (!hasAllowedAtomicMemoryOrder(I, Call))
              return "invalid memory order";
            if (!hasAllowedAtomicRMWOperation(I, Call))
              return "invalid atomicRMW operation";
          } break;
          // Disallow NaCl atomic_is_lock_free intrinsics which don't
          // have valid constant size type.
          case Intrinsic::nacl_atomic_is_lock_free:
            if (!hasAllowedLockFreeByteSize(Call))
              return "invalid atomic lock-free byte size";
            break;
        }

        // Allow the instruction and skip the later checks.
        return NULL;
      }

      // The callee is the last operand.
      PtrOperandIndex = Inst->getNumOperands() - 1;
      if (!isNormalizedPtr(Inst->getOperand(PtrOperandIndex)))
        return "bad function callee operand";
      break;
    }

    case Instruction::Switch: {
      // SwitchInst represents switch cases using array and vector
      // constants, which we normally reject, so we must check
      // SwitchInst specially here.
      const SwitchInst *Switch = cast<SwitchInst>(Inst);
      if (!isValidScalarOperand(Switch->getCondition()))
        return "bad switch condition";
      const Type *SwitchType = Switch->getCondition()->getType();
      if (!PNaClABITypeChecker::isValidSwitchConditionType(SwitchType))
        return PNaClABITypeChecker::ExpectedSwitchConditionType(SwitchType);

      // SwitchInst requires the cases to be ConstantInts, but it
      // doesn't require their types to be the same as the condition
      // value, so check all the cases too.
      for (SwitchInst::ConstCaseIt Case = Switch->case_begin(),
             E = Switch->case_end(); Case != E; ++Case) {
        if (!isValidScalarOperand(Case.getCaseValue()))
          return "bad switch case";
      }

      // Allow the instruction and skip the later checks.
      return NULL;
    }
  }

  if (ApplyDefaultOperandTypeChecks) {
    // Check the instruction's operands.  We have already checked any
    // pointer operands.  Any remaining operands must be scalars or vectors.
    for (unsigned OpNum = 0, E = Inst->getNumOperands(); OpNum < E; ++OpNum) {
      if (OpNum != PtrOperandIndex &&
          !(isValidScalarOperand(Inst->getOperand(OpNum)) ||
            isValidVectorOperand(Inst->getOperand(OpNum))))
        return "bad operand";
    }
  }

  // Check arithmetic attributes.
  if (const OverflowingBinaryOperator *Op =
          dyn_cast<OverflowingBinaryOperator>(Inst)) {
    if (Op->hasNoUnsignedWrap())
      return "has \"nuw\" attribute";
    if (Op->hasNoSignedWrap())
      return "has \"nsw\" attribute";
  }
  if (const PossiblyExactOperator *Op =
          dyn_cast<PossiblyExactOperator>(Inst)) {
    if (Op->isExact())
      return "has \"exact\" attribute";
  }

  // Allow the instruction.
  return NULL;
}

bool PNaClABIVerifyFunctions::runOnFunction(Function &F) {
  const DataLayout *DL = &F.getParent()->getDataLayout();
  SmallVector<StringRef, 8> MDNames;
  F.getContext().getMDKindNames(MDNames);

  for (Function::const_iterator FI = F.begin(), FE = F.end();
           FI != FE; ++FI) {
    for (BasicBlock::const_iterator BBI = FI->begin(), BBE = FI->end();
             BBI != BBE; ++BBI) {
      const Instruction *Inst = BBI;
      // Check the instruction opcode first.  This simplifies testing,
      // because some instruction opcodes must be rejected out of hand
      // (regardless of the instruction's result type) and the tests
      // check the reason for rejection.
      const char *Error = checkInstruction(DL, BBI);
      // Check the instruction's result type.
      bool BadResult = false;
      if (!Error && !(PNaClABITypeChecker::isValidScalarType(Inst->getType()) ||
                      PNaClABITypeChecker::isValidVectorType(Inst->getType()) ||
                      isNormalizedPtr(Inst) ||
                      isa<AllocaInst>(Inst))) {
        Error = "bad result type";
        BadResult = true;
      }
      if (Error) {
        Reporter->addError()
            << "Function " << F.getName() << " disallowed: " << Error << ": "
            << (BadResult ? PNaClABITypeChecker::getTypeName(BBI->getType())
                          : "") << " " << *BBI << "\n";
      }

      // Check instruction attachment metadata.
      SmallVector<std::pair<unsigned, MDNode*>, 4> MDForInst;
      BBI->getAllMetadata(MDForInst);

      for (unsigned i = 0, e = MDForInst.size(); i != e; i++) {
        if (!PNaClABIProps::isWhitelistedMetadata(MDForInst[i].first)) {
          Reporter->addError()
              << "Function " << F.getName()
              << " has disallowed instruction metadata: "
              << getMDNodeString(MDForInst[i].first, MDNames) << "\n";
        }
      }
    }
  }

  Reporter->checkForFatalErrors();
  return false;
}

// This method exists so that the passes can easily be run with opt -analyze.
// In this case the default constructor is used and we want to reset the error
// messages after each print.
void PNaClABIVerifyFunctions::print(llvm::raw_ostream &O, const Module *M)
    const {
  Reporter->printErrors(O);
  Reporter->reset();
}

char PNaClABIVerifyFunctions::ID = 0;
INITIALIZE_PASS(PNaClABIVerifyFunctions, "verify-pnaclabi-functions",
                "Verify functions for PNaCl", false, true)

FunctionPass *llvm::createPNaClABIVerifyFunctionsPass(
    PNaClABIErrorReporter *Reporter) {
  return new PNaClABIVerifyFunctions(Reporter);
}
