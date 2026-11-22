#!/bin/sh
# Fetches the original MilkDrop preset pack (as published by the projectM
# project) into assets/milkdrop/pack. The pack carries no licence statement,
# so it is fetched for personal use here rather than stored in this repo.
set -e
cd "$(dirname "$0")/../assets/milkdrop" 2>/dev/null || { mkdir -p "$(dirname "$0")/../assets/milkdrop"; cd "$(dirname "$0")/../assets/milkdrop"; }
[ -d pack ] || git clone --depth 1 https://github.com/projectM-visualizer/presets-milkdrop-original.git pack
echo "presets: $(find pack -name '*.milk' | wc -l)"
