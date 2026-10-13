#!/usr/bin/env bash
# usage: install-llvm.sh <major> <ubuntu-codename>
set -euo pipefail
V=$1
CODENAME=$2
curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc > /dev/null
echo "deb http://apt.llvm.org/$CODENAME/ llvm-toolchain-$CODENAME-$V main" | sudo tee /etc/apt/sources.list.d/llvm.list
sudo apt-get update -q
sudo apt-get install -y -q --no-install-recommends \
  llvm-$V-dev llvm-$V clang-$V libzstd-dev zlib1g-dev ninja-build cmake
sudo apt-get install -y -q --no-install-recommends libpolly-$V-dev || true
