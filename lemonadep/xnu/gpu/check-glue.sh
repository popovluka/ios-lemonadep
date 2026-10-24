#!/bin/bash
# Syntax-check bsd/dev/lemonadep_gpu.c (lemonadep_gpu_init.c) for aarch64
# against minimal stand-ins for the few XNU headers it includes. Real
# compilation happens in CI; this catches type/name mistakes first.
set -euo pipefail
G=$(cd "$(dirname "$0")" && pwd)
S=$(mktemp -d)
trap 'rm -rf "$S"' EXIT
mkdir -p "$S/sys" "$S/kern" "$S/machine" "$S/pexpert" "$S/lemonadep"
printf '#include <stdint.h>\n#include <stddef.h>\ntypedef uintptr_t vm_offset_t;\ntypedef unsigned long vm_size_t;\n' > "$S/sys/param.h"
printf '#include <stdio.h>\n' > "$S/sys/systm.h"
# kern/clock.h is where XNU declares delay() (sweet_rpmh.c relies on that).
printf '#include <stdint.h>\nstatic inline uint64_t mach_absolute_time(void) { return 0; }\nstatic inline void absolutetime_to_nanoseconds(uint64_t a, uint64_t *o) { *o = a; }\nvoid delay(int usec);\n' > "$S/kern/clock.h"
: > "$S/kern/thread.h"
: > "$S/machine/machine_routines.h"
printf 'typedef int kern_return_t;\ntypedef int wait_result_t;\ntypedef struct thread *thread_t;\ntypedef void (*thread_continue_t)(void *, wait_result_t);\n#define KERN_SUCCESS 0\nvoid thread_deallocate(thread_t);\n' > "$S/kern/sched_prim.h"
printf '#include <string.h>\n#include <stdint.h>\nstatic inline int PE_parse_boot_argn(const char *n, void *v, int sz) { (void)n; memset(v, 0, sz); return 0; }\n' > "$S/pexpert/pexpert.h"
cp "$G"/LemonadepGmuSource.h "$G"/LemonadepA660HwInit.h "$G"/lemonadep_gpu_reserve.c \
	"$G"/lemonadep_gmu_coldstart.c "$G"/lemonadep_gpu_smmu_map.c "$G"/lemonadep_gpu_smmu_map.h \
	"$G"/lemonadep_gmu_mem.c "$S/lemonadep/"
mkdir -p "$S/lemonadep/vendor" && cp -r "$G/vendor/ArmSmmu4K" "$S/lemonadep/vendor/"
cp "$G/lemonadep_gpu_init.c" "$S/lemonadep_gpu.c"
cd "$S"
aarch64-linux-gnu-gcc -std=gnu11 -fsyntax-only -Wall -Wextra -Wno-format -I. lemonadep_gpu.c
for f in Live Program Context Streams Mmio; do
	aarch64-linux-gnu-gcc -std=gnu11 -fsyntax-only -Wall -Wextra "lemonadep/vendor/ArmSmmu4K/ArmSmmu4K$f.c"
done
echo "gpu glue syntax check: ok"

# Wi-Fi stage 1: bsd/dev/lemonadep_wlan.c and the edited sweet_rpmh.c.
W="$G/../wlan"
mkdir -p "$S/lemonadep/wlan"
cp "$W"/lemonadep_wlan_pcie.c "$W"/LemonadepWlanSource.h "$W"/LemonadepPcie0Phy.h \
	"$W"/lemonadep_wlan_mhi.c "$W"/LemonadepMhiSource.h "$S/lemonadep/wlan/"
cp "$W/lemonadep_wlan_init.c" "$S/lemonadep_wlan.c"
printf '#include <errno.h>\n' > "$S/sys/errno.h"
aarch64-linux-gnu-gcc -std=gnu11 -fsyntax-only -Wall -Wextra -Wno-format -I. lemonadep_wlan.c
aarch64-linux-gnu-gcc -std=gnu11 -fsyntax-only -Wall -Wextra -Wno-format -DARM64_BOARD_CONFIG_SWEET \
	-include string.h -include stdbool.h -I. "$W/sweet_rpmh.c"
echo "wlan glue syntax check: ok"
