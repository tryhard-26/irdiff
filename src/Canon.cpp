#include "Canon.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugProgramInstruction.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

using namespace llvm;

namespace irdiff {

uint64_t hashStr(StringRef S) { return xxh3_64bits(S); }

static bool identChar(char C) {
  return isAlnum(C) || C == '.' || C == '_' || C == '$' || C == '-';
}

static size_t skipQuoted(StringRef S, size_t I) {
  size_t J = S.find('"', I + 1);
  return J == StringRef::npos ? S.size() : J + 1;
}

std::string rewrite(StringRef S, TokFn F) {
  std::string Out;
  Out.reserve(S.size() + 16);
  size_t I = 0, N = S.size();
  while (I < N) {
    char C = S[I];
    if (C == '"') {
      size_t J = skipQuoted(S, I);
      Out.append(S.data() + I, J - I);
      I = J;
      continue;
    }
    if (C == '%' && I + 4 < N && S[I + 1] == '_' && S[I + 2] == '_' &&
        (S[I + 3] == 'v' || S[I + 3] == 'b') && isDigit(S[I + 4])) {
      size_t J = I + 4;
      unsigned Id = 0;
      while (J < N && isDigit(S[J]))
        Id = Id * 10 + unsigned(S[J++] - '0');
      if (J == N || !identChar(S[J])) {
        F(S[I + 3] == 'v' ? Tok::Local : Tok::Block, Id, S.slice(I, J), Out);
        I = J;
        continue;
      }
    }
    if (C == '@' && I + 1 < N) {
      size_t J = I + 1;
      if (S[J] == '"')
        J = skipQuoted(S, J);
      else
        while (J < N && identChar(S[J]))
          ++J;
      if (J > I + 1) {
        F(Tok::Global, 0, S.slice(I, J), Out);
        I = J;
        continue;
      }
    }
    Out += C;
    ++I;
  }
  return Out;
}

std::string stripRefs(StringRef S) {
  S = S.trim();
  std::string Out;
  Out.reserve(S.size());
  size_t I = 0, N = S.size();
  while (I < N) {
    char C = S[I];
    if (C == '"') {
      size_t J = skipQuoted(S, I);
      Out.append(S.data() + I, J - I);
      I = J;
      continue;
    }
    if (C == ',' && I + 3 < N && S[I + 1] == ' ' && S[I + 2] == '!' &&
        isAlpha(S[I + 3]))
      break;
    if (C == '!' && I + 1 < N && isDigit(S[I + 1])) {
      Out += "!_";
      I += 1;
      while (I < N && isDigit(S[I]))
        ++I;
      continue;
    }
    if (C == '#' && I + 1 < N && isDigit(S[I + 1])) {
      while (!Out.empty() && Out.back() == ' ')
        Out.pop_back();
      I += 1;
      while (I < N && isDigit(S[I]))
        ++I;
      continue;
    }
    Out += C;
    ++I;
  }
  while (!Out.empty() && Out.back() == ' ')
    Out.pop_back();
  return Out;
}

ModuleCanon::ModuleCanon(Module &M, const Options &O)
    : M(M), Opts(O), Orig(&M), Canon(&M) {}

// restore original local names so inputs stay reusable
ModuleCanon::~ModuleCanon() {
  for (auto &[Fn, FI] : Funcs) {
    Function &F = *FI->F;
    unsigned Id = 0, B = 0;
    for (Argument &A : F.args())
      A.setName("");
    for (BasicBlock &BB : F) {
      BB.setName("");
      for (Instruction &I : BB)
        I.setName("");
    }
    for (Argument &A : F.args())
      A.setName(FI->RawNames[Id++]);
    for (BasicBlock &BB : F) {
      BB.setName(FI->RawBlockNames[B++]);
      for (Instruction &I : BB)
        I.setName(FI->RawNames[Id++]);
    }
  }
}

FuncInfo &ModuleCanon::func(Function &F) {
  if (F.isMaterializable())
    consumeError(F.materialize());
  auto &P = Funcs[&F];
  if (!P) {
    P = std::make_unique<FuncInfo>();
    P->F = &F;
    build(*P);
  }
  return *P;
}

static std::string asOperand(const Value &V, ModuleSlotTracker &MST,
                             bool Ty = false) {
  std::string S;
  raw_string_ostream OS(S);
  V.printAsOperand(OS, Ty, MST);
  return S;
}

std::string ModuleCanon::operand(const Value *V) {
  if (!V)
    return "<null>";
  return stripRefs(asOperand(*V, Canon, true));
}

std::string ModuleCanon::attrs(const AttributeSet &S) {
  std::string Out;
  for (const Attribute &A : S) {
    if (!Opts.CompareDebug && A.isStringAttribute() &&
        A.getKindAsString().starts_with("dbg"))
      continue;
    if (!Out.empty())
      Out += ' ';
    Out += A.getAsString();
  }
  return Out;
}

std::string ModuleCanon::mdText(const Metadata *Root) {
  std::string Out;
  raw_string_ostream OS(Out);
  SmallPtrSet<const MDNode *, 8> Stack;
  unsigned Budget = 256;
  std::function<void(const Metadata *)> Go = [&](const Metadata *MD) {
    if (!MD) {
      OS << "null";
      return;
    }
    if (!Budget--) {
      OS << "...";
      return;
    }
    if (auto *S = dyn_cast<MDString>(MD)) {
      OS << "!\"";
      printEscapedString(S->getString(), OS);
      OS << '"';
      return;
    }
    if (auto *V = dyn_cast<ValueAsMetadata>(MD)) {
      OS << asOperand(*V->getValue(), Canon, true);
      return;
    }
    if (auto *L = dyn_cast<DILocation>(MD)) {
      OS << "loc(" << L->getLine() << ':' << L->getColumn() << ')';
      return;
    }
    if (isa<DINode>(MD) || isa<DIExpression>(MD)) {
      std::string T;
      raw_string_ostream TS(T);
      MD->print(TS, Canon);
      OS << stripRefs(StringRef(T).split(" = ").second.empty()
                          ? StringRef(T)
                          : StringRef(T).split(" = ").second);
      return;
    }
    auto *N = dyn_cast<MDNode>(MD);
    if (!N) {
      OS << "?";
      return;
    }
    if (!Stack.insert(N).second) {
      OS << "<self>";
      return;
    }
    OS << (N->isDistinct() ? "distinct !{" : "!{");
    bool First = true;
    for (unsigned I = 0; I < N->getNumOperands(); ++I) {
      const Metadata *Op = N->getOperand(I).get();
      if (!Opts.CompareDebug && Op && isa<DILocation>(Op))
        continue;
      if (!First)
        OS << ", ";
      First = false;
      Go(Op);
    }
    OS << '}';
    Stack.erase(N);
  };
  Go(Root);
  return Out;
}

void ModuleCanon::build(FuncInfo &FI) {
  Function &F = *FI.F;
  Orig.incorporateFunction(F);
  FI.NumArgs = F.arg_size();
  unsigned NI = 0;
  for (BasicBlock &BB : F)
    NI += BB.size();
  FI.LocalNames.resize(FI.NumArgs + NI);
  FI.Insts.resize(NI);
  FI.Blocks.resize(F.size());

  unsigned Id = 0, B = 0, K = 0;
  for (Argument &A : F.args())
    FI.LocalNames[Id++] = asOperand(A, Orig);
  for (BasicBlock &BB : F) {
    BlockInfo &BI = FI.Blocks[B];
    BI.BB = &BB;
    BI.Label = asOperand(BB, Orig);
    BI.First = K;
    FI.BlockIdx[&BB] = B++;
    for (Instruction &I : BB) {
      if (!I.getType()->isVoidTy())
        FI.LocalNames[Id] = asOperand(I, Orig);
      FI.Insts[K].I = &I;
      FI.InstIdx[&I] = K;
      FI.Insts[K].Block = B - 1;
      ++Id;
      ++K;
    }
    BI.End = K;
  }

  for (Argument &A : F.args())
    FI.RawNames.push_back(A.getName().str());
  for (BasicBlock &BB : F) {
    FI.RawBlockNames.push_back(BB.getName().str());
    for (Instruction &I : BB)
      FI.RawNames.push_back(I.getName().str());
  }
  for (Argument &A : F.args())
    A.setName("");
  for (BasicBlock &BB : F) {
    BB.setName("");
    for (Instruction &I : BB)
      I.setName("");
  }
  Id = 0;
  for (Argument &A : F.args())
    A.setName("__v" + Twine(Id++));
  for (auto &BI : FI.Blocks)
    BI.BB->setName("__b" + Twine(&BI - FI.Blocks.data()));
  for (InstInfo &II : FI.Insts) {
    if (!II.I->getType()->isVoidTy())
      II.I->setName("__v" + Twine(Id));
    ++Id;
  }

  SmallVector<StringRef, 32> Kinds;
  F.getContext().getMDKindNames(Kinds);
  Canon.incorporateFunction(F);
  std::string Buf;
  for (InstInfo &II : FI.Insts) {
    Instruction &I = *II.I;
    Buf.clear();
    raw_string_ostream OS(Buf);
    I.print(OS, Canon);
    II.Canon = stripRefs(Buf);
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      if (Function *Callee = CB->getCalledFunction())
        if (Callee->getName().starts_with("llvm.dbg.")) {
          II.Debug = true;
          if (CB->arg_size() > 1)
            if (auto *MV = dyn_cast<MetadataAsValue>(CB->getArgOperand(1)))
              if (auto *V = dyn_cast<DILocalVariable>(MV->getMetadata()))
                FI.DebugVars.push_back(Callee->getName().drop_front(9).str() +
                                       " " + V->getName().str());
        }
      if (Opts.CompareAttributes) {
        std::string FA = attrs(CB->getAttributes().getFnAttrs());
        if (!FA.empty())
          II.Canon += " [fn-attrs: " + FA + "]";
      }
    }
    for (DbgVariableRecord &R : filterDbgVars(I.getDbgRecordRange()))
      if (auto *V = R.getVariable())
        FI.DebugVars.push_back(
            std::string(R.isDbgDeclare() ? "declare " : "value ") +
            V->getName().str());
    II.Pure = !I.mayHaveSideEffects() && !I.mayReadFromMemory() &&
              !I.isTerminator() && !isa<PHINode>(I);
    if (const DebugLoc &DL = I.getDebugLoc())
      II.DbgLoc =
          std::to_string(DL.getLine()) + ":" + std::to_string(DL.getCol());
    if (Opts.CompareMetadata) {
      SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
      I.getAllMetadataOtherThanDebugLoc(MDs);
      std::vector<std::string> Parts;
      for (auto &[Kind, Node] : MDs) {
        StringRef Name = Kind < Kinds.size() ? Kinds[Kind] : "?";
        if (!Opts.CompareDebug && Name == "DIAssignID")
          continue;
        Parts.push_back("!" + Name.str() + " " + mdText(Node));
      }
      llvm::sort(Parts);
      II.Md = join(Parts, ", ");
    }
  }
  for (auto &BI : FI.Blocks) {
    for (BasicBlock *S : successors(BI.BB))
      BI.Succ.push_back(FI.BlockIdx[S]);
    for (BasicBlock *P : predecessors(BI.BB)) {
      unsigned PI = FI.BlockIdx[P];
      if (!is_contained(BI.Pred, PI))
        BI.Pred.push_back(PI);
    }
  }
  llvm::sort(FI.DebugVars);
}

std::string ModuleCanon::display(const FuncInfo &FI, StringRef C) {
  return rewrite(C, [&](Tok T, unsigned Id, StringRef Raw, std::string &Out) {
    if (T == Tok::Local && Id < FI.LocalNames.size() &&
        !FI.LocalNames[Id].empty())
      Out += FI.LocalNames[Id];
    else if (T == Tok::Block && Id < FI.Blocks.size())
      Out += FI.Blocks[Id].Label;
    else
      Out += Raw;
  });
}

std::string ModuleCanon::globalLine(const GlobalValue &G) {
  std::string S;
  raw_string_ostream OS(S);
  G.print(OS, Canon);
  return stripRefs(S);
}

const char *linkageName(GlobalValue::LinkageTypes L) {
  switch (L) {
  case GlobalValue::ExternalLinkage:
    return "external";
  case GlobalValue::AvailableExternallyLinkage:
    return "available_externally";
  case GlobalValue::LinkOnceAnyLinkage:
    return "linkonce";
  case GlobalValue::LinkOnceODRLinkage:
    return "linkonce_odr";
  case GlobalValue::WeakAnyLinkage:
    return "weak";
  case GlobalValue::WeakODRLinkage:
    return "weak_odr";
  case GlobalValue::AppendingLinkage:
    return "appending";
  case GlobalValue::InternalLinkage:
    return "internal";
  case GlobalValue::PrivateLinkage:
    return "private";
  case GlobalValue::ExternalWeakLinkage:
    return "extern_weak";
  case GlobalValue::CommonLinkage:
    return "common";
  }
  return "?";
}

static const char *visName(GlobalValue::VisibilityTypes V) {
  switch (V) {
  case GlobalValue::DefaultVisibility:
    return "default";
  case GlobalValue::HiddenVisibility:
    return "hidden";
  case GlobalValue::ProtectedVisibility:
    return "protected";
  }
  return "?";
}

static const char *dllName(GlobalValue::DLLStorageClassTypes D) {
  switch (D) {
  case GlobalValue::DefaultStorageClass:
    return "";
  case GlobalValue::DLLImportStorageClass:
    return "dllimport";
  case GlobalValue::DLLExportStorageClass:
    return "dllexport";
  }
  return "?";
}

static const char *unnamedName(GlobalValue::UnnamedAddr U) {
  switch (U) {
  case GlobalValue::UnnamedAddr::None:
    return "";
  case GlobalValue::UnnamedAddr::Local:
    return "local_unnamed_addr";
  case GlobalValue::UnnamedAddr::Global:
    return "unnamed_addr";
  }
  return "?";
}

static std::string ccName(CallingConv::ID CC) {
  switch (CC) {
  case CallingConv::C:
    return "ccc";
  case CallingConv::Fast:
    return "fastcc";
  case CallingConv::Cold:
    return "coldcc";
  case CallingConv::GHC:
    return "ghccc";
  case CallingConv::PreserveMost:
    return "preserve_mostcc";
  case CallingConv::PreserveAll:
    return "preserve_allcc";
  case CallingConv::Swift:
    return "swiftcc";
  case CallingConv::SwiftTail:
    return "swifttailcc";
  case CallingConv::Tail:
    return "tailcc";
  case CallingConv::X86_StdCall:
    return "x86_stdcallcc";
  case CallingConv::X86_FastCall:
    return "x86_fastcallcc";
  case CallingConv::X86_VectorCall:
    return "x86_vectorcallcc";
  case CallingConv::X86_64_SysV:
    return "x86_64_sysvcc";
  case CallingConv::Win64:
    return "win64cc";
  case CallingConv::ARM_AAPCS:
    return "arm_aapcscc";
  case CallingConv::ARM_AAPCS_VFP:
    return "arm_aapcs_vfpcc";
  case CallingConv::AArch64_VectorCall:
    return "aarch64_vector_pcs";
  default:
    return "cc " + std::to_string(CC);
  }
}

static std::string typeStr(Type *T) {
  std::string S;
  raw_string_ostream OS(S);
  T->print(OS);
  return S;
}

static void commonFields(GlobalValue &G,
                         std::vector<std::pair<std::string, std::string>> &R) {
  R.push_back({"linkage", linkageName(G.getLinkage())});
  R.push_back({"visibility", visName(G.getVisibility())});
  R.push_back({"dll-storage", dllName(G.getDLLStorageClass())});
  R.push_back({"dso-local", G.isDSOLocal() ? "dso_local" : ""});
  R.push_back({"unnamed-addr", unnamedName(G.getUnnamedAddr())});
  R.push_back({"address-space", std::to_string(G.getAddressSpace())});
  R.push_back({"partition", G.getPartition().str()});
}

std::vector<std::pair<std::string, std::string>> headerFields(Function &F,
                                                              ModuleCanon &C) {
  std::vector<std::pair<std::string, std::string>> R;
  commonFields(F, R);
  R.push_back({"calling-convention", ccName(F.getCallingConv())});
  R.push_back({"return-type", typeStr(F.getReturnType())});
  std::string Params;
  for (Type *T : F.getFunctionType()->params())
    Params += (Params.empty() ? "" : ", ") + typeStr(T);
  if (F.isVarArg())
    Params += Params.empty() ? "..." : ", ...";
  R.push_back({"parameters", Params});
  AttributeList AL = F.getAttributes();
  R.push_back({"return-attributes", C.attrs(AL.getRetAttrs())});
  std::string PA;
  for (unsigned I = 0; I < F.arg_size(); ++I) {
    std::string S = C.attrs(AL.getParamAttrs(I));
    if (!S.empty())
      PA += (PA.empty() ? "" : "; ") + ("#" + std::to_string(I) + ": ") + S;
  }
  R.push_back({"parameter-attributes", PA});
  std::string FA, Cfg;
  for (const Attribute &A : AL.getFnAttrs()) {
    std::string &Dst = A.isStringAttribute() ? Cfg : FA;
    Dst += (Dst.empty() ? "" : " ") + A.getAsString();
  }
  R.push_back({"function-attributes", FA});
  R.push_back({"config-attributes", Cfg});
  R.push_back({"section", F.getSection().str()});
  R.push_back(
      {"alignment", F.getAlign() ? std::to_string(F.getAlign()->value()) : ""});
  R.push_back({"gc", F.hasGC() ? F.getGC() : ""});
  R.push_back({"personality",
               F.hasPersonalityFn() ? C.operand(F.getPersonalityFn()) : ""});
  R.push_back({"comdat", F.hasComdat() ? F.getComdat()->getName().str() : ""});
  R.push_back(
      {"prefix", F.hasPrefixData() ? C.operand(F.getPrefixData()) : ""});
  R.push_back(
      {"prologue", F.hasPrologueData() ? C.operand(F.getPrologueData()) : ""});
  std::string MD;
  if (C.Opts.CompareMetadata) {
    SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
    F.getAllMetadata(MDs);
    SmallVector<StringRef, 32> Kinds;
    F.getContext().getMDKindNames(Kinds);
    std::vector<std::string> Parts;
    for (auto &[K, N] : MDs) {
      if (K == LLVMContext::MD_dbg && !C.Opts.CompareDebug)
        continue;
      Parts.push_back("!" + Kinds[K].str() + " " + C.mdText(N));
    }
    llvm::sort(Parts);
    MD = join(Parts, ", ");
  }
  R.push_back({"metadata", MD});
  return R;
}

std::vector<std::pair<std::string, std::string>> globalFields(GlobalValue &G,
                                                              ModuleCanon &C) {
  std::vector<std::pair<std::string, std::string>> R;
  commonFields(G, R);
  R.push_back({"value-type", typeStr(G.getValueType())});
  if (auto *V = dyn_cast<GlobalVariable>(&G)) {
    R.push_back({"constant", V->isConstant() ? "constant" : "global"});
    R.push_back({"thread-local", V->isThreadLocal() ? "thread_local" : ""});
    R.push_back({"externally-initialized",
                 V->isExternallyInitialized() ? "externally_initialized" : ""});
    R.push_back({"initializer",
                 V->hasInitializer() ? C.operand(V->getInitializer()) : ""});
    R.push_back({"section", V->getSection().str()});
    R.push_back(
        {"comdat", V->hasComdat() ? V->getComdat()->getName().str() : ""});
    R.push_back({"alignment",
                 V->getAlign() ? std::to_string(V->getAlign()->value()) : ""});
    R.push_back(
        {"attributes", V->hasAttributes() ? C.attrs(V->getAttributes()) : ""});
    std::string MD;
    if (C.Opts.CompareMetadata) {
      SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
      V->getAllMetadata(MDs);
      SmallVector<StringRef, 32> Kinds;
      V->getContext().getMDKindNames(Kinds);
      std::vector<std::string> Parts;
      for (auto &[K, N] : MDs) {
        if (K == LLVMContext::MD_dbg && !C.Opts.CompareDebug)
          continue;
        Parts.push_back("!" + Kinds[K].str() + " " + C.mdText(N));
      }
      llvm::sort(Parts);
      MD = join(Parts, ", ");
    }
    R.push_back({"metadata", MD});
  } else if (auto *A = dyn_cast<GlobalAlias>(&G)) {
    R.push_back({"aliasee", C.operand(A->getAliasee())});
  } else if (auto *I = dyn_cast<GlobalIFunc>(&G)) {
    R.push_back({"resolver", C.operand(I->getResolver())});
  }
  return R;
}

} // namespace irdiff
