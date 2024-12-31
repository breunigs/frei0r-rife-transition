#!/bin/bash

set -euo pipefail

./build.sh

mkdir -p ~/.frei0r-1/lib/
cp rife_transition.so ~/.frei0r-1/lib/
