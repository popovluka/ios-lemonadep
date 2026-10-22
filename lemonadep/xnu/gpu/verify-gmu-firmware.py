#!/usr/bin/env python3
"""Parse the real a660_gmu.bin (lemonadep/fw-gpu/, pulled from this phone's own
/vendor -- see fw-gpu/pull.sh) with the exact non-legacy block format
a6xx_gmu_fw_load() uses (the pinned a6xx_gmu.c, ianchb/sm8550-mainline @ 42f3b40c):

    struct block_header { u32 addr, size, type, value; u32 data[]; };
    NEXT_BLK(blk) = blk + sizeof(header) + blk->size   (a6xx_gmu.c:843-861)
    a block with size==0 is skipped entirely            (a6xx_gmu.c:897-898)

A660 takes the non-legacy path (adreno_is_a650_family(): dtcm_base=0x10004000,
confirmed at a6xx_gmu.c:875-878) and is not gmu->legacy (that is only RGMU
chips and adreno_is_a630_family(), GEN1), so the block-walking loader applies,
not ShengGmu.c's fixed-region AdrenoFirmwarePlanGmu() assumption.

This is a real check, not a derivation: it fails loudly if the actual pulled
firmware doesn't match the segment layout this file documents, instead of
silently trusting notes written against a different firmware build.

    lemonadep/xnu/gpu/verify-gmu-firmware.py [path/to/a660_gmu.bin]
"""
import hashlib
import struct
import sys
from pathlib import Path

ITCM_BASE, TCM_LEN = 0x00000000, 0x4000
DTCM_BASE = 0x10004000  # a650_family/a7xx/a8xx value; A660 is a650_family (GEN4)
ICACHE_IOVA = 0x4000    # ShengGmu.c's convention; confirmed below against this firmware

path = Path(sys.argv[1] if len(sys.argv) > 1 else
            Path(__file__).resolve().parents[3] / 'lemonadep/fw-gpu/a660_gmu.bin')
data = path.read_bytes()

blocks = []
zero_size_count = 0
off = 0
header_bytes = 0
while off + 16 <= len(data):
    addr, size, typ, val = struct.unpack_from('<IIII', data, off)
    header_bytes += 16
    if size == 0:
        zero_size_count += 1
    else:
        blocks.append((addr, size, typ, val, off + 16))
    off += 16 + size
if off != len(data):
    raise SystemExit('block walk did not consume the whole file (%d of %d bytes) -- '
                      'format mismatch, do not trust the segment map below' % (off, len(data)))

payload_bytes = sum(b[1] for b in blocks)
print('file: %s (%d bytes), sha256=%s' % (path, len(data), hashlib.sha256(data).hexdigest()))
print('%d zero-size header blocks (skipped by the real loader) + %d payload blocks, '
      '%d bytes of headers + %d bytes of payload = %d (file is %d: %s)' %
      (zero_size_count, len(blocks), header_bytes, payload_bytes, header_bytes + payload_bytes,
       len(data), 'matches' if header_bytes + payload_bytes == len(data) else 'MISMATCH'))

for addr, size, typ, val, payoff in blocks:
    if ITCM_BASE <= addr < ITCM_BASE + TCM_LEN:
        region = 'ITCM (CM3_ITCM_START) offset 0x%x' % (addr - ITCM_BASE)
    elif DTCM_BASE <= addr < DTCM_BASE + TCM_LEN:
        region = 'DTCM (CM3_DTCM_START) offset 0x%x' % (addr - DTCM_BASE)
    else:
        region = 'icache (fw_block_mem, addr treated as IOVA) offset 0x%x from IOVA 0x%x' % (
            addr - ICACHE_IOVA, ICACHE_IOVA) if addr >= ICACHE_IOVA else 'UNRECOGNIZED region'
    print('  addr=0x%08x size=%6d -> %s' % (addr, size, region))

ok = len(blocks) == 4
if not ok:
    print('UNEXPECTED block count (%d, expected 4) -- re-check by hand' % len(blocks))

# Cross-check LemonadepGmuSource.h's hand-written table against this live parse: it was
# hand-typed once already with wrong file_offset values (caught only by this check being
# added afterwards), so never trust it un-verified again.
src = Path(__file__).with_name('LemonadepGmuSource.h')
if src.exists():
    import re
    text = src.read_text()
    m = re.search(r'LemonadepGmuFwBlocks\[4\]\s*=\s*\{(.*?)\};', text, re.S)
    rows = re.findall(r'\{\s*(\d+),\s*(0x[0-9a-fA-F]+)u?,\s*(\d+)u?,', m.group(1)) if m else []
    expect = [(off, addr, size) for addr, size, typ, val, off in blocks]
    got = [(int(o), int(a, 16), int(s)) for o, a, s in rows]
    if got == expect:
        print('header check: LemonadepGmuSource.h\'s LemonadepGmuFwBlocks matches this parse')
    else:
        ok = False
        print('header check: LemonadepGmuSource.h\'s LemonadepGmuFwBlocks does NOT match this '
              'parse -- fix it before trusting the driver code')
        print('  live:   %s' % expect)
        print('  header: %s' % got)
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
