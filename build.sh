#!/usr/bin/env bash
set -e


cd ~
cd gek

cmake -S . -B build

cd build
cmake --build .

./gek