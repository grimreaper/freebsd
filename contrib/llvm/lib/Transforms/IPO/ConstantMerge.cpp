//===- ConstantMerge.cpp - Merge duplicate global constants ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the interface to a pass that merges duplicate global
// constants together into a single constant that is shared.  This is useful
// because some passes (ie TraceValues) insert a lot of string constants into
// the program, regardless of whether or not an existing string is available.
//
// Algorithm: ConstantMerge is designed to build up a map of available constants
// and eliminate duplicates when it is initialized.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "constmerge"
#include "llvm/Transforms/IPO.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/Target/TargetData.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
using namespace llvm;

STATISTIC(NumMerged, "Number of global constants merged");

namespace {
  struct ConstantMerge : public ModulePass {
    static char ID; // Pass identification, replacement for typeid
    ConstantMerge() : ModulePass(ID) {
      initializeConstantMergePass(*PassRegistry::getPassRegistry());
    }

    // For this pass, process all of the globals in the module, eliminating
    // duplicate constants.
    bool runOnModule(Module &M);

    // Return true iff we can determine the alignment of this global variable.
    bool hasKnownAlignment(GlobalVariable *GV) const;

    // Return the alignment of the global, including converting the default
    // alignment to a concrete value.
    unsigned getAlignment(GlobalVariable *GV) const;

    const TargetData *TD;
  };
}

char ConstantMerge::ID = 0;
INITIALIZE_PASS(ConstantMerge, "constmerge",
                "Merge Duplicate Global Constants", false, false)

ModulePass *llvm::createConstantMergePass() { return new ConstantMerge(); }



/// Find values that are marked as llvm.used.
static void FindUsedValues(GlobalVariable *LLVMUsed,
                           SmallPtrSet<const GlobalValue*, 8> &UsedValues) {
  if (LLVMUsed == 0) return;
  ConstantArray *Inits = dyn_cast<ConstantArray>(LLVMUsed->getInitializer());
  if (Inits == 0) return;
  
  for (unsigned i = 0, e = Inits->getNumOperands(); i != e; ++i)
    if (GlobalValue *GV = 
        dyn_cast<GlobalValue>(Inits->getOperand(i)->stripPointerCasts()))
      UsedValues.insert(GV);
}

// True if A is better than B.
static bool IsBetterCannonical(const GlobalVariable &A,
                               const GlobalVariable &B) {
  if (!A.hasLocalLinkage() && B.hasLocalLinkage())
    return true;

  if (A.hasLocalLinkage() && !B.hasLocalLinkage())
    return false;

  return A.hasUnnamedAddr();
}

bool ConstantMerge::hasKnownAlignment(GlobalVariable *GV) const {
  return TD || GV->getAlignment() != 0;
}

unsigned ConstantMerge::getAlignment(GlobalVariable *GV) const {
  if (TD)
    return TD->getPreferredAlignment(GV);
  return GV->getAlignment();
}

bool ConstantMerge::runOnModule(Module &M) {
  TD = getAnalysisIfAvailable<TargetData>();

  // Find all the globals that are marked "used".  These cannot be merged.
  SmallPtrSet<const GlobalValue*, 8> UsedGlobals;
  FindUsedValues(M.getGlobalVariable("llvm.used"), UsedGlobals);
  FindUsedValues(M.getGlobalVariable("llvm.compiler.used"), UsedGlobals);
  
  // Map unique <constants, has-unknown-alignment> pairs to globals.  We don't
  // want to merge globals of unknown alignment with those of explicit
  // alignment.  If we have TargetData, we always know the alignment.
  DenseMap<PointerIntPair<Constant*, 1, bool>, GlobalVariable*> CMap;

  // Replacements - This vector contains a list of replacements to perform.
  SmallVector<std::pair<GlobalVariable*, GlobalVariable*>, 32> Replacements;

  bool MadeChange = false;

  // Iterate constant merging while we are still making progress.  Merging two
  // constants together may allow us to merge other constants together if the
  // second level constants have initializers which point to the globals that
  // were just merged.
  while (1) {

    // First: Find the canonical constants others will be merged with.
    for (Module::global_iterator GVI = M.global_begin(), E = M.global_end();
         GVI != E; ) {
      GlobalVariable *GV = GVI++;

      // If this GV is dead, remove it.
      GV->removeDeadConstantUsers();
      if (GV->use_empty() && GV->hasLocalLinkage()) {
        GV->eraseFromParent();
        continue;
      }

      // Only process constants with initializers in the default address space.
      if (!GV->isConstant() || !GV->hasDefinitiveInitializer() ||
          GV->getType()->getAddressSpace() != 0 || GV->hasSection() ||
          // Don't touch values marked with attribute(used).
          UsedGlobals.count(GV))
        continue;

      // This transformation is legal for weak ODR globals in the sense it
      // doesn't change semantics, but we really don't want to perform it
      // anyway; it's likely to pessimize code generation, and some tools
      // (like the Darwin linker in cases involving CFString) don't expect it.
      if (GV->isWeakForLinker())
        continue;

      Constant *Init = GV->getInitializer();

      // Check to see if the initializer is already known.
      PointerIntPair<Constant*, 1, bool> Pair(Init, hasKnownAlignment(GV));
      GlobalVariable *&Slot = CMap[Pair];

      // If this is the first constant we find or if the old one is local,
      // replace with the current one. If the current is externally visible
      // it cannot be replace, but can be the canonical constant we merge with.
      if (Slot == 0 || IsBetterCannonical(*GV, *Slot))
        Slot = GV;
    }

    // Second: identify all globals that can be merged together, filling in
    // the Replacements vector.  We cannot do the replacement in this pass
    // because doing so may cause initializers of other globals to be rewritten,
    // invalidating the Constant* pointers in CMap.
    for (Module::global_iterator GVI = M.global_begin(), E = M.global_end();
         GVI != E; ) {
      GlobalVariable *GV = GVI++;

      // Only process constants with initializers in the default address space.
      if (!GV->isConstant() || !GV->hasDefinitiveInitializer() ||
          GV->getType()->getAddressSpace() != 0 || GV->hasSection() ||
          // Don't touch values marked with attribute(used).
          UsedGlobals.count(GV))
        continue;

      // We can only replace constant with local linkage.
      if (!GV->hasLocalLinkage())
        continue;

      Constant *Init = GV->getInitializer();

      // Check to see if the initializer is already known.
      PointerIntPair<Constant*, 1, bool> Pair(Init, hasKnownAlignment(GV));
      GlobalVariable *Slot = CMap[Pair];

      if (!Slot || Slot == GV)
        continue;

      if (!Slot->hasUnnamedAddr() && !GV->hasUnnamedAddr())
        continue;

      if (!GV->hasUnnamedAddr())
        Slot->setUnnamedAddr(false);

      // Make all uses of the duplicate constant use the canonical version.
      Replacements.push_back(std::make_pair(GV, Slot));
    }

    if (Replacements.empty())
      return MadeChange;
    CMap.clear();

    // Now that we have figured out which replacements must be made, do them all
    // now.  This avoid invalidating the pointers in CMap, which are unneeded
    // now.
    for (unsigned i = 0, e = Replacements.size(); i != e; ++i) {
      // Bump the alignment if necessary.
      if (Replacements[i].first->getAlignment() ||
          Replacements[i].second->getAlignment()) {
        Replacements[i].second->setAlignment(std::max(
            Replacements[i].first->getAlignment(),
            Replacements[i].second->getAlignment()));
      }

      // Eliminate any uses of the dead global.
      Replacements[i].first->replaceAllUsesWith(Replacements[i].second);

      // Delete the global value from the module.
      assert(Replacements[i].first->hasLocalLinkage() &&
             "Refusing to delete an externally visible global variable.");
      Replacements[i].first->eraseFromParent();
    }

    NumMerged += Replacements.size();
    Replacements.clear();
  }
}
