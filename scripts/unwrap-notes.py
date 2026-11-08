#!/usr/bin/env python3
"""Unwraps Markdown for GitHub's release page, which keeps every line break as a <br>: each
paragraph and each list item becomes one line. Headings, blank lines, fenced code blocks, tables
and HTML lines stay as they are. Reads the file named (or stdin), writes stdout.

    scripts/unwrap-notes.py docs/release-notes/X.Y.Z.md
"""
import re
import sys

STARTS_BLOCK = re.compile(r"^\s*(#|<|\||[-*+]\s|\d+\.\s|>)")


def unwrap(text):
    out, joinable, fenced = [], False, False
    for line in text.split("\n"):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            out.append(line)
            joinable = False
            continue
        if fenced:
            out.append(line)
            continue
        if not line.strip():
            out.append(line)
            joinable = False
            continue
        if joinable and not STARTS_BLOCK.match(line):
            out[-1] = out[-1].rstrip() + " " + line.strip()
            continue
        out.append(line)
        # a heading, an HTML line or a table row takes no continuation lines
        joinable = not re.match(r"^\s*(#|<|\|)", line)
    return "\n".join(out)


if __name__ == "__main__":
    src = open(sys.argv[1], encoding="utf-8").read() if len(sys.argv) > 1 else sys.stdin.read()
    sys.stdout.write(unwrap(src))
