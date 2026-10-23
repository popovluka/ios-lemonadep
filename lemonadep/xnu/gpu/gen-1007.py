#!/usr/bin/env python3
"""Regenerate lemonadep/xnu/patches/1007-lemonadep-gpu-bringup.patch.

The GPU driver sources live here, in lemonadep/xnu/gpu/, where they are
host-tested; the patch injects them into the xnu tree as new files under
bsd/dev/ and wires them into the build. Generated, never hand-edited, so the
patch can't drift from the tested sources:

  python3 lemonadep/xnu/gpu/gen-1007.py

then run lemonadep/xnu/check-overlay.sh to confirm it still applies.
"""
import os

GPU = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(GPU, '..', '..', '..'))
OUT = os.path.join(REPO, 'lemonadep', 'xnu', 'patches', '1007-lemonadep-gpu-bringup.patch')

SMMU = 'vendor/ArmSmmu4K'
# (destination in the xnu tree, source under lemonadep/xnu/gpu/)
FILES = [
    ('bsd/dev/lemonadep_gpu.c', 'lemonadep_gpu_init.c'),
    ('bsd/dev/lemonadep/LemonadepGmuSource.h', 'LemonadepGmuSource.h'),
    ('bsd/dev/lemonadep/LemonadepA660HwInit.h', 'LemonadepA660HwInit.h'),
    ('bsd/dev/lemonadep/lemonadep_gpu_reserve.c', 'lemonadep_gpu_reserve.c'),
    ('bsd/dev/lemonadep/lemonadep_gmu_coldstart.c', 'lemonadep_gmu_coldstart.c'),
    ('bsd/dev/lemonadep/lemonadep_gpu_smmu_map.c', 'lemonadep_gpu_smmu_map.c'),
    ('bsd/dev/lemonadep/lemonadep_gpu_smmu_map.h', 'lemonadep_gpu_smmu_map.h'),
    ('bsd/dev/lemonadep/lemonadep_gmu_mem.c', 'lemonadep_gmu_mem.c'),
] + [(f'bsd/dev/lemonadep/{SMMU}/{n}', f'{SMMU}/{n}') for n in (
    'LICENSE', 'ArmSmmu4K.h', 'ArmSmmu4KContext.h', 'ArmSmmu4KContext.c',
    'ArmSmmu4KProgram.h', 'ArmSmmu4KProgram.c', 'ArmSmmu4KLive.h', 'ArmSmmu4KLive.c',
    'ArmSmmu4KStreams.h', 'ArmSmmu4KStreams.c', 'ArmSmmu4KMmio.h', 'ArmSmmu4KMmio.c')]

# Separate translation units (static names would collide in one); everything
# else is #included by bsd/dev/lemonadep_gpu.c.
BUILD = ['bsd/dev/lemonadep_gpu.c'] + [
    f'bsd/dev/lemonadep/{SMMU}/ArmSmmu4K{n}.c' for n in ('Live', 'Program', 'Context', 'Streams', 'Mmio')]

# Context lines are exactly what the sweet series (0072, last to touch these
# regions; 0077 edits sweet_ufs.c elsewhere) leaves behind.
files_hunk = ''.join(f'+{p}\t\tstandard\n' for p in BUILD)
HEADER = f'''--- a/bsd/conf/files
+++ b/bsd/conf/files
@@ -111,6 +111,{6 + len(BUILD)} @@
 bsd/dev/sweet_rpmh.c\t\t\tstandard
 bsd/dev/sweet_spmi.c\t\t\tstandard
 bsd/dev/sweet_usb.c\t\t\tstandard
{files_hunk} bsd/dev/mem.c\t\t\t\tstandard
 bsd/dev/munge.c \t\t\toptional config_requires_u32_munging
 bsd/dev/unix_startup.c\t\t\tstandard
--- a/bsd/dev/sweet_ufs.c
+++ b/bsd/dev/sweet_ufs.c
@@ -50,6 +50,7 @@
 extern void sweet_keys_init(void);
 extern void sweet_touch_init(void);
 extern void sweet_usb_probe(void);
+extern void lemonadep_gpu_init(void);

 #define REG_CAP         0x00
 #define REG_IS          0x20
@@ -659,6 +660,8 @@
 \tsweet_keys_init();
 \tsweet_touch_init();
 \tsweet_usb_probe();
+\t/* GPU bring-up (lemonadep/HANDOFF.md "GPU, Phase 8"/"Phase 9"). */
+\tlemonadep_gpu_init();
 \treturn 0;
 }}

'''


def new_file(dest, src):
    with open(os.path.join(GPU, src), encoding='utf-8') as f:
        lines = f.read().replace('\r\n', '\n').split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    return f'--- /dev/null\n+++ b/{dest}\n@@ -0,0 +1,{len(lines)} @@\n' + ''.join(f'+{l}\n' for l in lines)


with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
    f.write(HEADER + ''.join(new_file(d, s) for d, s in FILES))
print(f'{OUT}: {len(FILES)} new files, {len(BUILD)} build entries')
