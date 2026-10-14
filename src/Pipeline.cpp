#include "Pipeline.h"

#include <map>

using namespace llvm;

namespace irdiff {

std::vector<std::string> flattenPipeline(StringRef T) {
  std::vector<std::string> Out, Scope;
  std::string Cur;
  int Angle = 0;
  for (char C : T) {
    if (C == '<')
      ++Angle;
    if (C == '>')
      --Angle;
    if (Angle > 0 || (C != '(' && C != ')' && C != ',')) {
      Cur += C;
      continue;
    }
    std::string Prefix;
    for (auto &S : Scope)
      Prefix += S + "/";
    if (C == '(') {
      Scope.push_back(Cur);
    } else if (!Cur.empty()) {
      Out.push_back(Prefix + Cur);
    }
    if (C == ')' && !Scope.empty())
      Scope.pop_back();
    Cur.clear();
  }
  if (!Cur.empty())
    Out.push_back(Cur);
  return Out;
}

void diffPipelines(const PipelineResult &A, const PipelineResult &B,
                   ModuleDiff &D) {
  auto FA = flattenPipeline(A.PipelineText),
       FB = flattenPipeline(B.PipelineText);
  std::vector<uint64_t> HA, HB;
  for (auto &S : FA)
    HA.push_back(hashStr(S));
  for (auto &S : FB)
    HB.push_back(hashStr(S));
  std::vector<bool> KA(FA.size()), KB(FB.size());
  for (auto [I, J] : lcs(HA, HB)) {
    KA[I] = true;
    KB[J] = true;
  }
  auto Mk = [](StringRef Kind, std::string Subject, Sig S) {
    Change C;
    C.Cat = "pass";
    C.Kind = Kind.str();
    C.Subject = std::move(Subject);
    C.Significance = S;
    return C;
  };
  for (unsigned I = 0; I < FA.size(); ++I)
    if (!KA[I]) {
      Change C = Mk("removed", FA[I], Sig::Major);
      C.Details.push_back("pipeline position " + std::to_string(I + 1) +
                          " in A; absent at this position in B");
      D.PassChanges.push_back(C);
    }
  for (unsigned I = 0; I < FB.size(); ++I)
    if (!KB[I]) {
      Change C = Mk("added", FB[I], Sig::Major);
      C.Details.push_back("pipeline position " + std::to_string(I + 1) +
                          " in B; absent at this position in A");
      D.PassChanges.push_back(C);
    }
  std::map<std::string, std::pair<unsigned, unsigned>> Exec;
  for (auto &R : A.Runs)
    ++Exec[R.Pass].first;
  for (auto &R : B.Runs)
    ++Exec[R.Pass].second;
  std::map<std::string, std::pair<unsigned, unsigned>> Chg;
  for (auto &R : A.Runs)
    Chg[R.Pass].first += R.Changed;
  for (auto &R : B.Runs)
    Chg[R.Pass].second += R.Changed;
  for (auto &[P, N] : Exec)
    if (N.first != N.second || Chg[P].first != Chg[P].second) {
      Change C = Mk("modified", P, Sig::Minor);
      C.Before = std::to_string(N.first) + " runs, " +
                 std::to_string(Chg[P].first) + " changed IR";
      C.After = std::to_string(N.second) + " runs, " +
                std::to_string(Chg[P].second) + " changed IR";
      C.Details.push_back("executions: " + C.Before + " -> " + C.After);
      D.PassChanges.push_back(C);
    }
}

} // namespace irdiff
