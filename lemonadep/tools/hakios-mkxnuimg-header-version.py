#!/usr/bin/env python3
"""Make mkxnuimg.py honour the device's [boot_image] header_version.

It hardcodes --header_version 1, which is redwood's value; lemonadep.toml asks
for 3, and a OnePlus 9 Pro's bootloader rejects the v1 image and drops back to
fastboot. Read the value from Resources/Configs/<DEVICE>.toml, default 1.
"""
import os
import sys

p = os.path.expanduser("~/hakios/hakios-xnu-kernel/mkxnuimg.py")
s = open(p, encoding="utf-8").read()
orig = s

helper = '''
def _header_version():
    """[boot_image] header_version from the device's Silicium config, default 1.

    mkxnuimg.py used to hardcode 1, which is redwood's value. lemonadep asks
    for 3 and its boot loader refuses a v1 image (it returns to fastboot), so
    the per-device config has to win. A tiny reader, not tomllib: the file is
    five short tables and this has to work on the build hosts as they are.
    """
    for base in (MU, HERE):
        path = os.path.join(base, 'Resources', 'Configs', DEVICE + '.toml')
        if not os.path.isfile(path):
            continue
        section = None
        with open(path, encoding='utf-8') as f:
            for line in f:
                line = line.split('#', 1)[0].strip()
                if line.startswith('[') and line.endswith(']'):
                    section = line[1:-1].strip()
                elif section == 'boot_image' and '=' in line:
                    key, _, value = line.partition('=')
                    if key.strip() == 'header_version':
                        return str(int(value.strip()))
    return '1'


'''

anchor = "shim = open(parts['shim'], 'rb').read()"
if anchor not in s:
    sys.exit("anchor not found")
s = s.replace(anchor, helper.lstrip("\n") + anchor, 1)

old = "'--header_version', '1', '-o', args.output])"
new = "'--header_version', _header_version(), '-o', args.output])"
if old not in s:
    sys.exit("header_version call not found")
s = s.replace(old, new, 1)

if s == orig:
    sys.exit("no change")
open(p, "w", encoding="utf-8", newline="\n").write(s)
print("patched", p)
