#!/usr/bin/env python3
"""Extract overlay 1005's TCM message-framing functions verbatim from the
patched xnu tree, for a standalone host test (test_touch_tcm.c) that never
needs a real kernel build to catch a framing bug.

  gen_touch.py XNU OUTDIR
"""
import re, sys

xnu, out = sys.argv[1], sys.argv[2]
src = open(f'{xnu}/bsd/dev/sweet_touch.c').read()


def func(name):
    """One top-level function's text, from its signature line to the closing
    brace at column 0 (this file's own style: no nested top-level braces)."""
    m = re.search(r'\nstatic int\n%s\(.*?\n\{.*?\n\}\n' % re.escape(name), src, re.S)
    if not m:
        sys.exit(f'{name} not found')
    return m.group(0).strip() + '\n'


def defines(*names):
    out_lines = []
    for name in names:
        m = re.search(r'^#define %s\s+.*$' % re.escape(name), src, re.M)
        if not m:
            sys.exit(f'{name} not in sweet_touch.c')
        out_lines.append(m.group(0))
    return '\n'.join(out_lines) + '\n'


consts = defines('TCM_MARKER', 'TCM_CMD_CONTINUE_WRITE', 'TCM_STATUS_CONTINUED_READ')
write_fn = func('touch_tcm_write')
read_fn = func('touch_tcm_read')
open(f'{out}/touch_tcm_frag.inc', 'w').write(consts + '\n' + write_fn + '\n' + read_fn)
print(f'extracted: touch_tcm_write {write_fn.count(chr(10))} lines, '
      f'touch_tcm_read {read_fn.count(chr(10))} lines')
