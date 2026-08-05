#!/usr/bin/env bash
set -e

# Gebruik:  ./build.sh            -> bouwt en start de console-versie (gek)
#           ./build.sh --gui      -> bouwt en start de GUI-versie (gek_gui)
GUI=""
for arg in "$@"; do
    if [ "$arg" = "--gui" ]; then
        GUI=1
    fi
done

cd ~
cd gek

cmake -S . -B build

cd build
cmake --build .

if [ -n "$GUI" ]; then
    ./gek_gui
else
    ./gek
fi