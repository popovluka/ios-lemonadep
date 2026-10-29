#!/bin/bash
# Syntax-check patch 1007's generated GPU sources without a CI run.
#
#   lemonadep/tools/syntax-check-gpu.sh [WORKDIR]
#
# Materialises every new file the patch adds (extract-patch-tree.py) and runs
# clang -fsyntax-only over bsd/dev/lemonadep_gpu.c, which #includes the rest.
# The XNU headers are stubs, so this catches typos, bad types and bad printf
# formats -- not anything that depends on real XNU declarations.
#
# Known harmless noise: uint64_t is "unsigned long" on an x86-64 Linux host
# and "unsigned long long" on arm64 XNU, so the %llx formats in this file warn
# here and are correct in the real build. Judge the output by whether anything
# points at the lines you changed.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${1:-${TMPDIR:-/tmp}/lemonadep-syntax}"
PATCHFILE="$REPO/lemonadep/xnu/patches/1007-lemonadep-gpu-bringup.patch"

command -v clang >/dev/null || { echo "clang not found (WSL: apt-get install clang)" >&2; exit 1; }

rm -rf "$WORK/tree"
mkdir -p "$WORK/tree"
python3 "$REPO/lemonadep/tools/extract-patch-tree.py" "$PATCHFILE" "$WORK/tree" >/dev/null

STUB="$WORK/stub"
for d in kern machine pexpert sys; do mkdir -p "$STUB/$d"; done
for h in kern/sched_prim.h kern/thread.h machine/machine_routines.h sys/param.h; do
	printf '#pragma once\n' > "$STUB/$h"
done
cat > "$STUB/sys/systm.h" <<'EOF'
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
EOF
cat > "$STUB/kern/clock.h" <<'EOF'
#pragma once
unsigned long long mach_absolute_time(void);
void absolutetime_to_nanoseconds(unsigned long long, unsigned long long *);
EOF
cat > "$STUB/pexpert/pexpert.h" <<'EOF'
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef unsigned long vm_offset_t;
typedef unsigned long vm_size_t;
int printf(const char *, ...) __attribute__((format(printf, 1, 2)));
void delay(int);
void panic(const char *, ...);
int PE_parse_boot_argn(const char *, void *, int);
EOF

clang -fsyntax-only -std=gnu11 \
	-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
	-I "$STUB" -I "$WORK/tree/bsd/dev" \
	"$WORK/tree/bsd/dev/lemonadep_gpu.c"
echo "syntax check passed (stub headers; see the header comment)"
