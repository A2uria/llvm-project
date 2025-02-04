//===- StaticDataSplitter.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The pass uses branch profile data to assign hotness based section qualifiers
// for the following types of static data:
// - Jump tables
// - Other module-internal data
// - Constant pools (TODO)
//
// For the original RFC of this pass please see
// https://discourse.llvm.org/t/rfc-profile-guided-static-data-partitioning/83744

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/CodeGen/MBFIWrapper.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineBranchProbabilityInfo.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Target/TargetLoweringObjectFile.h"

using namespace llvm;

#define DEBUG_TYPE "static-data-splitter"

STATISTIC(NumHotJumpTables, "Number of hot jump tables seen");
STATISTIC(NumColdJumpTables, "Number of cold jump tables seen");
STATISTIC(NumUnknownJumpTables,
          "Number of jump tables with unknown hotness. Option "
          "-static-data-default-hotness specifies the hotness.");

class StaticDataSplitter : public MachineFunctionPass {
  const MachineBranchProbabilityInfo *MBPI = nullptr;
  const MachineBlockFrequencyInfo *MBFI = nullptr;
  const ProfileSummaryInfo *PSI = nullptr;

  void updateStats(bool ProfileAvailable, const MachineJumpTableInfo *MJTI);
  void updateJumpTableStats(bool ProfileAvailable,
                            const MachineJumpTableInfo &MJTI);

  // Use profiles to partition static data.
  bool partitionStaticDataWithProfiles(MachineFunction &MF);

  // If the global value is a local linkage global variable, return it.
  // Otherwise, return nullptr.
  const GlobalVariable *getLocalLinkageGlobalVariable(const GlobalValue *GV);

  // Returns true if the global variable is in one of {.rodata, .bss, .data,
  // .data.rel.ro} sections
  bool inStaticDataSection(const GlobalVariable *GV, const TargetMachine &TM);

  // Iterate all global variables in the module and update the section prefix
  // of the module-internal data.
  void updateGlobalVariableSectionPrefix(MachineFunction &MF);

  // Accummulated data profile count across machine functions in the module.
  DenseMap<const GlobalVariable *, APInt> DataProfileCounts;

public:
  static char ID;

  StaticDataSplitter() : MachineFunctionPass(ID) {
    initializeStaticDataSplitterPass(*PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override { return "Static Data Splitter"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    MachineFunctionPass::getAnalysisUsage(AU);
    AU.addRequired<MachineBranchProbabilityInfoWrapperPass>();
    AU.addRequired<MachineBlockFrequencyInfoWrapperPass>();
    AU.addRequired<ProfileSummaryInfoWrapperPass>();
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
};

bool StaticDataSplitter::runOnMachineFunction(MachineFunction &MF) {
  MBPI = &getAnalysis<MachineBranchProbabilityInfoWrapperPass>().getMBPI();
  MBFI = &getAnalysis<MachineBlockFrequencyInfoWrapperPass>().getMBFI();
  PSI = &getAnalysis<ProfileSummaryInfoWrapperPass>().getPSI();

  const bool ProfileAvailable = PSI && PSI->hasProfileSummary() && MBFI &&
                                MF.getFunction().hasProfileData();
  bool Changed = false;

  if (ProfileAvailable)
    Changed |= partitionStaticDataWithProfiles(MF);

  updateGlobalVariableSectionPrefix(MF);
  updateStats(ProfileAvailable, MF.getJumpTableInfo());
  return Changed;
}

bool StaticDataSplitter::partitionStaticDataWithProfiles(MachineFunction &MF) {
  int NumChangedJumpTables = 0;

  const TargetMachine &TM = MF.getTarget();
  MachineJumpTableInfo *MJTI = MF.getJumpTableInfo();

  // Jump table could be used by either terminating instructions or
  // non-terminating ones, so we walk all instructions and use
  // `MachineOperand::isJTI()` to identify jump table operands.
  // Similarly, `MachineOperand::isCPI()` can identify constant pool usages
  // in the same loop.
  for (const auto &MBB : MF) {
    for (const MachineInstr &I : MBB) {
      for (const MachineOperand &Op : I.operands()) {
        std::optional<uint64_t> Count = std::nullopt;
        if (!Op.isJTI() && !Op.isGlobal())
          continue;

        Count = MBFI->getBlockProfileCount(&MBB);

        if (Op.isJTI()) {
          assert(MJTI != nullptr && "Jump table info is not available.");
          const int JTI = Op.getIndex();
          // This is not a source block of jump table.
          if (JTI == -1)
            continue;

          auto Hotness = MachineFunctionDataHotness::Hot;

          // Hotness is based on source basic block hotness.
          // TODO: PSI APIs are about instruction hotness. Introduce API for
          // data access hotness.
          if (Count && PSI->isColdCount(*Count))
            Hotness = MachineFunctionDataHotness::Cold;

          if (MJTI->updateJumpTableEntryHotness(JTI, Hotness))
            ++NumChangedJumpTables;
        } else if (Op.isGlobal()) {
          // Find global variables with local linkage
          const GlobalVariable *GV =
              getLocalLinkageGlobalVariable(Op.getGlobal());
          if (!GV || !inStaticDataSection(GV, TM))
            continue;

          // Acccumulate data profile count across machine function
          // instructions.
          // TODO: Analyze global variable's initializers.
          if (Count) {
            auto [It, Inserted] =
                DataProfileCounts.try_emplace(GV, APInt(128, 0));
            It->second += *Count;
          }
        }
      }
    }
  }
  return NumChangedJumpTables > 0;
}

void StaticDataSplitter::updateJumpTableStats(
    bool ProfileAvailable, const MachineJumpTableInfo &MJTI) {
  if (!ProfileAvailable) {
    NumUnknownJumpTables += MJTI.getJumpTables().size();
    return;
  }

  for (size_t JTI = 0; JTI < MJTI.getJumpTables().size(); JTI++) {
    auto Hotness = MJTI.getJumpTables()[JTI].Hotness;
    if (Hotness == MachineFunctionDataHotness::Hot) {
      ++NumHotJumpTables;
    } else {
      assert(Hotness == MachineFunctionDataHotness::Cold &&
             "A jump table is either hot or cold when profile information is "
             "available.");
      ++NumColdJumpTables;
    }
  }
}

void StaticDataSplitter::updateStats(bool ProfileAvailable,
                                     const MachineJumpTableInfo *MJTI) {
  if (!AreStatisticsEnabled())
    return;

  if (MJTI)
    updateJumpTableStats(ProfileAvailable, *MJTI);
}

const GlobalVariable *
StaticDataSplitter::getLocalLinkageGlobalVariable(const GlobalValue *GV) {
  if (!GV || GV->isDeclarationForLinker())
    return nullptr;

  return GV->hasLocalLinkage() ? dyn_cast<GlobalVariable>(GV) : nullptr;
}

bool StaticDataSplitter::inStaticDataSection(const GlobalVariable *GV,
                                             const TargetMachine &TM) {
  assert(GV && "Caller guaranteed");

  // Skip LLVM reserved symbols.
  if (GV->getName().starts_with("llvm."))
    return false;

  SectionKind Kind = TargetLoweringObjectFile::getKindForGlobal(GV, TM);
  return Kind.isData() || Kind.isReadOnly() || Kind.isReadOnlyWithRel() ||
         Kind.isBSS();
}

void StaticDataSplitter::updateGlobalVariableSectionPrefix(
    MachineFunction &MF) {
  for (GlobalVariable &GV : MF.getFunction().getParent()->globals()) {
    if (GV.isDeclarationForLinker())
      continue;
    // DataProfileCounts accumulates data profile count across all machine
    // function instructions, and it can't model the indirect accesses through
    // other global variables' initializers.
    // TODO: Analyze the users of module-internal global variables and see
    // through the users' initializers. Do not place a global variable into
    // unlikely section if any of its users are potentially hot.
    auto Iter = DataProfileCounts.find(&GV);
    if (Iter == DataProfileCounts.end())
      continue;

    // StaticDataSplitter is made a machine function pass rather than a module
    // pass because (Lazy)MachineBlockFrequencyInfo is a machine-function
    // analysis pass and cannot be used for a legacy module pass.
    // As a result, we use `DataProfileCounts` to accumulate data
    // profile count across machine functions and update global variable section
    // prefix once per machine function.
    // FIXME: Make StaticDataSplitter a module pass under new pass manager
    // framework, and set global variable section prefix once per module after
    // analyzing all machine functions.
    if (PSI->isColdCount(Iter->second.getZExtValue())) {
      GV.updateSectionPrefix("unlikely", std::make_optional(StringRef("hot")));
    } else if (PSI->isHotCount(Iter->second.getZExtValue())) {
      GV.updateSectionPrefix("hot");
    }
  }
}

char StaticDataSplitter::ID = 0;

INITIALIZE_PASS_BEGIN(StaticDataSplitter, DEBUG_TYPE, "Split static data",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(MachineBranchProbabilityInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineBlockFrequencyInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ProfileSummaryInfoWrapperPass)
INITIALIZE_PASS_END(StaticDataSplitter, DEBUG_TYPE, "Split static data", false,
                    false)

MachineFunctionPass *llvm::createStaticDataSplitterPass() {
  return new StaticDataSplitter();
}
