#!/bin/bash
# Fetch the real, pinned Linux a6xx GPU driver/register sources (the same commit
# github.com/UniXNU/UniXNU's own scripts/data/adreno-inputs.json pins, sha256-verified
# against that record), then derive LemonadepA660HwInit.h from them -- the A660 equivalent
# of UniXNU's own scripts/derive-a740-hwinit.py, adapted because A660 is plain a6xx gen4,
# not an A7XX variant like UniXNU's own sheng (SM8550/Adreno 740) target.
#
#   lemonadep/xnu/gpu/fetch-and-derive.sh [WORKDIR]      (default /tmp/lemonadep-a660-src)
set -euo pipefail
W=${1:-/tmp/lemonadep-a660-src}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=ianchb/sm8550-mainline
COMMIT=42f3b40c702abbc37c3a04c95af008cd283aeaf3
BASE="https://raw.githubusercontent.com/$REPO/$COMMIT"

mkdir -p "$W/drivers/gpu/drm/msm/adreno" "$W/drivers/gpu/drm/msm/registers/adreno"
FILES="drivers/gpu/drm/msm/adreno/a6xx_catalog.c
drivers/gpu/drm/msm/adreno/a6xx_gpu.c
drivers/gpu/drm/msm/adreno/a6xx_gmu.c
drivers/gpu/drm/msm/adreno/a6xx_gmu.h
drivers/gpu/drm/msm/adreno/a6xx_hfi.c
drivers/gpu/drm/msm/adreno/a6xx_hfi.h
drivers/gpu/drm/msm/adreno/a6xx_gpu.h
drivers/gpu/drm/msm/adreno/adreno_gpu.h
drivers/gpu/drm/msm/registers/adreno/a6xx.xml
drivers/gpu/drm/msm/registers/adreno/a6xx_gmu.xml
drivers/gpu/drm/msm/registers/adreno/adreno_common.xml
drivers/gpu/drm/msm/registers/adreno/adreno_pm4.xml"

echo "== fetching $REPO @ $COMMIT (pinned by github.com/UniXNU/UniXNU's own record)"
for f in $FILES; do
	curl -sL "$BASE/$f" -o "$W/$f"
done

echo "== verifying against the sha256 recorded in github.com/UniXNU/UniXNU scripts/data/adreno-inputs.json"
python3 - "$W" << 'PYEOF'
import hashlib, os, sys
root = sys.argv[1]
# subset actually used by the derive script (the rest of adreno-inputs.json's list is for other
# UniXNU work -- GMU/HFI, SMMU, memory -- not needed for this header)
files = {
 'drivers/gpu/drm/msm/adreno/a6xx_catalog.c': 'ff1abc1b7735d6bd6aa07d5df776e2549be2ee70ffcc02ff24aac646dfd6a3c7',
 'drivers/gpu/drm/msm/adreno/a6xx_gpu.c': '833ec17decd679d0ae7738fa71843e009cb33012e138071cb7bb2c1a42ffe6d1',
 'drivers/gpu/drm/msm/adreno/a6xx_gmu.c': '0e9080435b24841812d1b153960d0a04c3ae4f5eea9a76e1b4ff64d7bd4a53ac',
 'drivers/gpu/drm/msm/adreno/a6xx_gmu.h': 'cfc3372ee52b9aac1cee1818a1f4d6fdd5e5bc7e091bf500422a70f834648b02',
 'drivers/gpu/drm/msm/adreno/adreno_gpu.h': '1de7a71a8fddef587ee06c960853a636ddde5550c65174b50ae0556c49de4f96',
 'drivers/gpu/drm/msm/registers/adreno/adreno_pm4.xml': 'f8137e5d31a3cfce27cba0a8bc269e460067dca9ccb95e620404b7fc752913fb',
 'drivers/gpu/drm/msm/registers/adreno/a6xx.xml': '1776505dbdb01d0355c9a828b857eaec61b286802d3509a33d6ced9e6ce54000',
}
bad = False
for f, expect in files.items():
    got = hashlib.sha256(open(os.path.join(root, f), 'rb').read()).hexdigest()
    ok = expect.startswith(got)
    print(('OK  ' if ok else 'FAIL') + ' ' + f)
    bad = bad or not ok
sys.exit(1 if bad else 0)
PYEOF

echo "== deriving LemonadepA660HwInit.h"
python3 "$HERE/derive-a660-hwinit.py" "$W" "$HERE/LemonadepA660HwInit.h" "${2:-}"
