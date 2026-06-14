#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
  printf 'usage: %s input.s output.bin\n' "$0" >&2
  exit 2
fi

input=$1
output=$2
mcpu=${AMDGPU_MCPU:-gfx900}
obj=${output}.o

clang -c -x assembler -target amdgcn-amd-amdhsa -mcpu="$mcpu" -o "$obj" "$input"

if command -v llvm-objcopy >/dev/null 2>&1; then
  llvm-objcopy -O binary -j .text "$obj" "$output"
elif command -v objcopy >/dev/null 2>&1; then
  objcopy -O binary -j .text "$obj" "$output"
else
  printf 'error: llvm-objcopy or objcopy is required\n' >&2
  exit 1
fi

rm -f "$obj"
