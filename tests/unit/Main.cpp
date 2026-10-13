#include "Test.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdio>
#include <iostream>

using namespace irdiff;

namespace t {

std::vector<Case> &registry() {
  static std::vector<Case> R;
  return R;
}

void fail(const char *File, int Line, const std::string &Msg) {
  throw Failure{std::string(llvm::sys::path::filename(File)) + ":" +
                std::to_string(Line) + ": " + Msg};
}

std::string fixture(const std::string &Name) {
  return std::string(IRDIFF_FIXTURES) + "/" + Name;
}

static std::string ScratchDir;

std::string scratch(const std::string &Name) {
  if (ScratchDir.empty()) {
    llvm::SmallString<128> P;
    llvm::sys::fs::createUniqueDirectory("irdiff-tests", P);
    ScratchDir = std::string(P);
  }
  return ScratchDir + "/" + Name;
}

ModuleDiff diff(const std::string &A, const std::string &B, DiffOptions O) {
  LoadResult LA = loadModuleFromBuffer(A, "input.ll", true);
  LoadResult LB = loadModuleFromBuffer(B, "input.ll", true);
  if (!LA.L)
    fail(__FILE__, __LINE__, "A does not parse: " + LA.Error);
  if (!LB.L)
    fail(__FILE__, __LINE__, "B does not parse: " + LB.Error);
  O.Rows = true;
  return diffModules(*LA.L, *LB.L, O);
}

std::vector<const Change *> changes(const ModuleDiff &D) {
  std::vector<const Change *> R;
  for (auto &C : D.ModuleChanges)
    R.push_back(&C);
  for (auto &F : D.Funcs)
    for (auto &C : F.Changes)
      R.push_back(&C);
  for (auto &C : D.RemarkChanges)
    R.push_back(&C);
  for (auto &C : D.PassChanges)
    R.push_back(&C);
  return R;
}

unsigned count(const ModuleDiff &D, const std::string &Cat,
               const std::string &Kind) {
  unsigned N = 0;
  for (auto *C : changes(D))
    N += C->Cat == Cat && (Kind.empty() || C->Kind == Kind);
  return N;
}

static bool mentions(const Change &C, const std::string &T) {
  if (C.Subject.find(T) != std::string::npos ||
      C.Before.find(T) != std::string::npos ||
      C.After.find(T) != std::string::npos)
    return true;
  for (auto &D : C.Details)
    if (D.find(T) != std::string::npos)
      return true;
  return false;
}

const Change *find(const ModuleDiff &D, const std::string &Cat,
                   const std::string &Kind, const std::string &Text) {
  for (auto *C : changes(D))
    if (C->Cat == Cat && (Kind.empty() || C->Kind == Kind) &&
        (Text.empty() || mentions(*C, Text)))
      return C;
  return nullptr;
}

bool hasDetail(const Change *C, const std::string &Text) {
  if (!C)
    return false;
  for (auto &D : C->Details)
    if (D.find(Text) != std::string::npos)
      return true;
  return false;
}

std::string dump(const ModuleDiff &D) {
  std::string S;
  for (auto *C : changes(D)) {
    S += "\n  [" + C->Cat + "/" + C->Kind + "/" + sigName(C->Significance) +
         "] " + C->Subject;
    if (!C->Before.empty() || !C->After.empty())
      S += " {" + C->Before + " -> " + C->After + "}";
    for (auto &X : C->Details)
      S += "\n      " + X;
  }
  return S;
}

unsigned atLeast(const ModuleDiff &D, Sig S) {
  unsigned N = 0;
  for (auto *C : changes(D))
    N += C->Significance >= S;
  return N;
}

static std::string slurp(const std::string &P) {
  auto B = llvm::MemoryBuffer::getFile(P);
  return B ? (*B)->getBuffer().str() : "";
}

Run run(const std::vector<std::string> &Args, const std::string &Stdin) {
  static unsigned N = 0;
  ++N;
  std::string In = scratch("stdin" + std::to_string(N));
  std::string Out = scratch("stdout" + std::to_string(N));
  std::string Err = scratch("stderr" + std::to_string(N));
  {
    std::error_code EC;
    llvm::raw_fd_ostream OS(In, EC);
    OS << Stdin;
  }
  std::vector<llvm::StringRef> Argv = {IRDIFF_BINARY};
  for (auto &A : Args)
    Argv.push_back(A);
  std::optional<llvm::StringRef> Redir[] = {
      llvm::StringRef(In), llvm::StringRef(Out), llvm::StringRef(Err)};
  std::string ErrMsg;
  Run R;
  R.Exit = llvm::sys::ExecuteAndWait(IRDIFF_BINARY, Argv, std::nullopt, Redir,
                                     120, 0, &ErrMsg);
  R.Out = slurp(Out);
  R.Err = slurp(Err);
  return R;
}

} // namespace t

int main(int argc, char **argv) {
  std::string Filter = argc > 1 ? argv[1] : "";
  if (Filter == "--list") {
    for (auto &C : t::registry())
      std::cout << C.Suite << "." << C.Name << "\n";
    return 0;
  }
  unsigned Pass = 0, Fail = 0;
  for (auto &C : t::registry()) {
    if (!Filter.empty() && Filter != C.Suite &&
        Filter != C.Suite + "." + C.Name)
      continue;
    try {
      C.Fn();
      ++Pass;
      std::cout << "[  OK  ] " << C.Suite << "." << C.Name << "\n";
    } catch (const t::Failure &F) {
      ++Fail;
      std::cout << "[ FAIL ] " << C.Suite << "." << C.Name << "\n  " << F.Msg
                << "\n";
    }
  }
  std::cout << Pass << " passed, " << Fail << " failed\n";
  if (Pass + Fail == 0) {
    std::cout << "no tests matched '" << Filter << "'\n";
    return 1;
  }
  return Fail ? 1 : 0;
}
