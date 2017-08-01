//===------ Simplify.cpp ----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Simplify a SCoP by removing unnecessary statements and accesses.
//
//===----------------------------------------------------------------------===//

#include "polly/Simplify.h"
#include "polly/ScopInfo.h"
#include "polly/ScopPass.h"
#include "polly/Support/GICHelper.h"
#include "polly/Support/ISLOStream.h"
#include "polly/Support/ISLTools.h"
#include "polly/Support/VirtualInstruction.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"
#define DEBUG_TYPE "polly-simplify"

using namespace llvm;
using namespace polly;

namespace {

/// Number of max disjuncts we allow in removeOverwrites(). This is to avoid
/// that the analysis of accesses in a statement is becoming too complex. Chosen
/// to be relatively small because all the common cases should access only few
/// array elements per statement.
static int const SimplifyMaxDisjuncts = 4;

STATISTIC(ScopsProcessed, "Number of SCoPs processed");
STATISTIC(ScopsModified, "Number of SCoPs simplified");

STATISTIC(PairUnequalAccRels, "Number of Load-Store pairs NOT removed because "
                              "of different access relations");
STATISTIC(InBetweenStore, "Number of Load-Store pairs NOT removed because "
                          "there is another store between them");
STATISTIC(TotalOverwritesRemoved, "Number of removed overwritten writes");
STATISTIC(TotalWritesCoalesced, "Number of writes coalesced with another");
STATISTIC(TotalRedundantWritesRemoved,
          "Number of writes of same value removed in any SCoP");
STATISTIC(TotalEmptyPartialAccessesRemoved,
          "Number of empty partial accesses removed");
STATISTIC(TotalDeadAccessesRemoved, "Number of dead accesses removed");
STATISTIC(TotalDeadInstructionsRemoved,
          "Number of unused instructions removed");
STATISTIC(TotalStmtsRemoved, "Number of statements removed in any SCoP");

static bool isImplicitRead(MemoryAccess *MA) {
  return MA->isRead() && MA->isOriginalScalarKind();
}

static bool isExplicitAccess(MemoryAccess *MA) {
  return MA->isOriginalArrayKind();
}

static bool isImplicitWrite(MemoryAccess *MA) {
  return MA->isWrite() && MA->isOriginalScalarKind();
}

/// Like isl::union_map::add_map, but may also return an underapproximated
/// result if getting too complex.
///
/// This is implemented by adding disjuncts to the results until the limit is
/// reached.
static isl::union_map underapproximatedAddMap(isl::union_map UMap,
                                              isl::map Map) {
  if (UMap.is_null() || Map.is_null())
    return {};

  isl::map PrevMap = UMap.extract_map(Map.get_space());

  // Fast path: If known that we cannot exceed the disjunct limit, just add
  // them.
  if (isl_map_n_basic_map(PrevMap.get()) + isl_map_n_basic_map(Map.get()) <=
      SimplifyMaxDisjuncts)
    return UMap.add_map(Map);

  isl::map Result = isl::map::empty(PrevMap.get_space());
  PrevMap.foreach_basic_map([&Result](isl::basic_map BMap) -> isl::stat {
    if (isl_map_n_basic_map(Result.get()) > SimplifyMaxDisjuncts)
      return isl::stat::error;
    Result = Result.unite(BMap);
    return isl::stat::ok;
  });
  Map.foreach_basic_map([&Result](isl::basic_map BMap) -> isl::stat {
    if (isl_map_n_basic_map(Result.get()) > SimplifyMaxDisjuncts)
      return isl::stat::error;
    Result = Result.unite(BMap);
    return isl::stat::ok;
  });

  isl::union_map UResult =
      UMap.subtract(isl::map::universe(PrevMap.get_space()));
  UResult.add_map(Result);

  return UResult;
}

/// Return a vector that contains MemoryAccesses in the order in
/// which they are executed.
///
/// The order is:
/// - Implicit reads (BlockGenerator::generateScalarLoads)
/// - Explicit reads and writes (BlockGenerator::generateArrayLoad,
///   BlockGenerator::generateArrayStore)
///   - In block statements, the accesses are in order in which their
///     instructions are executed.
///   - In region statements, that order of execution is not predictable at
///     compile-time.
/// - Implicit writes (BlockGenerator::generateScalarStores)
///   The order in which implicit writes are executed relative to each other is
///   undefined.
static SmallVector<MemoryAccess *, 32> getAccessesInOrder(ScopStmt &Stmt) {

  SmallVector<MemoryAccess *, 32> Accesses;

  for (MemoryAccess *MemAcc : Stmt)
    if (isImplicitRead(MemAcc))
      Accesses.push_back(MemAcc);

  for (MemoryAccess *MemAcc : Stmt)
    if (isExplicitAccess(MemAcc))
      Accesses.push_back(MemAcc);

  for (MemoryAccess *MemAcc : Stmt)
    if (isImplicitWrite(MemAcc))
      Accesses.push_back(MemAcc);

  return Accesses;
}

class Simplify : public ScopPass {
private:
  /// The last/current SCoP that is/has been processed.
  Scop *S;

  /// Number of writes that are overwritten anyway.
  int OverwritesRemoved = 0;

  /// Number of combined writes.
  int WritesCoalesced = 0;

  /// Number of redundant writes removed from this SCoP.
  int RedundantWritesRemoved = 0;

  /// Number of writes with empty access domain removed.
  int EmptyPartialAccessesRemoved = 0;

  /// Number of unused accesses removed from this SCoP.
  int DeadAccessesRemoved = 0;

  /// Number of unused instructions removed from this SCoP.
  int DeadInstructionsRemoved = 0;

  /// Number of unnecessary statements removed from the SCoP.
  int StmtsRemoved = 0;

  /// Return whether at least one simplification has been applied.
  bool isModified() const {
    return OverwritesRemoved > 0 || WritesCoalesced > 0 ||
           RedundantWritesRemoved > 0 || EmptyPartialAccessesRemoved > 0 ||
           DeadAccessesRemoved > 0 || DeadInstructionsRemoved > 0 ||
           StmtsRemoved > 0;
  }

  MemoryAccess *getReadAccessForValue(ScopStmt *Stmt, llvm::Value *Val) {
    if (!isa<Instruction>(Val))
      return nullptr;

    for (auto *MA : *Stmt) {
      if (!MA->isRead())
        continue;
      if (MA->getAccessValue() != Val)
        continue;

      return MA;
    }

    return nullptr;
  }

  /// Return a write access that occurs between @p From and @p To.
  ///
  /// In region statements the order is ignored because we cannot predict it.
  ///
  /// @param Stmt    Statement of both writes.
  /// @param From    Start looking after this access.
  /// @param To      Stop looking at this access, with the access itself.
  /// @param Targets Look for an access that may wrote to one of these elements.
  ///
  /// @return A write access between @p From and @p To that writes to at least
  ///         one element in @p Targets.
  MemoryAccess *hasWriteBetween(ScopStmt *Stmt, MemoryAccess *From,
                                MemoryAccess *To, isl::map Targets) {
    auto TargetsSpace = Targets.get_space();

    bool Started = Stmt->isRegionStmt();
    auto Accesses = getAccessesInOrder(*Stmt);
    for (auto *Acc : Accesses) {
      if (Acc->isLatestScalarKind())
        continue;

      if (Stmt->isBlockStmt() && From == Acc) {
        assert(!Started);
        Started = true;
        continue;
      }
      if (Stmt->isBlockStmt() && To == Acc) {
        assert(Started);
        return nullptr;
      }
      if (!Started)
        continue;

      if (!Acc->isWrite())
        continue;

      isl::map AccRel = Acc->getAccessRelation();
      auto AccRelSpace = AccRel.get_space();

      // Spaces being different means that they access different arrays.
      if (!TargetsSpace.has_equal_tuples(AccRelSpace))
        continue;

      AccRel = AccRel.intersect_domain(give(Acc->getStatement()->getDomain()));
      AccRel = AccRel.intersect_params(give(S->getContext()));
      auto CommonElt = Targets.intersect(AccRel);
      if (!CommonElt.is_empty())
        return Acc;
    }
    assert(Stmt->isRegionStmt() &&
           "To must be encountered in block statements");
    return nullptr;
  }

  /// Remove writes that are overwritten unconditionally later in the same
  /// statement.
  ///
  /// There must be no read of the same value between the write (that is to be
  /// removed) and the overwrite.
  void removeOverwrites() {
    for (auto &Stmt : *S) {
      auto Domain = give(Stmt.getDomain());
      isl::union_map WillBeOverwritten =
          isl::union_map::empty(give(S->getParamSpace()));

      SmallVector<MemoryAccess *, 32> Accesses(getAccessesInOrder(Stmt));

      // Iterate in reverse order, so the overwrite comes before the write that
      // is to be removed.
      for (auto *MA : reverse(Accesses)) {

        // In region statements, the explicit accesses can be in blocks that are
        // can be executed in any order. We therefore process only the implicit
        // writes and stop after that.
        if (Stmt.isRegionStmt() && isExplicitAccess(MA))
          break;

        auto AccRel = MA->getAccessRelation();
        AccRel = AccRel.intersect_domain(Domain);
        AccRel = AccRel.intersect_params(give(S->getContext()));

        // If a value is read in-between, do not consider it as overwritten.
        if (MA->isRead()) {
          // Invalidate all overwrites for the array it accesses to avoid too
          // complex isl sets.
          isl::map AccRelUniv = isl::map::universe(AccRel.get_space());
          WillBeOverwritten = WillBeOverwritten.subtract(AccRelUniv);
          continue;
        }

        // If all of a write's elements are overwritten, remove it.
        isl::union_map AccRelUnion = AccRel;
        if (AccRelUnion.is_subset(WillBeOverwritten)) {
          DEBUG(dbgs() << "Removing " << MA
                       << " which will be overwritten anyway\n");

          Stmt.removeSingleMemoryAccess(MA);
          OverwritesRemoved++;
          TotalOverwritesRemoved++;
        }

        // Unconditional writes overwrite other values.
        if (MA->isMustWrite()) {
          // Avoid too complex isl sets. If necessary, throw away some of the
          // knowledge.
          WillBeOverwritten =
              underapproximatedAddMap(WillBeOverwritten, AccRel);
        }
      }
    }
  }

  /// Combine writes that write the same value if possible.
  ///
  /// This function is able to combine:
  /// - Partial writes with disjoint domain.
  /// - Writes that write to the same array element.
  ///
  /// In all cases, both writes must write the same values.
  void coalesceWrites() {
    for (auto &Stmt : *S) {
      isl::set Domain =
          give(Stmt.getDomain()).intersect_params(give(S->getContext()));

      // We let isl do the lookup for the same-value condition. For this, we
      // wrap llvm::Value into an isl::set such that isl can do the lookup in
      // its hashtable implementation. llvm::Values are only compared within a
      // ScopStmt, so the map can be local to this scope. TODO: Refactor with
      // ZoneAlgorithm::makeValueSet()
      SmallDenseMap<Value *, isl::set> ValueSets;
      auto makeValueSet = [&ValueSets, this](Value *V) -> isl::set {
        assert(V);
        isl::set &Result = ValueSets[V];
        if (Result.is_null()) {
          isl_ctx *Ctx = S->getIslCtx();
          std::string Name =
              getIslCompatibleName("Val", V, ValueSets.size() - 1,
                                   std::string(), UseInstructionNames);
          isl::id Id = give(isl_id_alloc(Ctx, Name.c_str(), V));
          Result = isl::set::universe(
              isl::space(Ctx, 0, 0).set_tuple_id(isl::dim::set, Id));
        }
        return Result;
      };

      // List of all eligible (for coalescing) writes of the future.
      // { [Domain[] -> Element[]] -> [Value[] -> MemoryAccess[]] }
      isl::union_map FutureWrites =
          isl::union_map::empty(give(S->getParamSpace()));

      // Iterate over accesses from the last to the first.
      SmallVector<MemoryAccess *, 32> Accesses(getAccessesInOrder(Stmt));
      for (MemoryAccess *MA : reverse(Accesses)) {
        // In region statements, the explicit accesses can be in blocks that can
        // be executed in any order. We therefore process only the implicit
        // writes and stop after that.
        if (Stmt.isRegionStmt() && isExplicitAccess(MA))
          break;

        // { Domain[] -> Element[] }
        isl::map AccRel =
            MA->getLatestAccessRelation().intersect_domain(Domain);

        // { [Domain[] -> Element[]] }
        isl::set AccRelWrapped = AccRel.wrap();

        // { Value[] }
        isl::set ValSet;

        if (MA->isMustWrite() && (MA->isOriginalScalarKind() ||
                                  isa<StoreInst>(MA->getAccessInstruction()))) {
          // Normally, tryGetValueStored() should be used to determine which
          // element is written, but it can return nullptr; For PHI accesses,
          // getAccessValue() returns the PHI instead of the PHI's incoming
          // value. In this case, where we only compare values of a single
          // statement, this is fine, because within a statement, a PHI in a
          // successor block has always the same value as the incoming write. We
          // still preferably use the incoming value directly so we also catch
          // direct uses of that.
          Value *StoredVal = MA->tryGetValueStored();
          if (!StoredVal)
            StoredVal = MA->getAccessValue();
          ValSet = makeValueSet(StoredVal);

          // { Domain[] }
          isl::set AccDomain = AccRel.domain();

          // Parts of the statement's domain that is not written by this access.
          isl::set UndefDomain = Domain.subtract(AccDomain);

          // { Element[] }
          isl::set ElementUniverse =
              isl::set::universe(AccRel.get_space().range());

          // { Domain[] -> Element[] }
          isl::map UndefAnything =
              isl::map::from_domain_and_range(UndefDomain, ElementUniverse);

          // We are looking a compatible write access. The other write can
          // access these elements...
          isl::map AllowedAccesses = AccRel.unite(UndefAnything);

          // ... and must write the same value.
          // { [Domain[] -> Element[]] -> Value[] }
          isl::map Filter =
              isl::map::from_domain_and_range(AllowedAccesses.wrap(), ValSet);

          // Lookup future write that fulfills these conditions.
          // { [[Domain[] -> Element[]] -> Value[]] -> MemoryAccess[] }
          isl::union_map Filtered =
              FutureWrites.uncurry().intersect_domain(Filter.wrap());

          // Iterate through the candidates.
          Filtered.foreach_map([&, this](isl::map Map) -> isl::stat {
            MemoryAccess *OtherMA = (MemoryAccess *)Map.get_space()
                                        .get_tuple_id(isl::dim::out)
                                        .get_user();

            isl::map OtherAccRel =
                OtherMA->getLatestAccessRelation().intersect_domain(Domain);

            // The filter only guaranteed that some of OtherMA's accessed
            // elements are allowed. Verify that it only accesses allowed
            // elements. Otherwise, continue with the next candidate.
            if (!OtherAccRel.is_subset(AllowedAccesses).is_true())
              return isl::stat::ok;

            // The combined access relation.
            // { Domain[] -> Element[] }
            isl::map NewAccRel = AccRel.unite(OtherAccRel);
            simplify(NewAccRel);

            // Carry out the coalescing.
            Stmt.removeSingleMemoryAccess(MA);
            OtherMA->setNewAccessRelation(NewAccRel.copy());

            // We removed MA, OtherMA takes its role.
            MA = OtherMA;

            TotalWritesCoalesced++;
            WritesCoalesced++;

            // Don't look for more candidates.
            return isl::stat::error;
          });
        }

        // Two writes cannot be coalesced if there is another access (to some of
        // the written elements) between them. Remove all visited write accesses
        // from the list of eligible writes. Don't just remove the accessed
        // elements, but any MemoryAccess that touches any of the invalidated
        // elements.
        SmallPtrSet<MemoryAccess *, 2> TouchedAccesses;
        FutureWrites.intersect_domain(AccRelWrapped)
            .foreach_map([&TouchedAccesses](isl::map Map) -> isl::stat {
              MemoryAccess *MA = (MemoryAccess *)Map.get_space()
                                     .range()
                                     .unwrap()
                                     .get_tuple_id(isl::dim::out)
                                     .get_user();
              TouchedAccesses.insert(MA);
              return isl::stat::ok;
            });
        isl::union_map NewFutureWrites =
            isl::union_map::empty(FutureWrites.get_space());
        FutureWrites.foreach_map([&TouchedAccesses, &NewFutureWrites](
                                     isl::map FutureWrite) -> isl::stat {
          MemoryAccess *MA = (MemoryAccess *)FutureWrite.get_space()
                                 .range()
                                 .unwrap()
                                 .get_tuple_id(isl::dim::out)
                                 .get_user();
          if (!TouchedAccesses.count(MA))
            NewFutureWrites = NewFutureWrites.add_map(FutureWrite);
          return isl::stat::ok;
        });
        FutureWrites = NewFutureWrites;

        if (MA->isMustWrite() && !ValSet.is_null()) {
          // { MemoryAccess[] }
          auto AccSet =
              isl::set::universe(isl::space(S->getIslCtx(), 0, 0)
                                     .set_tuple_id(isl::dim::set, MA->getId()));

          // { Val[] -> MemoryAccess[] }
          isl::map ValAccSet = isl::map::from_domain_and_range(ValSet, AccSet);

          // { [Domain[] -> Element[]] -> [Value[] -> MemoryAccess[]] }
          isl::map AccRelValAcc =
              isl::map::from_domain_and_range(AccRelWrapped, ValAccSet.wrap());
          FutureWrites = FutureWrites.add_map(AccRelValAcc);
        }
      }
    }
  }

  /// Remove writes that just write the same value already stored in the
  /// element.
  void removeRedundantWrites() {
    // Delay actual removal to not invalidate iterators.
    SmallVector<MemoryAccess *, 8> StoresToRemove;

    for (auto &Stmt : *S) {
      for (auto *WA : Stmt) {
        if (!WA->isMustWrite())
          continue;
        if (!WA->isLatestArrayKind())
          continue;
        if (!isa<StoreInst>(WA->getAccessInstruction()) &&
            !WA->isOriginalScalarKind())
          continue;

        llvm::Value *ReadingValue = WA->tryGetValueStored();

        if (!ReadingValue)
          continue;

        auto RA = getReadAccessForValue(&Stmt, ReadingValue);
        if (!RA)
          continue;
        if (!RA->isLatestArrayKind())
          continue;

        auto WARel = WA->getLatestAccessRelation();
        WARel = WARel.intersect_domain(give(WA->getStatement()->getDomain()));
        WARel = WARel.intersect_params(give(S->getContext()));
        auto RARel = RA->getLatestAccessRelation();
        RARel = RARel.intersect_domain(give(RA->getStatement()->getDomain()));
        RARel = RARel.intersect_params(give(S->getContext()));

        if (!RARel.is_equal(WARel)) {
          PairUnequalAccRels++;
          DEBUG(dbgs() << "Not cleaning up " << WA
                       << " because of unequal access relations:\n");
          DEBUG(dbgs() << "      RA: " << RARel << "\n");
          DEBUG(dbgs() << "      WA: " << WARel << "\n");
          continue;
        }

        if (auto *Conflicting = hasWriteBetween(&Stmt, RA, WA, WARel)) {
          (void)Conflicting;
          InBetweenStore++;
          DEBUG(dbgs() << "Not cleaning up " << WA
                       << " because there is another store to the same element "
                          "between\n");
          DEBUG(Conflicting->print(dbgs()));
          continue;
        }

        StoresToRemove.push_back(WA);
      }
    }

    for (auto *WA : StoresToRemove) {
      auto Stmt = WA->getStatement();
      auto AccRel = WA->getAccessRelation();
      auto AccVal = WA->getAccessValue();

      DEBUG(dbgs() << "Cleanup of " << WA << ":\n");
      DEBUG(dbgs() << "      Scalar: " << *AccVal << "\n");
      DEBUG(dbgs() << "      AccRel: " << AccRel << "\n");
      (void)AccVal;
      (void)AccRel;

      Stmt->removeSingleMemoryAccess(WA);

      RedundantWritesRemoved++;
      TotalRedundantWritesRemoved++;
    }
  }

  /// Remove statements without side effects.
  void removeUnnecessaryStmts() {
    auto NumStmtsBefore = S->getSize();
    S->simplifySCoP(true);
    assert(NumStmtsBefore >= S->getSize());
    StmtsRemoved = NumStmtsBefore - S->getSize();
    DEBUG(dbgs() << "Removed " << StmtsRemoved << " (of " << NumStmtsBefore
                 << ") statements\n");
    TotalStmtsRemoved += StmtsRemoved;
  }

  /// Remove accesses that have an empty domain.
  void removeEmptyPartialAccesses() {
    for (ScopStmt &Stmt : *S) {
      // Defer the actual removal to not invalidate iterators.
      SmallVector<MemoryAccess *, 8> DeferredRemove;

      for (MemoryAccess *MA : Stmt) {
        if (!MA->isWrite())
          continue;

        isl::map AccRel = MA->getAccessRelation();
        if (!AccRel.is_empty().is_true())
          continue;

        DEBUG(dbgs() << "Removing " << MA
                     << " because it's a partial access that never occurs\n");
        DeferredRemove.push_back(MA);
      }

      for (MemoryAccess *MA : DeferredRemove) {
        Stmt.removeSingleMemoryAccess(MA);
        EmptyPartialAccessesRemoved++;
        TotalEmptyPartialAccessesRemoved++;
      }
    }
  }

  /// Mark all reachable instructions and access, and sweep those that are not
  /// reachable.
  void markAndSweep(LoopInfo *LI) {
    DenseSet<MemoryAccess *> UsedMA;
    DenseSet<VirtualInstruction> UsedInsts;

    // Get all reachable instructions and accesses.
    markReachable(S, LI, UsedInsts, UsedMA);

    // Remove all non-reachable accesses.
    // We need get all MemoryAccesses first, in order to not invalidate the
    // iterators when removing them.
    SmallVector<MemoryAccess *, 64> AllMAs;
    for (ScopStmt &Stmt : *S)
      AllMAs.append(Stmt.begin(), Stmt.end());

    for (MemoryAccess *MA : AllMAs) {
      if (UsedMA.count(MA))
        continue;
      DEBUG(dbgs() << "Removing " << MA << " because its value is not used\n");
      ScopStmt *Stmt = MA->getStatement();
      Stmt->removeSingleMemoryAccess(MA);

      DeadAccessesRemoved++;
      TotalDeadAccessesRemoved++;
    }

    // Remove all non-reachable instructions.
    for (ScopStmt &Stmt : *S) {
      if (!Stmt.isBlockStmt())
        continue;

      SmallVector<Instruction *, 32> AllInsts(Stmt.insts_begin(),
                                              Stmt.insts_end());
      SmallVector<Instruction *, 32> RemainInsts;

      for (Instruction *Inst : AllInsts) {
        auto It = UsedInsts.find({&Stmt, Inst});
        if (It == UsedInsts.end()) {
          DEBUG(dbgs() << "Removing "; Inst->print(dbgs());
                dbgs() << " because it is not used\n");
          DeadInstructionsRemoved++;
          TotalDeadInstructionsRemoved++;
          continue;
        }

        RemainInsts.push_back(Inst);

        // If instructions appear multiple times, keep only the first.
        UsedInsts.erase(It);
      }

      // Set the new instruction list to be only those we did not remove.
      Stmt.setInstructions(RemainInsts);
    }
  }

  /// Print simplification statistics to @p OS.
  void printStatistics(llvm::raw_ostream &OS, int Indent = 0) const {
    OS.indent(Indent) << "Statistics {\n";
    OS.indent(Indent + 4) << "Overwrites removed: " << OverwritesRemoved
                          << '\n';
    OS.indent(Indent + 4) << "Partial writes coalesced: " << WritesCoalesced
                          << "\n";
    OS.indent(Indent + 4) << "Redundant writes removed: "
                          << RedundantWritesRemoved << "\n";
    OS.indent(Indent + 4) << "Accesses with empty domains removed: "
                          << EmptyPartialAccessesRemoved << "\n";
    OS.indent(Indent + 4) << "Dead accesses removed: " << DeadAccessesRemoved
                          << '\n';
    OS.indent(Indent + 4) << "Dead instructions removed: "
                          << DeadInstructionsRemoved << '\n';
    OS.indent(Indent + 4) << "Stmts removed: " << StmtsRemoved << "\n";
    OS.indent(Indent) << "}\n";
  }

  /// Print the current state of all MemoryAccesses to @p OS.
  void printAccesses(llvm::raw_ostream &OS, int Indent = 0) const {
    OS.indent(Indent) << "After accesses {\n";
    for (auto &Stmt : *S) {
      OS.indent(Indent + 4) << Stmt.getBaseName() << "\n";
      for (auto *MA : Stmt)
        MA->print(OS);
    }
    OS.indent(Indent) << "}\n";
  }

public:
  static char ID;
  explicit Simplify() : ScopPass(ID) {}

  virtual void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequiredTransitive<ScopInfoRegionPass>();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.setPreservesAll();
  }

  virtual bool runOnScop(Scop &S) override {
    // Reset statistics of last processed SCoP.
    releaseMemory();
    assert(!isModified());

    // Prepare processing of this SCoP.
    this->S = &S;
    ScopsProcessed++;

    DEBUG(dbgs() << "Removing partial writes that never happen...\n");
    removeEmptyPartialAccesses();

    DEBUG(dbgs() << "Removing overwrites...\n");
    removeOverwrites();

    DEBUG(dbgs() << "Coalesce partial writes...\n");
    coalesceWrites();

    DEBUG(dbgs() << "Removing redundant writes...\n");
    removeRedundantWrites();

    DEBUG(dbgs() << "Cleanup unused accesses...\n");
    LoopInfo *LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    markAndSweep(LI);

    DEBUG(dbgs() << "Removing statements without side effects...\n");
    removeUnnecessaryStmts();

    if (isModified())
      ScopsModified++;
    DEBUG(dbgs() << "\nFinal Scop:\n");
    DEBUG(dbgs() << S);

    return false;
  }

  virtual void printScop(raw_ostream &OS, Scop &S) const override {
    assert(&S == this->S &&
           "Can only print analysis for the last processed SCoP");
    printStatistics(OS);

    if (!isModified()) {
      OS << "SCoP could not be simplified\n";
      return;
    }
    printAccesses(OS);
  }

  virtual void releaseMemory() override {
    S = nullptr;

    OverwritesRemoved = 0;
    WritesCoalesced = 0;
    RedundantWritesRemoved = 0;
    EmptyPartialAccessesRemoved = 0;
    DeadAccessesRemoved = 0;
    DeadInstructionsRemoved = 0;
    StmtsRemoved = 0;
  }
};

char Simplify::ID;
} // anonymous namespace

Pass *polly::createSimplifyPass() { return new Simplify(); }

INITIALIZE_PASS_BEGIN(Simplify, "polly-simplify", "Polly - Simplify", false,
                      false)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_END(Simplify, "polly-simplify", "Polly - Simplify", false,
                    false)
