"""gdiffuser.ini read/MERGE.

PROTOCOL.md line 17: "MERGE ini keys, never overwrite". Four agents share one virtual SD;
an overwrite silently disarms another agent's lever mid-run and its A/B becomes a lie.
This module only ever rewrites the keys it was asked to set, preserving every other key,
section, order, and (unlike the in-game menu writer) comments.

Key syntax on the CLI: `--ini k=v` sets `[debug] k`; `--ini section.k=v` targets a section;
`--ini variant.N.k=v` targets the launch variant section `[variant.N]` (n60launch).
"""

import os
import re

from . import util
from .util import GdxError

DEFAULT_SECTION = "debug"
VARIANT_PREFIX = "variant."

_SECTION_RE = re.compile(r"^\s*\[([^\]]+)\]\s*$")
_KEY_RE = re.compile(r"^(\s*)([A-Za-z_][A-Za-z0-9_.]*)(\s*=\s*)(.*?)(\s*)$")


def parse_spec(spec):
    """`k=v` or `section.k=v` -> (section, key, value)."""
    if "=" not in spec:
        raise GdxError("bad --ini %r: expected k=v or section.k=v" % spec, util.EXIT_USAGE)
    lhs, value = spec.split("=", 1)
    lhs = lhs.strip()
    value = value.strip()
    if lhs.startswith(VARIANT_PREFIX) and lhs.count(".") >= 2:
        # N60LAUNCH: `variant.N.ndsp_core1=1` -> [variant.N] ndsp_core1 (the section has a dot);
        # `variant.P.native.all=0` -> [variant.P] native.all (a variant key may name a section).
        name, key = lhs[len(VARIANT_PREFIX):].split(".", 1)
        section = VARIANT_PREFIX + name
    elif "." in lhs:
        section, key = lhs.split(".", 1)
    else:
        section, key = DEFAULT_SECTION, lhs
    section, key = section.strip(), key.strip()
    if not section or not key:
        raise GdxError("bad --ini %r: empty section or key" % spec, util.EXIT_USAGE)
    # `--ini` takes ONE key per flag. A comma-separated list is the natural typo, and it
    # used to be accepted in silence: `--ini a=1,b=2,c=3` wrote the whole string as the
    # VALUE of `a`, so b and c never reached the guest and the run looked ordinary. That
    # cost a full bisect across three commits chasing a parity failure that was a typo --
    # both legs had silently run with the same levers. No legitimate value contains `=`,
    # and a `,` in a value here has always meant the same mistake.
    if "=" in value or "," in value:
        raise GdxError(
            "bad --ini %r: a value may not contain '=' or ','.\n"
            "--ini takes one key per flag; repeat it:\n"
            "    %s" % (spec, "  ".join("--ini %s" % part
                                        for part in spec.split(",") if part)),
            util.EXIT_USAGE)
    return section, key, value


def read(path):
    """-> {section: {key: value}}. Missing file is an empty dict, not an error."""
    out = {}
    cur = None
    if not os.path.exists(path):
        return out
    for line in util.read_text(path).splitlines():
        s = line.strip()
        if not s or s.startswith(";") or s.startswith("#"):
            continue
        m = _SECTION_RE.match(line)
        if m:
            cur = m.group(1).strip()
            out.setdefault(cur, {})
            continue
        m = _KEY_RE.match(line)
        if m and cur is not None:
            out[cur][m.group(2)] = m.group(4)
    return out


def merge(path, settings, dry_run=False):
    """Set `settings` ({(section,key): value}) in `path`, preserving everything else.

    Returns (changed, before, after) where before/after map (section,key)->value for the
    touched keys only — the receipt an A/B report cites.
    """
    existing = read(path)
    before = {}
    for (section, key) in settings:
        before[(section, key)] = existing.get(section, {}).get(key)

    lines = util.read_text(path).splitlines() if os.path.exists(path) else []
    todo = dict(settings)
    out = []
    cur = None
    # Index of the last line belonging to each section, so a new key lands inside it.
    section_last = {}
    for i, line in enumerate(lines):
        m = _SECTION_RE.match(line)
        if m:
            cur = m.group(1).strip()
            section_last[cur] = i
            out.append(line)
            continue
        m = _KEY_RE.match(line)
        if m and cur is not None:
            key = m.group(2)
            if (cur, key) in todo:
                value = todo.pop((cur, key))
                out.append("%s%s%s%s%s" % (m.group(1), key, m.group(3), value, m.group(5)))
                section_last[cur] = i
                continue
        if cur is not None and line.strip():
            section_last[cur] = i
        out.append(line)

    # Keys whose section exists but the key does not: insert after the section's last line.
    for (section, key), value in sorted(todo.items()):
        if section in section_last:
            idx = section_last[section]
            out.insert(idx + 1, "%s = %s" % (key, value))
            for s in section_last:
                if section_last[s] > idx:
                    section_last[s] += 1
            section_last[section] = idx + 1
        else:
            if out and out[-1].strip():
                out.append("")
            out.append("[%s]" % section)
            out.append("%s = %s" % (key, value))
            section_last[section] = len(out) - 1

    after = {k: v for k, v in settings.items()}
    changed = any(before[k] != str(after[k]) for k in settings)

    if not dry_run:
        d = os.path.dirname(path)
        if d and not os.path.isdir(d):
            os.makedirs(d, exist_ok=True)
        tmp = path + ".gdxtmp"
        with open(tmp, "w") as f:
            f.write("\n".join(out).rstrip("\n") + "\n")
        os.replace(tmp, path)
    return changed, before, after


def effective(path, section=DEFAULT_SECTION):
    return read(path).get(section, {})


def truthy(value):
    return str(value).strip() not in ("", "0", "false", "False", "no", "off")
