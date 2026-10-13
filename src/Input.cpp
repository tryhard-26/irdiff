#include "Input.h"
#include "Compat.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace irdiff {

int llvmMajor() { return LLVM_VERSION_MAJOR; }
std::string llvmVersion() { return LLVM_VERSION_STRING; }

int parseProducerMajor(StringRef P) {
  if (P.contains("Apple"))
    return 0;
  SmallVector<StringRef, 2> M;
  if (Regex("(LLVM|clang version|LLVM version) ?([0-9]+)").match(P, &M))
    return std::atoi(M[2].str().c_str());
  return 0;
}

static std::string identOf(const Module &M) {
  if (auto *N = M.getNamedMetadata("llvm.ident"))
    if (N->getNumOperands())
      if (auto *T = dyn_cast<MDTuple>(N->getOperand(0)))
        if (T->getNumOperands())
          if (auto *S = dyn_cast<MDString>(T->getOperand(0)))
            return S->getString().str();
  return "";
}

void fillInfo(Loaded &L) {
  Module &M = *L.M;
  InputInfo &I = L.Info;
  I.Triple = tripleOf(M);
  I.DataLayout = M.getDataLayoutStr();
  I.SourceFile = M.getSourceFileName();
  if (I.Producer.empty())
    I.Producer = identOf(M);
  I.ProducerMajor = parseProducerMajor(I.Producer);
  I.Functions = I.Declarations = I.Instructions = 0;
  for (Function &F : M) {
    if (F.isDeclaration())
      ++I.Declarations;
    else
      ++I.Functions;
    I.Instructions += F.getInstructionCount();
  }
  I.Globals = M.global_size();
}

static const char *sniffNonIR(StringRef B) {
  if (B.starts_with("\x7f"
                    "ELF"))
    return "an ELF object file";
  if (B.starts_with("\xcf\xfa\xed\xfe") || B.starts_with("\xce\xfa\xed\xfe") ||
      B.starts_with("\xca\xfe\xba\xbe"))
    return "a Mach-O object file";
  if (B.starts_with("!<arch>"))
    return "an archive";
  if (B.starts_with("--- |") ||
      (B.contains("\nname:") && B.contains("\nbody:")))
    return "Machine IR (MIR)";
  if (B.ltrim().starts_with(".text") || B.contains("\t.section"))
    return "target assembly";
  return nullptr;
}

static std::string newerHint(int Major) {
  if (Major <= LLVM_VERSION_MAJOR)
    return "";
  return "input appears to be produced by LLVM " + std::to_string(Major) +
         ", newer than irdiff's LLVM " LLVM_VERSION_STRING
         "; use an irdiff built against LLVM " +
         std::to_string(Major) + " or newer";
}

static LoadResult finish(std::unique_ptr<Loaded> L, bool Verify) {
  LoadResult R;
  fillInfo(*L);
  if (Verify) {
    std::string Msg;
    raw_string_ostream OS(Msg);
    bool BrokenDebug = false;
    if (verifyModule(*L->M, &OS, &BrokenDebug)) {
      R.Code = ExitInput;
      R.Error = L->Info.Path + ": invalid IR (verifier failed):\n" + Msg;
      return R;
    }
    if (BrokenDebug) {
      StripDebugInfo(*L->M);
      L->Warnings.push_back("invalid debug info was stripped");
    }
  }
  R.L = std::move(L);
  return R;
}

LoadResult loadModuleFromBuffer(StringRef Text, StringRef Name, bool Verify) {
  LoadResult R;
  auto L = std::make_unique<Loaded>();
  L->Ctx = std::make_unique<LLVMContext>();
  L->Info.Path = Name.str();
  MemoryBufferRef Ref(Text, Name);
  if (Text.empty()) {
    R.Code = ExitInput;
    R.Error = Name.str() + ": empty input";
    return R;
  }
  if (isBitcode(reinterpret_cast<const unsigned char *>(Ref.getBufferStart()),
                reinterpret_cast<const unsigned char *>(Ref.getBufferEnd()))) {
    L->Info.Format = "bitcode";
    if (auto P = getBitcodeProducerString(Ref))
      L->Info.Producer = *P;
    else
      consumeError(P.takeError());
    int Major = parseProducerMajor(L->Info.Producer);
    if (auto Mods = getBitcodeModuleList(Ref)) {
      if (Mods->size() > 1)
        L->Warnings.push_back("bitcode contains " +
                              std::to_string(Mods->size()) +
                              " modules; only the first is compared");
    } else {
      consumeError(Mods.takeError());
    }
    auto M = parseBitcodeFile(Ref, *L->Ctx);
    if (!M) {
      std::string E = toString(M.takeError());
      std::string H = newerHint(Major);
      R.Code = H.empty() ? ExitInput : ExitIncompatible;
      R.Error = Name.str() + ": cannot read bitcode: " + E;
      if (!H.empty())
        R.Error += "\n  note: " + H;
      return R;
    }
    L->M = std::move(*M);
    return finish(std::move(L), Verify);
  }
  if (const char *What = sniffNonIR(Text)) {
    R.Code = ExitInput;
    R.Error = Name.str() + ": unsupported input: looks like " + What +
              ", expected LLVM IR (.ll) or bitcode (.bc)";
    return R;
  }
  L->Info.Format = "text";
  SMDiagnostic Diag;
  L->M = parseAssembly(Ref, Diag, *L->Ctx);
  if (!L->M) {
    std::string Msg;
    raw_string_ostream OS(Msg);
    Diag.print("irdiff", OS, false);
    R.Code = ExitInput;
    R.Error = Msg;
    while (!R.Error.empty() && R.Error.back() == '\n')
      R.Error.pop_back();
    SmallVector<StringRef, 3> G;
    if (Regex("(clang version|LLVM version) ([0-9]+)").match(Text, &G) &&
        !Text.contains("Apple clang")) {
      std::string H = newerHint(std::atoi(G[2].str().c_str()));
      if (!H.empty()) {
        R.Error += "\n  note: " + H;
        R.Code = ExitIncompatible;
      }
    }
    return R;
  }
  return finish(std::move(L), Verify);
}

LoadResult loadModule(const std::string &Path, bool Verify) {
  auto Buf = MemoryBuffer::getFileOrSTDIN(Path, false, false);
  if (!Buf) {
    LoadResult R;
    R.Code = ExitInput;
    R.Error = (Path == "-" ? std::string("<stdin>") : Path) +
              ": cannot read: " + Buf.getError().message();
    return R;
  }
  std::string Name = Path == "-" ? "<stdin>" : Path;
  return loadModuleFromBuffer((*Buf)->getBuffer(), Name, Verify);
}

void checkCompat(ModuleDiff &D) {
  const InputInfo &A = D.A, &B = D.B;
  auto Note = [&](std::string S) { D.Compat.push_back(std::move(S)); };
  for (const InputInfo *I : {&A, &B})
    if (I->ProducerMajor > LLVM_VERSION_MAJOR)
      Note(I->Path + " was produced by a newer LLVM (" + I->Producer +
           ") than irdiff's LLVM " LLVM_VERSION_STRING
           "; it parsed, but newer IR features may be misrepresented");
  if (A.ProducerMajor && B.ProducerMajor && A.ProducerMajor != B.ProducerMajor)
    Note("inputs come from different LLVM major versions (" +
         std::to_string(A.ProducerMajor) + " vs " +
         std::to_string(B.ProducerMajor) +
         "); IR auto-upgrade may hide or introduce differences");
  if (A.Format != B.Format)
    Note("inputs use different formats (" + A.Format + " vs " + B.Format + ")");
  if (A.Triple != B.Triple) {
    Note("target triples differ ('" + A.Triple + "' vs '" + B.Triple +
         "'); type sizes, ABI attributes and intrinsics may legitimately "
         "differ");
    D.Incompatible = true;
  }
  if (A.DataLayout != B.DataLayout) {
    Note("data layouts differ; pointer and alignment-dependent IR may differ");
    D.Incompatible = true;
  }
}

} // namespace irdiff
