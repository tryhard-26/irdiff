#pragma once

#include "Differ.h"
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace t {

struct Case {
  std::string Suite, Name;
  std::function<void()> Fn;
};
std::vector<Case> &registry();
struct Reg {
  Reg(const char *S, const char *N, std::function<void()> F) {
    registry().push_back({S, N, std::move(F)});
  }
};
struct Failure {
  std::string Msg;
};
[[noreturn]] void fail(const char *File, int Line, const std::string &Msg);

std::string fixture(const std::string &Name);
std::string scratch(const std::string &Name);

irdiff::ModuleDiff diff(const std::string &A, const std::string &B,
                        irdiff::DiffOptions O = {});
std::vector<const irdiff::Change *> changes(const irdiff::ModuleDiff &D);
unsigned count(const irdiff::ModuleDiff &D, const std::string &Cat,
               const std::string &Kind = "");
const irdiff::Change *find(const irdiff::ModuleDiff &D, const std::string &Cat,
                           const std::string &Kind,
                           const std::string &Text = "");
bool hasDetail(const irdiff::Change *C, const std::string &Text);
std::string dump(const irdiff::ModuleDiff &D);
unsigned atLeast(const irdiff::ModuleDiff &D, irdiff::Sig S);

struct Run {
  int Exit = -1;
  std::string Out, Err;
};
Run run(const std::vector<std::string> &Args, const std::string &Stdin = "");

} // namespace t

#define TEST(S, N)                                                             \
  static void S##_##N();                                                       \
  static t::Reg S##_##N##_reg(#S, #N, S##_##N);                                \
  static void S##_##N()

#define CHECK(C)                                                               \
  do {                                                                         \
    if (!(C))                                                                  \
      t::fail(__FILE__, __LINE__, "CHECK(" #C ")");                            \
  } while (0)

#define CHECK_MSG(C, M)                                                        \
  do {                                                                         \
    if (!(C))                                                                  \
      t::fail(__FILE__, __LINE__, std::string("CHECK(" #C "): ") + (M));       \
  } while (0)

#define CHECK_EQ(A, B)                                                         \
  do {                                                                         \
    auto _a = (A);                                                             \
    auto _b = (B);                                                             \
    if (!(_a == _b)) {                                                         \
      std::ostringstream _o;                                                   \
      _o << "CHECK_EQ(" #A ", " #B "): '" << _a << "' != '" << _b << "'";      \
      t::fail(__FILE__, __LINE__, _o.str());                                   \
    }                                                                          \
  } while (0)
