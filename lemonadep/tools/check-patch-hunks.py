import re, glob, sys

SEP = re.compile(r'^(@@ |--- |\+\+\+ |diff )')
HDR = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@')


def check(path):
    data = open(path, encoding="utf-8", errors="replace").read()
    if data.endswith("\n"):
        data = data[:-1]
    lines = data.split("\n")
    bad = []
    i = 0
    while i < len(lines):
        m = HDR.match(lines[i])
        if not m:
            i += 1
            continue
        o_dec = int(m.group(2)) if m.group(2) is not None else 1
        n_dec = int(m.group(4)) if m.group(4) is not None else 1
        old = new = 0
        pending_blanks = 0
        j = i + 1
        while j < len(lines):
            l = lines[j]
            if SEP.match(l):
                break
            if l.startswith("\\"):          # "\ No newline at end of file"
                j += 1
                continue
            if l == "":
                pending_blanks += 1          # might be context, might be a separator
                j += 1
                continue
            if pending_blanks:               # real content followed -> they were context
                old += pending_blanks
                new += pending_blanks
                pending_blanks = 0
            if l.startswith("+"):
                new += 1
            elif l.startswith("-"):
                old += 1
            elif l.startswith(" "):
                old += 1
                new += 1
            else:
                break
            j += 1
        # A blank line trailing a hunk is ambiguous in these files: sometimes a
        # genuine blank context line, sometimes just a separator before the next
        # hunk. Accept the hunk if either reading matches the declared counts.
        variants = {(old, new), (old + pending_blanks, new + pending_blanks)}
        if (o_dec, n_dec) not in variants:
            bad.append((i + 1, lines[i][:46], o_dec, n_dec, old, new))
        i = max(j, i + 1)
    return bad


files = sorted(glob.glob("xnu/patches/*.patch")) + sorted(glob.glob("lemonadep/xnu/patches/*.patch"))
total = 0
for f in files:
    b = check(f)
    if b:
        total += len(b)
        print("\n" + f)
        for ln, hdr, od, nd, oa, na in b:
            print("   line %d: %r  declared old=%d new=%d  actual old=%d new=%d" % (ln, hdr, od, nd, oa, na))
print("\n%d patches checked, %d mismatched hunks" % (len(files), total))
sys.exit(1 if total else 0)
