#ifndef LLVM_TARGET_IA64_SUBTARGET_H
#define LLVM_TARGET_IA64_SUBTARGET_H

#include "llvm/Target/TargetSubtargetInfo.h"

#define GET_SUBTARGETINFO_HEADER
#include "IA64GenSubtargetInfo.inc"

#include <string>

namespace llvm {

  class StringRef;

  class IA64Subtarget : public IA64GenSubtargetInfo {
    bool HasLongBranch;

  public:
    /// This constructor initializes the data members to match that
    /// of the specified triple.
    IA64Subtarget(const std::string &TT, const std::string &CPU,
        const std::string &FS);

    /// ParseSubtargetFeatures - Parses features string setting specified
    /// subtarget options.  Definition of function is auto generated by tblgen.
    void ParseSubtargetFeatures(StringRef CPU, StringRef FS);
  };

} // End llvm namespace

#endif  // LLVM_TARGET_IA64_SUBTARGET_H
