#!/usr/bin/env bash
set -e

# Usage:  ./build.sh            -> builds and starts the console version (flow)
#         ./build.sh --gui      -> builds and starts the GUI version (flow_gui)
GUI=""
for arg in "$@"; do
    if [ "$arg" = "--gui" ]; then
        GUI=1
    fi
done

cmake -S . -B build
cmake --build build

if [ -n "$GUI" ]; then
    ./build/flow_gui
else
    ./build/flow
fi
