//===- SandboxMemoryAccesses.cpp - Apply SFI sandboxing to used pointers --===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass applies SFI sandboxing to all memory access instructions in the IR.
// Pointers are truncated to a given number of bits and shifted into a memory
// region allocated by the runtime. The runtime reads the pointer bit size
// from the "__sfi_pointer_size" exported constant and stores the base of the
// correspondingly-sized memory region into the "__sfi_memory_base" global
// variable.
//
// This is meant to be the next to last pass of MinSFI, followed only by a CFI
// pass. Because there is no runtime verifier, it must be trusted to correctly
// sandbox all dereferenced pointers.
//
// Sandboxed instructions:
//  - load, store
//  - memcpy, memmove, memset
//  - @llvm.nacl.atomic.load.*
//  - @llvm.nacl.atomic.store.*
//  - @llvm.nacl.atomic.rmw.*
//  - @llvm.nacl.atomic.cmpxchg.*
//
// Whitelisted instructions:
//  - ptrtoint
//  - bitcast
//
// This pass fails if code contains instructions with pointer-type operands
// not listed above. PtrToInt and BitCast instructions are whitelisted because
// they do not access memory and therefore do not need to be sandboxed.
//
// The pass recognizes the pointer arithmetic produced by ExpandGetElementPtr
// and reuses its final integer value to save target instructions. This
// optimization, as well as the memcpy, memmove and memset intrinsics, is safe
// only if the runtime creates a guard region after the dedicated memory region.
// The guard region must be the same size as the memory region.
//
// Both 32-bit and 64-bit architectures are supported. The necessary pointer
// arithmetic generated by the pass always uses 64-bit integers. However, when
// compiling for 32-bit targets, the backend is expected to optimize the code
// by deducing that the top bits are always truncated during the final cast to
// a pointer.
//
// The size of the runtime address subspace can be changed with the
// "-minsfi-ptrsize" command-line option. Depending on the target architecture,
// the value of this constant can have an effect on the efficiency of the
// generated code. On x86-64 and AArch64, 32-bit subspace is the most efficient
// because pointers can be sandboxed without bit masking. On AArch32, subspaces
// of 24-31 bits will be more efficient because the bit mask fits into a single
// BIC instruction immediate. Code for x86 and MIPS is the same for all values.
//
//===----------------------------------------------------------------------===//

#include "llvm/Pass.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NaClAtomicIntrinsics.h"
#include "llvm/Transforms/MinSFI.h"
#include "llvm/Transforms/NaCl.h"

using namespace llvm;

static const char ExternalSymName_MemoryBase[] = "__sfi_memory_base";
static const char ExternalSymName_PointerSize[] = "__sfi_pointer_size";

namespace {
// This pass needs to be a ModulePass because it adds a GlobalVariable.
class SandboxMemoryAccesses : public ModulePass {
  Value *MemBaseVar;
  Value *PtrMask;
  DataLayout *DL;
  Type *I32;
  Type *I64;

  void sandboxPtrOperand(Instruction *Inst, unsigned int OpNum,
                         bool IsFirstClassValueAccess, Function &Func,
                         Value **MemBase);
  void sandboxLenOperand(Instruction *Inst, unsigned int OpNum);
  void checkDoesNotHavePointerOperands(Instruction *Inst);
  void runOnFunction(Function &Func);

 public:
  static char ID;
  SandboxMemoryAccesses() : ModulePass(ID), MemBaseVar(NULL), PtrMask(NULL),
                            DL(NULL), I32(NULL), I64(NULL) {
    initializeSandboxMemoryAccessesPass(*PassRegistry::getPassRegistry());
  }

  virtual bool runOnModule(Module &M);
};
}  // namespace

bool SandboxMemoryAccesses::runOnModule(Module &M) {
  DataLayout Layout(&M);
  DL = &Layout;
  I32 = Type::getInt32Ty(M.getContext());
  I64 = Type::getInt64Ty(M.getContext());

  // Create a global variable with external linkage that will hold the base
  // address of the sandbox. This variable is defined and initialized by
  // the runtime. We assume that all original global variables have been 
  // removed during the AllocateDataSegment pass.
  MemBaseVar = M.getOrInsertGlobal(ExternalSymName_MemoryBase, I64);

  // Create an exported global constant holding the size of the sandboxed
  // pointers. If it is smaller than 32 bits, prepare the corresponding bit mask
  // will later be applied on pointer and length arguments of instructions.
  unsigned int PointerSize = minsfi::GetPointerSizeInBits();
  new GlobalVariable(M, I32, /*isConstant=*/true,
                     GlobalVariable::ExternalLinkage,
                     ConstantInt::get(I32, PointerSize),
                     ExternalSymName_PointerSize);
  if (PointerSize < 32)
    PtrMask = ConstantInt::get(I32, (1U << PointerSize) - 1);

  for (Module::iterator Func = M.begin(), E = M.end(); Func != E; ++Func)
    runOnFunction(*Func);

  return true;
}

void SandboxMemoryAccesses::sandboxPtrOperand(Instruction *Inst,
                                              unsigned int OpNum,
                                              bool IsFirstClassValueAccess,
                                              Function &Func, Value **MemBase) {
  // Function must first acquire the sandbox memory region base from
  // the global variable. If this is the first sandboxed pointer, insert
  // the corresponding load instruction at the beginning of the function.
  if (!*MemBase) {
    Instruction *MemBaseInst = new LoadInst(MemBaseVar, "mem_base");
    Func.getEntryBlock().getInstList().push_front(MemBaseInst);
    *MemBase = MemBaseInst;
  }

  Value *Ptr = Inst->getOperand(OpNum);
  Value *Truncated = NULL, *OffsetConst = NULL;

  // The ExpandGetElementPtr pass replaces the getelementptr instruction
  // with pointer arithmetic. If we recognize that pointer arithmetic pattern
  // here, we can sandbox the pointer more efficiently than in the general
  // case below.
  //
  // The recognized pattern is:
  //   %0 = add i32 %x, <const>               ; treated as signed, must be >= 0
  //   %ptr = inttoptr i32 %0 to <type>*
  // and can be replaced with:
  //   %0 = zext i32 %x to i64
  //   %1 = add i64 %0, %mem_base
  //   %2 = add i64 %1, <const>               ; extended to i64
  //   %ptr = inttoptr i64 %2 to <type>*
  //
  // Since this enables the code to access memory outside the dedicated region,
  // this is safe only if the memory region is followed by an equally sized
  // guard region.

  bool OptimizeGEP = false;
  Instruction *RedundantCast = NULL, *RedundantAdd = NULL;
  if (IsFirstClassValueAccess) {
    if (IntToPtrInst *Cast = dyn_cast<IntToPtrInst>(Ptr)) {
      if (BinaryOperator *Op = dyn_cast<BinaryOperator>(Cast->getOperand(0))) {
        if (Op->getOpcode() == Instruction::Add) {
          if (Op->getType()->isIntegerTy(32)) {
            if (ConstantInt *CI = dyn_cast<ConstantInt>(Op->getOperand(1))) {
              Type *ValType = Ptr->getType()->getPointerElementType();
              int64_t MaxOffset = minsfi::GetAddressSubspaceSize() -
                                  DL->getTypeStoreSize(ValType);
              int64_t Offset = CI->getSExtValue();
              if ((Offset >= 0) && (Offset <= MaxOffset)) {
                Truncated = Op->getOperand(0);
                OffsetConst = ConstantInt::get(I64, Offset);
                RedundantCast = Cast;
                RedundantAdd = Op;
                OptimizeGEP = true;
              }
            }
          }
        }
      }
    }
  }

  // If the pattern above has not been recognized, start by truncating
  // the pointer to i32.
  if (!OptimizeGEP)
    Truncated = new PtrToIntInst(Ptr, I32, "", Inst);

  // If the address subspace is smaller than 32 bits, truncate the pointer
  // further with a bit mask.
  if (PtrMask)
    Truncated = BinaryOperator::CreateAnd(Truncated, PtrMask, "", Inst);

  // Sandbox the pointer by zero-extending it back to 64 bits, and adding
  // the memory region base.
  Instruction *Extend = new ZExtInst(Truncated, I64, "", Inst);
  Instruction *AddBase = BinaryOperator::CreateAdd(*MemBase, Extend, "", Inst);
  Instruction *AddOffset =
      OptimizeGEP ? BinaryOperator::CreateAdd(AddBase, OffsetConst, "", Inst)
                  : AddBase;
  Instruction *SandboxedPtr =
      new IntToPtrInst(AddOffset, Ptr->getType(), "", Inst);

  // Replace the pointer in the sandboxed operand
  Inst->setOperand(OpNum, SandboxedPtr);

  if (OptimizeGEP) {
    // Copy debug information
    CopyDebug(AddOffset, RedundantAdd);
    CopyDebug(SandboxedPtr, RedundantCast);

    // Remove instructions if now dead (order matters)
    if (RedundantCast->use_empty())
      RedundantCast->eraseFromParent();
    if (RedundantAdd->use_empty())
      RedundantAdd->eraseFromParent();
  }
}

void SandboxMemoryAccesses::sandboxLenOperand(Instruction *Inst,
                                              unsigned int OpNum) {
  // Length is assumed to be an i32 value. If the address subspace is smaller,
  // truncate the value with a bit mask.
  if (PtrMask) {
    Value *Len = Inst->getOperand(OpNum);
    Instruction *MaskedLen = BinaryOperator::CreateAnd(Len, PtrMask, "", Inst);
    Inst->setOperand(OpNum, MaskedLen);
  }
}

void SandboxMemoryAccesses::checkDoesNotHavePointerOperands(Instruction *Inst) {
  bool hasPointerOperand = false;

  // Handle Call instructions separately because they always contain
  // a pointer to the target function. Integrity of calls is guaranteed by CFI.
  // This pass therefore only checks the function's arguments.
  if (CallInst *Call = dyn_cast<CallInst>(Inst)) {
    for (unsigned int I = 0, E = Call->getNumArgOperands(); I < E; ++I)
      hasPointerOperand |= Call->getArgOperand(I)->getType()->isPointerTy();
  } else {
    for (unsigned int I = 0, E = Inst->getNumOperands(); I < E; ++I)
      hasPointerOperand |= Inst->getOperand(I)->getType()->isPointerTy();
  }

  if (hasPointerOperand)
    report_fatal_error("SandboxMemoryAccesses: unexpected instruction with "
                       "pointer-type operands");
}

void SandboxMemoryAccesses::runOnFunction(Function &Func) {
  Value *MemBase = NULL;

  for (Function::iterator BB = Func.begin(), E = Func.end(); BB != E; ++BB) {
    for (BasicBlock::iterator Inst = BB->begin(), E = BB->end(); Inst != E; 
         ++Inst) {
      if (isa<LoadInst>(Inst)) {
        sandboxPtrOperand(Inst, 0, true, Func, &MemBase);
      } else if (isa<StoreInst>(Inst)) {
        sandboxPtrOperand(Inst, 1, true, Func, &MemBase);
      } else if (isa<MemCpyInst>(Inst) || isa<MemMoveInst>(Inst)) {
        sandboxPtrOperand(Inst, 0, false, Func, &MemBase);
        sandboxPtrOperand(Inst, 1, false, Func, &MemBase);
        sandboxLenOperand(Inst, 2);
      } else if (isa<MemSetInst>(Inst)) {
        sandboxPtrOperand(Inst, 0, false, Func, &MemBase);
        sandboxLenOperand(Inst, 2);
      } else if (IntrinsicInst *IntrCall = dyn_cast<IntrinsicInst>(Inst)) {
        switch (IntrCall->getIntrinsicID()) {
        case Intrinsic::nacl_atomic_load:
        case Intrinsic::nacl_atomic_cmpxchg:
          sandboxPtrOperand(IntrCall, 0, true, Func, &MemBase);
          break;
        case Intrinsic::nacl_atomic_store:
        case Intrinsic::nacl_atomic_rmw:
        case Intrinsic::nacl_atomic_is_lock_free:
          sandboxPtrOperand(IntrCall, 1, true, Func, &MemBase);
          break;
        default:
          checkDoesNotHavePointerOperands(IntrCall);
        }
      } else if (!isa<PtrToIntInst>(Inst) && !isa<BitCastInst>(Inst)) {
        checkDoesNotHavePointerOperands(Inst);
      }
    }
  }
}

char SandboxMemoryAccesses::ID = 0;
INITIALIZE_PASS(SandboxMemoryAccesses, "minsfi-sandbox-memory-accesses",
                "Add SFI sandboxing to memory accesses", false, false)
