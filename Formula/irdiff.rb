class Irdiff < Formula
  desc "Compare LLVM IR and explain optimization changes"
  homepage "https://github.com/tryhard-26/irdiff"
  url "https://github.com/tryhard-26/irdiff/archive/refs/tags/v1.0.0.tar.gz"
  sha256 "b6237504b7ae88c2a2907e90efe9a05665c599e6504bcefbeaccf7214c11c15c"
  license "Apache-2.0"
  head "https://github.com/tryhard-26/irdiff.git", branch: "main"

  depends_on "cmake" => :build
  depends_on "ninja" => :build
  depends_on "llvm"

  def install
    system "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
           "-DIRDIFF_BUILD_TESTS=OFF",
           "-DLLVM_DIR=#{Formula["llvm"].opt_lib}/cmake/llvm",
           *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  test do
    (testpath/"a.ll").write <<~LL
      define i32 @f(i32 %x) {
        %r = add i32 %x, 1
        ret i32 %r
      }
    LL
    (testpath/"b.ll").write <<~LL
      define i32 @f(i32 %y) {
        %s = add nsw i32 %y, 1
        ret i32 %s
      }
    LL
    assert_match "flags: +nsw", shell_output("#{bin}/irdiff a.ll b.ll", 1)
    assert_match "irdiff #{version}", shell_output("#{bin}/irdiff --version")
  end
end
