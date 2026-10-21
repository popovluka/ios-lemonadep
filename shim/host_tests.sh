#!/bin/bash
# Host tests for the shim's board-independent code (payload scan, FDT walk,
# ADT patching). Needs gcc, dtc and python3.
set -euo pipefail
cd "$(dirname "$0")"
T=out/host-tests
mkdir -p "$T"
dtc -q -I dts -O dtb -o "$T/mini.dtb" test/mini.dts
python3 ../tools/mkadt.py "$T/lemonadep.adt" --board lemonadep --dram 0x100000000 0x40000000 2>/dev/null
gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o "$T/host_test_loader" \
	test/host_test_loader.c payload_scan.c fdt.c adt_patch.c
"$T/host_test_loader" "$T/mini.dtb" "$T/lemonadep.adt" "$T/patched.adt"
# The patched ADT must still parse and carry the new values.
python3 - "$T/patched.adt" <<'EOF'
import struct, sys
sys.path.insert(0, '../tools')
import adt
root, end = adt.parse(open(sys.argv[1], 'rb').read())
fb = next(c for c in root.children if c.name == 'sweet-fbcon')
got = {k: fb.prop(k) for k in ('width', 'height', 'stride', 'reg')}
assert struct.unpack('<I', got['width'])[0] == 1080, got
assert struct.unpack('<I', got['height'])[0] == 2412, got
assert struct.unpack('<I', got['stride'])[0] == 4352, got
assert struct.unpack('<2Q', got['reg']) == (0xe4d00000, 0x2300000), got
print('patched ADT parses: ok')
EOF
