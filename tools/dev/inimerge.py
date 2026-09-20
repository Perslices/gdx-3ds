#!/usr/bin/env python3
"""inimerge.py — set / delete keys in a gdiffuser.ini without touching anything else.

    tools/dev/inimerge.py <ini> [--set section.key=value ...] [--del section.key ...]

The in-game menu rewrites the file (comments are not preserved by it either), so this keeps
section order and untouched keys verbatim, appends missing sections at the end, and never
overwrites the file wholesale — the PROTOCOL "MERGE ini keys, never overwrite" rule. Stdlib only.
"""
import argparse
import re
import sys

SEC_RE = re.compile(r"^\s*\[([^\]]+)\]\s*$")
KEY_RE = re.compile(r"^\s*([^=;#\s]+)\s*=")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ini")
    ap.add_argument("--set", action="append", default=[], metavar="section.key=value")
    ap.add_argument("--del", dest="dels", action="append", default=[], metavar="section.key")
    args = ap.parse_args()

    sets = {}
    for item in args.set:
        left, _, value = item.partition("=")
        sec, _, key = left.partition(".")
        sets.setdefault(sec, {})[key] = value
    dels = {}
    for item in args.dels:
        sec, _, key = item.partition(".")
        dels.setdefault(sec, set()).add(key)

    try:
        with open(args.ini) as f:
            lines = f.read().splitlines()
    except FileNotFoundError:
        lines = []

    out = []
    cur = None
    pending = {}  # section -> keys not yet written

    def flush_section(sec):
        for key, value in pending.pop(sec, {}).items():
            out.append("%s = %s" % (key, value))

    for line in lines:
        m = SEC_RE.match(line)
        if m:
            if cur is not None:
                # keep the blank line(s) that precede the next header AFTER the new keys
                tail = []
                while out and out[-1].strip() == "":
                    tail.append(out.pop())
                flush_section(cur)
                out.extend(tail)
            cur = m.group(1)
            pending[cur] = dict(sets.get(cur, {}))
            out.append(line)
            continue
        k = KEY_RE.match(line)
        if k and cur is not None:
            key = k.group(1)
            if key in dels.get(cur, ()):
                continue
            if key in pending.get(cur, {}):
                out.append("%s = %s" % (key, pending[cur].pop(key)))
                continue
        out.append(line)
    if cur is not None:
        flush_section(cur)
    for sec in sets:
        if sec not in pending and not any(SEC_RE.match(l) and SEC_RE.match(l).group(1) == sec for l in lines):
            out.append("")
            out.append("[%s]" % sec)
            for key, value in sets[sec].items():
                out.append("%s = %s" % (key, value))
    with open(args.ini, "w") as f:
        f.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
