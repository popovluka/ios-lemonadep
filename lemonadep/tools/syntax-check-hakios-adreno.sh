#!/bin/bash
# Syntax-check the imported HakiOS Adreno driver (patch 1012) without a CI run.
#
#   lemonadep/tools/syntax-check-hakios-adreno.sh [WORKDIR]
#
# The XNU headers here are stubs, so this catches typos, duplicate definitions,
# missing symbols and bad printf formats -- not anything that depends on real
# XNU declarations. Known harmless noise: uint64_t is "unsigned long" on an
# x86-64 Linux host and "unsigned long long" on arm64 XNU, so %llx formats warn
# here and are correct in the real build.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${1:-${TMPDIR:-/tmp}/lemonadep-adreno-syntax}"
PATCHFILE="$REPO/lemonadep/xnu/patches/1012-lemonadep-hakios-adreno.patch"

command -v clang >/dev/null || { echo "clang not found (WSL: apt-get install clang)" >&2; exit 1; }

rm -rf "$WORK/tree"
mkdir -p "$WORK/tree"
python3 "$REPO/lemonadep/tools/extract-patch-tree.py" "$PATCHFILE" "$WORK/tree" >/dev/null

STUB="$WORK/stub"
for d in kern libkern mach vm sys pexpert machine; do mkdir -p "$STUB/$d"; done
for h in sys/stat.h machine/machine_routines.h; do printf '#pragma once\n' > "$STUB/$h"; done

cat > "$STUB/sys/systm.h" <<'EOF'
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
EOF
cat > "$STUB/libkern/libkern.h" <<'EOF'
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#ifndef __printflike
#define __printflike(a, b) __attribute__((format(printf, a, b)))
#endif
int printf(const char *, ...) __printflike(1, 2);
int snprintf(char *, size_t, const char *, ...) __printflike(3, 4);
int vsnprintf(char *, size_t, const char *, va_list);
int strcmp(const char *, const char *);
size_t strlen(const char *);
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void bzero(void *, size_t);
EOF
cat > "$STUB/mach/mach_types.h" <<'EOF'
#pragma once
#include <stdint.h>
typedef unsigned long vm_offset_t;
typedef unsigned long vm_size_t;
typedef int dev_t;
EOF
cat > "$STUB/mach/vm_param.h" <<'EOF'
#pragma once
#define PAGE_SIZE 4096
#define PAGE_MASK (PAGE_SIZE - 1)
EOF
cat > "$STUB/vm/vm_kern.h" <<'EOF'
#pragma once
#include <mach/mach_types.h>
/* xnu-8796.101.5's shapes (osfmk/vm/vm_kern.h): KMA_* are enum constants, not
   macros, and kmem_alloc() takes the flags then the tag -- an inline over
   kernel_memory_allocate(). No compatibility shim is needed for this tree. */
#define KERN_SUCCESS 0
enum { KMA_NONE = 0, KMA_KOBJECT = 0x04, KMA_ZERO = 0x80, KMA_DATA = 0x100 };
#define VM_KERN_MEMORY_IOKIT 10
extern vm_offset_t kernel_map;
int kmem_alloc(vm_offset_t map, vm_offset_t *addrp, vm_size_t size, int flags, int tag);
vm_size_t round_page(vm_size_t);
size_t strlcpy(char *, const char *, size_t);
EOF
cat > "$STUB/kern/clock.h" <<'EOF'
#pragma once
#include <stdint.h>
#define NSEC_PER_USEC 1000ull
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_SEC  1000000000ull
void delay_for_interval(uint32_t interval, uint32_t scale_factor);
unsigned long long mach_absolute_time(void);
void absolutetime_to_nanoseconds(unsigned long long, unsigned long long *);
EOF
cat > "$STUB/pexpert/pexpert.h" <<'EOF'
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <mach/mach_types.h>
void delay(int);
void panic(const char *, ...);
int PE_parse_boot_argn(const char *, void *, int);
int PE_parse_boot_arg_str(const char *, char *, int);
EOF
cat > "$STUB/pexpert/device_tree.h" <<'EOF'
#pragma once
#include <stdint.h>
typedef struct OpaqueDTEntry *DTEntry;
enum { kSuccess = 0 };
int SecureDTFindEntry(const char *, const char *, DTEntry *);
int SecureDTGetProperty(DTEntry, const char *, void const **, unsigned int *);
EOF

# --target=aarch64: the driver's TrustZone call names x0..x8 in asm operands,
# which a host-x86 clang rejects. Everything it includes is stubbed here, so no
# aarch64 sysroot is needed. This also makes uint64_t "unsigned long" as on
# arm64 XNU, so the %llx formats no longer warn spuriously.
clang --target=aarch64-unknown-none-elf -fsyntax-only -std=gnu11 \
	-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
	-Wno-unused-function -Wno-format-nonliteral \
	-I "$STUB" \
	"$WORK/tree/bsd/dev/hakios_adreno.c"
echo "syntax check passed (stub headers; see the header comment)"
