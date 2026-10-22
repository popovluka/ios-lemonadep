#!/usr/bin/env python3
"""Derive the A660 hw_init register table (a6xx_gpu.c hw_init, a6xx_set_hwcg, a6xx_set_cp_protect) from pinned
sources, the same method as UniXNU's scripts/derive-a740-hwinit.py (github.com/UniXNU/UniXNU), adapted for the A660
(a6xx gen4, not an A7XX variant) instead of the A740.

Sources (pinned, sha256-verified against github.com/UniXNU/UniXNU's own scripts/data/adreno-inputs.json record):
  ianchb/sm8550-mainline @ 42f3b40c702abbc37c3a04c95af008cd283aeaf3
  drivers/gpu/drm/msm/adreno/a6xx_catalog.c (a660_hwcg, a660_protect_regs, DECLARE_ADRENO_PROTECT(a660_protect,48))
  drivers/gpu/drm/msm/registers/adreno/{a6xx,adreno_common,a6xx_gmu}.xml
"""
import re, sys
from pathlib import Path

SRC = Path(sys.argv[1])  # .../adreno-src
XML = SRC / 'drivers/gpu/drm/msm/registers/adreno'
CATALOG = SRC / 'drivers/gpu/drm/msm/adreno/a6xx_catalog.c'
OUT = Path(sys.argv[2])  # output header path

# A660 is plain A6XX (gen4), not an A7XX variant -- select on A6XX, not A7XX as the a740 script does.
ORDER = ['A2XX', 'A3XX', 'A4XX', 'A5XX', 'A6XX', 'A7XX', 'A8XX']


def matches(variants):
    if variants is None:
        return True
    for part in variants.split(','):
        part = part.strip()
        if '-' in part:
            left, right = part.split('-', 1)
            low = ORDER.index(left) if left else 0
            high = ORDER.index(right) if right else len(ORDER) - 1
            if low <= ORDER.index('A6XX') <= high:
                return True
        elif part == 'A6XX':
            return True
    return False


def resolve(name, files, kind='reg(?:32|64)'):
    for f in files:
        text = (XML / f).read_text()
        hits = set()
        for m in re.finditer(r'<(' + kind + r') offset="(0x[0-9a-fA-F]+)" name="' + name + r'"([^>]*)>', text):
            v = re.search(r'variants="([^"]+)"', m.group(3))
            if matches(v.group(1) if v else None):
                hits.add(int(m.group(2), 16))
        if len(hits) > 1:
            raise SystemExit('ambiguous ' + name + ' in ' + f)
        if hits:
            return hits.pop()
    raise SystemExit('missing ' + name)


# Same register set the A740 script resolves (hw_init/submit-relevant registers), minus CP_BV_APRIV_CNTL,
# CP_BV_RB_RPTR_ADDR and CP_LPAC_APRIV_CNTL: checked against the real a6xx_gpu.c hw_init() (not assumed) -- all
# three are written only inside `if (adreno_is_a7xx(adreno_gpu))`, and a6xx.xml tags them `variants="A7XX"` with
# no A6XX entry at all. A660 is a6xx gen4, not A7XX, so it has none of the BV (bindless/visibility ring) or LPAC
# (low-power async compute) hardware these registers belong to.
GPU = ['GBIF_HALT', 'RBBM_GBIF_HALT', 'RBBM_SECVID_TSB_CNTL', 'RBBM_SECVID_TSB_TRUSTED_BASE',
       'RBBM_SECVID_TSB_TRUSTED_SIZE', 'RBBM_GBIF_CLIENT_QOS_CNTL', 'UCHE_GBIF_GX_CONFIG',
       'RBBM_PERFCTR_GPU_BUSY_MASKED', 'UCHE_TRAP_BASE', 'UCHE_WRITE_THRU_BASE', 'UCHE_GMEM_RANGE_MIN',
       'UCHE_GMEM_RANGE_MAX', 'UCHE_CACHE_WAYS', 'CP_AHB_CNTL', 'RBBM_PERFCTR_CNTL', 'RBBM_INTERFACE_HANG_INT_CNTL',
       'UCHE_CLIENT_PF', 'CP_PROTECT_CNTL', 'UCHE_CMDQ_CONFIG', 'CP_APRIV_CNTL',
       'RB_CMP_DBG_ECO_CNTL', 'RBBM_INT_0_MASK', 'CP_SQE_INSTR_BASE', 'CP_RB_BASE',
       'CP_RB_CNTL', 'CP_RB_RPTR_ADDR', 'CP_SQE_CNTL', 'CP_RB_RPTR', 'CP_RB_WPTR',
       'RBBM_STATUS', 'RBBM_INT_0_STATUS', 'CP_HW_FAULT', 'CP_INTERRUPT_STATUS', 'RBBM_CLOCK_CNTL',
       'RBBM_SECVID_TRUST_CNTL', 'RBBM_STATUS3', 'CP_SQE_STAT_ADDR', 'CP_SQE_STAT_DATA',
       'RB_NC_MODE_CNTL', 'TPL1_NC_MODE_CNTL', 'SP_NC_MODE_CNTL', 'CP_APERTURE_CNTL_HOST',
       'UCHE_MODE_CNTL', 'RBBM_NC_MODE_CNTL', 'RBBM_INT_CLEAR_CMD', 'CP_ALWAYS_ON_COUNTER']
# GRAS_NC_MODE_CNTL dropped: a6xx.xml tags it variants="A7XX" only (A8XX has its own offset, no A6XX entry at
# all), and a6xx_gpu.c writes REG_A7XX_GRAS_NC_MODE_CNTL only inside `if (adreno_is_a7xx(adreno_gpu))` --
# checked, not assumed. A660 doesn't touch this register.
GMU = ['GPU_GMU_AO_GMU_CGC_MODE_CNTL', 'GPU_GMU_AO_GMU_CGC_DELAY_CNTL', 'GPU_GMU_AO_GMU_CGC_HYST_CNTL']


def main():
    catalog = CATALOG.read_text()
    defines = [(n, resolve(n, ['a6xx.xml', 'adreno_common.xml'])) for n in GPU]
    defines += [(n, resolve(n, ['a6xx_gmu.xml'])) for n in GMU]
    defines.append(('CP_PROTECT', resolve('CP_PROTECT', ['a6xx.xml'], 'array')))

    block = re.search(r'a660_hwcg\[\]\s*=\s*\{(.*?)\{\},\s*\};', catalog, re.S).group(1)
    hwcg = []
    for name, value in re.findall(r'\{\s*REG_A[67]XX_([A-Z0-9_]+),\s*(0x[0-9a-fA-F]+)\s*\}', block):
        hwcg.append((name, resolve(name, ['a6xx.xml', 'adreno_common.xml']), int(value, 16)))
    if len(hwcg) != block.count('REG_'):
        raise SystemExit('unparsed a660_hwcg entry')

    pblock = re.search(r'a660_protect_regs\[\]\s*=\s*\{(.*?)\};', catalog, re.S).group(1)
    pblock = re.sub(r'/\*.*?\*/', '', pblock, flags=re.S)
    protect = []
    for entry in [e.strip() for e in pblock.split(',\n')]:
        entry = entry.strip().rstrip(',')
        if not entry:
            continue
        if entry == '0':
            protect.append(0)
            continue
        m = re.fullmatch(r'A6XX_PROTECT_(NORDWR|RDONLY)\((0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)\)', entry)
        if not m:
            raise SystemExit('unparsed protect entry ' + entry)
        reg, length = int(m.group(2), 16), int(m.group(3), 16)
        protect.append(((1 << 31) if m.group(1) == 'NORDWR' else 0) | ((length & 0x3fff) << 18) | (reg & 0x3ffff))
    count_max = int(re.search(r'DECLARE_ADRENO_PROTECT\(a660_protect,\s*(\d+)\)', catalog).group(1))
    writes = [(i, v) for i, v in enumerate(protect[:-1]) if v] + [(count_max - 1, protect[-1])]

    lines = ['/* SPDX-License-Identifier: MIT */', '#ifndef LEMONADEP_A660_HWINIT_H', '#define LEMONADEP_A660_HWINIT_H',
             '#include <stdint.h>',
             '/* Derived (by lemonadep/xnu/test/derive-a660-hwinit.py, adapted from UniXNU/UniXNU\'s',
             ' * scripts/derive-a740-hwinit.py for the A660 -- plain a6xx gen4, not an A7XX variant) from the',
             ' * pinned ianchb/sm8550-mainline @ 42f3b40c (sha256-verified against UniXNU\'s own',
             ' * scripts/data/adreno-inputs.json record): registers/adreno/{a6xx,adreno_common,a6xx_gmu}.xml and',
             ' * adreno/a6xx_catalog.c (a660_hwcg, a660_protect_regs, DECLARE_ADRENO_PROTECT(a660_protect,48)).',
             ' * Dword register offsets. Do not edit by hand; re-run the deriving script instead. */']
    for name, offset in defines:
        lines.append('#define A660_%s 0x%05x' % (name, offset))
    lines.append('static const uint32_t LemonadepA660Hwcg[][2]={')
    lines += ['    {0x%05x,0x%08x},   /* %s */' % (o, v, n) for n, o, v in hwcg]
    lines.append('};')
    lines.append('#define LEMONADEP_A660_HWCG_COUNT %d' % len(hwcg))
    lines.append('/* {CP_PROTECT index, value} in a6xx_set_cp_protect order. */')
    lines.append('static const uint32_t LemonadepA660Protect[][2]={')
    lines += ['    {%2d,0x%08x},' % w for w in writes]
    lines.append('};')
    lines.append('#define LEMONADEP_A660_PROTECT_COUNT %d' % len(writes))
    lines.append('#endif')
    text = '\n'.join(lines) + '\n'
    if '--check' in sys.argv[3:]:
        if OUT.read_text() != text:
            raise SystemExit('LemonadepA660HwInit.h is stale')
        print('PASS: %d registers, %d hwcg entries, %d protect writes (matches committed header)' %
              (len(defines), len(hwcg), len(writes)))
        return
    OUT.write_text(text)
    print('PASS: %d registers, %d hwcg entries, %d protect writes' % (len(defines), len(hwcg), len(writes)))


main()
