#!/usr/bin/env python3
"""Regenerate lemonadep/xnu/patches/1010-lemonadep-auto-reboot.patch.

Timed reset into fastboot/recovery (sweetreboot=<s>[,recovery]), polled from
the persistent-log thread. Edits are real diffs against a tree with every
earlier overlay applied but not 1010 itself (move 1010 aside, run
lemonadep/xnu/check-overlay.sh WORKDIR, use WORKDIR/xnu):

  python3 lemonadep/xnu/reboot/gen-1010.py /root/work/xnu-ref3/xnu
"""
import difflib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
OUT = os.path.join(REPO, 'lemonadep', 'xnu', 'patches', '1010-lemonadep-auto-reboot.patch')
XNU = sys.argv[1]

NEW = [
    ('bsd/dev/lemonadep_reboot.c', 'lemonadep_reboot.c'),
    ('bsd/dev/lemonadep/reboot/lemonadep_reboot_core.c', 'lemonadep_reboot_core.c'),
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


def ufs(t):
    t = once(t, 'extern void lemonadep_wlan_init(void);\n',
             'extern void lemonadep_wlan_init(void);\n'
             'extern void lemonadep_reboot_arm(void);\n'
             'extern void lemonadep_reboot_poll(void);\n')
    t = once(t, '\t\t(void)tsleep(&sweet_log_chan, PZERO, "sweetlog", hz * (int)sweet_log_secs);\n',
             '\t\t(void)tsleep(&sweet_log_chan, PZERO, "sweetlog", hz * (int)sweet_log_secs);\n'
             '\t\t/* sweetreboot=<s>: lemonadep/HANDOFF.md "Auto-reboot" */\n'
             '\t\tlemonadep_reboot_poll();\n')
    t = once(t, '\tif (kernel_thread_start(sweet_log_thread, NULL, &thread) == KERN_SUCCESS) {\n',
             '\tlemonadep_reboot_arm();\n'
             '\tif (kernel_thread_start(sweet_log_thread, NULL, &thread) == KERN_SUCCESS) {\n')
    return t


parts = [
    edit('bsd/conf/files', lambda t: once(
        t, 'bsd/dev/lemonadep_wlan.c\t\tstandard\n',
        'bsd/dev/lemonadep_wlan.c\t\tstandard\nbsd/dev/lemonadep_reboot.c\t\tstandard\n')),
    edit('bsd/dev/sweet_ufs.c', ufs),
]
for dest, src in NEW:
    lines = read(os.path.join(HERE, src)).split('\n')
    if lines[-1] == '':
        lines.pop()
    parts.append(f'--- /dev/null\n+++ b/{dest}\n@@ -0,0 +1,{len(lines)} @@\n' +
                 ''.join(f'+{l}\n' for l in lines))

with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
    f.write(''.join(parts))
print(f'{OUT}: {len(NEW)} new files, 2 edited')
