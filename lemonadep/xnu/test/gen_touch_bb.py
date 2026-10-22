#!/usr/bin/env python3
"""Make a host-testable copy of the patched bsd/dev/sweet_touch.c: the two MMIO
accessors (touch_rd/touch_wr) are redirected to mock_rd/mock_wr so that
test_touch_bb.c can model TLMM pins, an open-drain I2C bus and a Synaptics
TCM touch IC behind the real driver code (bit-bang, reset, TCM bring-up,
report parsing). Everything else is compiled verbatim.

  gen_touch_bb.py XNU OUTDIR
"""
import re, sys

xnu, out = sys.argv[1], sys.argv[2]
src = open(f'{xnu}/bsd/dev/sweet_touch.c').read()

rd = re.search(r'\nstatic uint32_t\ntouch_rd\(vm_offset_t base, uint32_t off\)\n\{.*?\n\}\n', src, re.S)
wr = re.search(r'\nstatic void\ntouch_wr\(vm_offset_t base, uint32_t off, uint32_t val\)\n\{.*?\n\}\n', src, re.S)
if not rd or not wr:
    sys.exit('touch_rd/touch_wr not found')
src = src.replace(rd.group(0), '''
uint32_t mock_rd(vm_offset_t base, uint32_t off);
static uint32_t
touch_rd(vm_offset_t base, uint32_t off)
{
	return mock_rd(base, off);
}
''')
src = src.replace(wr.group(0), '''
void mock_wr(vm_offset_t base, uint32_t off, uint32_t val);
static void
touch_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	mock_wr(base, off, val);
}
''')
if '__asm__' in src:
    sys.exit('unexpected inline asm left outside touch_rd/touch_wr')
# kernel headers are replaced by the declarations in test_touch_bb.c
src = re.sub(r'^#include <[^>]+>\n', '', src, flags=re.M)
open(f'{out}/sweet_touch_host.c', 'w').write(src)
print('extracted sweet_touch_host.c')
