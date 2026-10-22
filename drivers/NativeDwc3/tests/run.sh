#!/bin/bash
# Build and run the NativeDwc3 host unit tests (register decode + SMMU plan).
# Pure host build, like drivers/NativeUfs/tests: c++ -std=c++17, no kernel headers.
set -e
cd "$(dirname "$0")"
CXX=${CXX:-c++}
for t in Dwc3Decode Dwc3Smmu; do
  "$CXX" -std=c++17 -Wall -Wextra -Werror -O2 -o "/tmp/$t.$$" "$t.cpp"
  "/tmp/$t.$$"
  rm -f "/tmp/$t.$$"
done
echo "NativeDwc3 host tests: PASS"
