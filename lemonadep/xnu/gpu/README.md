# lemonadep GPU (Adreno 660) work

Not an overlay patch yet -- this is the sourced register data the future GPU
overlay's kernel driver will use, plus the toolchain investigation. See
`lemonadep/HANDOFF.md`'s "GPU, Phase 0/1" sections for the full narrative and
`lemonadep/HWMAP_lemonadep.md` section 8 for the hardware facts.

## `LemonadepA660HwInit.h`

The A660 equivalent of `github.com/UniXNU/UniXNU`'s own
`scripts/derive-a740-hwinit.py` / `drivers/NativeAdreno/NativeAdrenoA740HwInit.h`,
adapted because **A660 is plain a6xx gen4, not an A7XX variant** like UniXNU's
own sheng (SM8550/Adreno 740) target -- the variant selection, and which
registers even exist, differ (see the comments in `derive-a660-hwinit.py` for
the three registers dropped because they are genuinely A7XX-only, checked
against the real `a6xx_gpu.c` `hw_init()`, not assumed).

Pinned source: `ianchb/sm8550-mainline` @ `42f3b40c702abbc37c3a04c95af008cd283aeaf3`
-- the exact commit `github.com/UniXNU/UniXNU`'s own
`scripts/data/adreno-inputs.json` records, sha256-verified against that
record before deriving anything from it.

Regenerate (fetches ~500 KB of real Linux source into a temp dir, verifies it,
re-derives the header):

```sh
lemonadep/xnu/gpu/fetch-and-derive.sh
git diff lemonadep/xnu/gpu/LemonadepA660HwInit.h   # should be empty if nothing upstream changed
```

`derive-a660-hwinit.py SRC OUT --check` compares instead of writing, for a
gate (not yet wired into `check-overlay.sh`; there's no overlay to gate yet).

## What this is not yet

- The actual kernel GPU driver (user client implementing
  `github.com/UniXNU/NativeAdreno`'s `NativeAdrenoABI.h`, GMU/CP bring-up using
  these register values, SMMU page tables, submit/fence/IRQ).
- The Apple cross-toolchain build of Turnip (Mesa + UniXNU's
  `patches/mesa/turnip-darwin-kmd.patch`) and the Metal frontend bridge.

Both need real CI time and are the next pieces of work.
