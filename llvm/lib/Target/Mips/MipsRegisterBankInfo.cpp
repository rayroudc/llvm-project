//===- MipsRegisterBankInfo.cpp ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// This file implements the targeting of the RegisterBankInfo class for Mips.
/// \todo This should be generated by TableGen.
//===----------------------------------------------------------------------===//

#include "MipsRegisterBankInfo.h"
#include "MipsInstrInfo.h"
#include "llvm/CodeGen/GlobalISel/GISelChangeObserver.h"
#include "llvm/CodeGen/GlobalISel/LegalizationArtifactCombiner.h"
#include "llvm/CodeGen/GlobalISel/LegalizerHelper.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"

#define GET_TARGET_REGBANK_IMPL

#include "MipsGenRegisterBank.inc"

namespace llvm {
namespace Mips {
enum PartialMappingIdx {
  PMI_GPR,
  PMI_SPR,
  PMI_DPR,
  PMI_Min = PMI_GPR,
};

RegisterBankInfo::PartialMapping PartMappings[]{
    {0, 32, GPRBRegBank},
    {0, 32, FPRBRegBank},
    {0, 64, FPRBRegBank}
};

enum ValueMappingIdx {
    InvalidIdx = 0,
    GPRIdx = 1,
    SPRIdx = 4,
    DPRIdx = 7
};

RegisterBankInfo::ValueMapping ValueMappings[] = {
    // invalid
    {nullptr, 0},
    // up to 3 operands in GPRs
    {&PartMappings[PMI_GPR - PMI_Min], 1},
    {&PartMappings[PMI_GPR - PMI_Min], 1},
    {&PartMappings[PMI_GPR - PMI_Min], 1},
    // up to 3 operands in FPRs - single precission
    {&PartMappings[PMI_SPR - PMI_Min], 1},
    {&PartMappings[PMI_SPR - PMI_Min], 1},
    {&PartMappings[PMI_SPR - PMI_Min], 1},
    // up to 3 operands in FPRs - double precission
    {&PartMappings[PMI_DPR - PMI_Min], 1},
    {&PartMappings[PMI_DPR - PMI_Min], 1},
    {&PartMappings[PMI_DPR - PMI_Min], 1}
};

} // end namespace Mips
} // end namespace llvm

using namespace llvm;

MipsRegisterBankInfo::MipsRegisterBankInfo(const TargetRegisterInfo &TRI)
    : MipsGenRegisterBankInfo() {}

const RegisterBank &MipsRegisterBankInfo::getRegBankFromRegClass(
    const TargetRegisterClass &RC) const {
  using namespace Mips;

  switch (RC.getID()) {
  case Mips::GPR32RegClassID:
  case Mips::CPU16Regs_and_GPRMM16ZeroRegClassID:
  case Mips::GPRMM16MovePPairFirstRegClassID:
  case Mips::CPU16Regs_and_GPRMM16MovePPairSecondRegClassID:
  case Mips::GPRMM16MoveP_and_CPU16Regs_and_GPRMM16ZeroRegClassID:
  case Mips::GPRMM16MovePPairFirst_and_GPRMM16MovePPairSecondRegClassID:
  case Mips::SP32RegClassID:
  case Mips::GP32RegClassID:
    return getRegBank(Mips::GPRBRegBankID);
  case Mips::FGRCCRegClassID:
  case Mips::FGR32RegClassID:
  case Mips::FGR64RegClassID:
  case Mips::AFGR64RegClassID:
    return getRegBank(Mips::FPRBRegBankID);
  default:
    llvm_unreachable("Register class not supported");
  }
}

// Instructions where all register operands are floating point.
static bool isFloatingPointOpcode(unsigned Opc) {
  switch (Opc) {
  case TargetOpcode::G_FCONSTANT:
  case TargetOpcode::G_FADD:
  case TargetOpcode::G_FSUB:
  case TargetOpcode::G_FMUL:
  case TargetOpcode::G_FDIV:
  case TargetOpcode::G_FABS:
  case TargetOpcode::G_FSQRT:
  case TargetOpcode::G_FCEIL:
  case TargetOpcode::G_FFLOOR:
  case TargetOpcode::G_FPEXT:
  case TargetOpcode::G_FPTRUNC:
    return true;
  default:
    return false;
  }
}

// Instructions where use operands are floating point registers.
// Def operands are general purpose.
static bool isFloatingPointOpcodeUse(unsigned Opc) {
  switch (Opc) {
  case TargetOpcode::G_FPTOSI:
  case TargetOpcode::G_FPTOUI:
  case TargetOpcode::G_FCMP:
  case Mips::MFC1:
  case Mips::ExtractElementF64:
  case Mips::ExtractElementF64_64:
    return true;
  default:
    return isFloatingPointOpcode(Opc);
  }
}

// Instructions where def operands are floating point registers.
// Use operands are general purpose.
static bool isFloatingPointOpcodeDef(unsigned Opc) {
  switch (Opc) {
  case TargetOpcode::G_SITOFP:
  case TargetOpcode::G_UITOFP:
  case Mips::MTC1:
  case Mips::BuildPairF64:
  case Mips::BuildPairF64_64:
    return true;
  default:
    return isFloatingPointOpcode(Opc);
  }
}

static bool isAmbiguous(unsigned Opc) {
  switch (Opc) {
  case TargetOpcode::G_LOAD:
  case TargetOpcode::G_STORE:
  case TargetOpcode::G_PHI:
  case TargetOpcode::G_SELECT:
  case TargetOpcode::G_IMPLICIT_DEF:
    return true;
  default:
    return false;
  }
}

void MipsRegisterBankInfo::AmbiguousRegDefUseContainer::addDefUses(
    Register Reg, const MachineRegisterInfo &MRI) {
  assert(!MRI.getType(Reg).isPointer() &&
         "Pointers are gprb, they should not be considered as ambiguous.\n");
  for (MachineInstr &UseMI : MRI.use_instructions(Reg)) {
    MachineInstr *NonCopyInstr = skipCopiesOutgoing(&UseMI);
    // Copy with many uses.
    if (NonCopyInstr->getOpcode() == TargetOpcode::COPY &&
        !Register::isPhysicalRegister(NonCopyInstr->getOperand(0).getReg()))
      addDefUses(NonCopyInstr->getOperand(0).getReg(), MRI);
    else
      DefUses.push_back(skipCopiesOutgoing(&UseMI));
  }
}

void MipsRegisterBankInfo::AmbiguousRegDefUseContainer::addUseDef(
    Register Reg, const MachineRegisterInfo &MRI) {
  assert(!MRI.getType(Reg).isPointer() &&
         "Pointers are gprb, they should not be considered as ambiguous.\n");
  MachineInstr *DefMI = MRI.getVRegDef(Reg);
  UseDefs.push_back(skipCopiesIncoming(DefMI));
}

MachineInstr *
MipsRegisterBankInfo::AmbiguousRegDefUseContainer::skipCopiesOutgoing(
    MachineInstr *MI) const {
  const MachineFunction &MF = *MI->getParent()->getParent();
  const MachineRegisterInfo &MRI = MF.getRegInfo();
  MachineInstr *Ret = MI;
  while (Ret->getOpcode() == TargetOpcode::COPY &&
         !Register::isPhysicalRegister(Ret->getOperand(0).getReg()) &&
         MRI.hasOneUse(Ret->getOperand(0).getReg())) {
    Ret = &(*MRI.use_instr_begin(Ret->getOperand(0).getReg()));
  }
  return Ret;
}

MachineInstr *
MipsRegisterBankInfo::AmbiguousRegDefUseContainer::skipCopiesIncoming(
    MachineInstr *MI) const {
  const MachineFunction &MF = *MI->getParent()->getParent();
  const MachineRegisterInfo &MRI = MF.getRegInfo();
  MachineInstr *Ret = MI;
  while (Ret->getOpcode() == TargetOpcode::COPY &&
         !Register::isPhysicalRegister(Ret->getOperand(1).getReg()))
    Ret = MRI.getVRegDef(Ret->getOperand(1).getReg());
  return Ret;
}

MipsRegisterBankInfo::AmbiguousRegDefUseContainer::AmbiguousRegDefUseContainer(
    const MachineInstr *MI) {
  assert(isAmbiguous(MI->getOpcode()) &&
         "Not implemented for non Ambiguous opcode.\n");

  const MachineRegisterInfo &MRI = MI->getMF()->getRegInfo();

  if (MI->getOpcode() == TargetOpcode::G_LOAD)
    addDefUses(MI->getOperand(0).getReg(), MRI);

  if (MI->getOpcode() == TargetOpcode::G_STORE)
    addUseDef(MI->getOperand(0).getReg(), MRI);

  if (MI->getOpcode() == TargetOpcode::G_PHI) {
    addDefUses(MI->getOperand(0).getReg(), MRI);

    for (unsigned i = 1; i < MI->getNumOperands(); i += 2)
      addUseDef(MI->getOperand(i).getReg(), MRI);
  }

  if (MI->getOpcode() == TargetOpcode::G_SELECT) {
    addDefUses(MI->getOperand(0).getReg(), MRI);

    addUseDef(MI->getOperand(2).getReg(), MRI);
    addUseDef(MI->getOperand(3).getReg(), MRI);
  }

  if (MI->getOpcode() == TargetOpcode::G_IMPLICIT_DEF)
    addDefUses(MI->getOperand(0).getReg(), MRI);
}

bool MipsRegisterBankInfo::TypeInfoForMF::visit(
    const MachineInstr *MI, const MachineInstr *WaitingForTypeOfMI) {
  assert(isAmbiguous(MI->getOpcode()) && "Visiting non-Ambiguous opcode.\n");
  if (wasVisited(MI))
    return true; // InstType has already been determined for MI.

  startVisit(MI);
  AmbiguousRegDefUseContainer DefUseContainer(MI);

  // Visit instructions where MI's DEF operands are USED.
  if (visitAdjacentInstrs(MI, DefUseContainer.getDefUses(), true))
    return true;

  // Visit instructions that DEFINE MI's USE operands.
  if (visitAdjacentInstrs(MI, DefUseContainer.getUseDefs(), false))
    return true;

  // All MI's adjacent instructions, are ambiguous.
  if (!WaitingForTypeOfMI) {
    // This is chain of ambiguous instructions.
    setTypes(MI, InstType::Ambiguous);
    return true;
  }
  // Excluding WaitingForTypeOfMI, MI is either connected to chains of ambiguous
  // instructions or has no other adjacent instructions. Anyway InstType could
  // not be determined. There could be unexplored path from some of
  // WaitingForTypeOfMI's adjacent instructions to an instruction with only one
  // mapping available.
  // We are done with this branch, add MI to WaitingForTypeOfMI's WaitingQueue,
  // this way when WaitingForTypeOfMI figures out its InstType same InstType
  // will be assigned to all instructions in this branch.
  addToWaitingQueue(WaitingForTypeOfMI, MI);
  return false;
}

bool MipsRegisterBankInfo::TypeInfoForMF::visitAdjacentInstrs(
    const MachineInstr *MI, SmallVectorImpl<MachineInstr *> &AdjacentInstrs,
    bool isDefUse) {
  while (!AdjacentInstrs.empty()) {
    MachineInstr *AdjMI = AdjacentInstrs.pop_back_val();

    if (isDefUse ? isFloatingPointOpcodeUse(AdjMI->getOpcode())
                 : isFloatingPointOpcodeDef(AdjMI->getOpcode())) {
      setTypes(MI, InstType::FloatingPoint);
      return true;
    }

    // Determine InstType from register bank of phys register that is
    // 'isDefUse ? def : use' of this copy.
    if (AdjMI->getOpcode() == TargetOpcode::COPY) {
      setTypesAccordingToPhysicalRegister(MI, AdjMI, isDefUse ? 0 : 1);
      return true;
    }

    // Defaults to integer instruction. Includes G_MERGE_VALUES and
    // G_UNMERGE_VALUES.
    if (!isAmbiguous(AdjMI->getOpcode())) {
      setTypes(MI, InstType::Integer);
      return true;
    }

    // When AdjMI was visited first, MI has to continue to explore remaining
    // adjacent instructions and determine InstType without visiting AdjMI.
    if (!wasVisited(AdjMI) ||
        getRecordedTypeForInstr(AdjMI) != InstType::NotDetermined) {
      if (visit(AdjMI, MI)) {
        // InstType is successfully determined and is same as for AdjMI.
        setTypes(MI, getRecordedTypeForInstr(AdjMI));
        return true;
      }
    }
  }
  return false;
}

void MipsRegisterBankInfo::TypeInfoForMF::setTypes(const MachineInstr *MI,
                                                   InstType InstTy) {
  changeRecordedTypeForInstr(MI, InstTy);
  for (const MachineInstr *WaitingInstr : getWaitingQueueFor(MI)) {
    setTypes(WaitingInstr, InstTy);
  }
}

void MipsRegisterBankInfo::TypeInfoForMF::setTypesAccordingToPhysicalRegister(
    const MachineInstr *MI, const MachineInstr *CopyInst, unsigned Op) {
  assert((Register::isPhysicalRegister(CopyInst->getOperand(Op).getReg())) &&
         "Copies of non physical registers should not be considered here.\n");

  const MachineFunction &MF = *CopyInst->getMF();
  const MachineRegisterInfo &MRI = MF.getRegInfo();
  const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();
  const RegisterBankInfo &RBI =
      *CopyInst->getMF()->getSubtarget().getRegBankInfo();
  const RegisterBank *Bank =
      RBI.getRegBank(CopyInst->getOperand(Op).getReg(), MRI, TRI);

  if (Bank == &Mips::FPRBRegBank)
    setTypes(MI, InstType::FloatingPoint);
  else if (Bank == &Mips::GPRBRegBank)
    setTypes(MI, InstType::Integer);
  else
    llvm_unreachable("Unsupported register bank.\n");
}

MipsRegisterBankInfo::InstType
MipsRegisterBankInfo::TypeInfoForMF::determineInstType(const MachineInstr *MI) {
  visit(MI, nullptr);
  return getRecordedTypeForInstr(MI);
}

void MipsRegisterBankInfo::TypeInfoForMF::cleanupIfNewFunction(
    llvm::StringRef FunctionName) {
  if (MFName != FunctionName) {
    MFName = FunctionName;
    WaitingQueues.clear();
    Types.clear();
  }
}

static const MipsRegisterBankInfo::ValueMapping *getFprbMapping(unsigned Size) {
  return Size == 32 ? &Mips::ValueMappings[Mips::SPRIdx]
                    : &Mips::ValueMappings[Mips::DPRIdx];
}

static const unsigned CustomMappingID = 1;

// Only 64 bit mapping is available in fprb and will be marked as custom, i.e.
// will be split into two 32 bit registers in gprb.
static const MipsRegisterBankInfo::ValueMapping *
getGprbOrCustomMapping(unsigned Size, unsigned &MappingID) {
  if (Size == 32)
    return &Mips::ValueMappings[Mips::GPRIdx];

  MappingID = CustomMappingID;
  return &Mips::ValueMappings[Mips::DPRIdx];
}

const RegisterBankInfo::InstructionMapping &
MipsRegisterBankInfo::getInstrMapping(const MachineInstr &MI) const {

  static TypeInfoForMF TI;

  // Reset TI internal data when MF changes.
  TI.cleanupIfNewFunction(MI.getMF()->getName());

  unsigned Opc = MI.getOpcode();
  const MachineFunction &MF = *MI.getParent()->getParent();
  const MachineRegisterInfo &MRI = MF.getRegInfo();

  if (MI.getOpcode() != TargetOpcode::G_PHI) {
    const RegisterBankInfo::InstructionMapping &Mapping =
        getInstrMappingImpl(MI);
    if (Mapping.isValid())
      return Mapping;
  }

  using namespace TargetOpcode;

  unsigned NumOperands = MI.getNumOperands();
  const ValueMapping *OperandsMapping = &Mips::ValueMappings[Mips::GPRIdx];
  unsigned MappingID = DefaultMappingID;

  // Check if LLT sizes match sizes of available register banks.
  for (const MachineOperand &Op : MI.operands()) {
    if (Op.isReg()) {
      LLT RegTy = MRI.getType(Op.getReg());

      if (RegTy.isScalar() &&
          (RegTy.getSizeInBits() != 32 && RegTy.getSizeInBits() != 64))
        return getInvalidInstructionMapping();
    }
  }

  const LLT Op0Ty = MRI.getType(MI.getOperand(0).getReg());
  unsigned Op0Size = Op0Ty.getSizeInBits();
  InstType InstTy = InstType::Integer;

  switch (Opc) {
  case G_TRUNC:
  case G_ADD:
  case G_SUB:
  case G_MUL:
  case G_UMULH:
  case G_ZEXTLOAD:
  case G_SEXTLOAD:
  case G_GEP:
  case G_INTTOPTR:
  case G_PTRTOINT:
  case G_AND:
  case G_OR:
  case G_XOR:
  case G_SHL:
  case G_ASHR:
  case G_LSHR:
  case G_SDIV:
  case G_UDIV:
  case G_SREM:
  case G_UREM:
  case G_BRINDIRECT:
  case G_VASTART:
    OperandsMapping = &Mips::ValueMappings[Mips::GPRIdx];
    break;
  case G_STORE:
  case G_LOAD:
    if (!Op0Ty.isPointer())
      InstTy = TI.determineInstType(&MI);

    if (InstTy == InstType::FloatingPoint ||
        (Op0Size == 64 && InstTy == InstType::Ambiguous))
      OperandsMapping = getOperandsMapping(
          {getFprbMapping(Op0Size), &Mips::ValueMappings[Mips::GPRIdx]});
    else
      OperandsMapping =
          getOperandsMapping({getGprbOrCustomMapping(Op0Size, MappingID),
                              &Mips::ValueMappings[Mips::GPRIdx]});

    break;
  case G_PHI:
    if (!Op0Ty.isPointer())
      InstTy = TI.determineInstType(&MI);

    // PHI is copylike and should have one regbank in mapping for def register.
    if (InstTy == InstType::Integer && Op0Size == 64) {
      OperandsMapping =
          getOperandsMapping({&Mips::ValueMappings[Mips::DPRIdx]});
      return getInstructionMapping(CustomMappingID, /*Cost=*/1, OperandsMapping,
                                   /*NumOperands=*/1);
    }
    // Use default handling for PHI, i.e. set reg bank of def operand to match
    // register banks of use operands.
    return getInstrMappingImpl(MI);
  case G_SELECT: {
    if (!Op0Ty.isPointer())
      InstTy = TI.determineInstType(&MI);

    if (InstTy == InstType::FloatingPoint ||
        (Op0Size == 64 && InstTy == InstType::Ambiguous)) {
      const RegisterBankInfo::ValueMapping *Bank = getFprbMapping(Op0Size);
      OperandsMapping = getOperandsMapping(
          {Bank, &Mips::ValueMappings[Mips::GPRIdx], Bank, Bank});
      break;
    } else {
      const RegisterBankInfo::ValueMapping *Bank =
          getGprbOrCustomMapping(Op0Size, MappingID);
      OperandsMapping = getOperandsMapping(
          {Bank, &Mips::ValueMappings[Mips::GPRIdx], Bank, Bank});
    }
    break;
  }
  case G_IMPLICIT_DEF:
    if (!Op0Ty.isPointer())
      InstTy = TI.determineInstType(&MI);

    if (InstTy == InstType::FloatingPoint)
      OperandsMapping = getFprbMapping(Op0Size);
    else
      OperandsMapping = getGprbOrCustomMapping(Op0Size, MappingID);

    break;
  case G_UNMERGE_VALUES:
    OperandsMapping = getOperandsMapping({&Mips::ValueMappings[Mips::GPRIdx],
                                          &Mips::ValueMappings[Mips::GPRIdx],
                                          &Mips::ValueMappings[Mips::DPRIdx]});
    MappingID = CustomMappingID;
    break;
  case G_MERGE_VALUES:
    OperandsMapping = getOperandsMapping({&Mips::ValueMappings[Mips::DPRIdx],
                                          &Mips::ValueMappings[Mips::GPRIdx],
                                          &Mips::ValueMappings[Mips::GPRIdx]});
    MappingID = CustomMappingID;
    break;
  case G_FADD:
  case G_FSUB:
  case G_FMUL:
  case G_FDIV:
  case G_FABS:
  case G_FSQRT:
    OperandsMapping = getFprbMapping(Op0Size);
    break;
  case G_FCONSTANT:
    OperandsMapping = getOperandsMapping({getFprbMapping(Op0Size), nullptr});
    break;
  case G_FCMP: {
    unsigned Op2Size = MRI.getType(MI.getOperand(2).getReg()).getSizeInBits();
    OperandsMapping =
        getOperandsMapping({&Mips::ValueMappings[Mips::GPRIdx], nullptr,
                            getFprbMapping(Op2Size), getFprbMapping(Op2Size)});
    break;
  }
  case G_FPEXT:
    OperandsMapping = getOperandsMapping({&Mips::ValueMappings[Mips::DPRIdx],
                                          &Mips::ValueMappings[Mips::SPRIdx]});
    break;
  case G_FPTRUNC:
    OperandsMapping = getOperandsMapping({&Mips::ValueMappings[Mips::SPRIdx],
                                          &Mips::ValueMappings[Mips::DPRIdx]});
    break;
  case G_FPTOSI: {
    assert((Op0Size == 32) && "Unsupported integer size");
    unsigned SizeFP = MRI.getType(MI.getOperand(1).getReg()).getSizeInBits();
    OperandsMapping = getOperandsMapping(
        {&Mips::ValueMappings[Mips::GPRIdx], getFprbMapping(SizeFP)});
    break;
  }
  case G_SITOFP:
    assert((MRI.getType(MI.getOperand(1).getReg()).getSizeInBits() == 32) &&
           "Unsupported integer size");
    OperandsMapping = getOperandsMapping(
        {getFprbMapping(Op0Size), &Mips::ValueMappings[Mips::GPRIdx]});
    break;
  case G_CONSTANT:
  case G_FRAME_INDEX:
  case G_GLOBAL_VALUE:
  case G_JUMP_TABLE:
  case G_BRCOND:
    OperandsMapping =
        getOperandsMapping({&Mips::ValueMappings[Mips::GPRIdx], nullptr});
    break;
  case G_BRJT:
    OperandsMapping =
        getOperandsMapping({&Mips::ValueMappings[Mips::GPRIdx], nullptr,
                            &Mips::ValueMappings[Mips::GPRIdx]});
    break;
  case G_ICMP:
    OperandsMapping =
        getOperandsMapping({&Mips::ValueMappings[Mips::GPRIdx], nullptr,
                            &Mips::ValueMappings[Mips::GPRIdx],
                            &Mips::ValueMappings[Mips::GPRIdx]});
    break;
  default:
    return getInvalidInstructionMapping();
  }

  return getInstructionMapping(MappingID, /*Cost=*/1, OperandsMapping,
                               NumOperands);
}

using InstListTy = GISelWorkList<4>;
namespace {
class InstManager : public GISelChangeObserver {
  InstListTy &InstList;

public:
  InstManager(InstListTy &Insts) : InstList(Insts) {}

  void createdInstr(MachineInstr &MI) override { InstList.insert(&MI); }
  void erasingInstr(MachineInstr &MI) override {}
  void changingInstr(MachineInstr &MI) override {}
  void changedInstr(MachineInstr &MI) override {}
};
} // end anonymous namespace

void MipsRegisterBankInfo::setRegBank(MachineInstr &MI,
                                      MachineRegisterInfo &MRI) const {
  Register Dest = MI.getOperand(0).getReg();
  switch (MI.getOpcode()) {
  case TargetOpcode::G_STORE:
    // No def operands, skip this instruction.
    break;
  case TargetOpcode::G_CONSTANT:
  case TargetOpcode::G_LOAD:
  case TargetOpcode::G_SELECT:
  case TargetOpcode::G_PHI:
  case TargetOpcode::G_IMPLICIT_DEF: {
    assert(MRI.getType(Dest) == LLT::scalar(32) && "Unexpected operand type.");
    MRI.setRegBank(Dest, getRegBank(Mips::GPRBRegBankID));
    break;
  }
  case TargetOpcode::G_GEP: {
    assert(MRI.getType(Dest).isPointer() && "Unexpected operand type.");
    MRI.setRegBank(Dest, getRegBank(Mips::GPRBRegBankID));
    break;
  }
  default:
    llvm_unreachable("Unexpected opcode.");
  }
}

static void
combineAwayG_UNMERGE_VALUES(LegalizationArtifactCombiner &ArtCombiner,
                            MachineInstr &MI) {
  SmallVector<MachineInstr *, 2> DeadInstrs;
  ArtCombiner.tryCombineMerges(MI, DeadInstrs);
  for (MachineInstr *DeadMI : DeadInstrs)
    DeadMI->eraseFromParent();
}

void MipsRegisterBankInfo::applyMappingImpl(
    const OperandsMapper &OpdMapper) const {
  MachineInstr &MI = OpdMapper.getMI();
  InstListTy NewInstrs;
  MachineIRBuilder B(MI);
  MachineFunction *MF = MI.getMF();
  MachineRegisterInfo &MRI = OpdMapper.getMRI();
  const LegalizerInfo &LegInfo = *MF->getSubtarget().getLegalizerInfo();

  InstManager NewInstrObserver(NewInstrs);
  GISelObserverWrapper WrapperObserver(&NewInstrObserver);
  LegalizerHelper Helper(*MF, WrapperObserver, B);
  LegalizationArtifactCombiner ArtCombiner(B, MF->getRegInfo(), LegInfo);

  switch (MI.getOpcode()) {
  case TargetOpcode::G_LOAD:
  case TargetOpcode::G_STORE:
  case TargetOpcode::G_PHI:
  case TargetOpcode::G_SELECT:
  case TargetOpcode::G_IMPLICIT_DEF: {
    Helper.narrowScalar(MI, 0, LLT::scalar(32));
    // Handle new instructions.
    while (!NewInstrs.empty()) {
      MachineInstr *NewMI = NewInstrs.pop_back_val();
      // This is new G_UNMERGE that was created during narrowScalar and will
      // not be considered for regbank selection. RegBankSelect for mips
      // visits/makes corresponding G_MERGE first. Combine them here.
      if (NewMI->getOpcode() == TargetOpcode::G_UNMERGE_VALUES)
        combineAwayG_UNMERGE_VALUES(ArtCombiner, *NewMI);
      // This G_MERGE will be combined away when its corresponding G_UNMERGE
      // gets regBankSelected.
      else if (NewMI->getOpcode() == TargetOpcode::G_MERGE_VALUES)
        continue;
      else
        // Manually set register banks for def operands to 32 bit gprb.
        setRegBank(*NewMI, MRI);
    }
    return;
  }
  case TargetOpcode::G_UNMERGE_VALUES:
    combineAwayG_UNMERGE_VALUES(ArtCombiner, MI);
    return;
  default:
    break;
  }

  return applyDefaultMapping(OpdMapper);
}
