#!/usr/bin/env python3
"""Extract the 1001 code verbatim from the patched xnu tree into test sources.

  gen.py XNU OUTDIR
"""
import re, sys

xnu, out = sys.argv[1], sys.argv[2]
start = open(f'{xnu}/osfmk/arm64/start.s').read()
vm = open(f'{xnu}/osfmk/arm64/arm_vm_init.c').read()
proc = open(f'{xnu}/osfmk/arm64/proc_reg.h').read()
asm_h = open(f'{xnu}/osfmk/arm64/asm.h').read()


def block(src, must):
    """The '#if defined(SWEET)' ... '#endif' block containing `must`."""
    for m in re.finditer(r'#if defined\(SWEET\)\n(.*?)\n#endif\n', src, re.S):
        if must in m.group(1):
            return m.group(1)
    sys.exit(f'block with {must!r} not found')


band = block(start, 'Lsweet_mark_row')
fbmap = block(start, 'Lsweet_fb_block')

# Constants exactly as proc_reg.h defines them (16K granule names -> generic).
want = ['CACHE_ATTRINDX_WRITECOMB', 'SH_OUTER_MEMORY', 'TTE_SHIFT', 'ARM_TTE_VALID',
        'ARM_TTE_TYPE_BLOCK', 'ARM_TTE_BLOCK_AF', 'ARM_TTE_BLOCK_PNX', 'ARM_TTE_BLOCK_NX',
        'ARM_16K_TT_L2_SHIFT', 'ARM_16K_TT_L2_INDEX_MASK']
defs = []
for name in want:
    m = re.search(r'^#define %s\s+(.*?)(\s*/\*.*)?$' % re.escape(name), proc, re.M)
    if not m:
        sys.exit(f'{name} not in proc_reg.h')
    defs.append(f'#define {name} {m.group(1)}')
for fn in ('ARM_TTE_BLOCK_ATTRINDX', 'ARM_TTE_BLOCK_SH'):
    m = re.search(r'^#define %s\((\w)\)\s+(.*?)(\s*/\*.*)?$' % fn, proc, re.M)
    defs.append(f'#define {fn}({m.group(1)}) {m.group(2)}')
mov64 = re.search(r'\.macro MOV64\n.*?\.endmacro\n', asm_h, re.S).group(0)

consts = '\n'.join(defs) + '''
#define ARM_TT_L2_SHIFT ARM_16K_TT_L2_SHIFT
#define ARM_TT_L2_INDEX_MASK ARM_16K_TT_L2_INDEX_MASK
#define PGBYTES 0x4000
/* genassym values; checked by sweet_early.c's _Static_asserts in the kernel */
#define BA_VIDEO_BASE 40
#define BA_VIDEO_ROWBYTES 56
#define BA_VIDEO_HEIGHT 72
'''

open(f'{out}/frag.S', 'w').write(consts + mov64 + f'''
	.text
	.p2align 2
	/* run_band(boot_args) : x20 = boot_args */
	.globl run_band
run_band:
	stp	x20, x25, [sp, #-16]!
	mov	x20, x0
{band}
	ldp	x20, x25, [sp], #16
	ret

	/* run_fbmap(boot_args, pagetables) : x20 = boot_args, x25 = V=P root */
	.globl run_fbmap
run_fbmap:
	stp	x20, x25, [sp, #-16]!
	mov	x20, x0
	mov	x25, x1
{fbmap}
	ldp	x20, x25, [sp], #16
	ret
''')

m = re.search(r'(#define SWEET_EARLY_FB_VA .*?\n\}\n)#endif', vm, re.S)
open(f'{out}/map_final.inc', 'w').write(m.group(1))
# ELF (qemu run): same instructions, GNU-style named macro arguments.
elf_mov64 = mov64.replace('.macro MOV64', '.macro MOV64 reg:req, val:vararg').replace('$0', r'\reg').replace('$1', r'\val')
open(f'{out}/frag-elf.S', 'w').write(open(f'{out}/frag.S').read().replace(mov64, elf_mov64))
print('extracted: band %d lines, fbmap %d lines, map_final %d lines' %
      (band.count('\n') + 1, fbmap.count('\n') + 1, m.group(1).count('\n')))
