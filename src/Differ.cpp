#include "Compat.h"
#include "DiffDetail.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace irdiff {

using namespace detail;

static void wholeFunctionRows(ModuleCanon &C, FuncInfo &F, bool SideB,
                              FuncDiff &FD) {
  for (unsigned Blk = 0; Blk < F.Blocks.size(); ++Blk) {
    Row L;
    L.T = SideB ? Row::Add : Row::Del;
    (SideB ? L.Right : L.Left) = blockName(F, Blk) + ":";
    FD.Rows.push_back(L);
    for (unsigned I = F.Blocks[Blk].First; I < F.Blocks[Blk].End; ++I) {
      if (F.Insts[I].Debug)
        continue;
      Row R;
      R.T = SideB ? Row::Add : Row::Del;
      (SideB ? R.Right : R.Left) = "  " + instText(C, F, I);
      FD.Rows.push_back(R);
    }
  }
}

static std::string gname(ModuleCanon &C, GlobalValue &G) {
  return globalToken(G, C);
}

static void moduleLevel(ModuleDiff &D, Ctx &X) {
  Module &MA = X.CA.M, &MB = X.CB.M;
  auto Add = [&](Change C) { D.ModuleChanges.push_back(std::move(C)); };
  if (tripleOf(MA) != tripleOf(MB))
    Add(mk("module", "modified", Sig::Major, "", "", "target triple",
           tripleOf(MA), tripleOf(MB)));
  if (MA.getDataLayoutStr() != MB.getDataLayoutStr())
    Add(mk("module", "modified", Sig::Major, "", "", "data layout",
           MA.getDataLayoutStr(), MB.getDataLayoutStr()));
  if (MA.getSourceFileName() != MB.getSourceFileName())
    Add(mk("module", "modified", X.O.Strict ? Sig::Minor : Sig::Cosmetic, "",
           "", "source_filename", MA.getSourceFileName(),
           MB.getSourceFileName()));
  if (moduleAsm(MA) != moduleAsm(MB))
    Add(mk("module", "modified", Sig::Major, "", "", "module asm",
           moduleAsm(MA), moduleAsm(MB)));

  auto Flags = [&](Module &M, ModuleCanon &C) {
    std::map<std::string, std::string> R;
    SmallVector<Module::ModuleFlagEntry, 8> E;
    M.getModuleFlagsMetadata(E);
    for (auto &F : E)
      R[F.Key->getString().str()] = C.mdText(F.Val);
    return R;
  };
  auto FA = Flags(MA, X.CA), FB = Flags(MB, X.CB);
  auto FlagCat = [](StringRef K) {
    return K.contains("Debug") || K.contains("Dwarf") || K.contains("CodeView")
               ? "debug"
               : "config";
  };
  for (auto &[K, V] : FA) {
    if (StringRef(FlagCat(K)) == "debug" && !X.O.CompareDebug)
      continue;
    auto It = FB.find(K);
    if (It == FB.end())
      Add(mk(FlagCat(K), "removed", Sig::Minor, "", "",
             "module flag \"" + K + "\"", V));
    else if (It->second != V)
      Add(mk(FlagCat(K), "modified", Sig::Minor, "", "",
             "module flag \"" + K + "\"", V, It->second));
  }
  for (auto &[K, V] : FB)
    if (!FA.count(K) && (StringRef(FlagCat(K)) != "debug" || X.O.CompareDebug))
      Add(mk(FlagCat(K), "added", Sig::Minor, "", "",
             "module flag \"" + K + "\"", "", V));

  auto Named = [&](Module &M, ModuleCanon &C) {
    std::map<std::string, std::string> R;
    for (NamedMDNode &N : M.named_metadata()) {
      StringRef Name = N.getName();
      if (Name == "llvm.module.flags")
        continue;
      std::vector<std::string> Ops;
      for (MDNode *Op : N.operands())
        Ops.push_back(C.mdText(Op));
      if (Name == "llvm.ident" || Name == "llvm.commandline")
        llvm::sort(Ops);
      R[Name.str()] = join(Ops, ", ");
    }
    return R;
  };
  auto NA = Named(MA, X.CA), NB = Named(MB, X.CB);
  auto NamedCat = [&](StringRef K) -> std::pair<const char *, Sig> {
    if (K == "llvm.ident" || K == "llvm.commandline")
      return {"config", Sig::Minor};
    if (K.starts_with("llvm.dbg"))
      return {"debug", Sig::Minor};
    return {"metadata", Sig::Minor};
  };
  for (auto &[K, V] : NA) {
    auto [Cat, S] = NamedCat(K);
    if (Cat == StringRef("debug") && !X.O.CompareDebug)
      continue;
    auto It = NB.find(K);
    if (It == NB.end())
      Add(mk(Cat, "removed", S, "", "", "!" + K, V));
    else if (It->second != V)
      Add(mk(Cat, "modified", S, "", "", "!" + K, V, It->second));
  }
  for (auto &[K, V] : NB) {
    auto [Cat, S] = NamedCat(K);
    if (Cat == StringRef("debug") && !X.O.CompareDebug)
      continue;
    if (!NA.count(K))
      Add(mk(Cat, "added", S, "", "", "!" + K, "", V));
  }

  auto Types = [](Module &M) {
    std::map<std::string, std::string> R;
    for (StructType *T : M.getIdentifiedStructTypes()) {
      std::string S;
      raw_string_ostream OS(S);
      if (T->isOpaque())
        OS << "opaque";
      else
        T->print(OS, false, false);
      R[T->getName().str()] = S;
    }
    return R;
  };
  auto TA = Types(MA), TB = Types(MB);
  for (auto &[K, V] : TA) {
    auto It = TB.find(K);
    if (It == TB.end())
      Add(mk("type", "removed", Sig::Minor, "", "", "%" + K, V));
    else if (It->second != V)
      Add(mk("type", "modified", Sig::Major, "", "", "%" + K, V, It->second));
  }
  for (auto &[K, V] : TB)
    if (!TA.count(K))
      Add(mk("type", "added", Sig::Minor, "", "", "%" + K, "", V));
}

static void globals(ModuleDiff &D, Ctx &X, FuncPair &Mapper) {
  auto Add = [&](Change C) { D.ModuleChanges.push_back(std::move(C)); };
  auto Each = [&](auto RangeA, auto RangeB) {
    for (GlobalValue &G : RangeA) {
      auto It = X.GM.AtoB.find(&G);
      std::string NA = gname(X.CA, G);
      if (It == X.GM.AtoB.end()) {
        Add(mk("global", "removed", Sig::Major, "", "", NA,
               X.CA.globalLine(G)));
        continue;
      }
      GlobalValue &H = *It->second;
      std::string NB = gname(X.CB, H);
      if (NA != NB) {
        Change C = mk("global", "renamed",
                      G.hasLocalLinkage() ? Sig::Cosmetic : Sig::Major, "", "",
                      NA + " -> " + NB);
        C.Inferred = true;
        C.Details.push_back(NA.find('.') != std::string::npos || !G.hasName()
                                ? "matched by identical content (auto-numbered "
                                  "local symbol)"
                                : "matched by identical content");
        Add(C);
      }
      auto FA = globalFields(G, X.CA), FB = globalFields(H, X.CB);
      Change C = mk("global", "modified", Sig::Major, "", "", NA,
                    X.CA.globalLine(G), X.CB.globalLine(H));
      Sig Max = Sig::Cosmetic;
      for (unsigned I = 0; I < FA.size() && I < FB.size(); ++I) {
        std::string VB = Mapper.mapGlobals(FB[I].second);
        if (FA[I].second == VB)
          continue;
        const std::string &F = FA[I].first;
        if (F == "metadata" && !X.O.CompareMetadata)
          continue;
        if (F == "attributes" && !X.O.CompareAttributes)
          continue;
        Sig S = (F == "dso-local" || F == "unnamed-addr" || F == "metadata" ||
                 F == "partition")
                    ? Sig::Minor
                    : Sig::Major;
        Max = std::max(Max, S);
        std::string VA = FA[I].second.empty() ? "(none)" : FA[I].second;
        std::string Shown = FB[I].second.empty() ? "(none)" : FB[I].second;
        if (VA.size() > 120)
          VA = VA.substr(0, 117) + "...";
        if (Shown.size() > 120)
          Shown = Shown.substr(0, 117) + "...";
        C.Details.push_back(F + ": " + VA + " -> " + Shown);
      }
      if (!C.Details.empty()) {
        C.Significance = Max;
        Add(C);
      }
    }
    for (GlobalValue &H : RangeB)
      if (!X.GM.BtoA.count(&H))
        Add(mk("global", "added", Sig::Major, "", "", gname(X.CB, H), "",
               X.CB.globalLine(H)));
  };
  Each(X.CA.M.globals(), X.CB.M.globals());
  Each(X.CA.M.aliases(), X.CB.M.aliases());
  Each(X.CA.M.ifuncs(), X.CB.M.ifuncs());
}

static std::string declLine(Function &F, ModuleCanon &C) {
  std::string S;
  for (auto &[K, V] : headerFields(F, C))
    if (K == "return-type")
      S = V + " " + globalToken(F, C);
    else if (K == "parameters")
      S += "(" + V + ")";
  return S;
}

ModuleDiff diffModules(Loaded &LA, Loaded &LB, const DiffOptions &O) {
  ModuleDiff D;
  D.A = LA.Info;
  D.B = LB.Info;
  checkCompat(D);
  for (auto &W : LA.Warnings)
    D.Compat.push_back(LA.Info.Path + ": " + W);
  for (auto &W : LB.Warnings)
    D.Compat.push_back(LB.Info.Path + ": " + W);
  ModuleCanon CA(*LA.M, O), CB(*LB.M, O);
  GlobalMatch GM = matchGlobals(CA, CB);
  std::unique_ptr<AnalysisHost> HA, HB;
  if (O.RunAnalyses) {
    HA = std::make_unique<AnalysisHost>(*LA.M);
    HB = std::make_unique<AnalysisHost>(*LB.M);
  }
  Ctx X{CA, CB, GM, O, HA.get(), HB.get()};
  for (auto &N : GM.Ambiguous) {
    Change C = mk("match", "ambiguous", Sig::Minor, "", "", N);
    C.Confidence = "ambiguous";
    D.ModuleChanges.push_back(C);
  }
  moduleLevel(D, X);
  FuncInfo Dummy;
  FuncPair Mapper(CA, Dummy, CB, Dummy, GM);
  globals(D, X, Mapper);

  auto Wanted = [&](StringRef NA, StringRef NB) {
    return O.Functions.empty() || matchesAny(O.Functions, NA.str()) ||
           matchesAny(O.Functions, NB.str());
  };
  for (Function &F : *LA.M) {
    auto It = GM.AtoB.find(&F);
    Function *G = It == GM.AtoB.end() ? nullptr : cast<Function>(It->second);
    std::string NA = gname(CA, F), NB = G ? gname(CB, *G) : "";
    bool Decls = F.isDeclaration() && (!G || G->isDeclaration());
    if (O.ModuleOnly ? !Decls : !Wanted(NA, NB))
      continue;
    bool Intr = F.isIntrinsic();
    if (F.isDeclaration() && (!G || G->isDeclaration())) {
      if (!G) {
        D.ModuleChanges.push_back(mk(Intr ? "intrinsic" : "declaration",
                                     "removed", Intr ? Sig::Minor : Sig::Major,
                                     NA, "", NA, declLine(F, CA)));
        continue;
      }
      auto HA2 = headerFields(F, CA), HB2 = headerFields(*G, CB);
      for (unsigned I = 0; I < HA2.size(); ++I) {
        std::string VB = Mapper.mapGlobals(HB2[I].second);
        if (HA2[I].second == VB)
          continue;
        StringRef K = HA2[I].first;
        bool Attr = K.contains("attributes");
        if (Attr && !X.O.CompareAttributes && K != "config-attributes")
          continue;
        if (K == "metadata" && !X.O.CompareMetadata)
          continue;
        Change C = mk(Intr ? "intrinsic" : "declaration", "modified",
                      K == "config-attributes" || K == "metadata" ? Sig::Minor
                                                                  : Sig::Major,
                      NA, "", NA + " " + K.str(), HA2[I].second, HB2[I].second);
        if (K == "parameter-attributes")
          C.Details = paramDelta(HA2[I].second, VB);
        else if (Attr)
          C.Details = setDelta(HA2[I].second, VB);
        D.ModuleChanges.push_back(C);
      }
      continue;
    }
    FuncDiff FD;
    FD.NameA = NA;
    FD.NameB = NB;
    if (!G) {
      FD.Status = "removed";
      FD.Stats.InstsA = F.getInstructionCount();
      FD.Stats.BlocksA = F.size();
      FD.Changes.push_back(
          mk("function", "removed", Sig::Major, NA, "", NA, declLine(F, CA)));
      if (O.Rows)
        wholeFunctionRows(CA, CA.func(F), false, FD);
      D.Funcs.push_back(std::move(FD));
      continue;
    }
    if (F.isDeclaration() != G->isDeclaration()) {
      FD.Status = "modified";
      FD.Changes.push_back(
          mk("function", "modified", Sig::Major, NA, "", NA,
             F.isDeclaration() ? "declaration" : "definition",
             G->isDeclaration() ? "declaration" : "definition"));
      FD.Changes.back().Details.push_back(
          F.isDeclaration() ? "body is now defined in B"
                            : "body removed; only a declaration remains in B");
      if (O.Rows) {
        if (!F.isDeclaration())
          wholeFunctionRows(CA, CA.func(F), false, FD);
        else
          wholeFunctionRows(CB, CB.func(*G), true, FD);
      }
      D.Funcs.push_back(std::move(FD));
      continue;
    }
    FuncPair P(CA, CA.func(F), CB, CB.func(*G), GM);
    P.run();
    if (NA != NB) {
      Change C = mk("function", "renamed",
                    F.hasLocalLinkage() ? Sig::Minor : Sig::Major, NA, "",
                    NA + " -> " + NB);
      C.Inferred = true;
      C.Details.push_back("matched by structural content");
      FD.Changes.push_back(C);
    }
    FuncDiffer(X, P, FD).run();
    D.Funcs.push_back(std::move(FD));
  }
  for (Function &G : *LB.M) {
    if (GM.BtoA.count(&G))
      continue;
    std::string NB = gname(CB, G);
    if (O.ModuleOnly ? !G.isDeclaration() : !Wanted("", NB))
      continue;
    if (G.isDeclaration()) {
      bool Intr = G.isIntrinsic();
      D.ModuleChanges.push_back(mk(Intr ? "intrinsic" : "declaration", "added",
                                   Intr ? Sig::Minor : Sig::Major, NB, "", NB,
                                   "", declLine(G, CB)));
      continue;
    }
    FuncDiff FD;
    FD.NameB = NB;
    FD.Status = "added";
    FD.Stats.InstsB = G.getInstructionCount();
    FD.Stats.BlocksB = G.size();
    FD.Changes.push_back(
        mk("function", "added", Sig::Major, NB, "", NB, "", declLine(G, CB)));
    if (O.Rows)
      wholeFunctionRows(CB, CB.func(G), true, FD);
    D.Funcs.push_back(std::move(FD));
  }
  return D;
}

} // namespace irdiff
