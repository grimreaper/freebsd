#include "IA64FrameLowering.h"
#include "IA64InstrInfo.h"

#include "llvm/Function.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

void
IA64FrameLowering::emitPrologue(MachineFunction &MF) const
{
  llvm_unreachable(__func__);
}

void
IA64FrameLowering::emitEpilogue(MachineFunction &MF, MachineBasicBlock &MBB)
    const
{
  llvm_unreachable(__func__);
}

bool
IA64FrameLowering::hasFP(const MachineFunction &MF) const
{ 
  llvm_unreachable(__func__);
}
