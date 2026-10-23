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

# GMU cold-start / RSC-PDC handshake registers (a6xx_gmu_fw_start(), a6xx_rpmh_start(),
# a6xx_gmu_rpmh_init() in the pinned a6xx_gmu.c) for A660's real control-flow path -- traced
# branch by branch (not ShengGmu.c's A7XX path with substituted addresses): A660 is
# adreno_is_a650_family() (family==GEN3||GEN4) so it takes the FAL10-veto and "PDC configured
# in AOP" branches, but is NOT adreno_is_a7xx() so TCM retention is GMU_GENERAL_7 (not
# CX_MISC_TCM_RET_CNTL, which doesn't exist pre-A7XX) and it is NOT adreno_is_a740_family()
# (GEN2||GEN3 of the 7XX line) so the HIDDEN_TCS_CMD0_DATA+4 value is 0x80000000, not 0x80000021.
GMU_CORE_RSC = ['GMU_RSCC_CONTROL_REQ', 'GMU_RSCC_CONTROL_ACK', 'GMU_GENERAL_7']
RSCC = ['GPU_RSCC_RSC_STATUS0_DRV0', 'RSCC_PDC_SLAVE_ID_DRV0', 'RSCC_HIDDEN_TCS_CMD0_DATA',
        'RSCC_HIDDEN_TCS_CMD0_ADDR', 'RSCC_OVERRIDE_START_ADDR', 'RSCC_PDC_SEQ_START_ADDR',
        'RSCC_PDC_MATCH_VALUE_LO', 'RSCC_PDC_MATCH_VALUE_HI', 'RSCC_SEQ_MEM_0_DRV0',
        'RSCC_SEQ_BUSY_DRV0']
# A6XX_PDC domain (a6xx.xml): the register's own name has no "PDC_" prefix -- the C side gets
# REG_A6XX_PDC_GPU_ENABLE_PDC etc. by concatenating the domain name with the register name.
PDC = ['GPU_ENABLE_PDC', 'GPU_SEQ_START_ADDR']

# Rest of a6xx_gmu_fw_start(): HFI table pointer, the AHB fence range, CM3 config, chip ID and
# log-buffer registration, and the shared GBIF QoS writes. All traced for A660's real path (not
# ShengGmu.c's A7XX one, which gets two of these genuinely wrong for A660 -- see the emitted
# comment below for what and why).
GMU_FW_START = ['GMU_CM3_FW_INIT_RESULT', 'GMU_CM3_BOOT_CONFIG', 'GMU_HFI_QTBL_ADDR',
                'GMU_HFI_QTBL_INFO', 'GMU_AHB_FENCE_RANGE_0', 'GMU_CM3_CFG', 'GMU_HFI_SFR_ADDR',
                'GPU_GMU_CX_GMU_PWR_COL_CP_MSG', 'GMU_CM3_ITCM_START', 'GMU_CM3_DTCM_START']
# a6xx_gmu_power_config() + a6xx_gmu_start() (a6xx_gmu.c:801-841, 291-331): called at the tail
# of fw_start, after the GBIF writes -- found only by continuing to trace past what
# ShengGmu.c's A740 code showed, not present in the earlier fw_start list above. A660 is not
# adreno_is_a7xx()/a8xx(), so power_config's RPMH_CTRL enable (unconditional, not idle-level-
# gated) and gmu_start's real reset toggle both apply, using the plain-a6xx register for the
# log-wptr write (PWR_COL_CP_RESP, not GMU_GENERAL_9 -- the A7XX/A8XX-only register).
GMU_POWER_CONFIG = ['GMU_SYS_BUS_CONFIG', 'GMU_ICACHE_CONFIG', 'GMU_DCACHE_CONFIG',
                     'GMU_RPMH_CTRL', 'GMU_CM3_SYSRESET', 'GPU_GMU_CX_GMU_PWR_COL_CP_RESP']
GBIF_CX = ['GBIF_QSB_SIDE0']  # a640_gbif writes SIDE0..SIDE3 at +1 each, all the same value


# gmu_write()/gmu_read() (the Core window) do NOT use a plain offset<<2: the real macro is
# GMU_BYTE_OFFSET(gmu, offset) = (offset<<2) - gmu->mmio_offset, where mmio_offset =
# (GMU core reg base) - (kgsl_3d0_reg_memory / GPU reg base), set once at probe time
# (a6xx_gmu.c:2392). gmu_write_rscc()/pdc_write() do NOT have this correction (confirmed:
# they use `gmu->rscc + (offset<<2)` / `ptr + (offset<<2)` directly, their own mapped base) --
# this applies only to Core-window registers. Missing it was caught by test_gmu_coldstart.c's
# mock register bank crashing on an out-of-window offset, not assumed correct beforehand.
GMU_CORE_MMIO_OFFSET = 0x03d6a000 - 0x03d00000  # LEMONADEP_GMU_CORE_PA - LEMONADEP_GMU_GPU_PA


def core_byte_offset(dword_offset):
    off = (dword_offset << 2) - GMU_CORE_MMIO_OFFSET
    if off < 0 or off >= 0x34000:  # LEMONADEP_GMU_CORE_BYTES
        raise SystemExit('Core register at dword 0x%x (byte 0x%x after mmio_offset correction) '
                          'falls outside the real 0x34000-byte GMU window -- re-check' %
                          (dword_offset, off))
    return off


def main():
    catalog = CATALOG.read_text()
    # GPU is dword-offset, gpu_write()-convention (that macro shifts internally, no
    # mmio_offset correction -- the main kgsl_3d0_reg_memory window is its own base). GMU's 3
    # registers are gmu_write()-convention instead (Core window, WITH the mmio_offset
    # correction) -- moved below, byte-offset, alongside gmu_core_rsc/fw_start/power_config,
    # instead of living here with a silently different unit and no correction (that
    # inconsistency is exactly what hid the mmio_offset bug the first time around).
    defines = [(n, resolve(n, ['a6xx.xml', 'adreno_common.xml'])) for n in GPU]
    defines.append(('CP_PROTECT', resolve('CP_PROTECT', ['a6xx.xml'], 'array')))
    gmu_ao_cgc = [(n, resolve(n, ['a6xx_gmu.xml'])) for n in GMU]
    gmu_core_rsc = [(n, resolve(n, ['a6xx_gmu.xml'])) for n in GMU_CORE_RSC]
    rscc = [(n, resolve(n, ['a6xx_gmu.xml'])) for n in RSCC]
    pdc = [(n, resolve(n, ['a6xx.xml'])) for n in PDC]
    fw_start = [(n, resolve(n, ['a6xx_gmu.xml'])) for n in GMU_FW_START]
    gbif_cx = [(n, resolve(n, ['a6xx.xml'])) for n in GBIF_CX]
    power_config = [(n, resolve(n, ['a6xx_gmu.xml'])) for n in GMU_POWER_CONFIG]

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
             ' * Dword register offsets (gpu_write()/CP_PROTECT convention, the main GPU register',
             ' * window, no correction needed) below; the GMU/RSC/PDC section further down is',
             ' * byte offsets with the real mmio_offset correction applied where gmu_write() needs',
             ' * it -- see that section\'s own comment. Do not edit by hand; re-run the deriving',
             ' * script instead. */']
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

    lines.append('')
    lines.append('/* GMU cold-start / RSC-PDC handshake (a6xx_gmu_fw_start, a6xx_rpmh_start,')
    lines.append(' * a6xx_gmu_rpmh_init in the pinned a6xx_gmu.c), A660\'s real traced path --')
    lines.append(' * see lemonadep/HANDOFF.md "GPU, Phase 2 continued" for the branch-by-branch')
    lines.append(' * citations. Byte offsets (register offset << 2), not dword, to match the')
    lines.append(' * ShengGmuIo read32/write32(window, byte_offset, ...) contract. Core-window')
    lines.append(' * registers (gmu_core_rsc here, plus every GMU_* register further below) have')
    lines.append(' * GMU_BYTE_OFFSET\'s mmio_offset correction already applied (0x%x = GMU core' %
                  GMU_CORE_MMIO_OFFSET)
    lines.append(' * base - kgsl_3d0_reg_memory base, a6xx_gmu.c:2392) -- RSCC/PDC do not need')
    lines.append(' * it (gmu_write_rscc/pdc_write use their own mapped base directly, confirmed')
    lines.append(' * in the real source, not assumed). */')
    for n, o in gmu_ao_cgc:
        lines.append('#define A660_%s 0x%06x' % (n, core_byte_offset(o)))
    for n, o in gmu_core_rsc:
        lines.append('#define A660_%s 0x%06x' % (n, core_byte_offset(o)))
    for n, o in rscc:
        lines.append('#define A660_%s 0x%06x' % (n, o * 4))
    for n, o in pdc:
        lines.append('#define A660_PDC_%s 0x%06x' % (n, o * 4))
    lines.append('/* HIDDEN_TCS_CMD0 DATA/ADDR pairs step by +2 dwords per slot (0,2,4); A660')
    lines.append(' * writes slot 4\'s DATA as 0x80000000 (not A740\'s 0x80000021 -- A660 is not')
    lines.append(' * adreno_is_a740_family()). RSC sequencer microcode: A660 takes the')
    lines.append(' * a650_family/a7xx branch (not the plain-a6xx values). PDC is in the')
    lines.append(' * "configured in AOP" mode (pdc_in_aop=true for a650_family), so only the')
    lines.append(' * two PDC_GPU_* writes at the end happen -- the PDC sequencer uCode and')
    lines.append(' * TCS1/TCS3 command block (plain-a6xx/non-AOP path only) do not apply here. */')
    lines.append('/* Values this sequence writes INTO the registers above, as distinct from the')
    lines.append(' * A660_* MMIO offsets above (A660_RSCC_PDC_SEQ_START_ADDR is where to write;')
    lines.append(' * RSCC_VAL_PDC_SEQ_START_ADDR, which happens to equal A660_PDC_GPU_SEQ_START_ADDR')
    lines.append(' * numerically, is what to write there -- do not confuse the two). */')
    lines.append('#define LEMONADEP_A660_HIDDEN_TCS_CMD0_SLOT4_DATA 0x80000000u')
    lines.append('static const uint32_t LemonadepA660RscSeqMicrocode[5]={')
    lines.append('    0xeaaae5a0u,0xe1a1ebabu,0xa2e0a581u,0xecac82e2u,0x0020edadu,')
    lines.append('};')
    lines.append('#define LEMONADEP_A660_RSCC_VAL_PDC_SEQ_START_ADDR 0x4520u')
    lines.append('#define LEMONADEP_A660_RSCC_VAL_PDC_MATCH_VALUE_LO 0x4510u')
    lines.append('#define LEMONADEP_A660_RSCC_VAL_PDC_MATCH_VALUE_HI 0x4514u')
    lines.append('#define LEMONADEP_A660_PDC_VAL_GPU_ENABLE_PDC 0x80000001u')

    lines.append('')
    lines.append('/* Rest of a6xx_gmu_fw_start() (Core window): HFI table pointer, AHB fence')
    lines.append(' * range, CM3 config, chip ID + log buffer registration, GBIF QoS. A660 is NOT')
    lines.append(' * adreno_is_a7xx()/a8xx(), so it takes the plain-a6xx branches, which genuinely')
    lines.append(' * differ from ShengGmu.c\'s A740 values in two places -- confirmed against the')
    lines.append(' * real a6xx_gmu_fw_start(), not assumed:')
    lines.append(' *   - fence range is (upper=0xa,lower=0xa0), NOT A7XX/A8XX\'s (0x32,0x8a0/0x8c0)')
    lines.append(' *     the exact value ShengGmu.c writes for the A740.')
    lines.append(' *   - chip ID and the log buffer IOVA go to GMU_HFI_SFR_ADDR and')
    lines.append(' *     GPU_GMU_CX_GMU_PWR_COL_CP_MSG, NOT GMU_GENERAL_10/GMU_GENERAL_8 (those')
    lines.append(' *     are the A7XX/A8XX-only registers ShengGmu.c uses).')
    lines.append(' * Also includes GMU_CM3_ITCM_START/GMU_CM3_DTCM_START (a6xx_gmu_fw_load(),')
    lines.append(' * not fw_start) -- the firmware-block write targets. */')
    for n, o in fw_start:
        lines.append('#define A660_%s 0x%06x' % (n, core_byte_offset(o)))
    for n, o in gbif_cx:
        # GBIF is written via gpu_write() (the main kgsl_3d0_reg_memory window directly,
        # confirmed in a6xx_gmu_fw_start()), not gmu_write() -- no mmio_offset correction.
        lines.append('#define A660_%s 0x%06x' % (n, o * 4))
    lines.append('#define LEMONADEP_A660_FENCE_RANGE_UPPER 0xau')
    lines.append('#define LEMONADEP_A660_FENCE_RANGE_LOWER 0xa0u')
    lines.append('/* AHB_FENCE_RANGE_0 = BIT(31) | upper<<18 | lower, per the real FIELD_PREP calls. */')
    lines.append('#define LEMONADEP_A660_RSCC_VAL_CM3_CFG 0x4052u  /* avoids the snapshot/NMI jump */')
    lines.append('#define LEMONADEP_A660_VAL_HFI_QTBL_INFO 1u')
    lines.append('#define LEMONADEP_A660_VAL_CM3_BOOT_CONFIG 2u')
    # chipid massaging, a6xx_gmu_fw_start()'s plain-a6xx else branch (not the a7xx/a8xx one
    # ShengGmu.c uses): chip_id & 0xffff0000 | (chip_id<<4)&0xf000 | (chip_id<<8)&0x0f00.
    a660_chip_id = 0x06060001
    gmu_chipid = (a660_chip_id & 0xffff0000) | ((a660_chip_id << 4) & 0xf000) | \
        ((a660_chip_id << 8) & 0x0f00)
    gmu_chipid &= 0xffffffff
    lines.append('/* chip_id massaged the way a6xx_gmu_fw_start()\'s plain-a6xx branch does')
    lines.append(' * (chip_id & 0xffff0000 | (chip_id<<4)&0xf000 | (chip_id<<8)&0x0f00), computed')
    lines.append(' * here from A660\'s real chip_id 0x%08x, not hand-typed. Written to' % a660_chip_id)
    lines.append(' * GMU_HFI_SFR_ADDR (A660\'s real register; NOT GMU_GENERAL_10, the A7XX/A8XX one). */')
    lines.append('#define LEMONADEP_A660_GMU_CHIPID 0x%08xu' % gmu_chipid)

    lines.append('')
    lines.append('/* a6xx_gmu_power_config() + a6xx_gmu_start(): the real tail of fw_start,')
    lines.append(' * after the GBIF writes -- RPMH_CTRL\'s GPU-client vote enable is')
    lines.append(' * unconditional for A660 (not idle-level-gated), and the CM3 reset must')
    lines.append(' * actually be toggled 1->0 (this was missing from an earlier draft of this')
    lines.append(' * driver entirely -- see lemonadep/HANDOFF.md). */')
    for n, o in power_config:
        lines.append('#define A660_%s 0x%06x' % (n, core_byte_offset(o)))
    rpmh_mask = (1 << 0) | (1 << 4) | (1 << 8) | (1 << 9) | (1 << 10) | (1 << 11)
    lines.append('/* RPMH_INTERFACE|LLC_VOTE|DDR_VOTE|MX_VOTE|CX_VOTE|GFX_VOTE _ENABLE bits */')
    lines.append('#define LEMONADEP_A660_RPMH_CTRL_ENABLE_MASK 0x%03xu' % rpmh_mask)
    lines.append('#define LEMONADEP_A660_VAL_SYS_BUS_CONFIG 1u')
    lines.append('#define LEMONADEP_A660_VAL_ICACHE_CONFIG 1u')
    lines.append('#define LEMONADEP_A660_VAL_DCACHE_CONFIG 1u')
    lines.append('/* a6xx_gmu_start(): read DTCM_START+0xff8 first to pick the ready pattern')
    lines.append(' * (this is real, generation-independent logic in the pinned source, not')
    lines.append(' * an A740 simplification -- confirmed by reading a6xx_gmu_start() directly).')
    lines.append(' * val<=0x20010004 -> mask=0xffffffff, ready=0xbabeface; else -> mask=0x1ff,')
    lines.append(' * ready=0x100. */')
    lines.append('#define LEMONADEP_A660_GMU_START_PROBE_OFFSET 0xff8u  /* add to DTCM_START */')
    lines.append('#define LEMONADEP_A660_GMU_START_PROBE_THRESHOLD 0x20010004u')
    lines.append('#define LEMONADEP_A660_GMU_START_MASK_LOW 0xffffffffu')
    lines.append('#define LEMONADEP_A660_GMU_START_READY_LOW 0xbabefaceu')
    lines.append('#define LEMONADEP_A660_GMU_START_MASK_HIGH 0x1ffu')
    lines.append('#define LEMONADEP_A660_GMU_START_READY_HIGH 0x100u')
    lines.append('/* a640_gbif (shared by A660, confirmed in a6xx_catalog.c): write this same value')
    lines.append(' * to GBIF_QSB_SIDE0..SIDE3 (consecutive dwords, +1 each). */')
    lines.append('#define LEMONADEP_A660_GBIF_QSB_VALUE 0x00071620u')

    lines.append('')
    lines.append('/* a660_build_bw_table() in the pinned a6xx_hfi.c -- transcribed verbatim, not')
    lines.append(' * derived (it is already hand-written per chip in the real source, dispatched')
    lines.append(' * for us by adreno_is_a660(), an exact chip_id==660 match, confirmed our')
    lines.append(' * a6xx_catalog.c entry has no .bcms override that would select a different,')
    lines.append(' * dynamically-generated table instead). Part of the real, non-legacy')
    lines.append(' * a6xx_hfi_start() sequence: send_perf_table, THIS, enable_acd, enable_ifpc,')
    lines.append(' * send_core_fw_start, send_start (a6xx_hfi.c:972-1005; A660 is not')
    lines.append(' * gmu->legacy -- that is only RGMU chips and adreno_is_a630_family(), GEN1). */')
    lines.append('struct LemonadepA660BwTable {')
    lines.append('    uint32_t bw_level_num;')
    lines.append('    uint32_t ddr_cmds_num;')
    lines.append('    uint32_t ddr_wait_bitmask;')
    lines.append('    uint32_t ddr_cmds_addrs[3];')
    lines.append('    uint32_t ddr_cmds_data0[3];  /* only level 0 ("off") is populated */')
    lines.append('    uint32_t cnoc_cmds_num;')
    lines.append('    uint32_t cnoc_wait_bitmask;')
    lines.append('    uint32_t cnoc_cmds_addrs[1];')
    lines.append('    uint32_t cnoc_cmds_data0[1];')
    lines.append('    uint32_t cnoc_cmds_data1[1];')
    lines.append('};')
    lines.append('static const struct LemonadepA660BwTable LemonadepA660BwTable_ = {')
    lines.append('    .bw_level_num = 1,')
    lines.append('    .ddr_cmds_num = 3, .ddr_wait_bitmask = 0x01,')
    lines.append('    .ddr_cmds_addrs = {0x50004,0x500a0,0x50000},')
    lines.append('    .ddr_cmds_data0 = {0x40000000,0x40000000,0x40000000},')
    lines.append('    .cnoc_cmds_num = 1, .cnoc_wait_bitmask = 0x01,')
    lines.append('    .cnoc_cmds_addrs = {0x50070},')
    lines.append('    .cnoc_cmds_data0 = {0x40000000}, .cnoc_cmds_data1 = {0x60000001},')
    lines.append('};')
    lines.append('#endif')
    text = '\n'.join(lines) + '\n'
    gmu_seq_count = (len(gmu_ao_cgc) + len(gmu_core_rsc) + len(rscc) + len(pdc) + len(fw_start) +
                     len(gbif_cx) + len(power_config))
    if '--check' in sys.argv[3:]:
        if OUT.read_text() != text:
            raise SystemExit('LemonadepA660HwInit.h is stale')
        print('PASS: %d registers, %d hwcg entries, %d protect writes, %d GMU/RSC/PDC registers '
              '(matches committed header)' % (len(defines), len(hwcg), len(writes), gmu_seq_count))
        return
    OUT.write_text(text)
    print('PASS: %d registers, %d hwcg entries, %d protect writes, %d GMU/RSC/PDC registers' %
          (len(defines), len(hwcg), len(writes), gmu_seq_count))


main()
