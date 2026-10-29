#!/usr/bin/env python3
"""Port furangelz/hackios-xnu-kernel's bsd/dev/redwood_adreno.c (branch lemonadep,
commit c349c02 -- hardware-verified: all 22 steps, a picture drawn on the lemonadep
panel) into ios-sweet overlay patch 1012, replacing the hand-adapted hakios.adreno.c
port. This file already carries a `lemonadep_values` table (gcc_debug_gpucc=0x153,
chip_id=0x06060001, gmu_chip=0x06060100, zap_memory=0x8b51a000, etc.) selected by the
`board=lemonadep` boot-arg it already reads -- so none of our own frequency-counter
bypass hacks are needed; the real root cause was the wrong debug-mux select.

Only three things are shimmed, same pattern as the hakios port:
  - loader_log_putc / loader_return: no-ops (our own console log ring covers this)
  - redwood_tl: vsnprintf then printf
  - image_file(): looks up the four firmware blobs via PE_parse_boot_argn, as mkpayload
    --extra-blob already passes them (gmufw_pa/bytes, sqefw_pa/bytes, zapmdt_pa/bytes,
    zapb02_pa/bytes)
  - MiniIOMobileFramebufferShow: reports the draw via printf (phase 2: wire to the DPU)
  - RedwoodGPUStart/RedwoodGPUStop, pe_arm_get_soc_base_phys: stub (gpu=1 "by itself"
    path is unused; we always pass adreno=N explicitly)
  - #include <sys/redwood_input.h> dropped: nothing in this file calls into it, it only
    declares prototypes (redwood_gpu_ready & co.) that this file itself defines, for
    OTHER files (the IOKit service) to call -- not needed to compile this one.
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
SRC = Path.home() / "fkernel" / "xnu" / "files" / "bsd" / "dev" / "redwood_adreno.c"
DEST = "bsd/dev/hakios_adreno.c"  # keep the registered filename/build rule from 1012
PATCH = REPO / "lemonadep" / "xnu" / "patches" / "1012-lemonadep-hakios-adreno.patch"

SHIMS = '''
/* --- ios-sweet shims (lemonadep/tools/import-redwood-adreno.py) ------------------- */
extern vm_offset_t phystokv(uint64_t pa);
static bool image_asked_quietly;        /* start_by_itself()'s gpu=1 path is unused; kept for it to compile */

/* Forward declarations for the "GPU kept on" interface this file defines further down
 * but also calls from earlier in the file (redwood_input.h normally supplies these). */
bool redwood_gpu_ready(void);
bool redwood_gpu_parameter(uint32_t which, uint64_t *value);
uint64_t redwood_gpu_address(uint64_t size);
void redwood_gpu_address_free(uint64_t address, uint64_t size);
bool redwood_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void redwood_gpu_mapped(void);
void redwood_gpu_unmap(uint64_t address, uint64_t size);
void redwood_gpu_to_memory(void *start, uint64_t size);
uint32_t redwood_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
bool redwood_gpu_done(uint32_t fence);
uint32_t redwood_gpu_last(void);
void redwood_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7]);
void redwood_gpu_report(const char *why);
void redwood_gpu_stop(void);

static void
loader_log_putc(char c)
{
	(void)c;                                /* our own console log ring already has this line */
}

static void
loader_return(void)
{
}

static void
redwood_tl(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	printf("%s", line);
}

void
MiniIOMobileFramebufferShow(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t row_bytes)
{
	(void)pixels;
	printf("adreno: a %ux%u picture (stride %u) is drawn; not yet shown on the panel (phase 2)\\n",
	    width, height, row_bytes);
}

void
RedwoodGPUStart(void)
{
}

void
RedwoodGPUStop(void)
{
}

static const uint8_t *
image_file(const char *directory, const char *name, uint64_t *size)
{
	static const struct { const char *name, *pa_arg, *bytes_arg; } blobs[] = {
		{ "a660_gmu.bin", "gmufw_pa", "gmufw_bytes" },
		{ "a660_sqe.fw", "sqefw_pa", "sqefw_bytes" },
		{ "a660_zap.mdt", "zapmdt_pa", "zapmdt_bytes" },
		{ "a660_zap.b02", "zapb02_pa", "zapb02_bytes" },
	};
	uint64_t pa = 0, bytes = 0;

	(void)directory;
	for (unsigned i = 0; i < sizeof(blobs) / sizeof(blobs[0]); i++) {
		if (strcmp(name, blobs[i].name) != 0) {
			continue;
		}
		if (!PE_parse_boot_argn(blobs[i].pa_arg, &pa, sizeof(pa)) ||
		    !PE_parse_boot_argn(blobs[i].bytes_arg, &bytes, sizeof(bytes))) {
			return NULL;
		}
		*size = bytes;
		return (const uint8_t *)phystokv(pa);
	}
	return NULL;
}
/* --- end shims --------------------------------------------------------------------- */
'''


def edits(text):
	out = []

	# Drop the header this file doesn't need (only declares prototypes this file itself defines).
	text, n = re.subn(r'#include <sys/redwood_input\.h>\n', '', text, count=1)
	assert n == 1, "redwood_input.h include not found"

	# Drop the externs/decls we are replacing with shims above (same names, now static/local).
	for pattern, desc in [
		(r'extern void loader_log_putc\(char c\);\n', "loader_log_putc extern"),
		(r'extern void loader_return\(void\);\n', "loader_return extern"),
		(r'extern void redwood_tl\(const char \*format, \.\.\.\) __printflike\(1, 2\);\n', "redwood_tl extern"),
		(r'extern void MiniIOMobileFramebufferShow\([^)]*\);\n', "MiniIOMobileFramebufferShow extern"),
		(r'extern void RedwoodGPUStart\(void\);\n', "RedwoodGPUStart extern"),
		(r'extern void RedwoodGPUStop\(void\);[^\n]*\n', "RedwoodGPUStop extern"),
		# pe_arm_get_soc_base_phys's extern is left in place: the real XNU tree already
		# defines it (pexpert/pe_identify_machine.c); redefining it here is a duplicate
		# symbol at link time.
	]:
		text, n = re.subn(pattern, '', text, count=1)
		assert n == 1, f"{desc} not found"

	# Cut the real image_file() (we replace it with the boot-arg blob version in SHIMS).
	m = re.search(
		r'static bool image_asked_quietly;.*?\n\}\n',
		text, re.DOTALL,
	)
	assert m, "image_file() block not found"
	text = text[:m.start()] + text[m.end():]

	# Insert shims right after the `quiet` flag declaration (step/pause_ms/etc. and say()/printf
	# are already declared by that point; this mirrors the working hakios port's anchor).
	anchor = 'static bool quiet;                      /* repeating accesses that have been announced and seen to work: say nothing */\n'
	assert text.count(anchor) == 1, "quiet-flag anchor not found"
	text = text.replace(anchor, anchor + SHIMS, 1)

	return text


def unified_diff_hunk(lines):
	return [
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


def main():
	text = SRC.read_text()
	text = edits(text)
	lines = text.splitlines(keepends=True)
	out = unified_diff_hunk(lines)
	for line in lines:
		body = line[:-1] if line.endswith("\n") else line
		out.append("+" + body)
	PATCH.write_text("\n".join(out) + "\n")
	print(f"wrote {PATCH} ({len(lines)} lines of {DEST})")


if __name__ == "__main__":
	sys.exit(main())
