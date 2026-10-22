#!/usr/bin/env bash
# Fetches third-party dependencies into external/ at the versions this project is built against.
set -euo pipefail
cd "$(dirname "$0")/../external" 2>/dev/null || { mkdir -p "$(dirname "$0")/../external"; cd "$(dirname "$0")/../external"; }

clone() { # url dir ref
    if [ ! -d "$2" ]; then git clone --recursive "$1" "$2"; fi
    git -C "$2" checkout -q "$3"
    git -C "$2" submodule update --init --recursive -q
}

clone https://github.com/azahar-emu/dynarmic dynarmic a4660158
clone https://github.com/libsdl-org/SDL SDL release-3.2.24
clone https://github.com/ocornut/imgui imgui v1.92.4

if [ ! -d boost/boost ]; then
    curl -sL -o boost.tar.gz https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.gz
    mkdir -p boost && tar -xzf boost.tar.gz -C boost --strip-components=1 boost_1_86_0/boost
    rm boost.tar.gz
fi
echo "Dependencies ready."
