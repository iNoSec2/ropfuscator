// ==============================================================================
//   X86 ROPFUSCATOR
//   part of the ROPfuscator project
// ==============================================================================
// This module is simply the frontend of ROPfuscator for LLVM.
// It also provides statics about the processed functions.
//

#include "ROPfuscatorCore.h"
#include "BinAutopsy.h"
#include "Debug.h"
#include "LivenessAnalysis.h"
#include "MathUtil.h"
#include "OpaqueConstruct.h"
#include "ROPEngine.h"
#include "ROPfuscatorConfig.h"
#include "X86.h"
#include "X86AssembleHelper.h"
#include "X86MachineFunctionInfo.h"
#include "X86RegisterInfo.h"
#include "X86Subtarget.h"
#include "X86TargetMachine.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <utility>

using namespace llvm;

namespace ropf {

namespace {

const std::string POSSIBLE_LIBC_FOLDERS[] = {
    "/lib/i386-linux-gnu",
    "/usr/lib/i386-linux-gnu",
    "/lib32",
    "/usr/lib32",
    "/usr/local/lib",
    "/lib",
    "/usr/lib",
};

std::string findLibcPath() {
  for (auto &dir : POSSIBLE_LIBC_FOLDERS) {
    // searching for libc in regular files only
    std::error_code ec;
    for (auto dir_it = llvm::sys::fs::directory_iterator(dir, ec),
              dir_end = llvm::sys::fs::directory_iterator();
         !ec && dir_it != dir_end; dir_it.increment(ec)) {
      auto st = dir_it->status();
      if (st && st->type() == llvm::sys::fs::file_type::regular_file &&
          llvm::sys::path::filename(dir_it->path()) == "libc.so.6") {
        std::string libraryPath = dir_it->path();
        dbg_fmt("[*] Using library path: {}\n", libraryPath);
        return libraryPath;
      }
    }
  }
  return "";
}

} // namespace

#ifdef ROPFUSCATOR_INSTRUCTION_STAT
struct ROPfuscatorCore::ROPChainStatEntry {
  static const int entry_size = static_cast<int>(ROPChainStatus::COUNT);
  int data[entry_size];

  int &operator[](ROPChainStatus status) {
    return data[static_cast<int>(status)];
  }

  int operator[](ROPChainStatus status) const {
    return data[static_cast<int>(status)];
  }

  int total() const { return std::accumulate(&data[0], &data[entry_size], 0); }

  ROPChainStatEntry() { memset(data, 0, sizeof(data)); }

  static constexpr const char *DEBUG_FMT_NORMAL =
      "stat: ropfuscated {0} / total {6}\n[not-implemented: {1} | "
      "no-register: {2} | no-gadget: {3} "
      "| unsupported: {4} | unsupported-esp: {5}]";
  static constexpr const char *DEBUG_FMT_SIMPLE =
      "{0}\t{1}\t{2}\t{3}\t{4}\t{5}\t{6}";

  std::ostream &print_to(std::ostream &os, const char *fmt) const {
    const ROPChainStatEntry &entry = *this;
    fmt::print(os, fmt, entry[ROPChainStatus::OK],
               entry[ROPChainStatus::ERR_NOT_IMPLEMENTED],
               entry[ROPChainStatus::ERR_NO_REGISTER_AVAILABLE],
               entry[ROPChainStatus::ERR_NO_GADGETS_AVAILABLE],
               entry[ROPChainStatus::ERR_UNSUPPORTED],
               entry[ROPChainStatus::ERR_UNSUPPORTED_STACKPOINTER],
               entry.total());
    return os;
  }

  friend std::ostream &operator<<(std::ostream &os,
                                  const ROPChainStatEntry &entry) {
    return entry.print_to(os, DEBUG_FMT_NORMAL);
  }

  std::string to_string(const char *fmt) const {
    std::stringstream ss;
    print_to(ss, fmt);
    return ss.str();
  }

  static std::string header_string(const char *fmt) {
    return fmt::format(fmt, "ropfuscated", "not-implemented", "no-register",
                       "no-gadget", "unsupported", "unsupported-esp", "total");
  }
};
#endif

// ----------------------------------------------------------------

namespace {

// Lowered ROP Chain
// These classes represent more lower level of machine code than ROP chain
// and directly output machine code.

// base class
struct ROPChainPushInst {
  std::shared_ptr<OpaqueConstruct> opaqueConstant;
  virtual void compile(X86AssembleHelper &as) = 0;
  virtual ~ROPChainPushInst() = default;
};

// immediate (immediate operand, etc)
struct PUSH_IMM : public ROPChainPushInst {
  int64_t value;
  explicit PUSH_IMM(int64_t value) : value(value) {}
  virtual void compile(X86AssembleHelper &as) override {
    if (opaqueConstant) {
      uint32_t opaque =
          *opaqueConstant->getOutput().findValue(OpaqueStorage::EAX);
      // compute opaque constant to eax
      opaqueConstant->compile(as, 0);
      // adjust eax to be the constant
      uint32_t diff = value - opaque;
      as.add(as.reg(X86::EAX), as.imm(diff));
      // push eax
      as.push(as.reg(X86::EAX));
    } else {
      // push $imm
      as.push(as.imm(value));
    }
  }
  virtual ~PUSH_IMM() = default;
};

// global variable (immediate operand, etc)
struct PUSH_GV : public ROPChainPushInst {
  const llvm::GlobalValue *gv;
  int64_t offset;
  PUSH_GV(const llvm::GlobalValue *gv, int64_t offset)
      : gv(gv), offset(offset) {}
  virtual void compile(X86AssembleHelper &as) override {
    if (opaqueConstant) {
      uint32_t opaque =
          *opaqueConstant->getOutput().findValue(OpaqueStorage::EAX);
      // compute opaque constant to eax
      opaqueConstant->compile(as, 0);
      // adjust eax to be the constant
      uint32_t diff = offset - opaque;
      as.add(as.reg(X86::EAX), as.imm(gv, diff));
      // push eax
      as.push(as.reg(X86::EAX));
    } else {
      // push global_symbol
      as.push(as.imm(gv, offset));
    }
  }
  virtual ~PUSH_GV() = default;
};

// gadget with single or multiple addresses
struct PUSH_GADGET : public ROPChainPushInst {
  const Symbol *anchor;
  uint32_t offset;
  explicit PUSH_GADGET(const Symbol *anchor, uint32_t offset)
      : anchor(anchor), offset(offset) {}
  virtual void compile(X86AssembleHelper &as) override {
    if (opaqueConstant) {
      auto opaqueValues =
          *opaqueConstant->getOutput().findValues(OpaqueStorage::EAX);
      // compute opaque constant to eax
      opaqueConstant->compile(as, 0);
      // add eax, $symbol
      as.add(as.reg(X86::EAX), as.label(anchor->Label));
      // push eax
      as.push(as.reg(X86::EAX));
    } else {
      // push $symbol+offset
      as.push(as.addOffset(as.label(anchor->Label), offset));
    }
  }
  virtual ~PUSH_GADGET() = default;
};

// local label
struct PUSH_LABEL : public ROPChainPushInst {
  X86AssembleHelper::Label label;
  explicit PUSH_LABEL(const X86AssembleHelper::Label &label) : label(label) {}
  virtual void compile(X86AssembleHelper &as) override {
    if (opaqueConstant) {
      uint32_t value =
          *opaqueConstant->getOutput().findValue(OpaqueStorage::EAX);
      // compute opaque constant to eax
      opaqueConstant->compile(as, 0);
      // adjust eax to jump target address
      as.add(as.reg(X86::EAX), as.addOffset(label, -value));
      // push eax
      as.push(as.reg(X86::EAX));
    } else {
      // push label
      as.push(label);
    }
  }
  virtual ~PUSH_LABEL() = default;
};

// push esp
struct PUSH_ESP : public ROPChainPushInst {
  virtual void compile(X86AssembleHelper &as) override {
    as.push(as.reg(X86::ESP));
  }
  virtual ~PUSH_ESP() = default;
};

// push eflags
struct PUSH_EFLAGS : public ROPChainPushInst {
  virtual void compile(X86AssembleHelper &as) override { as.pushf(); }
  virtual ~PUSH_EFLAGS() = default;
};

void generateChainLabels(std::string &chainLabel, std::string &resumeLabel,
                         StringRef funcName, int chainID) {
  chainLabel = fmt::format("{}_chain_{}", funcName.str(), chainID);
  resumeLabel = fmt::format("resume_{}", chainLabel);

  // replacing $ with _
  std::replace(chainLabel.begin(), chainLabel.end(), '$', '_');
}

void putLabelInMBB(MachineBasicBlock &MBB, X86AssembleHelper::Label label) {
  X86AssembleHelper as(MBB, MBB.begin());
  as.putLabel(label);
}

} // namespace

ROPfuscatorCore::ROPfuscatorCore(llvm::Module &module,
                                 const ROPfuscatorConfig &config)
    : config(config), BA(nullptr), TII(nullptr) {}

ROPfuscatorCore::~ROPfuscatorCore() {
#ifdef ROPFUSCATOR_INSTRUCTION_STAT
  if (config.globalConfig.printInstrStat) {
    dbg_fmt(
        "{}\t{}\t{}\n", "op-id", "op-name",
        ROPChainStatEntry::header_string(ROPChainStatEntry::DEBUG_FMT_SIMPLE));
    for (auto &kv : instr_stat) {
      dbg_fmt("{}\t{}\t{}\n", kv.first, TII->getName(kv.first),
              kv.second.to_string(ROPChainStatEntry::DEBUG_FMT_SIMPLE));
    }
  }
#endif
}

void ROPfuscatorCore::insertROPChain(ROPChain &chain, MachineBasicBlock &MBB,
                                     MachineInstr &MI, int chainID,
                                     const ObfuscationParameter &param) {
  auto as = X86AssembleHelper(MBB, MI.getIterator());

  bool isLastInstrInBlock = MI.getNextNode() == nullptr;
  bool resumeLabelRequired = false;
  std::map<int, int> espOffsetMap;
  int espoffset = 0;
  std::vector<const Symbol *> versionedSymbols;

  // stack layout:
  // (A) if FlagSaveMode == SAVE_AFTER_EXEC:
  // 1. saved-regs
  // 2. ROP chain
  // 3. flags
  // 4. return addr
  //
  // (B) if FlagSaveMode == SAVE_BEFORE_EXEC or NOT_SAVED:
  // 1. saved-regs (and flags)
  // 2. ROP chain
  // 3. return address

  if (chain.hasUnconditionalJump || chain.hasConditionalJump) {
    // continuation of the ROP chain (resume address) is already on the chain
  } else {
    // push resume address on the chain
    chain.emplace_back(ChainElem::createJmpFallthrough());
  }

  X86AssembleHelper::Label asChainLabel, asResumeLabel;
  if (config.globalConfig.useChainLabel) {
    std::string chainLabel, resumeLabel;
    generateChainLabels(chainLabel, resumeLabel, MBB.getParent()->getName(),
                        chainID);
    asChainLabel = as.label(chainLabel);
    asResumeLabel = as.label(resumeLabel);
  } else {
    asChainLabel = as.label();
    asResumeLabel = as.label();
  }

  // Convert ROP chain to push instructions
  std::vector<std::shared_ptr<ROPChainPushInst>> pushchain;

  if (chain.flagSave == FlagSaveMode::SAVE_AFTER_EXEC) {
    assert(!chain.hasUnconditionalJump || !chain.hasConditionalJump);

    // If the obfuscated instruction will NOT modify flags,
    // (and if the chain execution might modify the flags,)
    // the flags should be restored after the ROP chain is executed.
    // flag is saved at the bottom of the stack
    // pushf (EFLAGS register backup)
    ROPChainPushInst *push = new PUSH_EFLAGS();
    pushchain.emplace_back(push);
    // modify isLastInstrInBlock flag, since we will emit popf instruction later
    isLastInstrInBlock = false;
    espoffset -= 4;
  }

  // Pushes each chain element on the stack in reverse order
  for (auto elem = chain.rbegin(); elem != chain.rend(); ++elem) {
    switch (elem->type) {

    case ChainElem::Type::IMM_VALUE: {
      // Push the immediate value onto the stack
      ROPChainPushInst *push = new PUSH_IMM(elem->value);
      if (param.opaquePredicateEnabled && param.obfuscateImmediateOperand) {
        push->opaqueConstant = OpaqueConstructFactory::createOpaqueConstant32(
            OpaqueStorage::EAX, param.opaqueConstantAlgorithm);
      }
      pushchain.emplace_back(push);
      break;
    }

    case ChainElem::Type::IMM_GLOBAL: {
      // Push the global symbol onto the stack
      ROPChainPushInst *push = new PUSH_GV(elem->global, elem->value);
      if (param.opaquePredicateEnabled && param.obfuscateImmediateOperand) {
        push->opaqueConstant = OpaqueConstructFactory::createOpaqueConstant32(
            OpaqueStorage::EAX, param.opaqueConstantAlgorithm);
      }
      pushchain.emplace_back(push);
      break;
    }

    case ChainElem::Type::GADGET: {
      // Get a random symbol to reference this gadget in memory
      const Symbol *sym = BA->getRandomSymbol();
      // Choose a random address in the gadget
      const std::vector<uint64_t> &addresses = elem->microgadget->addresses;
      std::vector<uint32_t> offsets;
      int num_branches = 1;
      if (param.opaqueBranchDivergenceEnabled)
        num_branches = std::min((size_t)param.opaqueBranchDivergenceMaxBranches,
                                addresses.size());
      // pick num_branches elements randomly
      std::sample(addresses.begin(), addresses.end(),
                  std::back_inserter(offsets), num_branches,
                  math::Random::engine());
      for (uint32_t &offset : offsets) {
        offset -= sym->Address;
      }

      // .symver directive: necessary to prevent aliasing when more
      // symbols have the same name. We do this exclusively when the
      // symbol Version is not "Base" (i.e., it is the only one
      // available).
      if (!sym->isUsed && sym->Version != "Base") {
        versionedSymbols.push_back(sym);
        sym->isUsed = true;
      }

      ROPChainPushInst *push = new PUSH_GADGET(sym, offsets[0]);
      if (param.opaquePredicateEnabled) {
        std::shared_ptr<OpaqueConstruct> opaqueConstant;
        if (num_branches > 1) {
          opaqueConstant =
              OpaqueConstructFactory::createBranchingOpaqueConstant32(
                  OpaqueStorage::EAX, offsets.size(),
                  param.opaqueBranchDivergenceAlgorithm);
        } else {
          opaqueConstant = OpaqueConstructFactory::createOpaqueConstant32(
              OpaqueStorage::EAX, param.opaqueConstantAlgorithm);
        }
        auto opaqueValues =
            *opaqueConstant->getOutput().findValues(OpaqueStorage::EAX);
        auto adjuster = OpaqueConstructFactory::createValueAdjustor(
            OpaqueStorage::EAX, opaqueValues, offsets);
        push->opaqueConstant =
            OpaqueConstructFactory::compose(adjuster, opaqueConstant);
      }
      pushchain.emplace_back(push);
      break;
    }

    case ChainElem::Type::JMP_BLOCK: {
      MachineBasicBlock *targetMBB = elem->jmptarget;
      MBB.addSuccessorWithoutProb(targetMBB);
      auto targetLabel = as.label();
      putLabelInMBB(*targetMBB, targetLabel);

      ROPChainPushInst *push = new PUSH_LABEL(targetLabel);
      if (param.opaquePredicateEnabled && param.obfuscateBranchTarget) {
        push->opaqueConstant = OpaqueConstructFactory::createOpaqueConstant32(
            OpaqueStorage::EAX, param.opaqueConstantAlgorithm);
      }
      pushchain.emplace_back(push);
      break;
    }

    case ChainElem::Type::JMP_FALLTHROUGH: {
      // push label
      X86AssembleHelper::Label targetLabel = {nullptr};
      if (isLastInstrInBlock) {
        for (auto it = MBB.succ_begin(); it != MBB.succ_end(); ++it) {
          if (MBB.isLayoutSuccessor(*it)) {
            auto *targetMBB = *it;
            targetLabel = asResumeLabel;
            putLabelInMBB(*targetMBB, targetLabel);
            break;
          }
        }
      } else {
        targetLabel = asResumeLabel;
        resumeLabelRequired = true;
      }
      if (targetLabel.symbol) {
        ROPChainPushInst *push = new PUSH_LABEL(targetLabel);
        if (param.opaquePredicateEnabled && param.obfuscateBranchTarget) {
          push->opaqueConstant = OpaqueConstructFactory::createOpaqueConstant32(
              OpaqueStorage::EAX, param.opaqueConstantAlgorithm);
        }
        pushchain.emplace_back(push);
      } else {
        // call or conditional jump at the end of function:
        // probably calling "no-return" functions like exit()
        // so we just put dummy return address here
        ROPChainPushInst *push = new PUSH_IMM(0);
        pushchain.emplace_back(push);
      }
      break;
    }

    case ChainElem::Type::ESP_PUSH: {
      // push esp
      ROPChainPushInst *push = new PUSH_ESP();
      pushchain.emplace_back(push);
      espOffsetMap[elem->esp_id] = espoffset;
      break;
    }

    case ChainElem::Type::ESP_OFFSET: {
      // push $(imm - espoffset)
      auto it = espOffsetMap.find(elem->esp_id);
      if (it == espOffsetMap.end()) {
        dbg_fmt("Internal error: ESP_OFFSET should precede corresponding "
                "ESP_PUSH\n");
        exit(1);
      }
      ROPChainPushInst *push = new PUSH_IMM(elem->value - it->second);
      pushchain.emplace_back(push);
      break;
    }
    }

    espoffset -= 4;
  }

  // EMIT PROLOGUE

  // symbol version directives
  if (!versionedSymbols.empty()) {
    std::stringstream ss;
    for (auto *sym : versionedSymbols) {
      if (ss.tellp() > 0) {
        ss << "\n";
      }
      ss << sym->getSymverDirective();
    }
    as.inlineasm(ss.str());
  }

  // save registers (and flags if necessary) on top of the stack
  std::set<unsigned int> savedRegs;

  // compute clobbered registers
  if (param.opaquePredicateEnabled) {
    for (auto &push : pushchain) {
      if (auto &op = push->opaqueConstant) {
        auto clobbered = op->getClobberedRegs();
        savedRegs.insert(clobbered.begin(), clobbered.end());
      }
    }
  }
  if (chain.flagSave == FlagSaveMode::SAVE_BEFORE_EXEC) {
    savedRegs.insert(X86::EFLAGS);
  } else {
    savedRegs.erase(X86::EFLAGS);
  }
  if (!savedRegs.empty()) {
    // lea esp, [esp-4*(N+1)]   # where N = chain size
    as.lea(as.reg(X86::ESP), as.mem(X86::ESP, espoffset));
    // save registers (and flags)
    for (auto it = savedRegs.begin(); it != savedRegs.end(); ++it) {
      if (*it == X86::EFLAGS) {
        as.pushf();
      } else {
        as.push(as.reg(*it));
      }
    }
    // lea esp, [esp+4*(N+1+M)]
    // where N = chain size, M = num of saved registers
    as.lea(as.reg(X86::ESP),
           as.mem(X86::ESP, 4 * savedRegs.size() - espoffset));
  }

  // funcName_chain_X:
  as.putLabel(asChainLabel);

  // emit rop chain
  for (auto &push : pushchain) {
    push->compile(as);
  }

  // EMIT EPILOGUE
  // restore registers (and flags)
  if (!savedRegs.empty()) {
    // lea esp, [esp-4*N]   # where N = num of saved registers
    as.lea(as.reg(X86::ESP), as.mem(X86::ESP, -4 * savedRegs.size()));
    // restore registers (and flags)
    for (auto it = savedRegs.rbegin(); it != savedRegs.rend(); ++it) {
      if (*it == X86::EFLAGS) {
        as.popf();
      } else {
        as.pop(as.reg(*it));
      }
    }
  }

  // ret
  as.ret();

  // resume_funcName_chain_X:
  if (resumeLabelRequired) {
    // If the label is inserted when ROP chain terminates with jump,
    // AsmPrinter::isBlockOnlyReachableByFallthrough() doesn't work correctly
    as.putLabel(asResumeLabel);
  }

  // restore eflags, if eflags should be restored AFTER chain execution
  if (chain.flagSave == FlagSaveMode::SAVE_AFTER_EXEC) {
    // popf (EFLAGS register restore)
    as.popf();
  }
}

void ROPfuscatorCore::obfuscateFunction(MachineFunction &MF) {
  // create a new singleton instance of Binary Autopsy
  if (BA == nullptr) {
    if (config.globalConfig.libraryPath.empty()) {
      config.globalConfig.libraryPath = findLibcPath();
    }
    BA = BinaryAutopsy::getInstance(config.globalConfig, MF);
  }

  if (TII == nullptr) {
    // description of the target ISA (used to generate new instructions, below)
    const X86Subtarget &target = MF.getSubtarget<X86Subtarget>();

    if (target.is64Bit()) {
      dbg_fmt("Error: currently ROPfuscator only works for 32bit.\n");
      exit(1);
    }

    TII = target.getInstrInfo();
  }

  std::string funcName = MF.getName().str();
  ObfuscationParameter param = config.getParameter(funcName);
  if (!param.obfuscationEnabled) {
    return;
  }

  // stats
  int processed = 0, obfuscated = 0;

  // ASM labels for each ROP chain
  int chainID = 0;

  // original instructions that have been successfully ROPified and that will be
  // removed at the end
  std::vector<MachineInstr *> instrToDelete;

  for (MachineBasicBlock &MBB : MF) {
    // perform register liveness analysis to get a list of registers that can be
    // safely clobbered to compute temporary data
    ScratchRegMap MBBScratchRegs = performLivenessAnalysis(MBB);

    ROPChain chain0; // merged chain
    MachineInstr *prevMI = nullptr;
    for (auto it = MBB.begin(), it_end = MBB.end(); it != it_end; ++it) {
      MachineInstr &MI = *it;

      if (MI.isDebugInstr())
        continue;

      DEBUG_WITH_TYPE(PROCESSED_INSTR, dbg_fmt("    {}", MI));
      processed++;

      // get the list of scratch registers available for this instruction
      std::vector<unsigned int> MIScratchRegs =
          MBBScratchRegs.find(&MI)->second;

      // Do this instruction and/or following instructions
      // use current flags (i.e. affected by current flags)?
      bool shouldFlagSaved = !TII->isSafeToClobberEFLAGS(MBB, it);
      // Does this instruction modify (define/kill) flags?
      // bool isFlagModifiedInInstr = false;
      // Example instruction sequence describing how these booleans are set:
      //   mov eax, 1    # false, false
      //   add eax, 1    # false, true
      //   cmp eax, ebx  # false, true
      //   mov ecx, 1    # true,  false (caution!)
      //   mov edx, 2    # true,  false (caution!)
      //   je .Local1    # true,  false
      //   add eax, ebx  # false, true
      //   adc ecx, edx  # true,  true
      //   adc ecx, 1    # true,  true

      ROPChain result;
      ROPChainStatus status =
          ROPEngine(*BA).ropify(MI, MIScratchRegs, shouldFlagSaved, result);

      bool isJump = result.hasConditionalJump || result.hasUnconditionalJump;
      if (isJump && result.flagSave == FlagSaveMode::SAVE_AFTER_EXEC) {
        // when flag should be saved after resume, jmp instruction cannot be
        // ROPified
        status = ROPChainStatus::ERR_UNSUPPORTED;
      }

#ifdef ROPFUSCATOR_INSTRUCTION_STAT
      instr_stat[MI.getOpcode()][status]++;
#endif

      if (status != ROPChainStatus::OK) {
        DEBUG_WITH_TYPE(PROCESSED_INSTR,
                        dbg_fmt("{}\t✗ Unsupported instruction{}\n", COLOR_RED,
                                COLOR_RESET));

        if (chain0.valid()) {
          insertROPChain(chain0, MBB, *prevMI, chainID++, param);
          chain0.clear();
        }
        continue;
      }
      // add current instruction in the To-Delete list
      instrToDelete.push_back(&MI);

      if (chain0.canMerge(result)) {
        chain0.merge(result);
      } else {
        if (chain0.valid()) {
          insertROPChain(chain0, MBB, *prevMI, chainID++, param);
          chain0.clear();
        }
        chain0 = std::move(result);
      }
      prevMI = &MI;

      DEBUG_WITH_TYPE(PROCESSED_INSTR,
                      dbg_fmt("{}\t✓ Replaced{}\n", COLOR_GREEN, COLOR_RESET));

      obfuscated++;
    }

    if (chain0.valid()) {
      insertROPChain(chain0, MBB, *prevMI, chainID++, param);
      chain0.clear();
    }

    // delete old vanilla instructions only after we finished to iterate through
    // the basic block
    for (auto &MI : instrToDelete)
      MI->eraseFromParent();

    instrToDelete.clear();
  }

  // print obfuscation stats for this function
  DEBUG_WITH_TYPE(OBF_STATS,
                  dbg_fmt("{}: {}/{} ({}%) instructions obfuscated\n", funcName,
                          obfuscated, processed,
                          (obfuscated * 100) / processed));
}

} // namespace ropf
