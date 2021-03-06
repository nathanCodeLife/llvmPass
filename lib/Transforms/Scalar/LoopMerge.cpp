//===- LoopMerge.cpp - Loop Merge Pass ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements the loop merge pass.
/// The implementation is largely based on the following document:
///
///       Code Transformations to Augment the Scope of Loop Merge in a
///         Production Compiler
///       Christopher Mark Barton
///       MSc Thesis
///       https://webdocs.cs.ualberta.ca/~amaral/thesis/ChristopherBartonMSc.pdf
///
/// The general approach taken is to collect sets of control flow equivalent
/// loops and test whether they can be merged. The necessary conditions for
/// merge are:
///    1. The loops must be adjacent (there cannot be any statements between
///       the two loops).
///    2. The loops must be conforming (they must execute the same number of
///       iterations).
///    3. The loops must be control flow equivalent (if one loop executes, the
///       other is guaranteed to execute).
///    4. There cannot be any negative distance dependencies between the loops.
/// If all of these conditions are satisfied, it is safe to merge the loops.
///
/// This implementation creates MergeCandidates that represent the loop and the
/// necessary information needed by merge. It then operates on the merge
/// candidates, first confirming that the candidate is eligible for merge. The
/// candidates are then collected into control flow equivalent sets, sorted in
/// dominance order. Each set of control flow equivalent candidates is then
/// traversed, attempting to merge pairs of candidates in the set. If all
/// requirements for merge are met, the two candidates are merged, creating a
/// new (merged) candidate which is then added back into the set to consider for
/// additional merge.
///
/// This implementation currently does not make any modifications to remove
/// conditions for merge. Code transformations to make loops conform to each of
/// the conditions for merge are discussed in more detail in the document
/// above. These can be added to the current implementation in the future.
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/LoopMerge.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/DomTreeUpdater.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

#define DEBUG_TYPE "loop-merge"

STATISTIC(MergeCounter, "Count number of loop merges performed");
STATISTIC(NumMergeCandidates, "Number of candidates for loop merge");
STATISTIC(InvalidPreheader, "Loop has invalid preheader");
STATISTIC(InvalidHeader, "Loop has invalid header");
STATISTIC(InvalidExitingBlock, "Loop has invalid exiting blocks");
STATISTIC(InvalidExitBlock, "Loop has invalid exit block");
STATISTIC(InvalidLatch, "Loop has invalid latch");
STATISTIC(InvalidLoop, "Loop is invalid");
STATISTIC(AddressTakenBB, "Basic block has address taken");
STATISTIC(MayThrowException, "Loop may throw an exception");
STATISTIC(ContainsVolatileAccess, "Loop contains a volatile access");
STATISTIC(NotSimplifiedForm, "Loop is not in simplified form");
STATISTIC(InvalidDependencies, "Dependencies prevent merge");
STATISTIC(InvalidTripCount,
          "Loop does not have invariant backedge taken count");
STATISTIC(UncomputableTripCount, "SCEV cannot compute trip count of loop");
STATISTIC(NonEqualTripCount, "Candidate trip counts are not the same");
STATISTIC(NonAdjacent, "Candidates are not adjacent");
STATISTIC(NonEmptyPreheader, "Candidate has a non-empty preheader");

enum MergeDependenceAnalysisChoice {
  FUSION_DEPENDENCE_ANALYSIS_SCEV,
  FUSION_DEPENDENCE_ANALYSIS_DA,
  FUSION_DEPENDENCE_ANALYSIS_ALL,
};

static cl::opt<MergeDependenceAnalysisChoice> MergeDependenceAnalysis(
    "loop-merge-dependence-analysis",
    cl::desc("Which dependence analysis should loop merge use?"),
    cl::values(clEnumValN(FUSION_DEPENDENCE_ANALYSIS_SCEV, "scev",
                          "Use the scalar evolution interface"),
               clEnumValN(FUSION_DEPENDENCE_ANALYSIS_DA, "da",
                          "Use the dependence analysis interface"),
               clEnumValN(FUSION_DEPENDENCE_ANALYSIS_ALL, "all",
                          "Use all available analyses")),
    cl::Hidden, cl::init(FUSION_DEPENDENCE_ANALYSIS_ALL), cl::ZeroOrMore);

#ifndef NDEBUG
static cl::opt<bool>
    VerboseMergeDebugging("loop-merge-verbose-debug",
                           cl::desc("Enable verbose debugging for Loop Merge"),
                           cl::Hidden, cl::init(false), cl::ZeroOrMore);
#endif
/// This class is used to represent a candidate for loop merge. When it is
/// constructed, it checks the conditions for loop merge to ensure that it
/// represents a valid candidate. It caches several parts of a loop that are
/// used throughout loop merge (e.g., loop preheader, loop header, etc) instead
/// of continually querying the underlying Loop to retrieve these values. It is
/// assumed these will not change throughout loop merge.
///
/// The invalidate method should be used to indicate that the MergeCandidate is
/// no longer a valid candidate for merge. Similarly, the isValid() method can
/// be used to ensure that the MergeCandidate is still valid for merge.
struct MergeCandidate {
  /// Cache of parts of the loop used throughout loop merge. These should not
  /// need to change throughout the analysis and transformation.
  /// These parts are cached to avoid repeatedly looking up in the Loop class.

  /// Preheader of the loop this candidate represents
  BasicBlock *Preheader;
  /// Header of the loop this candidate represents
  BasicBlock *Header;
  /// Blocks in the loop that exit the loop
  BasicBlock *ExitingBlock;
  /// The successor block of this loop (where the exiting blocks go to)
  BasicBlock *ExitBlock;
  /// Latch of the loop
  BasicBlock *Latch;
  /// The loop that this merge candidate represents
  Loop *L;
  /// Vector of instructions in this loop that read from memory
  SmallVector<Instruction *, 16> MemReads;
  /// Vector of instructions in this loop that write to memory
  SmallVector<Instruction *, 16> MemWrites;
  /// Are all of the members of this merge candidate still valid
  bool Valid;

  /// Dominator and PostDominator trees are needed for the
  /// MergeCandidateCompare function, required by MergeCandidateSet to
  /// determine where the MergeCandidate should be inserted into the set. These
  /// are used to establish ordering of the MergeCandidates based on dominance.
  const DominatorTree *DT;
  const PostDominatorTree *PDT;

  MergeCandidate(Loop *L, const DominatorTree *DT,
                  const PostDominatorTree *PDT)
      : Preheader(L->getLoopPreheader()), Header(L->getHeader()),
        ExitingBlock(L->getExitingBlock()), ExitBlock(L->getExitBlock()),
        Latch(L->getLoopLatch()), L(L), Valid(true), DT(DT), PDT(PDT) {

    // Walk over all blocks in the loop and check for conditions that may
    // prevent merge. For each block, walk over all instructions and collect
    // the memory reads and writes If any instructions that prevent merge are
    // found, invalidate this object and return.
    for (BasicBlock *BB : L->blocks()) {
      if (BB->hasAddressTaken()) {
        AddressTakenBB++;
        invalidate();
        return;
      }

      for (Instruction &I : *BB) {
        if (I.mayThrow()) {
          MayThrowException++;
          invalidate();
          return;
        }
        if (StoreInst *SI = dyn_cast<StoreInst>(&I)) {
          if (SI->isVolatile()) {
            ContainsVolatileAccess++;
            invalidate();
            return;
          }
        }
        if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
          if (LI->isVolatile()) {
            ContainsVolatileAccess++;
            invalidate();
            return;
          }
        }
        if (I.mayWriteToMemory())
          MemWrites.push_back(&I);
        if (I.mayReadFromMemory())
          MemReads.push_back(&I);
      }
    }
  }

  /// Check if all members of the class are valid.
  bool isValid() const {
    return Preheader && Header && ExitingBlock && ExitBlock && Latch && L &&
           !L->isInvalid() && Valid;
  }

  /// Verify that all members are in sync with the Loop object.
  void verify() const {
    assert(isValid() && "Candidate is not valid!!");
    assert(!L->isInvalid() && "Loop is invalid!");
    assert(Preheader == L->getLoopPreheader() && "Preheader is out of sync");
    assert(Header == L->getHeader() && "Header is out of sync");
    assert(ExitingBlock == L->getExitingBlock() &&
           "Exiting Blocks is out of sync");
    assert(ExitBlock == L->getExitBlock() && "Exit block is out of sync");
    assert(Latch == L->getLoopLatch() && "Latch is out of sync");
  }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  LLVM_DUMP_METHOD void dump() const {
    dbgs() << "\tPreheader: " << (Preheader ? Preheader->getName() : "nullptr")
           << "\n"
           << "\tHeader: " << (Header ? Header->getName() : "nullptr") << "\n"
           << "\tExitingBB: "
           << (ExitingBlock ? ExitingBlock->getName() : "nullptr") << "\n"
           << "\tExitBB: " << (ExitBlock ? ExitBlock->getName() : "nullptr")
           << "\n"
           << "\tLatch: " << (Latch ? Latch->getName() : "nullptr") << "\n";
  }
#endif

private:
  // This is only used internally for now, to clear the MemWrites and MemReads
  // list and setting Valid to false. I can't envision other uses of this right
  // now, since once MergeCandidates are put into the MergeCandidateSet they
  // are immutable. Thus, any time we need to change/update a MergeCandidate,
  // we must create a new one and insert it into the MergeCandidateSet to
  // ensure the MergeCandidateSet remains ordered correctly.
  void invalidate() {
    MemWrites.clear();
    MemReads.clear();
    Valid = false;
  }
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                                     const MergeCandidate &FC) {
  if (FC.isValid())
    OS << FC.Preheader->getName();
  else
    OS << "<Invalid>";

  return OS;
}

struct MergeCandidateCompare {
  /// Comparison functor to sort two Control Flow Equivalent merge candidates
  /// into dominance order.
  /// If LHS dominates RHS and RHS post-dominates LHS, return true;
  /// IF RHS dominates LHS and LHS post-dominates RHS, return false;
  bool operator()(const MergeCandidate &LHS,
                  const MergeCandidate &RHS) const {
    const DominatorTree *DT = LHS.DT;

    // Do not save PDT to local variable as it is only used in asserts and thus
    // will trigger an unused variable warning if building without asserts.
    assert(DT && LHS.PDT && "Expecting valid dominator tree");

    if (DT->dominates(LHS.Preheader, RHS.Preheader)) {
      // Verify RHS Postdominates LHS
      assert(LHS.PDT->dominates(RHS.Preheader, LHS.Preheader));
      return true;
    }

    if (DT->dominates(RHS.Preheader, LHS.Preheader)) {
      // RHS dominates LHS
      // Verify LHS post-dominates RHS
      assert(LHS.PDT->dominates(LHS.Preheader, RHS.Preheader));
      return false;
    }
    // If LHS does not dominate RHS and RHS does not dominate LHS then there is
    // no dominance relationship between the two MergeCandidates. Thus, they
    // should not be in the same set together.
    llvm_unreachable(
        "No dominance relationship between these merge candidates!");
  }
};

namespace {
using LoopVector = SmallVector<Loop *, 4>;

// Set of Control Flow Equivalent (CFE) Merge Candidates, sorted in dominance
// order. Thus, if FC0 comes *before* FC1 in a MergeCandidateSet, then FC0
// dominates FC1 and FC1 post-dominates FC0.
// std::set was chosen because we want a sorted data structure with stable
// iterators. A subsequent patch to loop merge will enable merging non-ajdacent
// loops by moving intervening code around. When this intervening code contains
// loops, those loops will be moved also. The corresponding MergeCandidates
// will also need to be moved accordingly. As this is done, having stable
// iterators will simplify the logic. Similarly, having an efficient insert that
// keeps the MergeCandidateSet sorted will also simplify the implementation.
using MergeCandidateSet = std::set<MergeCandidate, MergeCandidateCompare>;
using MergeCandidateCollection = SmallVector<MergeCandidateSet, 4>;
} // namespace

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                                     const MergeCandidateSet &CandSet) {
  for (auto IT : CandSet)
    OS << IT << "\n";

  return OS;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
static void
printMergeCandidates(const MergeCandidateCollection &MergeCandidates) {
  LLVM_DEBUG(dbgs() << "Merge Candidates: \n");
  for (const auto &CandidateSet : MergeCandidates) {
    LLVM_DEBUG({
      dbgs() << "*** Merge Candidate Set ***\n";
      dbgs() << CandidateSet;
      dbgs() << "****************************\n";
    });
  }
}
#endif

/// Collect all loops in function at the same nest level, starting at the
/// outermost level.
///
/// This data structure collects all loops at the same nest level for a
/// given function (specified by the LoopInfo object). It starts at the
/// outermost level.
struct LoopDepthTree {
  using LoopsOnLevelTy = SmallVector<LoopVector, 4>;
  using iterator = LoopsOnLevelTy::iterator;
  using const_iterator = LoopsOnLevelTy::const_iterator;

  LoopDepthTree(LoopInfo &LI) : Depth(1) {
    if (!LI.empty())
      LoopsOnLevel.emplace_back(LoopVector(LI.rbegin(), LI.rend()));
  }

  /// Test whether a given loop has been removed from the function, and thus is
  /// no longer valid.
  bool isRemovedLoop(const Loop *L) const { return RemovedLoops.count(L); }

  /// Record that a given loop has been removed from the function and is no
  /// longer valid.
  void removeLoop(const Loop *L) { RemovedLoops.insert(L); }

  /// Descend the tree to the next (inner) nesting level
  void descend() {
    LoopsOnLevelTy LoopsOnNextLevel;

    for (const LoopVector &LV : *this)
      for (Loop *L : LV)
        if (!isRemovedLoop(L) && L->begin() != L->end())
          LoopsOnNextLevel.emplace_back(LoopVector(L->begin(), L->end()));

    LoopsOnLevel = LoopsOnNextLevel;
    RemovedLoops.clear();
    Depth++;
  }

  bool empty() const { return size() == 0; }
  size_t size() const { return LoopsOnLevel.size() - RemovedLoops.size(); }
  unsigned getDepth() const { return Depth; }

  iterator begin() { return LoopsOnLevel.begin(); }
  iterator end() { return LoopsOnLevel.end(); }
  const_iterator begin() const { return LoopsOnLevel.begin(); }
  const_iterator end() const { return LoopsOnLevel.end(); }

private:
  /// Set of loops that have been removed from the function and are no longer
  /// valid.
  SmallPtrSet<const Loop *, 8> RemovedLoops;

  /// Depth of the current level, starting at 1 (outermost loops).
  unsigned Depth;

  /// Vector of loops at the current depth level that have the same parent loop
  LoopsOnLevelTy LoopsOnLevel;
};

#ifndef NDEBUG
static void printLoopVector(const LoopVector &LV) {
  dbgs() << "****************************\n";
  for (auto L : LV)
    printLoop(*L, dbgs());
  dbgs() << "****************************\n";
}
#endif

static void reportLoopMerge(const MergeCandidate &FC0,
                             const MergeCandidate &FC1,
                             OptimizationRemarkEmitter &ORE) {
  using namespace ore;
  ORE.emit(
      OptimizationRemark(DEBUG_TYPE, "LoopMerge", FC0.Preheader->getParent())
      << "Merged " << NV("Cand1", StringRef(FC0.Preheader->getName()))
      << " with " << NV("Cand2", StringRef(FC1.Preheader->getName())));
}

struct LoopMerger {
private:
  // Sets of control flow equivalent merge candidates for a given nest level.
  MergeCandidateCollection MergeCandidates;

  LoopDepthTree LDT;
  DomTreeUpdater DTU;

  LoopInfo &LI;
  DominatorTree &DT;
  DependenceInfo &DI;
  ScalarEvolution &SE;
  PostDominatorTree &PDT;
  OptimizationRemarkEmitter &ORE;

public:
  LoopMerger(LoopInfo &LI, DominatorTree &DT, DependenceInfo &DI,
            ScalarEvolution &SE, PostDominatorTree &PDT,
            OptimizationRemarkEmitter &ORE, const DataLayout &DL)
      : LDT(LI), DTU(DT, PDT, DomTreeUpdater::UpdateStrategy::Lazy), LI(LI),
        DT(DT), DI(DI), SE(SE), PDT(PDT), ORE(ORE) {}

  /// This is the main entry point for loop merge. It will traverse the
  /// specified function and collect candidate loops to merge, starting at the
  /// outermost nesting level and working inwards.
  bool mergeLoops(Function &F) {
#ifndef NDEBUG
    if (VerboseMergeDebugging) {
      LI.print(dbgs());
    }
#endif

    LLVM_DEBUG(dbgs() << "Performing Loop Merge on function " << F.getName()
                      << "\n");
    bool Changed = false;

    while (!LDT.empty()) {
      LLVM_DEBUG(dbgs() << "Got " << LDT.size() << " loop sets for depth "
                        << LDT.getDepth() << "\n";);

      for (const LoopVector &LV : LDT) {
        assert(LV.size() > 0 && "Empty loop set was build!");

        // Skip singleton loop sets as they do not offer merge opportunities on
        // this level.
        if (LV.size() == 1)
          continue;
#ifndef NDEBUG
        if (VerboseMergeDebugging) {
          LLVM_DEBUG({
            dbgs() << "  Visit loop set (#" << LV.size() << "):\n";
            printLoopVector(LV);
          });
        }
#endif

        collectMergeCandidates(LV);
        Changed |= mergeCandidates();
      }

      // Finished analyzing candidates at this level.
      // Descend to the next level and clear all of the candidates currently
      // collected. Note that it will not be possible to merge any of the
      // existing candidates with new candidates because the new candidates will
      // be at a different nest level and thus not be control flow equivalent
      // with all of the candidates collected so far.
      LLVM_DEBUG(dbgs() << "Descend one level!\n");
      LDT.descend();
      MergeCandidates.clear();
    }

    if (Changed)
      LLVM_DEBUG(dbgs() << "Function after Loop Merge: \n"; F.dump(););

#ifndef NDEBUG
    assert(DT.verify());
    assert(PDT.verify());
    LI.verify(DT);
    SE.verify();
#endif

    LLVM_DEBUG(dbgs() << "Loop Merge complete\n");
    return Changed;
  }

private:
  /// Determine if two merge candidates are control flow equivalent.
  ///
  /// Two merge candidates are control flow equivalent if when one executes,
  /// the other is guaranteed to execute. This is determined using dominators
  /// and post-dominators: if A dominates B and B post-dominates A then A and B
  /// are control-flow equivalent.
  bool isControlFlowEquivalent(const MergeCandidate &FC0,
                               const MergeCandidate &FC1) const {
    assert(FC0.Preheader && FC1.Preheader && "Expecting valid preheaders");

    if (DT.dominates(FC0.Preheader, FC1.Preheader))
      return PDT.dominates(FC1.Preheader, FC0.Preheader);

    if (DT.dominates(FC1.Preheader, FC0.Preheader))
      return PDT.dominates(FC0.Preheader, FC1.Preheader);

    return false;
  }

  /// Determine if a merge candidate (representing a loop) is eligible for
  /// merge. Note that this only checks whether a single loop can be merged - it
  /// does not check whether it is *legal* to merge two loops together.
  bool eligibleForMerge(const MergeCandidate &FC) const {
    if (!FC.isValid()) {
      LLVM_DEBUG(dbgs() << "FC " << FC << " has invalid CFG requirements!\n");
      if (!FC.Preheader)
        InvalidPreheader++;
      if (!FC.Header)
        InvalidHeader++;
      if (!FC.ExitingBlock)
        InvalidExitingBlock++;
      if (!FC.ExitBlock)
        InvalidExitBlock++;
      if (!FC.Latch)
        InvalidLatch++;
      if (FC.L->isInvalid())
        InvalidLoop++;

      return false;
    }

    // Require ScalarEvolution to be able to determine a trip count.
    if (!SE.hasLoopInvariantBackedgeTakenCount(FC.L)) {
      LLVM_DEBUG(dbgs() << "Loop " << FC.L->getName()
                        << " trip count not computable!\n");
      InvalidTripCount++;
      return false;
    }

    if (!FC.L->isLoopSimplifyForm()) {
      LLVM_DEBUG(dbgs() << "Loop " << FC.L->getName()
                        << " is not in simplified form!\n");
      NotSimplifiedForm++;
      return false;
    }

    return true;
  }

  /// Iterate over all loops in the given loop set and identify the loops that
  /// are eligible for merge. Place all eligible merge candidates into Control
  /// Flow Equivalent sets, sorted by dominance.
  void collectMergeCandidates(const LoopVector &LV) {
    for (Loop *L : LV) {
      MergeCandidate CurrCand(L, &DT, &PDT);
      if (!eligibleForMerge(CurrCand))
        continue;

      // Go through each list in MergeCandidates and determine if L is control
      // flow equivalent with the first loop in that list. If it is, append LV.
      // If not, go to the next list.
      // If no suitable list is found, start another list and add it to
      // MergeCandidates.
      bool FoundSet = false;

      for (auto &CurrCandSet : MergeCandidates) {
        if (isControlFlowEquivalent(*CurrCandSet.begin(), CurrCand)) {
          CurrCandSet.insert(CurrCand);
          FoundSet = true;
#ifndef NDEBUG
          if (VerboseMergeDebugging)
            LLVM_DEBUG(dbgs() << "Adding " << CurrCand
                              << " to existing candidate set\n");
#endif
          break;
        }
      }
      if (!FoundSet) {
        // No set was found. Create a new set and add to MergeCandidates
#ifndef NDEBUG
        if (VerboseMergeDebugging)
          LLVM_DEBUG(dbgs() << "Adding " << CurrCand << " to new set\n");
#endif
        MergeCandidateSet NewCandSet;
        NewCandSet.insert(CurrCand);
        MergeCandidates.push_back(NewCandSet);
      }
      NumMergeCandidates++;
    }
  }

  /// Determine if it is beneficial to merge two loops.
  ///
  /// For now, this method simply returns true because we want to merge as much
  /// as possible (primarily to test the pass). This method will evolve, over
  /// time, to add heuristics for profitability of merge.
  bool isBeneficialMerge(const MergeCandidate &FC0,
                          const MergeCandidate &FC1) {
    return true;
  }

  /// Determine if two merge candidates have the same trip count (i.e., they
  /// execute the same number of iterations).
  ///
  /// Note that for now this method simply returns a boolean value because there
  /// are no mechanisms in loop merge to handle different trip counts. In the
  /// future, this behaviour can be extended to adjust one of the loops to make
  /// the trip counts equal (e.g., loop peeling). When this is added, this
  /// interface may need to change to return more information than just a
  /// boolean value.
  bool identicalTripCounts(const MergeCandidate &FC0,
                           const MergeCandidate &FC1) const {
    const SCEV *TripCount0 = SE.getBackedgeTakenCount(FC0.L);
    if (isa<SCEVCouldNotCompute>(TripCount0)) {
      UncomputableTripCount++;
      LLVM_DEBUG(dbgs() << "Trip count of first loop could not be computed!");
      return false;
    }

    const SCEV *TripCount1 = SE.getBackedgeTakenCount(FC1.L);
    if (isa<SCEVCouldNotCompute>(TripCount1)) {
      UncomputableTripCount++;
      LLVM_DEBUG(dbgs() << "Trip count of second loop could not be computed!");
      return false;
    }
    LLVM_DEBUG(dbgs() << "\tTrip counts: " << *TripCount0 << " & "
                      << *TripCount1 << " are "
                      << (TripCount0 == TripCount1 ? "identical" : "different")
                      << "\n");

    return (TripCount0 == TripCount1);
  }

  /// Walk each set of control flow equivalent merge candidates and attempt to
  /// merge them. This does a single linear traversal of all candidates in the
  /// set. The conditions for legal merge are checked at this point. If a pair
  /// of merge candidates passes all legality checks, they are merged together
  /// and a new merge candidate is created and added to the MergeCandidateSet.
  /// The original merge candidates are then removed, as they are no longer
  /// valid.
  bool mergeCandidates() {
    bool Merged = false;
    LLVM_DEBUG(printMergeCandidates(MergeCandidates));
    for (auto &CandidateSet : MergeCandidates) {
      if (CandidateSet.size() < 2)
        continue;

      LLVM_DEBUG(dbgs() << "Attempting merge on Candidate Set:\n"
                        << CandidateSet << "\n");

      for (auto FC0 = CandidateSet.begin(); FC0 != CandidateSet.end(); ++FC0) {
        assert(!LDT.isRemovedLoop(FC0->L) &&
               "Should not have removed loops in CandidateSet!");
        auto FC1 = FC0;
        for (++FC1; FC1 != CandidateSet.end(); ++FC1) {
          assert(!LDT.isRemovedLoop(FC1->L) &&
                 "Should not have removed loops in CandidateSet!");

          LLVM_DEBUG(dbgs() << "Attempting to merge candidate \n"; FC0->dump();
                     dbgs() << " with\n"; FC1->dump(); dbgs() << "\n");

          FC0->verify();
          FC1->verify();

          if (!identicalTripCounts(*FC0, *FC1)) {
            LLVM_DEBUG(dbgs() << "Merge candidates do not have identical trip "
                                 "counts. Not merging.\n");
            NonEqualTripCount++;
            continue;
          }

          if (!isAdjacent(*FC0, *FC1)) {
            LLVM_DEBUG(dbgs()
                       << "Merge candidates are not adjacent. Not merging.\n");
            NonAdjacent++;
            continue;
          }

          // For now we skip merging if the second candidate has any instructions
          // in the preheader. This is done because we currently do not have the
          // safety checks to determine if it is save to move the preheader of
          // the second candidate past the body of the first candidate. Once
          // these checks are added, this condition can be removed.
          if (!isEmptyPreheader(*FC1)) {
            LLVM_DEBUG(dbgs() << "Merge candidate does not have empty "
                                 "preheader. Not merging.\n");
            NonEmptyPreheader++;
            continue;
          }

          if (!dependencesAllowMerge(*FC0, *FC1)) {
            LLVM_DEBUG(dbgs() << "Memory dependencies do not allow merge!\n");
            continue;
          }

          bool BeneficialToMerge = isBeneficialMerge(*FC0, *FC1);
          LLVM_DEBUG(dbgs()
                     << "\tMerge appears to be "
                     << (BeneficialToMerge ? "" : "un") << "profitable!\n");
          if (!BeneficialToMerge)
            continue;

          // All analysis has completed and has determined that merge is legal
          // and profitable. At this point, start transforming the code and
          // perform merge.

          LLVM_DEBUG(dbgs() << "\tMerge is performed: " << *FC0 << " and "
                            << *FC1 << "\n");

          // Report merge to the Optimization Remarks.
          // Note this needs to be done *before* performMerge because
          // performMerge will change the original loops, making it not
          // possible to identify them after merge is complete.
          reportLoopMerge(*FC0, *FC1, ORE);

          MergeCandidate MergedCand(performMerge(*FC0, *FC1), &DT, &PDT);
          MergedCand.verify();
          assert(eligibleForMerge(MergedCand) &&
                 "Merged candidate should be eligible for merge!");

          // Notify the loop-depth-tree that these loops are not valid objects
          // anymore.
          LDT.removeLoop(FC1->L);

          CandidateSet.erase(FC0);
          CandidateSet.erase(FC1);

          auto InsertPos = CandidateSet.insert(MergedCand);

          assert(InsertPos.second &&
                 "Unable to insert TargetCandidate in CandidateSet!");

          // Reset FC0 and FC1 the new (merged) candidate. Subsequent iterations
          // of the FC1 loop will attempt to merge the new (merged) loop with the
          // remaining candidates in the current candidate set.
          FC0 = FC1 = InsertPos.first;

          LLVM_DEBUG(dbgs() << "Candidate Set (after merge): " << CandidateSet
                            << "\n");

          Merged = true;
        }
      }
    }
    return Merged;
  }

  /// Rewrite all additive recurrences in a SCEV to use a new loop.
  class AddRecLoopReplacer : public SCEVRewriteVisitor<AddRecLoopReplacer> {
  public:
    AddRecLoopReplacer(ScalarEvolution &SE, const Loop &OldL, const Loop &NewL,
                       bool UseMax = true)
        : SCEVRewriteVisitor(SE), Valid(true), UseMax(UseMax), OldL(OldL),
          NewL(NewL) {}

    const SCEV *visitAddRecExpr(const SCEVAddRecExpr *Expr) {
      const Loop *ExprL = Expr->getLoop();
      SmallVector<const SCEV *, 2> Operands;
      if (ExprL == &OldL) {
        Operands.append(Expr->op_begin(), Expr->op_end());
        return SE.getAddRecExpr(Operands, &NewL, Expr->getNoWrapFlags());
      }

      if (OldL.contains(ExprL)) {
        bool Pos = SE.isKnownPositive(Expr->getStepRecurrence(SE));
        if (!UseMax || !Pos || !Expr->isAffine()) {
          Valid = false;
          return Expr;
        }
        return visit(Expr->getStart());
      }

      for (const SCEV *Op : Expr->operands())
        Operands.push_back(visit(Op));
      return SE.getAddRecExpr(Operands, ExprL, Expr->getNoWrapFlags());
    }

    bool wasValidSCEV() const { return Valid; }

  private:
    bool Valid, UseMax;
    const Loop &OldL, &NewL;
  };

  /// Return false if the access functions of \p I0 and \p I1 could cause
  /// a negative dependence.
  bool accessDiffIsPositive(const Loop &L0, const Loop &L1, Instruction &I0,
                            Instruction &I1, bool EqualIsInvalid) {
    Value *Ptr0 = getLoadStorePointerOperand(&I0);
    Value *Ptr1 = getLoadStorePointerOperand(&I1);
    if (!Ptr0 || !Ptr1)
      return false;

    const SCEV *SCEVPtr0 = SE.getSCEVAtScope(Ptr0, &L0);
    const SCEV *SCEVPtr1 = SE.getSCEVAtScope(Ptr1, &L1);
#ifndef NDEBUG
    if (VerboseMergeDebugging)
      LLVM_DEBUG(dbgs() << "    Access function check: " << *SCEVPtr0 << " vs "
                        << *SCEVPtr1 << "\n");
#endif
    AddRecLoopReplacer Rewriter(SE, L0, L1);
    SCEVPtr0 = Rewriter.visit(SCEVPtr0);
#ifndef NDEBUG
    if (VerboseMergeDebugging)
      LLVM_DEBUG(dbgs() << "    Access function after rewrite: " << *SCEVPtr0
                        << " [Valid: " << Rewriter.wasValidSCEV() << "]\n");
#endif
    if (!Rewriter.wasValidSCEV())
      return false;

    // TODO: isKnownPredicate doesnt work well when one SCEV is loop carried (by
    //       L0) and the other is not. We could check if it is monotone and test
    //       the beginning and end value instead.

    BasicBlock *L0Header = L0.getHeader();
    auto HasNonLinearDominanceRelation = [&](const SCEV *S) {
      const SCEVAddRecExpr *AddRec = dyn_cast<SCEVAddRecExpr>(S);
      if (!AddRec)
        return false;
      return !DT.dominates(L0Header, AddRec->getLoop()->getHeader()) &&
             !DT.dominates(AddRec->getLoop()->getHeader(), L0Header);
    };
    if (SCEVExprContains(SCEVPtr1, HasNonLinearDominanceRelation))
      return false;

    ICmpInst::Predicate Pred =
        EqualIsInvalid ? ICmpInst::ICMP_SGT : ICmpInst::ICMP_SGE;
    bool IsAlwaysGE = SE.isKnownPredicate(Pred, SCEVPtr0, SCEVPtr1);
#ifndef NDEBUG
    if (VerboseMergeDebugging)
      LLVM_DEBUG(dbgs() << "    Relation: " << *SCEVPtr0
                        << (IsAlwaysGE ? "  >=  " : "  may <  ") << *SCEVPtr1
                        << "\n");
#endif
    return IsAlwaysGE;
  }

  /// Return true if the dependences between @p I0 (in @p L0) and @p I1 (in
  /// @p L1) allow loop merge of @p L0 and @p L1. The dependence analyses
  /// specified by @p DepChoice are used to determine this.
  bool dependencesAllowMerge(const MergeCandidate &FC0,
                              const MergeCandidate &FC1, Instruction &I0,
                              Instruction &I1, bool AnyDep,
                              MergeDependenceAnalysisChoice DepChoice) {
#ifndef NDEBUG
    if (VerboseMergeDebugging) {
      LLVM_DEBUG(dbgs() << "Check dep: " << I0 << " vs " << I1 << " : "
                        << DepChoice << "\n");
    }
#endif
    switch (DepChoice) {
    case FUSION_DEPENDENCE_ANALYSIS_SCEV:
      return accessDiffIsPositive(*FC0.L, *FC1.L, I0, I1, AnyDep);
    case FUSION_DEPENDENCE_ANALYSIS_DA: {
      auto DepResult = DI.depends(&I0, &I1, true);
      if (!DepResult)
        return true;
#ifndef NDEBUG
      if (VerboseMergeDebugging) {
        LLVM_DEBUG(dbgs() << "DA res: "; DepResult->dump(dbgs());
                   dbgs() << " [#l: " << DepResult->getLevels() << "][Ordered: "
                          << (DepResult->isOrdered() ? "true" : "false")
                          << "]\n");
        LLVM_DEBUG(dbgs() << "DepResult Levels: " << DepResult->getLevels()
                          << "\n");
      }
#endif

      if (DepResult->getNextPredecessor() || DepResult->getNextSuccessor())
        LLVM_DEBUG(
            dbgs() << "TODO: Implement pred/succ dependence handling!\n");

      // TODO: Can we actually use the dependence info analysis here?
      return false;
    }

    case FUSION_DEPENDENCE_ANALYSIS_ALL:
      return dependencesAllowMerge(FC0, FC1, I0, I1, AnyDep,
                                    FUSION_DEPENDENCE_ANALYSIS_SCEV) ||
             dependencesAllowMerge(FC0, FC1, I0, I1, AnyDep,
                                    FUSION_DEPENDENCE_ANALYSIS_DA);
    }

    llvm_unreachable("Unknown merge dependence analysis choice!");
  }

  /// Perform a dependence check and return if @p FC0 and @p FC1 can be merged.
  bool dependencesAllowMerge(const MergeCandidate &FC0,
                              const MergeCandidate &FC1) {
    LLVM_DEBUG(dbgs() << "Check if " << FC0 << " can be merged with " << FC1
                      << "\n");
    assert(FC0.L->getLoopDepth() == FC1.L->getLoopDepth());
    assert(DT.dominates(FC0.Preheader, FC1.Preheader));

    for (Instruction *WriteL0 : FC0.MemWrites) {
      for (Instruction *WriteL1 : FC1.MemWrites)
        if (!dependencesAllowMerge(FC0, FC1, *WriteL0, *WriteL1,
                                    /* AnyDep */ false,
                                    MergeDependenceAnalysis)) {
          InvalidDependencies++;
          return false;
        }
      for (Instruction *ReadL1 : FC1.MemReads)
        if (!dependencesAllowMerge(FC0, FC1, *WriteL0, *ReadL1,
                                    /* AnyDep */ false,
                                    MergeDependenceAnalysis)) {
          InvalidDependencies++;
          return false;
        }
    }

    for (Instruction *WriteL1 : FC1.MemWrites) {
      for (Instruction *WriteL0 : FC0.MemWrites)
        if (!dependencesAllowMerge(FC0, FC1, *WriteL0, *WriteL1,
                                    /* AnyDep */ false,
                                    MergeDependenceAnalysis)) {
          InvalidDependencies++;
          return false;
        }
      for (Instruction *ReadL0 : FC0.MemReads)
        if (!dependencesAllowMerge(FC0, FC1, *ReadL0, *WriteL1,
                                    /* AnyDep */ false,
                                    MergeDependenceAnalysis)) {
          InvalidDependencies++;
          return false;
        }
    }

    // Walk through all uses in FC1. For each use, find the reaching def. If the
    // def is located in FC0 then it is is not safe to merge.
    for (BasicBlock *BB : FC1.L->blocks())
      for (Instruction &I : *BB)
        for (auto &Op : I.operands())
          if (Instruction *Def = dyn_cast<Instruction>(Op))
            if (FC0.L->contains(Def->getParent())) {
              InvalidDependencies++;
              return false;
            }

    return true;
  }

  /// Determine if the exit block of \p FC0 is the preheader of \p FC1. In this
  /// case, there is no code in between the two merge candidates, thus making
  /// them adjacent.
  bool isAdjacent(const MergeCandidate &FC0,
                  const MergeCandidate &FC1) const {
    return FC0.ExitBlock == FC1.Preheader;
  }

  bool isEmptyPreheader(const MergeCandidate &FC) const {
    return FC.Preheader->size() == 1;
  }

  /// Merge two merge candidates, creating a new merged loop.
  ///
  /// This method contains the mechanics of merging two loops, represented by \p
  /// FC0 and \p FC1. It is assumed that \p FC0 dominates \p FC1 and \p FC1
  /// postdominates \p FC0 (making them control flow equivalent). It also
  /// assumes that the other conditions for merge have been met: adjacent,
  /// identical trip counts, and no negative distance dependencies exist that
  /// would prevent merge. Thus, there is no checking for these conditions in
  /// this method.
  ///
  /// Merge is performed by rewiring the CFG to update successor blocks of the
  /// components of tho loop. Specifically, the following changes are done:
  ///
  ///   1. The preheader of \p FC1 is removed as it is no longer necessary
  ///   (because it is currently only a single statement block).
  ///   2. The latch of \p FC0 is modified to jump to the header of \p FC1.
  ///   3. The latch of \p FC1 i modified to jump to the header of \p FC0.
  ///   4. All blocks from \p FC1 are removed from FC1 and added to FC0.
  ///
  /// All of these modifications are done with dominator tree updates, thus
  /// keeping the dominator (and post dominator) information up-to-date.
  ///
  /// This can be improved in the future by actually merging blocks during
  /// merge. For example, the preheader of \p FC1 can be merged with the
  /// preheader of \p FC0. This would allow loops with more than a single
  /// statement in the preheader to be merged. Similarly, the latch blocks of the
  /// two loops could also be merged into a single block. This will require
  /// analysis to prove it is safe to move the contents of the block past
  /// existing code, which currently has not been implemented.
  Loop *performMerge(const MergeCandidate &FC0, const MergeCandidate &FC1) {
    assert(FC0.isValid() && FC1.isValid() &&
           "Expecting valid merge candidates");

    LLVM_DEBUG(dbgs() << "Merge Candidate 0: \n"; FC0.dump();
               dbgs() << "Merge Candidate 1: \n"; FC1.dump(););

    assert(FC1.Preheader == FC0.ExitBlock);
    assert(FC1.Preheader->size() == 1 &&
           FC1.Preheader->getSingleSuccessor() == FC1.Header);

    // Remember the phi nodes originally in the header of FC0 in order to rewire
    // them later. However, this is only necessary if the new loop carried
    // values might not dominate the exiting branch. While we do not generally
    // test if this is the case but simply insert intermediate phi nodes, we
    // need to make sure these intermediate phi nodes have different
    // predecessors. To this end, we filter the special case where the exiting
    // block is the latch block of the first loop. Nothing needs to be done
    // anyway as all loop carried values dominate the latch and thereby also the
    // exiting branch.
    SmallVector<PHINode *, 8> OriginalFC0PHIs;
    if (FC0.ExitingBlock != FC0.Latch)
      for (PHINode &PHI : FC0.Header->phis())
        OriginalFC0PHIs.push_back(&PHI);

    // Replace incoming blocks for header PHIs first.
    FC1.Preheader->replaceSuccessorsPhiUsesWith(FC0.Preheader);
    FC0.Latch->replaceSuccessorsPhiUsesWith(FC1.Latch);

    // Then modify the control flow and update DT and PDT.
    SmallVector<DominatorTree::UpdateType, 8> TreeUpdates;

    // The old exiting block of the first loop (FC0) has to jump to the header
    // of the second as we need to execute the code in the second header block
    // regardless of the trip count. That is, if the trip count is 0, so the
    // back edge is never taken, we still have to execute both loop headers,
    // especially (but not only!) if the second is a do-while style loop.
    // However, doing so might invalidate the phi nodes of the first loop as
    // the new values do only need to dominate their latch and not the exiting
    // predicate. To remedy this potential problem we always introduce phi
    // nodes in the header of the second loop later that select the loop carried
    // value, if the second header was reached through an old latch of the
    // first, or undef otherwise. This is sound as exiting the first implies the
    // second will exit too, __without__ taking the back-edge. [Their
    // trip-counts are equal after all.
    // KB: Would this sequence be simpler to just just make FC0.ExitingBlock go
    // to FC1.Header? I think this is basically what the three sequences are
    // trying to accomplish; however, doing this directly in the CFG may mean
    // the DT/PDT becomes invalid
    FC0.ExitingBlock->getTerminator()->replaceUsesOfWith(FC1.Preheader,
                                                         FC1.Header);
    TreeUpdates.emplace_back(DominatorTree::UpdateType(
        DominatorTree::Delete, FC0.ExitingBlock, FC1.Preheader));
    TreeUpdates.emplace_back(DominatorTree::UpdateType(
        DominatorTree::Insert, FC0.ExitingBlock, FC1.Header));

    // The pre-header of L1 is not necessary anymore.
    assert(pred_begin(FC1.Preheader) == pred_end(FC1.Preheader));
    FC1.Preheader->getTerminator()->eraseFromParent();
    new UnreachableInst(FC1.Preheader->getContext(), FC1.Preheader);
    TreeUpdates.emplace_back(DominatorTree::UpdateType(
        DominatorTree::Delete, FC1.Preheader, FC1.Header));

    // Moves the phi nodes from the second to the first loops header block.
    while (PHINode *PHI = dyn_cast<PHINode>(&FC1.Header->front())) {
      if (SE.isSCEVable(PHI->getType()))
        SE.forgetValue(PHI);
      if (PHI->hasNUsesOrMore(1))
        PHI->moveBefore(&*FC0.Header->getFirstInsertionPt());
      else
        PHI->eraseFromParent();
    }

    // Introduce new phi nodes in the second loop header to ensure
    // exiting the first and jumping to the header of the second does not break
    // the SSA property of the phis originally in the first loop. See also the
    // comment above.
    Instruction *L1HeaderIP = &FC1.Header->front();
    for (PHINode *LCPHI : OriginalFC0PHIs) {
      int L1LatchBBIdx = LCPHI->getBasicBlockIndex(FC1.Latch);
      assert(L1LatchBBIdx >= 0 &&
             "Expected loop carried value to be rewired at this point!");

      Value *LCV = LCPHI->getIncomingValue(L1LatchBBIdx);

      PHINode *L1HeaderPHI = PHINode::Create(
          LCV->getType(), 2, LCPHI->getName() + ".afterFC0", L1HeaderIP);
      L1HeaderPHI->addIncoming(LCV, FC0.Latch);
      L1HeaderPHI->addIncoming(UndefValue::get(LCV->getType()),
                               FC0.ExitingBlock);

      LCPHI->setIncomingValue(L1LatchBBIdx, L1HeaderPHI);
    }

    // Replace latch terminator destinations.
    FC0.Latch->getTerminator()->replaceUsesOfWith(FC0.Header, FC1.Header);
    FC1.Latch->getTerminator()->replaceUsesOfWith(FC1.Header, FC0.Header);

    // If FC0.Latch and FC0.ExitingBlock are the same then we have already
    // performed the updates above.
    if (FC0.Latch != FC0.ExitingBlock)
      TreeUpdates.emplace_back(DominatorTree::UpdateType(
          DominatorTree::Insert, FC0.Latch, FC1.Header));

    TreeUpdates.emplace_back(DominatorTree::UpdateType(DominatorTree::Delete,
                                                       FC0.Latch, FC0.Header));
    TreeUpdates.emplace_back(DominatorTree::UpdateType(DominatorTree::Insert,
                                                       FC1.Latch, FC0.Header));
    TreeUpdates.emplace_back(DominatorTree::UpdateType(DominatorTree::Delete,
                                                       FC1.Latch, FC1.Header));

    // Update DT/PDT
    DTU.applyUpdates(TreeUpdates);

    LI.removeBlock(FC1.Preheader);
    DTU.deleteBB(FC1.Preheader);
    DTU.flush();

    // Is there a way to keep SE up-to-date so we don't need to forget the loops
    // and rebuild the information in subsequent passes of merge?
    SE.forgetLoop(FC1.L);
    SE.forgetLoop(FC0.L);

    // Merge the loops.
    SmallVector<BasicBlock *, 8> Blocks(FC1.L->block_begin(),
                                        FC1.L->block_end());
    for (BasicBlock *BB : Blocks) {
      FC0.L->addBlockEntry(BB);
      FC1.L->removeBlockFromLoop(BB);
      if (LI.getLoopFor(BB) != FC1.L)
        continue;
      LI.changeLoopFor(BB, FC0.L);
    }
    while (!FC1.L->empty()) {
      const auto &ChildLoopIt = FC1.L->begin();
      Loop *ChildLoop = *ChildLoopIt;
      FC1.L->removeChildLoop(ChildLoopIt);
      FC0.L->addChildLoop(ChildLoop);
    }

    // Delete the now empty loop L1.
    LI.erase(FC1.L);

#ifndef NDEBUG
    assert(!verifyFunction(*FC0.Header->getParent(), &errs()));
    assert(DT.verify(DominatorTree::VerificationLevel::Fast));
    assert(PDT.verify());
    LI.verify(DT);
    SE.verify();
#endif

    MergeCounter++;

    LLVM_DEBUG(dbgs() << "Merge done:\n");

    return FC0.L;
  }
};

struct LoopMergeLegacy : public FunctionPass {

  static char ID;

  LoopMergeLegacy() : FunctionPass(ID) {
    initializeLoopMergeLegacyPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequiredID(LoopSimplifyID);
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<PostDominatorTreeWrapperPass>();
    AU.addRequired<OptimizationRemarkEmitterWrapperPass>();
    AU.addRequired<DependenceAnalysisWrapperPass>();

    AU.addPreserved<ScalarEvolutionWrapperPass>();
    AU.addPreserved<LoopInfoWrapperPass>();
    AU.addPreserved<DominatorTreeWrapperPass>();
    AU.addPreserved<PostDominatorTreeWrapperPass>();
  }

  bool runOnFunction(Function &F) override {
    if (skipFunction(F))
      return false;
    auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
    auto &DI = getAnalysis<DependenceAnalysisWrapperPass>().getDI();
    auto &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();
    auto &PDT = getAnalysis<PostDominatorTreeWrapperPass>().getPostDomTree();
    auto &ORE = getAnalysis<OptimizationRemarkEmitterWrapperPass>().getORE();

    const DataLayout &DL = F.getParent()->getDataLayout();
    LoopMerger LF(LI, DT, DI, SE, PDT, ORE, DL);
    return LF.mergeLoops(F);
  }
};

PreservedAnalyses LoopMergePass::run(Function &F, FunctionAnalysisManager &AM) {
  auto &LI = AM.getResult<LoopAnalysis>(F);
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &DI = AM.getResult<DependenceAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
  auto &PDT = AM.getResult<PostDominatorTreeAnalysis>(F);
  auto &ORE = AM.getResult<OptimizationRemarkEmitterAnalysis>(F);

  const DataLayout &DL = F.getParent()->getDataLayout();
  LoopMerger LF(LI, DT, DI, SE, PDT, ORE, DL);
  bool Changed = LF.mergeLoops(F);
  if (!Changed)
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserve<DominatorTreeAnalysis>();
  PA.preserve<PostDominatorTreeAnalysis>();
  PA.preserve<ScalarEvolutionAnalysis>();
  PA.preserve<LoopAnalysis>();
  return PA;
}

char LoopMergeLegacy::ID = 0;

INITIALIZE_PASS_BEGIN(LoopMergeLegacy, "loop-merge", "Loop Merge", false,
                      false)
INITIALIZE_PASS_DEPENDENCY(PostDominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DependenceAnalysisWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(OptimizationRemarkEmitterWrapperPass)
INITIALIZE_PASS_END(LoopMergeLegacy, "loop-merge", "Loop Merge", false, false)

FunctionPass *llvm::createLoopMergePass() { return new LoopMergeLegacy(); }
