"""Replace the comment block that documents a macro.

Locates the `#ifndef NAME` / `#define NAME` that a comment block introduces,
walks back to that block, and swaps it for supplied text. Used to condense the
lab-notebook comments in openvlc_board.h into descriptions of what the knob
does now, without touching a line of code.

    python tools/rewrite_comment.py <file> <MACRO> <text-file>

The text file holds the replacement body, one paragraph per blank-line-separated
group, without comment decoration -- the decoration is added here so every
rewritten block comes out formatted the same way.
"""

from __future__ import annotations

import io
import sys

NL = chr(10)


def find_block(lines, macro):
    """(start, end) of the comment block introducing `macro`, end exclusive."""

    anchor = None
    for i, line in enumerate(lines):
        s = line.strip()
        if s.startswith("#ifndef " + macro) or s.startswith("#define " + macro):
            anchor = i
            break
    if anchor is None:
        raise SystemExit("macro not found: " + macro)

    # Walk back over blank lines to the */ that closes the block.
    j = anchor - 1
    while j >= 0 and not lines[j].strip():
        j -= 1
    if j < 0 or not lines[j].strip().endswith("*/"):
        raise SystemExit("no comment block directly above " + macro)
    end = j + 1
    while j >= 0 and not lines[j].strip().startswith("/*"):
        j -= 1
    if j < 0:
        raise SystemExit("unterminated comment above " + macro)
    return j, end


def decorate(body: str):
    out = ["/*"]
    for para in [p for p in body.strip().split(NL + NL)]:
        if len(out) > 1:
            out.append(" *")
        for line in para.strip().split(NL):
            out.append((" * " + line.strip()).rstrip())
    out.append(" */")
    return out


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    path, macro, textfile = sys.argv[1:]
    lines = io.open(path, encoding="utf-8", newline="").read().split(NL)
    start, end = find_block(lines, macro)
    body = io.open(textfile, encoding="utf-8").read()
    new = decorate(body)
    before = end - start
    lines[start:end] = new
    io.open(path, "w", encoding="utf-8", newline="").write(NL.join(lines))
    print("  %-42s %3d -> %3d lines" % (macro, before, len(new)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
