#include "Match.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <map>

using namespace llvm;

namespace irdiff {

Pairs lcs(ArrayRef<uint64_t> A, ArrayRef<uint64_t> B, unsigned MaxD) {
  Pairs Out, Tail;
  unsigned P = 0;
  while (P < A.size() && P < B.size() && A[P] == B[P]) {
    Out.push_back({P, P});
    ++P;
  }
  unsigned EA = A.size(), EB = B.size();
  while (EA > P && EB > P && A[EA - 1] == B[EB - 1]) {
    --EA;
    --EB;
    Tail.push_back({EA, EB});
  }
  int N = EA - P, M = EB - P;
  if (N > 0 && M > 0) {
    ArrayRef<uint64_t> X = A.slice(P, N), Y = B.slice(P, M);
    int Max = N + M;
    std::vector<int> V(2 * Max + 3, 0);
    int Off = Max + 1;
    std::vector<std::vector<int>> Trace;
    int Found = -1;
    for (int D = 0; D <= Max && D <= int(MaxD); ++D) {
      for (int K = -D; K <= D; K += 2) {
        int XI = (K == -D || (K != D && V[Off + K - 1] < V[Off + K + 1]))
                     ? V[Off + K + 1]
                     : V[Off + K - 1] + 1;
        int YI = XI - K;
        while (XI < N && YI < M && X[XI] == Y[YI]) {
          ++XI;
          ++YI;
        }
        V[Off + K] = XI;
        if (XI >= N && YI >= M) {
          Found = D;
          break;
        }
      }
      Trace.emplace_back(V.begin() + Off - D, V.begin() + Off + D + 1);
      if (Found >= 0)
        break;
    }
    // >MaxD edits: middle left unmatched (linear-space Myers lifts cap)
    if (Found >= 0) {
      Pairs Mid;
      int XI = N, YI = M;
      for (int D = Found; D > 0; --D) {
        const std::vector<int> &PV = Trace[D - 1];
        auto At = [&](int K) { return PV[K + (D - 1)]; };
        int K = XI - YI;
        int PK = (K == -D || (K != D && At(K - 1) < At(K + 1))) ? K + 1 : K - 1;
        int PX = At(PK), PY = PX - PK;
        while (XI > PX && YI > PY) {
          --XI;
          --YI;
          Mid.push_back({P + XI, P + YI});
        }
        XI = PX;
        YI = PY;
      }
      while (XI > 0 && YI > 0) {
        --XI;
        --YI;
        Mid.push_back({P + XI, P + YI});
      }
      Out.insert(Out.end(), Mid.rbegin(), Mid.rend());
    }
  }
  Out.insert(Out.end(), Tail.rbegin(), Tail.rend());
  return Out;
}

double similarity(ArrayRef<uint64_t> A, ArrayRef<uint64_t> B) {
  if (A.empty() && B.empty())
    return 1.0;
  return 2.0 * lcs(A, B, 400).size() / double(A.size() + B.size());
}

std::string globalToken(const GlobalValue &G, ModuleCanon &C) {
  std::string S;
  raw_string_ostream OS(S);
  G.printAsOperand(OS, false, C.mst());
  return S;
}

static bool autoNamed(const GlobalValue &G) {
  if (!G.hasLocalLinkage())
    return false;
  if (!G.hasName())
    return true;
  StringRef N = G.getName();
  if (N.starts_with("."))
    return true;
  size_t D = N.rfind('.');
  return D != StringRef::npos && D + 1 < N.size() &&
         all_of(N.substr(D + 1), isDigit);
}

static StringRef baseName(StringRef N) {
  size_t L = N.find(".llvm.");
  if (L != StringRef::npos)
    N = N.substr(0, L);
  while (true) {
    size_t D = N.rfind('.');
    if (D == StringRef::npos || D + 1 >= N.size() ||
        !all_of(N.substr(D + 1), isDigit))
      return N;
    N = N.substr(0, D);
  }
}

static uint64_t funcHash(ModuleCanon &C, Function &F) {
  if (F.isDeclaration())
    return 0;
  FuncInfo &FI = C.func(F);
  if (FI.Hash)
    return FI.Hash;
  std::string All;
  for (InstInfo &I : FI.Insts) {
    if (I.Debug)
      continue;
    All +=
        rewrite(I.Canon, [](Tok T, unsigned, StringRef Raw, std::string &Out) {
          Out += T == Tok::Global ? Raw : StringRef("%_");
        });
    All += '\n';
  }
  FI.Hash = hashStr(All) | 1;
  return FI.Hash;
}

static uint64_t contentKey(ModuleCanon &C, GlobalValue &G) {
  if (auto *F = dyn_cast<Function>(&G))
    return funcHash(C, *F);
  if (G.isDeclaration())
    return 0;
  std::string K;
  for (auto &[Field, Val] : globalFields(G, C))
    if (Field == "value-type" || Field == "initializer" ||
        Field == "constant" || Field == "aliasee" || Field == "section" ||
        Field == "resolver")
      K += Val + "\x1f";
  return hashStr(K) | 1;
}

template <typename RA, typename RB>
static void matchKind(RA RangeA, RB RangeB, ModuleCanon &CA, ModuleCanon &CB,
                      GlobalMatch &GM) {
  std::vector<GlobalValue *> LA, LB;
  for (auto &G : RangeA)
    LA.push_back(&G);
  for (auto &G : RangeB)
    LB.push_back(&G);
  auto Link = [&](GlobalValue *A, GlobalValue *B) {
    GM.AtoB[A] = B;
    GM.BtoA[B] = A;
    if (A->getName() != B->getName() || !A->hasName())
      GM.Renamed.push_back({A, B});
  };
  auto Free = [&](GlobalValue *G, bool SideB) {
    return SideB ? !GM.BtoA.count(G) : !GM.AtoB.count(G);
  };
  StringMap<GlobalValue *> ByName;
  for (GlobalValue *B : LB)
    if (B->hasName())
      ByName[B->getName()] = B;

  for (GlobalValue *A : LA)
    if (A->hasName() && !autoNamed(*A))
      if (GlobalValue *B = ByName.lookup(A->getName()))
        if (Free(B, true))
          Link(A, B);

  auto Unique = [&](auto KeyFn, bool Renamed) {
    std::map<std::string,
             std::pair<std::vector<GlobalValue *>, std::vector<GlobalValue *>>>
        Buckets;
    for (GlobalValue *A : LA)
      if (Free(A, false)) {
        std::string K = KeyFn(CA, *A);
        if (!K.empty())
          Buckets[K].first.push_back(A);
      }
    for (GlobalValue *B : LB)
      if (Free(B, true)) {
        std::string K = KeyFn(CB, *B);
        if (!K.empty())
          Buckets[K].second.push_back(B);
      }
    for (auto &[K, P] : Buckets) {
      if (P.first.size() == 1 && P.second.size() == 1)
        Link(P.first[0], P.second[0]);
      else if (Renamed && !P.first.empty() && !P.second.empty())
        GM.Ambiguous.push_back(std::to_string(P.first.size()) +
                               " candidate(s) in A (" +
                               globalToken(*P.first[0], CA) + ", ...) and " +
                               std::to_string(P.second.size()) + " in B (" +
                               globalToken(*P.second[0], CB) +
                               ", ...) have identical content; left unmatched");
    }
  };
  Unique(
      [](ModuleCanon &C, GlobalValue &G) -> std::string {
        if (!autoNamed(G))
          return "";
        uint64_t H = contentKey(C, G);
        return H ? baseName(G.getName()).str() + "\x1f" + utostr(H) : "";
      },
      false);
  for (GlobalValue *A : LA)
    if (Free(A, false) && A->hasName())
      if (GlobalValue *B = ByName.lookup(A->getName()))
        if (Free(B, true))
          Link(A, B);
  Unique(
      [](ModuleCanon &C, GlobalValue &G) -> std::string {
        if (!G.hasLocalLinkage())
          return "";
        uint64_t H = contentKey(C, G);
        return H ? utostr(H) : "";
      },
      true);
  Unique(
      [](ModuleCanon &, GlobalValue &G) -> std::string {
        if (!G.hasName() || G.isDeclaration())
          return "";
        StringRef B = baseName(G.getName());
        return B == G.getName() && !G.getName().contains(".llvm.") ? ""
                                                                   : B.str();
      },
      false);
}

GlobalMatch matchGlobals(ModuleCanon &A, ModuleCanon &B) {
  GlobalMatch GM;
  matchKind(A.M.functions(), B.M.functions(), A, B, GM);
  matchKind(A.M.globals(), B.M.globals(), A, B, GM);
  matchKind(A.M.aliases(), B.M.aliases(), A, B, GM);
  matchKind(A.M.ifuncs(), B.M.ifuncs(), A, B, GM);
  StringSet<> ATokens;
  for (GlobalValue &G : A.M.global_values())
    ATokens.insert(globalToken(G, A));
  for (auto &[GA, GB] : GM.Renamed) {
    std::string TA = globalToken(*GA, A), TB = globalToken(*GB, B);
    if (TA != TB)
      GM.TokBtoA[TB] = TA;
  }
  for (GlobalValue &G : B.M.global_values())
    if (!GM.BtoA.count(&G)) {
      std::string T = globalToken(G, B);
      if (ATokens.count(T))
        GM.TokBtoA[T] = T + "{B}";
    }
  return GM;
}

FuncPair::FuncPair(ModuleCanon &CA, FuncInfo &A, ModuleCanon &CB, FuncInfo &B,
                   const GlobalMatch &G)
    : CA(CA), CB(CB), A(A), B(B), G(G) {
  VA.assign(A.LocalNames.size(), -1);
  VB.assign(B.LocalNames.size(), -1);
  for (unsigned I = 0; I < A.NumArgs && I < B.NumArgs; ++I) {
    VA[I] = I;
    VB[I] = I;
  }
}

std::string FuncPair::mapGlobals(StringRef S) const {
  return rewrite(S, [&](Tok T, unsigned, StringRef Raw, std::string &Out) {
    if (T == Tok::Global) {
      auto It = G.TokBtoA.find(Raw);
      Out += It == G.TokBtoA.end() ? Raw.str() : It->second;
    } else {
      Out += Raw;
    }
  });
}

std::string FuncPair::strictText(bool SideB, StringRef Canon) const {
  const FuncInfo &F = SideB ? B : A;
  const std::vector<int> &V = SideB ? VB : VA;
  return rewrite(
      Canon, [&](Tok T, unsigned Id, StringRef Raw, std::string &Out) {
        switch (T) {
        case Tok::Local:
          if (Id < V.size() && V[Id] >= 0)
            Out += "%" + utostr(SideB ? Id : unsigned(V[Id]));
          else
            Out += (SideB ? "%b" : "%a") + utostr(Id);
          return;
        case Tok::Block:
          if (Id < F.Blocks.size() && F.Blocks[Id].Match >= 0)
            Out += "%^" + utostr(SideB ? Id : unsigned(F.Blocks[Id].Match));
          else
            Out += (SideB ? "%^b" : "%^a") + utostr(Id);
          return;
        case Tok::Global:
          if (SideB) {
            auto It = G.TokBtoA.find(Raw);
            Out += It == G.TokBtoA.end() ? Raw.str() : It->second;
          } else {
            Out += Raw;
          }
          return;
        }
      });
}

std::string FuncPair::strictA(unsigned I) const {
  const InstInfo &II = A.Insts[I];
  std::string S = strictText(false, II.Canon);
  if (!II.Md.empty())
    S += " | " + strictText(false, II.Md);
  return S;
}

std::string FuncPair::strictB(unsigned I) const {
  const InstInfo &II = B.Insts[I];
  std::string S = strictText(true, II.Canon);
  if (!II.Md.empty())
    S += " | " + strictText(true, II.Md);
  return S;
}

std::string FuncPair::key(bool SideB, unsigned I, bool Wild) const {
  const FuncInfo &F = SideB ? B : A;
  const std::vector<int> &V = SideB ? VB : VA;
  return rewrite(F.Insts[I].Canon, [&](Tok T, unsigned Id, StringRef Raw,
                                       std::string &Out) {
    if (T == Tok::Local) {
      if (Id < V.size() && V[Id] >= 0)
        Out += "%" + utostr(SideB ? Id : unsigned(V[Id]));
      else
        Out += Wild ? std::string("%_") : (SideB ? "%b" : "%a") + utostr(Id);
    } else if (T == Tok::Block) {
      int M = F.Blocks[Id].Match;
      if (M >= 0)
        Out += "%^" + utostr(SideB ? Id : unsigned(M));
      else
        Out += Wild ? std::string("%_") : (SideB ? "%^b" : "%^a") + utostr(Id);
    } else if (SideB) {
      auto It = G.TokBtoA.find(Raw);
      Out += It == G.TokBtoA.end() ? Raw.str() : It->second;
    } else {
      Out += Raw;
    }
  });
}

std::vector<uint64_t> FuncPair::blockShapes(bool SideB, unsigned Blk) const {
  const FuncInfo &F = SideB ? B : A;
  std::vector<uint64_t> R;
  for (unsigned I = F.Blocks[Blk].First; I < F.Blocks[Blk].End; ++I)
    if (!F.Insts[I].Debug)
      R.push_back(F.Insts[I].Shape);
  return R;
}

void FuncPair::link(unsigned IA, unsigned IB, How H) {
  A.Insts[IA].Match = IB;
  A.Insts[IA].How = H;
  B.Insts[IB].Match = IA;
  B.Insts[IB].How = H;
  VA[A.idOf(IA)] = B.idOf(IB);
  VB[B.idOf(IB)] = A.idOf(IA);
}

void FuncPair::unlink(unsigned IA) {
  int IB = A.Insts[IA].Match;
  if (IB < 0)
    return;
  A.Insts[IA].Match = -1;
  B.Insts[IB].Match = -1;
  VA[A.idOf(IA)] = -1;
  VB[B.idOf(IB)] = -1;
}

void FuncPair::run() {
  auto Shapes = [&](bool SideB) {
    FuncInfo &F = SideB ? B : A;
    for (InstInfo &I : F.Insts) {
      std::string S = rewrite(
          I.Canon, [&](Tok T, unsigned, StringRef Raw, std::string &Out) {
            if (T != Tok::Global) {
              Out += "%_";
            } else if (SideB) {
              auto It = G.TokBtoA.find(Raw);
              Out += It == G.TokBtoA.end() ? Raw.str() : It->second;
            } else {
              Out += Raw;
            }
          });
      I.Shape = hashStr(S);
    }
    for (unsigned BI = 0; BI < F.Blocks.size(); ++BI) {
      auto Seq = blockShapes(SideB, BI);
      F.Blocks[BI].Hash = hashStr(StringRef(
          reinterpret_cast<const char *>(Seq.data()), Seq.size() * 8));
    }
  };
  Shapes(false);
  Shapes(true);
  matchBlocks();
  for (unsigned R = 0; R < 3; ++R)
    alignBlocks(R);
  crossMatch();
  contentAlign();
  crossMatch();
  positional();
  crossMatch();
  while (defUse(true))
    crossMatch();
}

// align leftovers of unmatched blks w/ blk holding most of them
void FuncPair::contentAlign() {
  for (int Side = 0; Side < 2; ++Side) {
    bool SB = Side == 1;
    FuncInfo &F = SB ? B : A, &O = SB ? A : B;
    std::vector<std::vector<unsigned>> Other(O.Blocks.size());
    std::vector<std::vector<uint64_t>> OtherK(O.Blocks.size());
    for (unsigned I = 0; I < O.Insts.size(); ++I)
      if (O.Insts[I].Match < 0 && !O.Insts[I].Debug) {
        Other[O.Insts[I].Block].push_back(I);
        OtherK[O.Insts[I].Block].push_back(hashStr(key(!SB, I, true)));
      }
    for (unsigned X = 0; X < F.Blocks.size(); ++X) {
      if (F.Blocks[X].Match >= 0)
        continue;
      std::vector<unsigned> Mine;
      std::vector<uint64_t> MineK;
      for (unsigned I = F.Blocks[X].First; I < F.Blocks[X].End; ++I)
        if (F.Insts[I].Match < 0 && !F.Insts[I].Debug) {
          Mine.push_back(I);
          MineK.push_back(hashStr(key(SB, I, true)));
        }
      if (Mine.empty())
        continue;
      int Best = -1;
      Pairs BestP;
      for (unsigned Y = 0; Y < O.Blocks.size(); ++Y) {
        if (Other[Y].empty())
          continue;
        Pairs P = lcs(MineK, OtherK[Y], 400);
        if (P.size() > BestP.size()) {
          BestP = std::move(P);
          Best = Y;
        }
      }
      if (Best < 0 || BestP.size() * 2 < Mine.size())
        continue;
      for (auto [I, J] : BestP) {
        unsigned Mi = Mine[I], Oj = Other[Best][J];
        if (SB)
          link(Oj, Mi, HowPosition);
        else
          link(Mi, Oj, HowPosition);
      }
      std::vector<unsigned> Keep;
      std::vector<uint64_t> KeepK;
      for (unsigned J = 0; J < Other[Best].size(); ++J)
        if ((SB ? A : B).Insts[Other[Best][J]].Match < 0) {
          Keep.push_back(Other[Best][J]);
          KeepK.push_back(OtherK[Best][J]);
        }
      Other[Best] = std::move(Keep);
      OtherK[Best] = std::move(KeepK);
    }
  }
}

void FuncPair::matchBlocks() {
  auto &BA = A.Blocks, &BB = B.Blocks;
  if (BA.empty() || BB.empty())
    return;
  std::vector<std::vector<uint64_t>> SA(BA.size()), SB(BB.size());
  for (unsigned I = 0; I < BA.size(); ++I)
    SA[I] = blockShapes(false, I);
  for (unsigned I = 0; I < BB.size(); ++I)
    SB[I] = blockShapes(true, I);
  std::vector<std::pair<unsigned, unsigned>> Work;
  auto Link = [&](unsigned X, unsigned Y, float S, bool Amb) {
    BA[X].Match = Y;
    BB[Y].Match = X;
    BA[X].Score = BB[Y].Score = S;
    BA[X].Ambiguous = BB[Y].Ambiguous = Amb;
    Work.push_back({X, Y});
  };
  auto Sim = [&](unsigned X, unsigned Y) {
    if (BA[X].Hash == BB[Y].Hash)
      return 1.0;
    return similarity(SA[X], SB[Y]);
  };
  Link(0, 0, float(Sim(0, 0)), false);

  std::map<uint64_t, std::pair<std::vector<unsigned>, std::vector<unsigned>>>
      ByHash;
  for (unsigned I = 1; I < BA.size(); ++I)
    ByHash[BA[I].Hash].first.push_back(I);
  for (unsigned I = 1; I < BB.size(); ++I)
    ByHash[BB[I].Hash].second.push_back(I);
  for (auto &[H, P] : ByHash)
    if (P.first.size() == 1 && P.second.size() == 1)
      Link(P.first[0], P.second[0], 1.0f, false);

  auto Numeric = [](StringRef L) {
    return L.size() > 1 && all_of(L.drop_front(), isDigit);
  };
  StringMap<unsigned> LabelB;
  for (unsigned I = 0; I < BB.size(); ++I)
    if (BB[I].Match < 0 && !Numeric(BB[I].Label))
      LabelB[BB[I].Label] = I;
  for (unsigned I = 0; I < BA.size(); ++I) {
    if (BA[I].Match >= 0 || Numeric(BA[I].Label))
      continue;
    auto It = LabelB.find(BA[I].Label);
    if (It == LabelB.end() || BB[It->second].Match >= 0)
      continue;
    double S = Sim(I, It->second);
    if (S >= 0.25)
      Link(I, It->second, float(S), false);
  }

  auto Propagate = [&]() {
    while (!Work.empty()) {
      auto [X, Y] = Work.back();
      Work.pop_back();
      auto &SX = BA[X].Succ, &SY = BB[Y].Succ;
      if (SX.size() == SY.size()) {
        for (unsigned K = 0; K < SX.size(); ++K)
          if (BA[SX[K]].Match < 0 && BB[SY[K]].Match < 0) {
            double S = Sim(SX[K], SY[K]);
            if (S >= 0.4)
              Link(SX[K], SY[K], float(S), false);
          }
      }
      for (unsigned XS : SX) {
        if (BA[XS].Match >= 0)
          continue;
        int Best = -1;
        double BS = 0, Second = 0;
        for (unsigned YS : SY) {
          if (BB[YS].Match >= 0)
            continue;
          double S = Sim(XS, YS);
          if (S > BS) {
            Second = BS;
            BS = S;
            Best = YS;
          } else if (S > Second) {
            Second = S;
          }
        }
        if (Best >= 0 && BS >= 0.6 && BS - Second > 0.05)
          Link(XS, Best, float(BS), false);
      }
      std::vector<unsigned> PX, PY;
      for (unsigned P : BA[X].Pred)
        if (BA[P].Match < 0)
          PX.push_back(P);
      for (unsigned P : BB[Y].Pred)
        if (BB[P].Match < 0)
          PY.push_back(P);
      if (PX.size() == 1 && PY.size() == 1) {
        double S = Sim(PX[0], PY[0]);
        if (S >= 0.4)
          Link(PX[0], PY[0], float(S), false);
      }
    }
  };
  for (auto &P : BA)
    if (P.Match >= 0)
      Work.push_back({unsigned(&P - BA.data()), unsigned(P.Match)});
  Propagate();

  std::vector<unsigned> UA, UB;
  for (unsigned I = 0; I < BA.size(); ++I)
    if (BA[I].Match < 0)
      UA.push_back(I);
  for (unsigned I = 0; I < BB.size(); ++I)
    if (BB[I].Match < 0)
      UB.push_back(I);
  if (UA.empty() || UB.empty())
    return;
  // O(n*m) cand scan; pruned by term kind when large
  bool Prune = UA.size() * UB.size() > 40000;
  struct Cand {
    float Score;
    unsigned X, Y;
  };
  std::vector<Cand> Cands;
  auto Neighbors = [&](unsigned X, unsigned Y) {
    unsigned Tot = 0, Hit = 0;
    for (unsigned P : BA[X].Pred)
      if (BA[P].Match >= 0) {
        ++Tot;
        Hit += is_contained(BB[Y].Pred, unsigned(BA[P].Match));
      }
    for (unsigned S : BA[X].Succ)
      if (BA[S].Match >= 0) {
        ++Tot;
        Hit += is_contained(BB[Y].Succ, unsigned(BA[S].Match));
      }
    return Tot ? double(Hit) / Tot : 0.0;
  };
  for (unsigned X : UA)
    for (unsigned Y : UB) {
      if (Prune && BA[X].BB->getTerminator()->getOpcode() !=
                       BB[Y].BB->getTerminator()->getOpcode())
        continue;
      double S = Sim(X, Y);
      if (S < 0.3)
        continue;
      double Score = 0.7 * S + 0.3 * Neighbors(X, Y);
      if (Score >= 0.45)
        Cands.push_back({float(Score), X, Y});
    }
  std::stable_sort(
      Cands.begin(), Cands.end(),
      [](const Cand &L, const Cand &R) { return L.Score > R.Score; });
  for (unsigned I = 0; I < Cands.size(); ++I) {
    const Cand &C = Cands[I];
    if (BA[C.X].Match >= 0 || BB[C.Y].Match >= 0)
      continue;
    bool Amb = false;
    for (unsigned J = 0; J < Cands.size(); ++J) {
      const Cand &D = Cands[J];
      if (J == I || D.Score < C.Score - 0.05f)
        continue;
      if ((D.X == C.X && BB[D.Y].Match < 0) ||
          (D.Y == C.Y && BA[D.X].Match < 0)) {
        Amb = true;
        break;
      }
    }
    Link(C.X, C.Y, C.Score, Amb);
  }
  Propagate();
}

void FuncPair::alignBlocks(unsigned Round) {
  for (unsigned X = 0; X < A.Blocks.size(); ++X) {
    int Y = A.Blocks[X].Match;
    if (Y < 0)
      continue;
    std::vector<unsigned> IA, IB;
    std::vector<uint64_t> KA, KB;
    for (unsigned I = A.Blocks[X].First; I < A.Blocks[X].End; ++I)
      if (!A.Insts[I].Debug) {
        IA.push_back(I);
        KA.push_back(Round == 0 ? A.Insts[I].Shape
                                : hashStr(key(false, I, true)));
      }
    for (unsigned I = B.Blocks[Y].First; I < B.Blocks[Y].End; ++I)
      if (!B.Insts[I].Debug) {
        IB.push_back(I);
        KB.push_back(Round == 0 ? B.Insts[I].Shape
                                : hashStr(key(true, I, true)));
      }
    for (unsigned I : IA)
      unlink(I);
    for (auto [P, Q] : lcs(KA, KB))
      link(IA[P], IB[Q], HowAligned);
  }
}

void FuncPair::crossMatch() {
  for (unsigned Iter = 0; Iter < 16; ++Iter) {
    bool Progress = false;
    std::map<uint64_t, std::pair<std::vector<unsigned>, std::vector<unsigned>>>
        Buckets;
    for (unsigned I = 0; I < A.Insts.size(); ++I)
      if (A.Insts[I].Match < 0 && !A.Insts[I].Debug)
        Buckets[hashStr(key(false, I, false))].first.push_back(I);
    for (unsigned I = 0; I < B.Insts.size(); ++I)
      if (B.Insts[I].Match < 0 && !B.Insts[I].Debug)
        Buckets[hashStr(key(true, I, false))].second.push_back(I);
    for (auto &[H, P] : Buckets)
      if (P.first.size() == 1 && P.second.size() == 1) {
        link(P.first[0], P.second[0], HowUnique);
        Progress = true;
      }
    if (defUse(false))
      Progress = true;
    if (!Progress)
      break;
  }
}

bool FuncPair::defUse(bool Loose) {
  auto &IdxA = A.InstIdx, &IdxB = B.InstIdx;
  std::map<unsigned, unsigned> Proposal;
  std::map<unsigned, unsigned> Claims;
  for (unsigned I = 0; I < A.Insts.size(); ++I) {
    InstInfo &AI = A.Insts[I];
    if (AI.Match >= 0 || AI.Debug || AI.I->getType()->isVoidTy())
      continue;
    int Cand = -1;
    bool Conflict = false;
    for (const Use &U : AI.I->uses()) {
      auto *UI = dyn_cast<Instruction>(U.getUser());
      if (!UI)
        continue;
      auto It = IdxA.find(UI);
      if (It == IdxA.end() || A.Insts[It->second].Match < 0)
        continue;
      Instruction *UB = B.Insts[A.Insts[It->second].Match].I;
      Value *Op = nullptr;
      if (auto *PA = dyn_cast<PHINode>(UI)) {
        auto *PB = dyn_cast<PHINode>(UB);
        int BlkA = A.BlockIdx.lookup(PA->getIncomingBlock(U));
        int BlkB = A.Blocks[BlkA].Match;
        int K =
            PB && BlkB >= 0 ? PB->getBasicBlockIndex(B.Blocks[BlkB].BB) : -1;
        if (K >= 0)
          Op = PB->getIncomingValue(K);
      } else if (U.getOperandNo() < UB->getNumOperands()) {
        Op = UB->getOperand(U.getOperandNo());
      }
      auto *OI = dyn_cast_or_null<Instruction>(Op);
      if (!OI)
        continue;
      auto JIt = IdxB.find(OI);
      if (JIt == IdxB.end() || B.Insts[JIt->second].Match >= 0 ||
          B.Insts[JIt->second].Debug)
        continue;
      Instruction *BI = OI;
      bool Ok =
          BI->getOpcode() == AI.I->getOpcode() ||
          (Loose && ((isa<BinaryOperator>(BI) && isa<BinaryOperator>(AI.I)) ||
                     (isa<CastInst>(BI) && isa<CastInst>(AI.I)) ||
                     (isa<CmpInst>(BI) && isa<CmpInst>(AI.I))));
      if (!Ok)
        continue;
      if (Cand < 0)
        Cand = int(JIt->second);
      else if (Cand != int(JIt->second))
        Conflict = true;
    }
    if (Cand >= 0 && !Conflict) {
      Proposal[I] = unsigned(Cand);
      ++Claims[unsigned(Cand)];
    }
  }
  bool Any = false;
  for (auto [I, J] : Proposal)
    if (Claims[J] == 1) {
      link(I, J, Loose ? HowDefUseLoose : HowDefUse);
      Any = true;
    }
  return Any;
}

void FuncPair::positional() {
  auto OpKey = [](const InstInfo &I) {
    std::string S = I.I->getOpcodeName();
    StringRef C = I.Canon;
    size_t Eq = C.find(" = ");
    StringRef Rest = Eq == StringRef::npos ? C : C.substr(Eq + 3);
    std::pair<StringRef, StringRef> Parts = Rest.split(' ');
    if (Parts.first == S)
      S += ' ' + Parts.second.split(' ').first.str();
    return hashStr(S);
  };
  for (unsigned X = 0; X < A.Blocks.size(); ++X) {
    int Y = A.Blocks[X].Match;
    if (Y < 0)
      continue;
    const BlockInfo &BX = A.Blocks[X], &BY = B.Blocks[Y];
    std::vector<std::pair<unsigned, unsigned>> Anchors;
    unsigned LastB = BY.First;
    for (unsigned I = BX.First; I < BX.End; ++I) {
      int M = A.Insts[I].Match;
      if (M >= int(BY.First) && M < int(BY.End) && unsigned(M) >= LastB) {
        Anchors.push_back({I, unsigned(M)});
        LastB = M;
      }
    }
    std::map<unsigned, std::pair<std::vector<unsigned>, std::vector<unsigned>>>
        Gaps;
    unsigned G = 0;
    for (unsigned I = BX.First; I < BX.End; ++I) {
      if (G < Anchors.size() && Anchors[G].first == I) {
        ++G;
        continue;
      }
      if (A.Insts[I].Match < 0 && !A.Insts[I].Debug)
        Gaps[G].first.push_back(I);
    }
    G = 0;
    for (unsigned I = BY.First; I < BY.End; ++I) {
      if (G < Anchors.size() && Anchors[G].second == I) {
        ++G;
        continue;
      }
      if (B.Insts[I].Match < 0 && !B.Insts[I].Debug)
        Gaps[G].second.push_back(I);
    }
    for (auto &[Gap, P] : Gaps) {
      if (P.first.empty() || P.second.empty())
        continue;
      std::vector<uint64_t> KA, KB;
      for (unsigned I : P.first)
        KA.push_back(OpKey(A.Insts[I]));
      for (unsigned I : P.second)
        KB.push_back(OpKey(B.Insts[I]));
      for (auto [Q, R] : lcs(KA, KB))
        link(P.first[Q], P.second[R], HowPosition);
    }
  }
}

} // namespace irdiff
