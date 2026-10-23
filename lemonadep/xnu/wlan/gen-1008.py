#!/usr/bin/env python3
"""Regenerate lemonadep/xnu/patches/1008-lemonadep-wlan-pcie.patch.

Wi-Fi stage 1 lives in lemonadep/xnu/wlan/ (host-tested there). This injects
it into xnu as new files and edits three existing ones. Edits are real diffs
against a tree that already has every earlier patch applied (lemonadep/xnu/
check-overlay.sh WORKDIR leaves one at WORKDIR/xnu), so hunk context can't be
wrong:

  python3 lemonadep/xnu/wlan/gen-1008.py /root/work/xnu-ref/xnu
"""
import difflib
import os
import sys

WLAN = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(WLAN, '..', '..', '..'))
OUT = os.path.join(REPO, 'lemonadep', 'xnu', 'patches', '1008-lemonadep-wlan-pcie.patch')
XNU = sys.argv[1]

NEW = [
    ('bsd/dev/lemonadep_wlan.c', 'lemonadep_wlan_init.c'),
    ('bsd/dev/lemonadep/wlan/lemonadep_wlan_pcie.c', 'lemonadep_wlan_pcie.c'),
    ('bsd/dev/lemonadep/wlan/LemonadepWlanSource.h', 'LemonadepWlanSource.h'),
    ('bsd/dev/lemonadep/wlan/LemonadepPcie0Phy.h', 'LemonadepPcie0Phy.h'),
    ('bsd/dev/lemonadep/wlan/lemonadep_wlan_mhi.c', 'lemonadep_wlan_mhi.c'),
    ('bsd/dev/lemonadep/wlan/LemonadepMhiSource.h', 'LemonadepMhiSource.h'),
]


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read().replace('\r\n', '\n')


def edit(rel, change):
    old = read(os.path.join(XNU, rel))
    new = change(old)
    assert new != old, f'{rel}: edit did not apply'
    return ''.join(difflib.unified_diff(old.splitlines(True), new.splitlines(True),
                                        f'a/{rel}', f'b/{rel}', n=3))


def once(text, anchor, replacement):
    assert text.count(anchor) == 1, f'anchor not unique: {anchor!r}'
    return text.replace(anchor, replacement)


parts = [
    edit('bsd/conf/files', lambda t: once(
        t, 'bsd/dev/lemonadep_gpu.c\t\tstandard\n',
        'bsd/dev/lemonadep_gpu.c\t\tstandard\nbsd/dev/lemonadep_wlan.c\t\tstandard\n')),
    edit('bsd/dev/sweet_ufs.c', lambda t: once(once(
        t, 'extern void lemonadep_gpu_init(void);\n',
        'extern void lemonadep_gpu_init(void);\nextern void lemonadep_wlan_init(void);\n'),
        '\tlemonadep_gpu_init();\n',
        '\tlemonadep_gpu_init();\n'
        '\t/* Wi-Fi stage 1 (lemonadep/HANDOFF.md "Wi-Fi, stage 1"). */\n'
        '\tlemonadep_wlan_init();\n')),
    edit('bsd/dev/sweet_rpmh.c', lambda t: read(os.path.join(WLAN, 'sweet_rpmh.c'))),
]
for dest, src in NEW:
    lines = read(os.path.join(WLAN, src)).split('\n')
    if lines[-1] == '':
        lines.pop()
    parts.append(f'--- /dev/null\n+++ b/{dest}\n@@ -0,0 +1,{len(lines)} @@\n' +
                 ''.join(f'+{l}\n' for l in lines))

with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
    f.write(''.join(parts))
print(f'{OUT}: {len(NEW)} new files, 3 edited')
