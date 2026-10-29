#!/usr/bin/env python3
"""Generate the ios-sweet overlay patch that imports HakiOS's Adreno driver.

  lemonadep/tools/import-hakios-adreno.py HAKIOS_REPO [-o PATCH]

`drivers/gpu/hakios.adreno.c` in oosexgang/hakios-xnu-kernel (branch `hakios`)
wakes an Adreno 660 in 22 boot-arg-driven steps, ending at a picture on the
panel, and it already uses the addresses lemonadep needs: every GPU MMIO window
(gcc 0x00100000, gpucc 0x03d90000, gmu 0x03d6a000, gpu 0x03d00000, cx_misc
0x03d9e000, rscc 0x03de0000, pdc 0x0b290000, smmu 0x03da0000) and CMD_DB
0x80860000 match the values this project verified on real lemonadep silicon.

A generator rather than a copy, so a new upstream revision is one re-run, and so
the adaptation is reviewable as a short list instead of a 3850-line diff. Every
anchor below must match exactly; if upstream moves, this fails loudly rather
than emitting a patch that silently drops a change.

Licence: that file is APSL 2.0 (hakios-xnu-kernel's LICENSE puts `drivers/`
under the Apple Public Source License 2.0 as an XNU derivative). It keeps its
header, and the inserted notice records origin, revision and licence.

What is adapted, and why:

1. `#include <sys/hakios.input.h>` is dropped: nothing in the file uses it.
2. `image_file()` reads from HakiOS's small root image through
   `hakios_image_file()`. ios-sweet has no such image; firmware travels as
   extra blobs placed by tools/mkpayload.py and announced by boot-args, which
   is how a660_gmu.bin already reaches lemonadep_gpu.c. Replaced with a lookup
   over those.
3. The `pe_arm_get_soc_base_phys() != HAKIOS_SOC_BASE` guard refuses to touch
   the GPU unless the SoC base is redwood's. Replaced with ios-sweet's
   `board=lemonadep` check, which means the same thing here.
4. Six symbols belong to HakiOS's own kernel and are shimmed: `loader_log_putc`
   and `loader_return` (its boot loader), `MiniIOMobileFramebufferShow` (its
   framebuffer -- lemonadep has redwood_dpu instead, so for now the draw is
   reported, not shown), `hakios_tl` (its trace log -> printf), and
   `HakiosGPUStart`/`HakiosGPUStop` (the IOKit service of hakios.gpu.cpp, which
   is phase 2 of this port).

`hakios.gpu.cpp`, MiniIOSurface and MiniIOMobileFramebuffer are deliberately
not imported yet: `adreno=N` alone runs steps 1..N and tears down after each
run, which is the right shape for bring-up on hardware.
"""
import argparse
import os
import subprocess
import sys

SRC = "drivers/gpu/hakios.adreno.c"
DEST = "bsd/dev/hakios_adreno.c"

NOTICE = """/*
 * Imported into ios-sweet from HakiOS by lemonadep/tools/import-hakios-adreno.py.
 *
 *   source:   oosexgang/hakios-xnu-kernel, branch hakios, {rev}
 *   file:     {src}
 *   licence:  Apple Public Source License 2.0 (that repository's LICENSE puts
 *             drivers/ under APSL 2.0 as a derivative work of Apple's XNU);
 *             this copy keeps the original header below and its notices.
 *
 * The driver is unchanged except for the adaptations the generator documents:
 * the firmware comes from mkpayload blobs instead of HakiOS's root image, the
 * board guard is ios-sweet's, and six HakiOS kernel symbols are shimmed. Its
 * register addresses already match lemonadep; see HANDOFF.md.
 */
"""

SHIMS = """
/* ---- ios-sweet shims for HakiOS kernel services (import-hakios-adreno.py) -- */

extern vm_offset_t phystokv(uint64_t pa);

/*
 * No kmem_alloc adaptation is needed, and an earlier version of this generator
 * was wrong to add one. xnu-8796.101.5 (macOS 13.5, what this tree builds)
 * already has KMA_DATA and the five-argument kmem_alloc() that HakiOS uses --
 * osfmk/vm/vm_kern.h:578 and :643, the latter an inline over
 * kernel_memory_allocate(). The guard tried was `#ifndef KMA_DATA`, which is
 * always true because KMA_DATA is an enum constant and not a macro, so the
 * compatibility path fired on a tree that did not want it and called a
 * kmem_alloc_flags() that does not exist here.
 */

/*
 * The prototypes sys/hakios.input.h gave this file. That header is HakiOS's own
 * and is not imported, so the declarations it provided are restored here: the
 * "GPU kept on" interface that hakios.gpu.cpp calls (phase 2 of this port, but
 * the functions are defined in this file and need prototypes), and this
 * driver's own entry points.
 */
bool hakios_gpu_ready(void);
bool hakios_gpu_parameter(uint32_t which, uint64_t *value);
uint64_t hakios_gpu_address(uint64_t size);
void hakios_gpu_address_free(uint64_t address, uint64_t size);
bool hakios_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void hakios_gpu_mapped(void);
void hakios_gpu_unmap(uint64_t address, uint64_t size);
void hakios_gpu_to_memory(void *start, uint64_t size);
uint32_t hakios_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
bool hakios_gpu_done(uint32_t fence);
uint32_t hakios_gpu_last(void);
void hakios_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7]);
void hakios_gpu_report(const char *why);
void hakios_gpu_stop(void);
void hakios_adreno_init(void);
void hakios_adreno_stop(void);
void hakios_adreno_quiesce(void);
void hakios_adreno_minute(void);

/*
 * HakiOS's boot loader takes a character at a time from the kernel and writes
 * the tail of the log to storage. ios-sweet keeps its console log in a ring at
 * the end of userdata (patch 0047, read back with tools/sweet-log-pull.sh), and
 * printf already goes there, so a per-character sink would duplicate it.
 */
static void
loader_log_putc(char c)
{
\t(void)c;
}

/* Returns control to HakiOS's loader after a run. Nothing to return to here. */
static void
loader_return(void)
{
}

/* HakiOS's trace line. vsnprintf, as this file's own say() uses: it is the
 * formatter XNU is known to provide here. */
static void __printflike(1, 2)
hakios_tl(const char *format, ...)
{
\tchar line[400];
\tva_list args;

\tva_start(args, format);
\tvsnprintf(line, sizeof(line), format, args);
\tva_end(args);
\tprintf("%s", line);
}

/*
 * Step 22 hands a drawn picture to HakiOS's framebuffer. lemonadep drives its
 * panel through redwood_dpu (patch 1011) instead, which does not take a buffer
 * this way yet, so the draw is reported and not shown. Phase 2 of the port.
 */
static void
MiniIOMobileFramebufferShow(const uint8_t *pixels, uint32_t width, uint32_t height,
    uint32_t row_bytes)
{
\tprintf("adreno: the GPU drew %ux%u (%u bytes a row) at %p; showing it needs the DPU "
\t    "path (HANDOFF.md, phase 2 of the HakiOS GPU port)\\n", width, height, row_bytes, pixels);
}

/* The IOKit service that keeps the GPU for the window server: not imported yet. */
static void
HakiosGPUStart(void)
{
\tprintf("adreno: HakiosGPU is not imported yet; the GPU is released at the end of the run\\n");
}

static void
HakiosGPUStop(void)
{
}

/*
 * Firmware. ios-sweet has no small root image: tools/mkpayload.py places each
 * file as an extra blob and reports where it went in a pair of boot-args, the
 * mechanism a660_gmu.bin already arrives by (lemonadep_gpu.c reads gmufw_pa and
 * gmufw_bytes). lemonadep/m6-hakios-gpu-build.sh passes all four.
 */
static bool image_asked_quietly;

static const uint8_t *
image_file(const char *directory, const char *name, uint64_t *size)
{
\tstatic const struct {
\t\tconst char *name, *pa_arg, *size_arg;
\t} blobs[] = {
\t\t{ "a660_gmu.bin", "gmufw_pa", "gmufw_bytes" },
\t\t{ "a660_sqe.fw", "sqefw_pa", "sqefw_bytes" },
\t\t{ "a660_zap.mdt", "zapmdt_pa", "zapmdt_bytes" },
\t\t{ "a660_zap.b02", "zapb02_pa", "zapb02_bytes" },
\t};

\t(void)directory;
\tfor (unsigned i = 0; i < sizeof(blobs) / sizeof(blobs[0]); i++) {
\t\tuint64_t pa = 0;
\t\tuint32_t bytes = 0;

\t\tif (strcmp(name, blobs[i].name) != 0) {
\t\t\tcontinue;
\t\t}
\t\tif (PE_parse_boot_argn(blobs[i].pa_arg, &pa, sizeof(pa)) &&
\t\t    PE_parse_boot_argn(blobs[i].size_arg, &bytes, sizeof(bytes)) &&
\t\t    pa != 0 && bytes != 0) {
\t\t\t*size = bytes;
\t\t\tif (!image_asked_quietly) {
\t\t\t\tprintf("adreno: %u: %s: %u bytes at pa 0x%llx\\n", step, name, bytes, pa);
\t\t\t}
\t\t\treturn (const uint8_t *)phystokv(pa);
\t\t}
\t\tbreak;
\t}
\tif (!image_asked_quietly) {
\t\tprintf("adreno: %u: no blob for %s: pass it with mkpayload --extra-blob "
\t\t    "(lemonadep/m6-hakios-gpu-build.sh)\\n", step, name);
\t}
\treturn NULL;
}

/*
 * measure_clocks() exists to answer one question the hardware will not: whether
 * the GPU block's AHB register bus has its clock, because gpucc 0x1078 reads 0
 * either way. It answers it with the SoC's frequency counter, whose debug-mux
 * constants are SC7280/SM7325's. On lemonadep those registers read back
 * 0x80000000 and the crystal measures 0 Hz, so the counter cannot be believed
 * here, and the step aborted with every clock already correctly programmed --
 * gmu_clk_src = 0x602 (200 MHz) and cx_gdscr = 0xf8222000 are in the log.
 *
 * On this phone the question has an answer from evidence instead: GPUCC
 * registers are read and written over that bus throughout this driver and
 * ios-sweet's own, and patch 1007's driver brought the GMU's firmware up
 * through it (HANDOFF.md, "GMU MILESTONE REACHED"). So record that and go on,
 * rather than treat an inapplicable measurement as a failure. Finding lahaina's
 * own mux constants would be better; nobody has them yet.
 */
static bool
lemonadep_ahb_is_known_good(void)
{
\tprintf("adreno: %u: the counter's mux constants belong to another SoC; taking the register bus as "
\t    "clocked, which this phone has already shown it is (HANDOFF.md)\\n", step);
\treturn true;
}

/* ---- end of the ios-sweet shims ------------------------------------------- */
"""

BOARD_GUARD = """\tchar board[16] = { 0 };

\t/* ios-sweet's own board check, in place of HakiOS's SoC-base test: these
\t * addresses are the 9 Pro's, and nothing here runs anywhere else. */
\tif (!PE_parse_boot_argn("board", board, sizeof(board)) ||
\t    strcmp(board, "lemonadep") != 0) {
\t\tprintf("adreno: THE GPU IS NOT STARTED: this wants board=lemonadep\\n");
\t\treturn false;
\t}
"""


def edits(text):
    """(old, new, description) in the order they are applied."""
    return [
        ("#include <sys/hakios.input.h>\n", "",
         "drop the hakios.input.h include; the shims restore the prototypes it gave"),

        # The externs the shims replace. Each is removed on its own so that an
        # upstream rename fails here instead of leaving a duplicate definition.
        ("extern void loader_log_putc(char c);\n", "",
         "loader_log_putc becomes a shim"),
        ("extern void loader_return(void);\n", "",
         "loader_return becomes a shim"),
        ("extern void hakios_tl(const char *format, ...) __printflike(1, 2);\n", "",
         "hakios_tl becomes a shim"),
        ("extern void HakiosGPUStart(void);\n", "",
         "HakiosGPUStart becomes a shim"),
        ("extern void HakiosGPUStop(void);       /* takes the clients' lock, then calls "
         "hakios_gpu_stop() */\n", "",
         "HakiosGPUStop becomes a shim"),
        ("extern void MiniIOMobileFramebufferShow(const uint8_t *pixels, uint32_t width, "
         "uint32_t height, uint32_t row_bytes);\n", "",
         "MiniIOMobileFramebufferShow becomes a shim"),
        ("extern vm_offset_t pe_arm_get_soc_base_phys(void);\n", "",
         "the SoC-base guard is replaced by the board check"),

        # The whole image_file block, replaced by the shims further down.
        ("static bool image_asked_quietly;        /* image_file() says nothing: "
         "is the file there at all */\n", "",
         "image_asked_quietly moves into the shims"),
        ("extern const uint8_t *hakios_image_file(const char *dir, const char *name, "
         "uint64_t *size, int *disk_out);\n", "",
         "hakios_image_file is not available here"),

        # The frequency counter's verdict, which is inapplicable on lahaina: see
        # lemonadep_ahb_is_known_good() in the shims for why continuing is right.
        ("\t\tprintf(\"adreno: %u: the counter did not finish: it cannot be used as it is set "
         "up here\\n\", step);\n\t\treturn false;\n",
         "\t\tprintf(\"adreno: %u: the counter did not finish: it cannot be used as it is set "
         "up here\\n\", step);\n\t\treturn lemonadep_ahb_is_known_good();\n",
         "an unusable counter no longer fails step 5"),
        ("\t\tprintf(\"adreno: %u: the counter does not show the crystal's rate: nothing it says "
         "can be used\\n\", step);\n\t\treturn false;\n",
         "\t\tprintf(\"adreno: %u: the counter does not show the crystal's rate: nothing it says "
         "can be used\\n\", step);\n\t\treturn lemonadep_ahb_is_known_good();\n",
         "a counter that cannot see the crystal no longer fails step 5"),

        # cx_ready() gates every step from 6 on, and consults the same counter
        # separately: on-device step 5 passed and step 6 still refused with
        # "ahb measures 0 Hz". Same substitution, same reasoning.
        ("\tif (ahb_hz == MEASURE_FAILED || ahb_hz < 1000000) {\n"
         "\t\tprintf(\"adreno: %u: without that clock a register of the GPU block would not "
         "answer: not touching it\\n\", step);\n\t\treturn false;\n\t}\n",
         "\tif (ahb_hz == MEASURE_FAILED || ahb_hz < 1000000) {\n"
         "\t\treturn lemonadep_ahb_is_known_good();\n\t}\n",
         "cx_ready() no longer gates steps 6+ on the unusable counter"),
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("repo", help="a checkout of oosexgang/hakios-xnu-kernel")
    ap.add_argument("-o", "--output", help="patch to write")
    args = ap.parse_args()

    src = os.path.join(args.repo, SRC)
    if not os.path.isfile(src):
        sys.exit(f"import-hakios-adreno.py: {src} not found")
    try:
        rev = subprocess.run(["git", "-C", args.repo, "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, check=True).stdout.strip()
    except Exception:
        rev = "unknown revision"

    text = open(src, encoding="utf-8").read()
    applied = []
    for old, new, why in edits(text):
        if text.count(old) != 1:
            sys.exit(f"import-hakios-adreno.py: anchor for '{why}' occurs "
                     f"{text.count(old)} times, expected once:\n  {old[:70]!r}")
        text = text.replace(old, new, 1)
        applied.append(why)

    # The HakiOS image_file() body: cut from its comment to its closing brace.
    start = text.find("/*\n * A file of the small image that travels with the kernel")
    end = text.find("\n}\n", text.find("image_file(const char *directory", start))
    if start < 0 or end < 0:
        sys.exit("import-hakios-adreno.py: could not find image_file()'s block")
    text = text[:start] + text[end + len("\n}\n"):]
    applied.append("replace image_file() with the mkpayload-blob lookup")

    if "pe_arm_get_soc_base_phys() != HAKIOS_SOC_BASE" not in text:
        sys.exit("import-hakios-adreno.py: the SoC-base guard moved")
    guard_start = text.find("\tif (pe_arm_get_soc_base_phys() != HAKIOS_SOC_BASE) {")
    guard_end = text.find("\t}\n", guard_start) + len("\t}\n")
    text = text[:guard_start] + BOARD_GUARD + text[guard_end:]
    applied.append("board=lemonadep guard")

    # The shims go after the step variables -- image_file() reports `step` -- and
    # before say() calls loader_log_putc, which is one of them. Note this file
    # does `#define printf say` further down, so a shim placed here gets the real
    # printf, which is what it wants.
    anchor = ("static bool quiet;                      /* repeating accesses that have "
              "been announced and seen to work: say nothing */")
    if text.count(anchor) != 1:
        sys.exit("import-hakios-adreno.py: the declaration block moved; the shims must "
                 "land after `step` and before say() uses loader_log_putc")
    text = text.replace(anchor, anchor + "\n" + SHIMS, 1)
    applied.append("insert the ios-sweet shims after the step variables")

    body = NOTICE.format(rev=rev, src=SRC) + text
    lines = body.split("\n")
    if lines and lines[-1] == "":
        lines.pop()

    # bsd/conf/files has to be edited here, by the last patch of the series,
    # and not in 1007. Each overlay uses the lines the previous ones added as
    # its own context, so an insertion anywhere earlier in that block breaks
    # the ones after it: putting this line in 1007 after ArmSmmu4KMmio.c stopped
    # 1011 applying, because 1011's hunk has ArmSmmu4KMmio.c followed directly
    # by bsd/dev/mem.c, and putting it mid-block stopped 1008 and 1010. Nothing
    # applies after 1012, so anchoring on the finished block is safe.
    #
    # The line numbers are the tree with 1007 (+6), 1008 (+1), 1010 (+1) and
    # 1011 (+4) applied, which puts bsd/dev/mem.c at 126. The tabs match the
    # column the surrounding entries use.
    out = [
        "--- a/bsd/conf/files",
        "+++ b/bsd/conf/files",
        "@@ -123,4 +123,5 @@",
        " bsd/dev/redwood_gauge.c\t\t\tstandard",
        " bsd/dev/redwood_battery.c\t\tstandard",
        " bsd/dev/lemonadep_hackios_glue.c\tstandard",
        "+bsd/dev/hakios_adreno.c\t\tstandard",
        " bsd/dev/mem.c\t\t\t\tstandard",
        "--- /dev/null",
        f"+++ b/{DEST}",
        f"@@ -0,0 +1,{len(lines)} @@",
    ]
    out += ["+" + l for l in lines]
    patch = "\n".join(out) + "\n"

    dest = args.output or "-"
    if dest == "-":
        sys.stdout.write(patch)
    else:
        with open(dest, "w", encoding="utf-8", newline="\n") as f:
            f.write(patch)
    print(f"import-hakios-adreno.py: {len(lines)} lines from {rev}", file=sys.stderr)
    for why in applied:
        print(f"  - {why}", file=sys.stderr)


if __name__ == "__main__":
    main()
