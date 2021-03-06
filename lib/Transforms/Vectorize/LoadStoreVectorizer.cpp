//===----- LoadStoreVectorizer.cpp - GPU Load & Store Vectorizer ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/VectorUtils.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "load-store-vectorizer"
STATISTIC(NumVectorInstructions, "Number of vector accesses generated");
STATISTIC(NumScalarsVectorized, "Number of scalar accesses vectorized");

namespace {

// TODO: Remove this
static const unsigned TargetBaseAlign = 4;

class Vectorizer {
  typedef SmallVector<Value *, 8> ValueList;
  typedef MapVector<Value *, ValueList> ValueListMap;

  Function &F;
  AliasAnalysis &AA;
  DominatorTree &DT;
  ScalarEvolution &SE;
  TargetTransformInfo &TTI;
  const DataLayout &DL;
  IRBuilder<> Builder;
  ValueListMap StoreRefs;
  ValueListMap LoadRefs;

public:
  Vectorizer(Function &F, AliasAnalysis &AA, DominatorTree &DT,
             ScalarEvolution &SE, TargetTransformInfo &TTI)
    : F(F), AA(AA), DT(DT), SE(SE), TTI(TTI),
      DL(F.getParent()->getDataLayout()),
      Builder(SE.getContext()) {}

  bool run();

private:
  Value *getPointerOperand(Value *I);

  unsigned getPointerAddressSpace(Value *I);

  unsigned getAlignment(LoadInst *LI) const {
    unsigned Align = LI->getAlignment();
    if (Align != 0)
      return Align;

    return DL.getABITypeAlignment(LI->getType());
  }

  unsigned getAlignment(StoreInst *SI) const {
    unsigned Align = SI->getAlignment();
    if (Align != 0)
      return Align;

    return DL.getABITypeAlignment(SI->getValueOperand()->getType());
  }

  bool isConsecutiveAccess(Value *A, Value *B);

  /// Reorders the users of I after vectorization to ensure that I dominates its
  /// users.
  void reorder(Instruction *I);

  /// Returns the first and the last instructions in Chain.
  std::pair<BasicBlock::iterator, BasicBlock::iterator>
  getBoundaryInstrs(ArrayRef<Value *> Chain);

  /// Erases the original instructions after vectorizing.
  void eraseInstructions(ArrayRef<Value *> Chain);

  /// "Legalize" the vector type that would be produced by combining \p
  /// ElementSizeBits elements in \p Chain. Break into two pieces such that the
  /// total size of each piece is 1, 2 or a multiple of 4 bytes. \p Chain is
  /// expected to have more than 4 elements.
  std::pair<ArrayRef<Value *>, ArrayRef<Value *>>
  splitOddVectorElts(ArrayRef<Value *> Chain, unsigned ElementSizeBits);

  /// Checks if there are any instructions which may affect the memory accessed
  /// in the chain between \p From and \p To. The elements of \p Chain should be
  /// all loads or all stores.
  bool isVectorizable(ArrayRef<Value *> Chain, BasicBlock::iterator From,
                      BasicBlock::iterator To);

  /// Collects load and store instructions to vectorize.
  void collectInstructions(BasicBlock *BB);

  /// Processes the collected instructions, the \p Map. The elements of \p Map
  /// should be all loads or all stores.
  bool vectorizeChains(ValueListMap &Map);

  /// Finds the load/stores to consecutive memory addresses and vectorizes them.
  bool vectorizeInstructions(ArrayRef<Value *> Instrs);

  /// Vectorizes the load instructions in Chain.
  bool vectorizeLoadChain(ArrayRef<Value *> Chain);

  /// Vectorizes the store instructions in Chain.
  bool vectorizeStoreChain(ArrayRef<Value *> Chain);
};

class LoadStoreVectorizer : public FunctionPass {
public:
  static char ID;

  LoadStoreVectorizer() : FunctionPass(ID) {
    initializeLoadStoreVectorizerPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;

  const char *getPassName() const override {
    return "GPU Load and Store Vectorizer";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AAResultsWrapperPass>();
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<TargetTransformInfoWrapperPass>();
    AU.setPreservesCFG();
  }
};
}

INITIALIZE_PASS_BEGIN(LoadStoreVectorizer, DEBUG_TYPE,
                      "Vectorize load and Store instructions", false, false);
INITIALIZE_PASS_DEPENDENCY(SCEVAAWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GlobalsAAWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)
INITIALIZE_PASS_END(LoadStoreVectorizer, DEBUG_TYPE,
                    "Vectorize load and store instructions", false, false);

char LoadStoreVectorizer::ID = 0;

Pass *llvm::createLoadStoreVectorizerPass() {
  return new LoadStoreVectorizer();
}

bool LoadStoreVectorizer::runOnFunction(Function &F) {
  // Don't vectorize when the attribute NoImplicitFloat is used.
  if (skipFunction(F) || F.hasFnAttribute(Attribute::NoImplicitFloat))
    return false;

  AliasAnalysis &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
  DominatorTree &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  ScalarEvolution &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  TargetTransformInfo &TTI
    = getAnalysis<TargetTransformInfoWrapperPass>().getTTI(F);

  Vectorizer V(F, AA, DT, SE, TTI);
  return V.run();
}

// Vectorizer Implementation
bool Vectorizer::run() {
  bool Changed = false;

  // Scan the blocks in the function in post order.
  for (BasicBlock *BB : post_order(&F)) {
    collectInstructions(BB);
    Changed |= vectorizeChains(LoadRefs);
    Changed |= vectorizeChains(StoreRefs);
  }

  return Changed;
}

Value *Vectorizer::getPointerOperand(Value *I) {
  if (LoadInst *LI = dyn_cast<LoadInst>(I))
    return LI->getPointerOperand();
  if (StoreInst *SI = dyn_cast<StoreInst>(I))
    return SI->getPointerOperand();
  return nullptr;
}

unsigned Vectorizer::getPointerAddressSpace(Value *I) {
  if (LoadInst *L = dyn_cast<LoadInst>(I))
    return L->getPointerAddressSpace();
  if (StoreInst *S = dyn_cast<StoreInst>(I))
    return S->getPointerAddressSpace();
  return -1;
}

// FIXME: Merge with llvm::isConsecutiveAccess
bool Vectorizer::isConsecutiveAccess(Value *A, Value *B) {
  Value *PtrA = getPointerOperand(A);
  Value *PtrB = getPointerOperand(B);
  unsigned ASA = getPointerAddressSpace(A);
  unsigned ASB = getPointerAddressSpace(B);

  // Check that the address spaces match and that the pointers are valid.
  if (!PtrA || !PtrB || (ASA != ASB))
    return false;

  // Make sure that A and B are different pointers of the same size type.
  unsigned PtrBitWidth = DL.getPointerSizeInBits(ASA);
  Type *PtrATy = PtrA->getType()->getPointerElementType();
  Type *PtrBTy = PtrB->getType()->getPointerElementType();
  if (PtrA == PtrB ||
      DL.getTypeStoreSize(PtrATy) != DL.getTypeStoreSize(PtrBTy) ||
      DL.getTypeStoreSize(PtrATy->getScalarType()) !=
      DL.getTypeStoreSize(PtrBTy->getScalarType()))
    return false;

  APInt Size(PtrBitWidth, DL.getTypeStoreSize(PtrATy));

  APInt OffsetA(PtrBitWidth, 0), OffsetB(PtrBitWidth, 0);
  PtrA = PtrA->stripAndAccumulateInBoundsConstantOffsets(DL, OffsetA);
  PtrB = PtrB->stripAndAccumulateInBoundsConstantOffsets(DL, OffsetB);

  APInt OffsetDelta = OffsetB - OffsetA;

  // Check if they are based on the same pointer. That makes the offsets
  // sufficient.
  if (PtrA == PtrB)
    return OffsetDelta == Size;

  // Compute the necessary base pointer delta to have the necessary final delta
  // equal to the size.
  APInt BaseDelta = Size - OffsetDelta;

  // Compute the distance with SCEV between the base pointers.
  const SCEV *PtrSCEVA = SE.getSCEV(PtrA);
  const SCEV *PtrSCEVB = SE.getSCEV(PtrB);
  const SCEV *C = SE.getConstant(BaseDelta);
  const SCEV *X = SE.getAddExpr(PtrSCEVA, C);
  if (X == PtrSCEVB)
    return true;

  // Sometimes even this doesn't work, because SCEV can't always see through
  // patterns that look like (gep (ext (add (shl X, C1), C2))). Try checking
  // things the hard way.

  // Look through GEPs after checking they're the same except for the last
  // index.
  GetElementPtrInst *GEPA = dyn_cast<GetElementPtrInst>(getPointerOperand(A));
  GetElementPtrInst *GEPB = dyn_cast<GetElementPtrInst>(getPointerOperand(B));
  if (!GEPA || !GEPB || GEPA->getNumOperands() != GEPB->getNumOperands())
    return false;
  unsigned FinalIndex = GEPA->getNumOperands() - 1;
  for (unsigned i = 0; i < FinalIndex; i++)
    if (GEPA->getOperand(i) != GEPB->getOperand(i))
      return false;

  Instruction *OpA = dyn_cast<Instruction>(GEPA->getOperand(FinalIndex));
  Instruction *OpB = dyn_cast<Instruction>(GEPB->getOperand(FinalIndex));
  if (!OpA || !OpB || OpA->getOpcode() != OpB->getOpcode() ||
      OpA->getType() != OpB->getType())
    return false;

  // Only look through a ZExt/SExt.
  if (!isa<SExtInst>(OpA) && !isa<ZExtInst>(OpA))
    return false;

  bool Signed = isa<SExtInst>(OpA);

  OpA = dyn_cast<Instruction>(OpA->getOperand(0));
  OpB = dyn_cast<Instruction>(OpB->getOperand(0));
  if (!OpA || !OpB || OpA->getType() != OpB->getType())
    return false;

  // Now we need to prove that adding 1 to OpA won't overflow.
  bool Safe = false;
  // First attempt: if OpB is an add with NSW/NUW, and OpB is 1 added to OpA,
  // we're okay.
  if (OpB->getOpcode() == Instruction::Add &&
      isa<ConstantInt>(OpB->getOperand(1)) &&
      cast<ConstantInt>(OpB->getOperand(1))->getSExtValue() > 0) {
    if (Signed)
      Safe = cast<BinaryOperator>(OpB)->hasNoSignedWrap();
    else
      Safe = cast<BinaryOperator>(OpB)->hasNoUnsignedWrap();
  }

  unsigned BitWidth = OpA->getType()->getScalarSizeInBits();

  // Second attempt:
  // If any bits are known to be zero other than the sign bit in OpA, we can
  // add 1 to it while guaranteeing no overflow of any sort.
  if (!Safe) {
    APInt KnownZero(BitWidth, 0);
    APInt KnownOne(BitWidth, 0);
    computeKnownBits(OpA, KnownZero, KnownOne, DL, 0, nullptr, OpA, &DT);
    KnownZero &= ~APInt::getHighBitsSet(BitWidth, 1);
    if (KnownZero != 0)
      Safe = true;
  }

  if (!Safe)
    return false;

  const SCEV *OffsetSCEVA = SE.getSCEV(OpA);
  const SCEV *OffsetSCEVB = SE.getSCEV(OpB);
  const SCEV *One = SE.getConstant(APInt(BitWidth, 1));
  const SCEV *X2 = SE.getAddExpr(OffsetSCEVA, One);
  return X2 == OffsetSCEVB;
}

void Vectorizer::reorder(Instruction *I) {
  Instruction *InsertAfter = I;
  for (User *U : I->users()) {
    Instruction *User = dyn_cast<Instruction>(U);
    if (!User || User->getOpcode() == Instruction::PHI)
      continue;

    if (!DT.dominates(I, User)) {
      User->removeFromParent();
      User->insertAfter(InsertAfter);
      InsertAfter = User;
      reorder(User);
    }
  }
}

std::pair<BasicBlock::iterator, BasicBlock::iterator>
Vectorizer::getBoundaryInstrs(ArrayRef<Value *> Chain) {
  Instruction *C0 = cast<Instruction>(Chain[0]);
  BasicBlock::iterator FirstInstr = C0->getIterator();
  BasicBlock::iterator LastInstr = C0->getIterator();

  BasicBlock *BB = C0->getParent();
  unsigned NumFound = 0;
  for (Instruction &I : *BB) {
    if (!is_contained(Chain, &I))
      continue;

    ++NumFound;
    if (NumFound == 1) {
      FirstInstr = I.getIterator();
    }
    if (NumFound == Chain.size()) {
      LastInstr = I.getIterator();
      break;
    }
  }

  // Range is [first, last).
  return std::make_pair(FirstInstr, ++LastInstr);
}

void Vectorizer::eraseInstructions(ArrayRef<Value *> Chain) {
  SmallVector<Instruction *, 16> Instrs;
  for (Value *V : Chain) {
    Value *PtrOperand = getPointerOperand(V);
    assert(PtrOperand && "Instruction must have a pointer operand.");
    Instrs.push_back(cast<Instruction>(V));
    if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(PtrOperand))
      Instrs.push_back(GEP);
  }

  // Erase instructions.
  for (Value *V : Instrs) {
    Instruction *Instr = cast<Instruction>(V);
    if (Instr->use_empty())
      Instr->eraseFromParent();
  }
}

std::pair<ArrayRef<Value *>, ArrayRef<Value *>>
Vectorizer::splitOddVectorElts(ArrayRef<Value *> Chain,
                               unsigned ElementSizeBits) {
  unsigned ElemSizeInBytes = ElementSizeBits / 8;
  unsigned SizeInBytes = ElemSizeInBytes * Chain.size();
  unsigned NumRight = (SizeInBytes % 4) / ElemSizeInBytes;
  unsigned NumLeft = Chain.size() - NumRight;
  return std::make_pair(Chain.slice(0, NumLeft), Chain.slice(NumLeft));
}

bool Vectorizer::isVectorizable(ArrayRef<Value *> Chain,
                                BasicBlock::iterator From,
                                BasicBlock::iterator To) {
  SmallVector<std::pair<Value *, unsigned>, 16> MemoryInstrs;
  SmallVector<std::pair<Value *, unsigned>, 16> ChainInstrs;

  unsigned Idx = 0;
  for (auto I = From, E = To; I != E; ++I, ++Idx) {
    if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
      if (!is_contained(Chain, &*I))
        MemoryInstrs.push_back({ &*I, Idx });
      else
        ChainInstrs.push_back({ &*I, Idx });
    } else if (I->mayHaveSideEffects()) {
      DEBUG(dbgs() << "LSV: Found side-effecting operation: " << *I << '\n');
      return false;
    }
  }

  assert(Chain.size() == ChainInstrs.size() &&
         "All instructions in the Chain must exist in [From, To).");

  for (auto EntryMem : MemoryInstrs) {
    Value *V = EntryMem.first;
    unsigned VIdx = EntryMem.second;
    for (auto EntryChain : ChainInstrs) {
      Value *VV = EntryChain.first;
      unsigned VVIdx = EntryChain.second;
      if (isa<LoadInst>(V) && isa<LoadInst>(VV))
        continue;

      // We can ignore the alias as long as the load comes before the store,
      // because that means we won't be moving the load past the store to
      // vectorize it (the vectorized load is inserted at the location of the
      // first load in the chain).
      if (isa<StoreInst>(V) && isa<LoadInst>(VV) && VVIdx < VIdx)
        continue;

      // Same case, but in reverse.
      if (isa<LoadInst>(V) && isa<StoreInst>(VV) && VVIdx > VIdx)
        continue;

      Instruction *M0 = cast<Instruction>(V);
      Instruction *M1 = cast<Instruction>(VV);

      if (!AA.isNoAlias(MemoryLocation::get(M0), MemoryLocation::get(M1))) {
        DEBUG(
          Value *Ptr0 = getPointerOperand(M0);
          Value *Ptr1 = getPointerOperand(M1);

          dbgs() << "LSV: Found alias.\n"
                    "        Aliasing instruction and pointer:\n"
            << *V << " aliases " << *Ptr0 << '\n'
            << "        Aliased instruction and pointer:\n"
            << *VV << " aliases " << *Ptr1 << '\n'
          );

        return false;
      }
    }
  }

  return true;
}

void Vectorizer::collectInstructions(BasicBlock *BB) {
  LoadRefs.clear();
  StoreRefs.clear();

  for (Instruction &I : *BB) {
    if (!I.mayReadOrWriteMemory())
      continue;

    if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
      if (!LI->isSimple())
        continue;

      Type *Ty = LI->getType();
      if (!VectorType::isValidElementType(Ty->getScalarType()))
        continue;

      // Skip weird non-byte sizes. They probably aren't worth the effort of
      // handling correctly.
      unsigned TySize = DL.getTypeSizeInBits(Ty);
      if (TySize < 8)
        continue;

      Value *Ptr = LI->getPointerOperand();
      unsigned AS = Ptr->getType()->getPointerAddressSpace();
      unsigned VecRegSize = TTI.getLoadStoreVecRegBitWidth(AS);

      // No point in looking at these if they're too big to vectorize.
      if (TySize > VecRegSize / 2)
        continue;

      // Make sure all the users of a vector are constant-index extracts.
      if (isa<VectorType>(Ty) &&
          !all_of(LI->users(), [LI](const User *U) {
            const Instruction *UI = cast<Instruction>(U);
            return isa<ExtractElementInst>(UI) &&
                   isa<ConstantInt>(UI->getOperand(1));
          }))
        continue;

      // TODO: Target hook to filter types.

      // Save the load locations.
      Value *ObjPtr = GetUnderlyingObject(Ptr, DL);
      LoadRefs[ObjPtr].push_back(LI);

    } else if (StoreInst *SI = dyn_cast<StoreInst>(&I)) {
      if (!SI->isSimple())
        continue;

      Type *Ty = SI->getValueOperand()->getType();
      if (!VectorType::isValidElementType(Ty->getScalarType()))
        continue;

      // Skip weird non-byte sizes. They probably aren't worth the effort of
      // handling correctly.
      unsigned TySize = DL.getTypeSizeInBits(Ty);
      if (TySize < 8)
        continue;

      Value *Ptr = SI->getPointerOperand();
      unsigned AS = Ptr->getType()->getPointerAddressSpace();
      unsigned VecRegSize = TTI.getLoadStoreVecRegBitWidth(AS);
      if (TySize > VecRegSize / 2)
        continue;

      if (isa<VectorType>(Ty) &&
          !all_of(SI->users(), [SI](const User *U) {
            const Instruction *UI = cast<Instruction>(U);
            return isa<ExtractElementInst>(UI) &&
                   isa<ConstantInt>(UI->getOperand(1));
          }))
        continue;

      // Save store location.
      Value *ObjPtr = GetUnderlyingObject(Ptr, DL);
      StoreRefs[ObjPtr].push_back(SI);
    }
  }
}

bool Vectorizer::vectorizeChains(ValueListMap &Map) {
  bool Changed = false;

  for (const std::pair<Value *, ValueList> &Chain : Map) {
    unsigned Size = Chain.second.size();
    if (Size < 2)
      continue;

    DEBUG(dbgs() << "LSV: Analyzing a chain of length " << Size << ".\n");

    // Process the stores in chunks of 64.
    for (unsigned CI = 0, CE = Size; CI < CE; CI += 64) {
      unsigned Len = std::min<unsigned>(CE - CI, 64);
      ArrayRef<Value *> Chunk(&Chain.second[CI], Len);
      Changed |= vectorizeInstructions(Chunk);
    }
  }

  return Changed;
}

bool Vectorizer::vectorizeInstructions(ArrayRef<Value *> Instrs) {
  DEBUG(dbgs() << "LSV: Vectorizing " << Instrs.size() << " instructions.\n");
  SmallSetVector<int, 16> Heads, Tails;
  int ConsecutiveChain[64];

  // Do a quadratic search on all of the given stores and find all of the pairs
  // of stores that follow each other.
  for (int i = 0, e = Instrs.size(); i < e; ++i) {
    ConsecutiveChain[i] = -1;
    for (int j = e - 1; j >= 0; --j) {
      if (i == j)
        continue;

      if (isConsecutiveAccess(Instrs[i], Instrs[j])) {
        if (ConsecutiveChain[i] != -1) {
          int CurDistance = std::abs(ConsecutiveChain[i] - i);
          int NewDistance = std::abs(ConsecutiveChain[i] - j);
          if (j < i || NewDistance > CurDistance)
            continue; // Should not insert.
        }

        Tails.insert(j);
        Heads.insert(i);
        ConsecutiveChain[i] = j;
      }
    }
  }

  bool Changed = false;
  SmallPtrSet<Value *, 16> VectorizedValues;

  for (int Head : Heads) {
    if (Tails.count(Head))
      continue;

    // We found an instr that starts a chain. Now follow the chain and try to
    // vectorize it.
    SmallVector<Value *, 16> Operands;
    int I = Head;
    while (I != -1 && (Tails.count(I) || Heads.count(I))) {
      if (VectorizedValues.count(Instrs[I]))
        break;

      Operands.push_back(Instrs[I]);
      I = ConsecutiveChain[I];
    }

    bool Vectorized = false;
    if (isa<LoadInst>(*Operands.begin()))
      Vectorized = vectorizeLoadChain(Operands);
    else
      Vectorized = vectorizeStoreChain(Operands);

    // Mark the vectorized instructions so that we don't vectorize them again.
    if (Vectorized)
      VectorizedValues.insert(Operands.begin(), Operands.end());
    Changed |= Vectorized;
  }

  return Changed;
}

bool Vectorizer::vectorizeStoreChain(ArrayRef<Value *> Chain) {
  StoreInst *S0 = cast<StoreInst>(Chain[0]);

  // If the vector has an int element, default to int for the whole load.
  Type *StoreTy;
  for (const auto &V : Chain) {
    StoreTy = cast<StoreInst>(V)->getValueOperand()->getType();
    if (StoreTy->isIntOrIntVectorTy())
      break;

    if (StoreTy->isPtrOrPtrVectorTy()) {
      StoreTy = Type::getIntNTy(F.getParent()->getContext(),
                                DL.getTypeSizeInBits(StoreTy));
      break;
    }
  }

  unsigned Sz = DL.getTypeSizeInBits(StoreTy);
  unsigned AS = S0->getPointerAddressSpace();
  unsigned VecRegSize = TTI.getLoadStoreVecRegBitWidth(AS);
  unsigned VF = VecRegSize / Sz;
  unsigned ChainSize = Chain.size();

  if (!isPowerOf2_32(Sz) || VF < 2 || ChainSize < 2)
    return false;

  // Store size should be 1B, 2B or multiple of 4B.
  // TODO: Target hook for size constraint?
  unsigned SzInBytes = (Sz / 8) * ChainSize;
  if (SzInBytes > 2 && SzInBytes % 4 != 0) {
    DEBUG(dbgs() << "LSV: Size should be 1B, 2B "
                    "or multiple of 4B. Splitting.\n");
    if (SzInBytes == 3)
      return vectorizeStoreChain(Chain.slice(0, ChainSize - 1));

    auto Chains = splitOddVectorElts(Chain, Sz);
    return vectorizeStoreChain(Chains.first) |
           vectorizeStoreChain(Chains.second);
  }

  VectorType *VecTy;
  VectorType *VecStoreTy = dyn_cast<VectorType>(StoreTy);
  if (VecStoreTy)
    VecTy = VectorType::get(StoreTy->getScalarType(),
                            Chain.size() * VecStoreTy->getNumElements());
  else
    VecTy = VectorType::get(StoreTy, Chain.size());

  // If it's more than the max vector size, break it into two pieces.
  // TODO: Target hook to control types to split to.
  if (ChainSize > VF) {
    DEBUG(dbgs() << "LSV: Vector factor is too big."
                    " Creating two separate arrays.\n");
    return vectorizeStoreChain(Chain.slice(0, VF)) |
           vectorizeStoreChain(Chain.slice(VF));
  }

  DEBUG(
    dbgs() << "LSV: Stores to vectorize:\n";
    for (Value *V : Chain)
      V->dump();
  );

  // Check alignment restrictions.
  unsigned Alignment = getAlignment(S0);

  // If the store is going to be misaligned, don't vectorize it.
  // TODO: Check TLI.allowsMisalignedMemoryAccess
  if ((Alignment % SzInBytes) != 0 && (Alignment % TargetBaseAlign) != 0) {
    if (S0->getPointerAddressSpace() == 0) {
      // If we're storing to an object on the stack, we control its alignment,
      // so we can cheat and change it!
      Value *V = GetUnderlyingObject(S0->getPointerOperand(), DL);
      if (AllocaInst *AI = dyn_cast_or_null<AllocaInst>(V)) {
        AI->setAlignment(TargetBaseAlign);
        Alignment = TargetBaseAlign;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }

  BasicBlock::iterator First, Last;
  std::tie(First, Last) = getBoundaryInstrs(Chain);

  if (!isVectorizable(Chain, First, Last))
    return false;

  // Set insert point.
  Builder.SetInsertPoint(&*Last);

  Value *Vec = UndefValue::get(VecTy);

  if (VecStoreTy) {
    unsigned VecWidth = VecStoreTy->getNumElements();
    for (unsigned I = 0, E = Chain.size(); I != E; ++I) {
      StoreInst *Store = cast<StoreInst>(Chain[I]);
      for (unsigned J = 0, NE = VecStoreTy->getNumElements(); J != NE; ++J) {
        unsigned NewIdx = J + I * VecWidth;
        Value *Extract = Builder.CreateExtractElement(Store->getValueOperand(),
                                                      Builder.getInt32(J));
        if (Extract->getType() != StoreTy->getScalarType())
          Extract = Builder.CreateBitCast(Extract, StoreTy->getScalarType());

        Value *Insert = Builder.CreateInsertElement(Vec, Extract,
                                                    Builder.getInt32(NewIdx));
        Vec = Insert;
      }
    }
  } else {
    for (unsigned I = 0, E = Chain.size(); I != E; ++I) {
      StoreInst *Store = cast<StoreInst>(Chain[I]);
      Value *Extract = Store->getValueOperand();
      if (Extract->getType() != StoreTy->getScalarType())
        Extract = Builder.CreateBitOrPointerCast(Extract, StoreTy->getScalarType());

      Value *Insert = Builder.CreateInsertElement(Vec, Extract,
                                                  Builder.getInt32(I));
      Vec = Insert;
    }
  }

  Value *Bitcast =
    Builder.CreateBitCast(S0->getPointerOperand(), VecTy->getPointerTo(AS));
  StoreInst *SI = cast<StoreInst>(Builder.CreateStore(Vec, Bitcast));
  propagateMetadata(SI, Chain);
  SI->setAlignment(Alignment);

  eraseInstructions(Chain);
  ++NumVectorInstructions;
  NumScalarsVectorized += Chain.size();
  return true;
}

bool Vectorizer::vectorizeLoadChain(ArrayRef<Value *> Chain) {
  LoadInst *L0 = cast<LoadInst>(Chain[0]);

  // If the vector has an int element, default to int for the whole load.
  Type *LoadTy;
  for (const auto &V : Chain) {
    LoadTy = cast<LoadInst>(V)->getType();
    if (LoadTy->isIntOrIntVectorTy())
      break;

    if (LoadTy->isPtrOrPtrVectorTy()) {
      LoadTy = Type::getIntNTy(F.getParent()->getContext(),
                               DL.getTypeSizeInBits(LoadTy));
      break;
    }

  }

  unsigned Sz = DL.getTypeSizeInBits(LoadTy);
  unsigned AS = L0->getPointerAddressSpace();
  unsigned VecRegSize = TTI.getLoadStoreVecRegBitWidth(AS);
  unsigned VF = VecRegSize / Sz;
  unsigned ChainSize = Chain.size();

  if (!isPowerOf2_32(Sz) || VF < 2 || ChainSize < 2)
    return false;

  // Load size should be 1B, 2B or multiple of 4B.
  // TODO: Should size constraint be a target hook?
  unsigned SzInBytes = (Sz / 8) * ChainSize;
  if (SzInBytes > 2 && SzInBytes % 4 != 0) {
    DEBUG(dbgs() << "LSV: Size should be 1B, 2B or multiple of 4B. Splitting.\n");
    if (SzInBytes == 3)
      return vectorizeLoadChain(Chain.slice(0, ChainSize - 1));
    auto Chains = splitOddVectorElts(Chain, Sz);
    return vectorizeLoadChain(Chains.first) | vectorizeLoadChain(Chains.second);
  }

  VectorType *VecTy;
  VectorType *VecLoadTy = dyn_cast<VectorType>(LoadTy);
  if (VecLoadTy)
    VecTy = VectorType::get(LoadTy->getScalarType(),
                            Chain.size() * VecLoadTy->getNumElements());
  else
    VecTy = VectorType::get(LoadTy, Chain.size());

  // If it's more than the max vector size, break it into two pieces.
  // TODO: Target hook to control types to split to.
  if (ChainSize > VF) {
    DEBUG(dbgs() << "LSV: Vector factor is too big. "
                    "Creating two separate arrays.\n");
    return vectorizeLoadChain(Chain.slice(0, VF)) |
           vectorizeLoadChain(Chain.slice(VF));
  }

  // Check alignment restrictions.
  unsigned Alignment = getAlignment(L0);

  // If the load is going to be misaligned, don't vectorize it.
  // TODO: Check TLI.allowsMisalignedMemoryAccess and remove TargetBaseAlign.
  if ((Alignment % SzInBytes) != 0 && (Alignment % TargetBaseAlign) != 0) {
    if (L0->getPointerAddressSpace() == 0) {
      // If we're loading from an object on the stack, we control its alignment,
      // so we can cheat and change it!
      Value *V = GetUnderlyingObject(L0->getPointerOperand(), DL);
      if (AllocaInst *AI = dyn_cast_or_null<AllocaInst>(V)) {
        AI->setAlignment(TargetBaseAlign);
        Alignment = TargetBaseAlign;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }

  DEBUG(
    dbgs() << "LSV: Loads to vectorize:\n";
    for (Value *V : Chain)
      V->dump();
  );

  BasicBlock::iterator First, Last;
  std::tie(First, Last) = getBoundaryInstrs(Chain);

  if (!isVectorizable(Chain, First, Last))
    return false;

  // Set insert point.
  Builder.SetInsertPoint(&*Last);

  Value *Bitcast =
    Builder.CreateBitCast(L0->getPointerOperand(), VecTy->getPointerTo(AS));

  LoadInst *LI = cast<LoadInst>(Builder.CreateLoad(Bitcast));
  propagateMetadata(LI, Chain);
  LI->setAlignment(Alignment);

  if (VecLoadTy) {
    SmallVector<Instruction *, 16> InstrsToErase;
    SmallVector<Instruction *, 16> InstrsToReorder;

    unsigned VecWidth = VecLoadTy->getNumElements();
    for (unsigned I = 0, E = Chain.size(); I != E; ++I) {
      for (auto Use : Chain[I]->users()) {
        Instruction *UI = cast<Instruction>(Use);
        unsigned Idx = cast<ConstantInt>(UI->getOperand(1))->getZExtValue();
        unsigned NewIdx = Idx + I * VecWidth;
        Value *V = Builder.CreateExtractElement(LI, Builder.getInt32(NewIdx));
        Instruction *Extracted = cast<Instruction>(V);
        if (Extracted->getType() != UI->getType())
          Extracted =
            cast<Instruction>(Builder.CreateBitCast(Extracted, UI->getType()));

        // Replace the old instruction.
        UI->replaceAllUsesWith(Extracted);
        InstrsToReorder.push_back(Extracted);
        InstrsToErase.push_back(UI);
      }
    }

    for (Instruction *ModUser : InstrsToReorder)
      reorder(ModUser);

    for (auto I : InstrsToErase)
      I->eraseFromParent();
  } else {
    SmallVector<Instruction *, 16> InstrsToReorder;

    for (unsigned I = 0, E = Chain.size(); I != E; ++I) {
      Value *V = Builder.CreateExtractElement(LI, Builder.getInt32(I));
      Instruction *Extracted = cast<Instruction>(V);
      Instruction *UI = cast<Instruction>(Chain[I]);
      if (Extracted->getType() != UI->getType()) {
        Extracted =
          cast<Instruction>(Builder.CreateBitOrPointerCast(Extracted, UI->getType()));
      }

      // Replace the old instruction.
      UI->replaceAllUsesWith(Extracted);
      InstrsToReorder.push_back(Extracted);
    }

    for (Instruction *ModUser : InstrsToReorder)
      reorder(ModUser);
  }

  eraseInstructions(Chain);

  ++NumVectorInstructions;
  NumScalarsVectorized += Chain.size();
  return true;
}
