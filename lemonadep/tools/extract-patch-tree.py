#!/usr/bin/env python3
"""Materialise every new-file hunk of a patch into a directory tree.

  extract_tree.py PATCH OUTDIR

Only pure additions (`--- /dev/null`) are written, which is what patch 1007 is
apart from its two edits to existing files. Lets the generated sources be
syntax-checked without a full XNU checkout.
"""
import os
import sys


def main():
    patch, outdir = sys.argv[1], sys.argv[2]
    lines = open(patch, encoding="utf-8").read().split("\n")
    i, n = 0, len(lines)
    written = 0
    while i < n:
        if lines[i] == "--- /dev/null" and i + 1 < n and lines[i + 1].startswith("+++ b/"):
            path = lines[i + 1][len("+++ b/"):]
            i += 2
            if i < n and lines[i].startswith("@@"):
                i += 1
            body = []
            while i < n and lines[i].startswith("+"):
                body.append(lines[i][1:])
                i += 1
            dest = os.path.join(outdir, path)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(body) + "\n")
            print("%-60s %4d lines" % (path, len(body)))
            written += 1
        else:
            i += 1
    print("wrote", written, "files into", outdir)


if __name__ == "__main__":
    main()
