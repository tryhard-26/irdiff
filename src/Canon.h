#pragma once

#include "Model.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/ModuleSlotTracker.h"
#include <memory>

namespace irdiff {

struct InstInfo {
  llvm::Instruction *I = nullptr;
  unsigned Block = 0;
  std::string Canon;
  std::string Md;
  std::string DbgLoc;
  bool Debug = false;
  bool Pure = false;
  uint64_t Shape = 0;
  int Match = -1;
  uint8_t How = 0;
};

struct BlockInfo {
  llvm::BasicBlock *BB = nullptr;
  std::string Label;
  unsigned First = 0, End = 0;
  std::vector<unsigned> Succ, Pred;
  int Match = -1;
  float Score = 0;
  bool Ambiguous = false;
  uint64_t Hash = 0;
};

struct FuncInfo {
  llvm::Function *F = nullptr;
  unsigned NumArgs = 0;
  std::vector<std::string> LocalNames;
  std::vector<BlockInfo> Blocks;
  std::vector<InstInfo> Insts;
  llvm::DenseMap<const llvm::BasicBlock *, unsigned> BlockIdx;
  llvm::DenseMap<const llvm::Instruction *, unsigned> InstIdx;
  std::vector<std::string> DebugVars;
  uint64_t Hash = 0;

  int instOf(unsigned Id) const {
    return Id < NumArgs ? -1 : int(Id - NumArgs);
  }
  unsigned idOf(unsigned Inst) const { return Inst + NumArgs; }
};

enum class Tok { Local, Block, Global };
using TokFn =
    llvm::function_ref<void(Tok, unsigned, llvm::StringRef, std::string &)>;

// rewrite %__vN, %__bN, @glob toks in canon text
std::string rewrite(llvm::StringRef Canon, TokFn F);
// drop md attachments, !N slots, #N attr-group refs
std::string stripRefs(llvm::StringRef Printed);

class ModuleCanon {
public:
  ModuleCanon(llvm::Module &M, const Options &O);
  llvm::Module &M;
  const Options &Opts;

  FuncInfo &func(llvm::Function &F);
  std::string display(const FuncInfo &FI, llvm::StringRef Canon);
  std::string operand(const llvm::Value *V);
  std::string globalLine(const llvm::GlobalValue &G);
  std::string mdText(const llvm::Metadata *MD);
  std::string attrs(const llvm::AttributeSet &S);
  llvm::ModuleSlotTracker &mst() { return Canon; }

private:
  llvm::ModuleSlotTracker Orig, Canon;
  llvm::DenseMap<const llvm::Function *, std::unique_ptr<FuncInfo>> Funcs;
  void build(FuncInfo &FI);
};

// ordered (field, val) pairs for fn header
std::vector<std::pair<std::string, std::string>> headerFields(llvm::Function &F,
                                                              ModuleCanon &C);
std::vector<std::pair<std::string, std::string>>
globalFields(llvm::GlobalValue &G, ModuleCanon &C);
const char *linkageName(llvm::GlobalValue::LinkageTypes L);

uint64_t hashStr(llvm::StringRef S);

} // namespace irdiff
