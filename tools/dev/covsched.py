#!/usr/bin/env python3
"""covsched.py — derive a replay's SHOT schedule from a `[cov]` coverage census.

Why this exists (NEXT.md B15/B43). The parity gate samples a handful of frames by `SHOT`, and
what a SHOT does not sample the gate cannot see. Five separate tracks have been bitten by the
same hole:

  * `explosions` sampled the WRONG TEXTURE on all 640 explosions (`fxceiling=3`) and all 8
    canonical hashes stayed bit-identical — no SHOT lands on a frame containing an explosion
    (they live at 4036-4216; the nearest SHOT is 4000);
  * `geomloss`'s machines truncation REACHES PIXELS, hashed identically at all 8 SHOTs, and
    diverged at frame 3870 under `fh_every=30`;
  * `menus-longtail` carried a divergence at frame 4145 that the standing 24-point
    `divergent=0` claim never saw;
  * `machfix`'s 4P material bug was invisible to a frame hash unless the scene also recycled a
    texture id;
  * `dropsites` had to FORCE all four of its sites because none had ever executed.

Dense per-frame hashing is the brute-force answer and is not viable as the standing gate. The
answer is to place a small number of samples where the content actually is. `[debug] cov=1`
(port/3ds/gfx/gdx3ds_cov.c) records, per frame, the distinct features the frame touched. This
tool reads that log and answers three questions:

    covsched.py cover  <cov.log>                       what fraction of the features a schedule covers
    covsched.py derive <cov.log> [--rewrite in out]    the new schedule, and the .gdxrec carrying it
    covsched.py report <cov.log> --old T,T,...         old vs new, side by side

The schedule is a small set cover with three mandatory additions, because a pure set cover is
a cover of FEATURES and a replay is also a sequence of SCENES:

  1. the first frame of every game mode the replay visits (a mode is a screen, and a screen
     that is never sampled is a screen nobody checks);
  2. a frame on each side of every mode transition (`--edge`), because a transition is where
     one screen's state is torn down over another's;
  3. greedy set cover over everything else, preferring frames that are STABLE — a feature that
     is present for one frame only is the most fragile thing to sample, so the cover scores a
     candidate by how many still-uncovered features it carries, and breaks ties towards the
     frame nearest the middle of the run of frames that carry them.

THE CENSUS'S LEVER SET IS PART OF THE SCHEDULE'S VALIDITY CLAIM, and it is the easiest thing
here to get wrong. A schedule derived from a census run with a NARROWER lever set than the one
the gate (or tools/dev/covdrift.sh) later uses can under-cover, because the wider run draws
features the derive never saw. Measured on explosion-pileup: a derive census that inherited
`[stereo] enabled=0` reported 239 keys and 1.49M marks, while covdrift.sh's run -- which pins
`stereo.enabled=1` explicitly, along with tmemfast/trifast/trectbatch -- reported 251 keys and
3.23M marks. The 12 extra keys were all right-eye texture variants and the schedule happened to
cover them anyway, which was luck, not design.

So: DERIVE WITH THE SAME LEVERS THE DRIFT CHECK ASSERTS WITH. tools/dev/covdrift.sh's
run_census() holds the canonical set (every [native] and run-control [debug] key explicit,
zeros included -- gdx-dev --ini writes through to a persistent per-instance ini, so a lever
omitted is a lever inherited). The easy way to get one is to let covdrift.sh do the run:

    tools/dev/covdrift.sh --rederive <replay>      # runs the census, and on a FAIL prints the
                                                   # exact `derive --rewrite` command to fix it

WHAT 100% COVERAGE DOES NOT MEAN, measured (ekmenu-progress.md, NEXT.md B11b). Every family
here keys a CLASS, not an identity -- family 06 is (unit, atlas/own, fmt, log2 w, log2 h,
filter, clamp) and family 0C is a log2 magnitude bucket. So content that reuses an existing
shape mints NO new key and is invisible to both the census and the cover report. The Expansion
Kit build draws two menu signs the shipped build never draws (aMenuSignCourseEditTex /
aMenuSignCreateMachineTex, main_menu.c:576) and its census has exactly ONE key the shipped
census does not -- and that key is the boot logo, not a sign: all eight signs are 0x40 x 0x56,
so the two new ones land in the same family-06 key as the six already there, and two more
64x86 texrects do not move a family-0C bucket either. The committed menus-longtail schedule
still sampled them, via the mode-anchor rule and the --max-gap floor, and a pixel compare
(tools/dev/framediff.py between the two builds' SHOT BMPs) is what PROVED they were drawn.

That class blindness is deliberate -- see gdx3ds_cov.h's family-0C comment on why a sensor
finer than the quantity's frame-to-frame variation mints a key per ANIMATION frame and makes
the set cover sample animation frames. But read the number for what it is:

    100% here means NO NEW FEATURE CLASS GOES UNSAMPLED. It does NOT mean every new draw was
    checked, and for a same-size, same-format texture swap it cannot. Use framediff.py.

Stdlib only (PROTOCOL.md: host tools are stdlib-only).
"""

import argparse
import re
import sys

COV_F = re.compile(r"\[cov\] f=(\d+) t=(-?\d+) m=(-?\d+)(.*)$")
COV_SUM = re.compile(r"\[cov\] sum ([0-9a-f]{2}):([0-9a-f]{6}) first=(\d+) last=(\d+) n=(\d+)")
COV_DONE = re.compile(r"\[cov\] done keys=(\d+) frames=(\d+) marks=(\d+) dropframe=(\d+) dropdict=(\d+)")
COV_TOK = re.compile(r"([+-])([0-9a-f]{2}):([0-9a-f]{6})")
FH = re.compile(r"\[fh\] frame=(\d+) tick=(\d+) .*?hash=([0-9a-f]+) tag=(\S+)")

FAMILY = {
    0x01: "mode",
    0x02: "fx",
    0x03: "machine",
    0x04: "combiner",
    0x05: "rendermode",
    0x06: "texture",
    0x07: "clip",
    0x08: "viewport",
    0x09: "tlut",
    0x0A: "texcache",
    0x0B: "target",
    0x0C: "batch",
}

# port/3ds/gfx/gdx3ds_fx_record.h: EXPLOSION is 0 and OTHER is 7, not the other way round.
FX_CLASS = {0: "explosion", 1: "debris", 2: "spark", 3: "cspark", 4: "smoke", 5: "pitff",
            6: "fallx", 7: "other"}
CLIP_EVENT = {0: "clip-needed", 1: "clip-fan", 2: "clip-trunc", 3: "reject", 4: "all-out", 5: "cull"}
TC_EVENT = {0: "hit", 1: "miss", 2: "evict", 3: "upload"}


def key_name(key):
    fam, val = key >> 24, key & 0xFFFFFF
    name = FAMILY.get(fam, "fam%02x" % fam)
    if fam == 0x02:
        return "fx:%s" % FX_CLASS.get(val, val)
    if fam == 0x03:
        return "machine:slot%d/lod%d" % (val & 0xFF, ((val >> 8) & 0xFF) + 1)
    if fam == 0x07:
        return "clip:%s%s" % (CLIP_EVENT.get(val & 0xFF, val & 0xFF),
                              ("/n=%d" % (val >> 8)) if (val >> 8) else "")
    if fam == 0x0A:
        return "texcache:%s" % TC_EVENT.get(val, val)
    if fam == 0x0B:
        return "target:%s" % ("texture" if val else "screen")
    if fam == 0x01:
        return "mode:%d" % val
    if fam == 0x0C:
        # (path, vertex class, triangle class) — log2 buckets, see port/3ds/include/gdx3ds_cov.h.
        return "batch:%s/v2^%d/t2^%d" % ("idx" if (val >> 8) & 1 else "arr",
                                         (val >> 4) & 0xF, val & 0xF)
    return "%s:%06x" % (name, val)


class Census(object):
    """Per-frame feature sets, reconstructed from the [cov] delta stream."""

    def __init__(self):
        self.frames = []        # ordered frame numbers
        self.tick = {}          # frame -> replay tick
        self.mode = {}          # frame -> game mode
        self.sets = {}          # frame -> frozenset(keys)
        self.summary = {}       # key -> (first, last, n)
        self.done = None

    @property
    def keys(self):
        return set(self.summary) if self.summary else set().union(*self.sets.values())


def parse(path):
    c = Census()
    live = set()
    last = None
    with open(path, errors="replace") as fh:
        for raw in fh:
            m = COV_SUM.search(raw)
            if m:
                c.summary[(int(m.group(1), 16) << 24) | int(m.group(2), 16)] = (
                    int(m.group(3)), int(m.group(4)), int(m.group(5)))
                continue
            m = COV_DONE.search(raw)
            if m:
                c.done = dict(keys=int(m.group(1)), frames=int(m.group(2)), marks=int(m.group(3)),
                              dropframe=int(m.group(4)), dropdict=int(m.group(5)))
                continue
            m = COV_F.search(raw)
            if not m:
                continue
            frame, tick, mode, rest = int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)
            # Frames between two delta lines carry the previous line's set unchanged: that is
            # what makes the delta encoding lossless, and it is why the tool must fill them in.
            if last is not None:
                for f in range(last + 1, frame):
                    c.frames.append(f)
                    c.tick[f] = c.tick[last] + (f - last)
                    c.mode[f] = c.mode[last]
                    c.sets[f] = c.sets[last]
            for sign, fam, val in COV_TOK.findall(rest):
                key = (int(fam, 16) << 24) | int(val, 16)
                if sign == "+":
                    live.add(key)
                else:
                    live.discard(key)
            c.frames.append(frame)
            c.tick[frame] = tick
            c.mode[frame] = mode
            c.sets[frame] = frozenset(live)
            last = frame
    # The run's tail after the LAST delta line carries that line's set unchanged too, and
    # `[cov] done frames=` is how many frames the guest actually ran. Without this the cover
    # silently stops at the last feature CHANGE, which on a long in-race tail is thousands of
    # frames early.
    if last is not None and c.done is not None:
        for f in range(last + 1, c.done["frames"] + 1):
            c.frames.append(f)
            c.tick[f] = c.tick[last] + (f - last)
            c.mode[f] = c.mode[last]
            c.sets[f] = c.sets[last]
    if not c.frames:
        sys.stderr.write("covsched: no [cov] f= lines in %s ([debug] cov=1 not set?)\n" % path)
    return c


def frames_of(c, key):
    return [f for f in c.frames if key in c.sets[f]]


def coverage(c, frames):
    """Which of the run's features a set of sampled frames touches."""
    seen = set()
    for f in frames:
        seen |= c.sets.get(f, frozenset())
    allk = c.keys
    return seen & allk, allk


def mode_anchors(c, edge):
    """The FIRST FRAME AFTER EVERY MODE CHANGE, the first frame of every mode, and `edge`
    frames each side of every change.

    The first rule is the one `menus4145` paid for. Its divergence window was ticks 4144-4184,
    contiguous, opening on the very first frame of `mode 6 -> 19` and closing 41 frames later:
    the transition wipe into RECORDS_COURSE_SELECT. The nearest SHOTs were 64 frames before and
    76 after, and both agreed on both builds. The event was never below the hash's RESOLUTION;
    it was beside the hash's SCHEDULE. Sampling any single frame inside such a window is enough
    to catch it, and "the frame a mode change lands on" is where those windows start -- so it
    is a rule over every transition in the corpus, not a frame number for one replay.

    Note the difference from "first frame of every mode": a replay that enters mode 19 three
    times has three transition wipes, and only the first would be anchored by mode novelty."""
    out, seen_modes, prev = set(), set(), None
    for f in c.frames:
        m = c.mode[f]
        if m not in seen_modes:
            seen_modes.add(m)
            out.add(f)
        if prev is not None and m != prev:
            out.add(f)  # the first frame after the change: the transition wipe's own frame
            for d in range(1, edge + 1):
                for g in (f - d, f + d):
                    if g in c.sets:
                        out.add(g)
        prev = m
    return out


def fill_gaps(c, chosen, max_gap, min_frame, avoid):
    """A floor sampling rate: no two consecutive samples further apart than `max_gap`.

    This is a MITIGATION, not a guarantee, and the difference matters. The set cover covers
    every feature the census has a sensor for; it says nothing about a run of frames that
    exercises the same features with different DATA. `tunnel-silence-records` is the case that
    forced this: its feature set saturates by frame 1851 and the pure cover then left a
    751-frame hole, which is four times the 180-frame gap that hid `menus4145`'s divergence.
    A floor closes the worst holes for a handful of extra samples; it cannot promise to catch a
    41-frame event, which would need a sample every 41 frames and 146 shots on a 6000-frame
    replay. The guarantee this tool offers is feature coverage; this is a safety net under it."""
    if not max_gap:
        return chosen
    out = set(chosen)
    ordered = sorted(f for f in c.frames if f >= min_frame)
    if not ordered:
        return out
    marks = sorted(out) or [ordered[0]]
    prev = ordered[0] - 1
    for cur in marks + [ordered[-1] + 1]:
        while cur - prev > max_gap:
            want = prev + max_gap
            pick = next((f for f in range(want, prev, -1) if f in c.sets and f not in avoid), None)
            if pick is None or pick <= prev:
                break
            out.add(pick)
            prev = pick
        prev = max(prev, cur)
    return out


def signature_spend(c, chosen, sig_budget, rule, min_frame, avoid, family=None):
    """Spend a BOUNDED extra budget of SHOTs on the most distinctive frame SIGNATURES.

    WHY THIS EXISTS (NEXT.md B55, and it is a structural gap, not a sensor gap).

    A greedy set cover covers KEYS, not key-SETS. Once every individual feature a frame
    carries appears somewhere else in the schedule, the cover has no reason to ever choose
    that frame -- however distinctive its COMBINATION is. `batchcov` measured the consequence
    on the canonical replay: the machines-truncation bug (`meshcapverts=64`, `meshtruncfix=0`
    vs `1`) reaches pixels on exactly two frames, 3870 and 3889 of 6028, and each of those
    frames carries a batch signature that is UNIQUE IN THE WHOLE RUN -- as do 121 of 6028
    frames (2.0%), and 308 of 6028 (5.1%) on the full feature set. The census singles them
    out perfectly well. The cover simply never picks them.

    So `coverage2`'s boundary was stated one notch too strong. It is not "no census can
    distinguish those frames"; it is "no set COVER will select them", and that is fixable.

    A FULL signature cover is not affordable: 542 distinct full feature sets and 294 distinct
    batch signatures on that one replay, against a 49-SHOT schedule. What is affordable is a
    BUDGET -- keep the key cover and its mandatory rules exactly as they are, then spend
    `sig_budget` further SHOTs on the frames whose signatures the schedule has least to say
    about. The budget is stated in SHOTs because SHOT cost is the whole constraint
    (`coverage2`: +0.2 s of wall clock for 41 extra SHOTs, after `shot_fastwrite`).

    THREE RULES, and the measurements that pick between them are in
    docs/research/native/sigcover-progress.md. Each takes the same budget and differs only in
    how it ranks candidates:

      `farthest`  FARTHEST-FIRST (maximin symmetric difference). Repeatedly take the frame
                  whose feature set is furthest -- in symmetric-difference size -- from EVERY
                  frame already scheduled, then fold it in and repeat. This is the rule that
                  works, and it works because it is the only one of the three that scores a
                  frame against the SCHEDULE rather than against the run: a frame is worth a
                  SHOT exactly when nothing already sampled looks like it. On the canonical
                  replay it picks frame 3889 as its SECOND extra SHOT.
      `unique`    frames whose full signature is unique in the run, ranked by rarity weight.
      `rarity`    every frame ranked by sum(1/residency) over its keys, deduplicated by
                  signature.

    The last two rank by a property of the RUN, so they spend the budget on the rarest
    features rather than on the least-represented combinations -- and both walk straight into
    `coverage2` gotcha 3, because the rarest features in a run are its one-frame transients.
    On the canonical replay both of them pick frame 1704 first, which is adjacent to the
    documented non-deterministic transient at 1702/1703. They need 58 and 84 extra SHOTs
    respectively to reach frame 3889. They are kept because the comparison is the evidence
    for `farthest`, not because anything should use them.

    `family` restricts the signature to one census family (e.g. 0x0C, batch). Measured and
    NOT recommended: a batch-only signature is 16-17 keys wide against the full set's 96, so
    its distances quantise into a handful of values, the rule ties everywhere and the budget
    goes to arbitrary tie-breaks. `farthest` over the batch family alone does not reach
    either target frame within 200 extra SHOTs.

    This runs AFTER fill_gaps() on purpose: every frame it adds can only shrink an unsampled
    gap, so the --max-gap floor the schedule was derived at is preserved by construction, and
    the cost of the option is exactly `sig_budget` SHOTs and never more.
    """
    def sig(f):
        s = c.sets[f]
        return frozenset(k for k in s if (k >> 24) == family) if family else s

    pool = [f for f in c.frames if f >= min_frame and f not in avoid and f not in chosen]
    if not pool or sig_budget <= 0:
        return []
    res = dict((k, c.summary.get(k, (0, 0, 1))[2]) for k in c.keys)

    def rarity(f):
        return sum(1.0 / max(1, res.get(k, 1)) for k in sig(f))

    if rule in ("unique", "rarity"):
        if rule == "unique":
            counts = {}
            for f in c.frames:
                counts[sig(f)] = counts.get(sig(f), 0) + 1
            pool = [f for f in pool if counts[sig(f)] == 1]
        # Deduplicate by signature: two frames carrying the same set are the same sample.
        pool.sort(key=lambda f: (-rarity(f), f))
        seen, out = set(sig(f) for f in chosen), []
        for f in pool:
            if sig(f) in seen:
                continue
            seen.add(sig(f))
            out.append(f)
            if len(out) >= sig_budget:
                break
        return out

    if rule != "farthest":
        raise ValueError("unknown --sig-rule %r" % rule)

    # Farthest-first. The distance cache makes this O(budget * frames) rather than
    # O(budget * frames * schedule): only the frame just added can lower a min-distance.
    picked = []
    sel_sets = [sig(f) for f in chosen]
    mind = {}
    for f in pool:
        s = sig(f)
        mind[f] = min(len(s ^ t) for t in sel_sets) if sel_sets else len(s)
    for _ in range(sig_budget):
        best = None
        best_score = (0, 0)
        for f in pool:
            if f in picked:
                continue
            score = (mind[f], -f)          # ties -> the earliest frame, so the rule is stable
            if best is None or score > best_score:
                best, best_score = f, score
        # Distance 0 means some scheduled frame already carries exactly this signature: there
        # is nothing distinctive left to buy, so stop rather than spend the rest of the budget
        # on duplicates.
        if best is None or mind[best] == 0:
            break
        picked.append(best)
        bs = sig(best)
        for f in pool:
            d = len(sig(f) ^ bs)
            if d < mind[f]:
                mind[f] = d
    return picked


def derive(c, edge=1, budget=None, min_frame=120, avoid=(), max_gap=300,
           sig_budget=0, sig_rule="farthest", sig_family=None):
    """Mandatory mode anchors + a greedy set cover of everything they miss + a gap floor.

    `min_frame` keeps samples out of the boot window, where the guest is still bringing the
    window, the SD and the o2r up and a capture is not a frame of the game.

    `sig_budget` is OPT-IN and defaults to 0, which is the shipped algorithm unchanged. Every
    committed schedule in replays/ was derived without it and tools/dev/covdrift.sh asserts
    against those schedules, so turning it on by default would silently re-break the corpus --
    which is `batchcov`'s and `covfinish`'s shared lesson, and the reason B55 asked for an
    option rather than a new default. See signature_spend()."""
    avoid = set(avoid)
    chosen = set(f for f in mode_anchors(c, edge) if f in c.sets and f >= min_frame and f not in avoid)
    covered = set()
    for f in chosen:
        covered |= c.sets[f]
    allk = c.keys
    # Candidate scoring prefers the MIDDLE of a feature's residency: the first frame a feature
    # appears on is the one most likely to be a one-frame transient of the transition, and a
    # schedule that only ever samples transients is a schedule that tests the wrong frame.
    while True:
        want = allk - covered
        if not want:
            break
        best, best_score = None, (0, 0)
        for f in c.frames:
            if f in chosen or f < min_frame or f in avoid:
                continue
            gain = len(c.sets[f] & want)
            if gain == 0:
                continue
            # tiebreak: prefer a frame whose features are long-lived around it
            stability = min((c.summary.get(k, (f, f, 1))[2] for k in (c.sets[f] & want)), default=1)
            score = (gain, stability)
            if score > best_score:
                best, best_score = f, score
        if best is None:
            break
        chosen.add(best)
        covered |= c.sets[best]
        if budget is not None and len(chosen) >= budget:
            break
    chosen = set(fill_gaps(c, chosen, max_gap, min_frame, avoid))
    if sig_budget:
        chosen |= set(signature_spend(c, chosen, sig_budget, sig_rule, min_frame,
                                      avoid, sig_family))
    return sorted(chosen)


def label_for(c, frame, used):
    base = "m%02d_%d" % (c.mode.get(frame, -1) & 0xFF, c.tick.get(frame, frame))
    lbl, n = base, 1
    while lbl in used:
        n += 1
        lbl = "%s.%d" % (base, n)
    used.add(lbl)
    return lbl


def rewrite_gdxrec(src, dst, ticks_labels):
    """Replace every SHOT marker in a .gdxrec with the derived schedule."""
    want = dict(ticks_labels)
    out, hit = [], set()
    with open(src) as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            parts = line.split()
            if not parts or not parts[0].isdigit():
                out.append(line)
                continue
            tick = int(parts[0])
            if "SHOT" in parts:
                parts = parts[:parts.index("SHOT")]
            if tick in want:
                parts += ["SHOT", want[tick]]
                hit.add(tick)
            out.append(" ".join(parts))
    missed = sorted(set(want) - hit)
    with open(dst, "w") as fh:
        fh.write("\n".join(out) + "\n")
    return len(hit), missed


def fmt_pct(n, d):
    return "%d/%d (%.1f%%)" % (n, d, 100.0 * n / d if d else 0.0)


def per_family(c, frames):
    seen, allk = coverage(c, frames)
    fams = {}
    for k in allk:
        fam = k >> 24
        got, tot = fams.get(fam, (0, 0))
        fams[fam] = (got + (1 if k in seen else 0), tot + 1)
    return fams


def gdxrec_ticks(path):
    """The SHOT ticks a .gdxrec actually carries — the committed schedule.

    The drift check must read the schedule from the recording rather than be handed a list,
    or it verifies a list somebody typed instead of the one the gate will run.
    """
    ticks = []
    with open(path) as fh:
        for raw in fh:
            parts = raw.split()
            if len(parts) >= 2 and parts[0].isdigit() and "SHOT" in parts[1:]:
                ticks.append(int(parts[0]))
    return ticks


def largest_gap(c, frames):
    """(gap, lo, hi) for the widest unsampled stretch, boot window and tail included.

    Feature coverage alone cannot see a replay that grew. `tunnel-silence-records` saturates
    its feature vocabulary at frame 1851 and then runs 2800 more frames of the same vocabulary
    with different data; extend any replay into a screen it already visits and coverage stays
    at 100% while the schedule's real sampling rate collapses. The shipped schedules carry a
    --max-gap floor for exactly that, so the drift check asserts the floor too.
    """
    if not c.frames:
        return (0, 0, 0)
    lo_f, hi_f = c.frames[0], c.frames[-1]
    marks = sorted(set(f for f in frames if lo_f <= f <= hi_f))
    worst, a, b = 0, lo_f, lo_f
    prev = lo_f
    for cur in marks + [hi_f]:
        if cur - prev > worst:
            worst, a, b = cur - prev, prev, cur
        prev = cur
    return (worst, a, b)


def missed_features(c, frames, limit=None):
    """Missed keys, widest-lived first — the actionable half of a coverage failure."""
    seen, allk = coverage(c, frames)
    miss = sorted(allk - seen, key=lambda k: (-c.summary.get(k, (0, 0, 0))[2], k))
    return miss if limit is None else miss[:limit]



def print_gap(c, frames, limit=40):
    seen, allk = coverage(c, frames)
    miss = sorted(allk - seen, key=lambda k: (-c.summary.get(k, (0, 0, 0))[2], k))
    for k in miss[:limit]:
        first, last, n = c.summary.get(k, (0, 0, 0))
        print("    MISSED %-28s first=%-6d last=%-6d frames=%d" % (key_name(k), first, last, n))
    if len(miss) > limit:
        print("    ... and %d more" % (len(miss) - limit))


def cmd_cover(args):
    c = parse(args.log)
    if args.gdxrec:
        frames = [t + 1 for t in gdxrec_ticks(args.gdxrec)]
    elif args.frames:
        frames = [int(x) for x in args.frames.split(",")]
    elif args.ticks:
        frames = [int(x) + 1 for x in args.ticks.split(",")]
    else:
        frames = []
    seen, allk = coverage(c, frames)
    pct = 100.0 * len(seen) / len(allk) if allk else 100.0
    gap, gap_lo, gap_hi = largest_gap(c, frames)
    print("log        %s" % args.log)
    if args.gdxrec:
        print("schedule   %s (%d SHOTs)" % (args.gdxrec, len(frames)))
    print("frames     %d sampled of %d" % (len(frames), len(c.frames)))
    print("features   %s" % fmt_pct(len(seen), len(allk)))
    print("worst gap  %d frames (%d..%d)" % (gap, gap_lo, gap_hi))
    for fam, (got, tot) in sorted(per_family(c, frames).items()):
        print("  %-12s %s" % (FAMILY.get(fam, "fam%02x" % fam), fmt_pct(got, tot)))
    if args.gaps:
        print_gap(c, frames)

    # --require / --max-gap turn this from a report into an assertion. The failure has to name
    # WHICH features went unsampled: a bare percentage tells nobody what to look at, and the
    # whole point of the census is that "68%" was never the actionable number.
    fails = []
    if args.require is not None and pct + 1e-9 < args.require:
        miss = missed_features(c, frames)
        fails.append("coverage %.1f%% < %.1f%% required -- %d feature(s) unsampled"
                     % (pct, args.require, len(miss)))
        byfam = {}
        for k in miss:
            byfam.setdefault(FAMILY.get(k >> 24, "fam%02x" % (k >> 24)), []).append(k)
        for fam in sorted(byfam):
            ks = byfam[fam]
            names = ", ".join(key_name(k).split(":", 1)[1] for k in ks[:8])
            if len(ks) > 8:
                names += ", +%d more" % (len(ks) - 8)
            fails.append("  UNSAMPLED %-11s %d: %s" % (fam, len(ks), names))
        for k in miss[:args.name_limit]:
            first, last, n = c.summary.get(k, (0, 0, 0))
            fails.append("  UNSAMPLED %-28s first=%-6d last=%-6d frames=%d"
                         % (key_name(k), first, last, n))
        if len(miss) > args.name_limit:
            fails.append("  ... %d further unsampled features, `covsched.py cover --gaps` for all"
                         % (len(miss) - args.name_limit))
    if args.max_gap and gap > args.max_gap:
        fails.append("worst unsampled gap %d frames (%d..%d) > --max-gap %d -- the schedule's "
                     "floor sampling rate has decayed, which feature coverage alone cannot see"
                     % (gap, gap_lo, gap_hi, args.max_gap))
    if fails:
        print("COVER FAIL")
        for line in fails:
            print(line)
        return 1
    if args.require is not None or args.max_gap:
        print("COVER OK   %.1f%% features, worst gap %d frames" % (pct, gap))
    return 0


def cmd_derive(args):
    c = parse(args.log)
    frames = derive(c, edge=args.edge, budget=args.budget, min_frame=args.min_frame,
                    avoid=set(int(x) for x in args.avoid.split(",")) if args.avoid else set(),
                    max_gap=args.max_gap, sig_budget=args.sig_budget, sig_rule=args.sig_rule,
                    sig_family=int(args.sig_family, 0) if args.sig_family else None)
    seen, allk = coverage(c, frames)
    used = set()
    sched = [(c.tick[f], label_for(c, f, used), f) for f in frames]
    print("log        %s" % args.log)
    print("frames     %d of %d sampled  (1 every %.0f)" %
          (len(frames), len(c.frames), len(c.frames) / float(len(frames) or 1)))
    print("features   %s" % fmt_pct(len(seen), len(allk)))
    print("ticks      %s" % ",".join(str(t) for t, _, _ in sched))
    if args.verbose:
        for tick, label, f in sched:
            print("  tick=%-6d frame=%-6d mode=%-3d %s" % (tick, f, c.mode[f], label))
    if args.rewrite:
        src, dst = args.rewrite
        hit, missed = rewrite_gdxrec(src, dst, [(t, l) for t, l, _ in sched])
        print("rewrote    %s -> %s  (%d SHOTs placed)" % (src, dst, hit))
        if missed:
            print("WARNING    %d derived ticks are past the end of the recording: %s" %
                  (len(missed), missed))
            return 1
    return 0


def cmd_report(args):
    c = parse(args.log)
    old = [int(x) + 1 for x in args.old.split(",")] if args.old else []
    new = derive(c, edge=args.edge, budget=args.budget, min_frame=args.min_frame,
                 avoid=set(int(x) for x in args.avoid.split(",")) if args.avoid else set(),
                 max_gap=args.max_gap, sig_budget=args.sig_budget, sig_rule=args.sig_rule,
                 sig_family=int(args.sig_family, 0) if args.sig_family else None)
    o_seen, allk = coverage(c, old)
    n_seen, _ = coverage(c, new)
    o_fam, n_fam = per_family(c, old), per_family(c, new)
    print("%-13s %-22s %-22s" % ("family", "OLD (%d shots)" % len(old), "NEW (%d shots)" % len(new)))
    for fam in sorted(allk and set(k >> 24 for k in allk)):
        print("%-13s %-22s %-22s" % (FAMILY.get(fam, "fam%02x" % fam),
                                     fmt_pct(*o_fam.get(fam, (0, 0))), fmt_pct(*n_fam.get(fam, (0, 0)))))
    print("%-13s %-22s %-22s" % ("TOTAL", fmt_pct(len(o_seen), len(allk)), fmt_pct(len(n_seen), len(allk))))
    if args.gaps:
        print("  what the OLD schedule never sampled:")
        print_gap(c, old, limit=args.gaps)
    return 0


def cmd_frames(args):
    """Which frames carry a feature — the direct answer to `does the schedule see X`."""
    c = parse(args.log)
    pat = args.feature.lower()
    for k in sorted(c.keys):
        if pat in key_name(k).lower():
            fs = frames_of(c, k)
            span = "%d-%d" % (fs[0], fs[-1]) if fs else "-"
            print("%-30s frames=%-6d span=%-16s %s" %
                  (key_name(k), len(fs), span, ",".join(str(x) for x in fs[:20])))
    return 0


def cmd_lint(args):
    """Static schedule invariants — no census, no emulator, milliseconds (NEXT.md B48).

    The expensive half of the drift check (tools/dev/covdrift.sh) compares a schedule against
    the CONTENT its replay draws, which only exists when the game runs. This is the half that
    needs nothing, so tools/ci-3ds.sh can carry it on every build:

      * a corpus recording must CARRY a schedule. B47 was exactly this: explosion-pileup.gdxs
        carried the "placement is derived elsewhere" banner while the .gdxrec still held 39
        hand-placed shots covering 51.9% of the replay, and nothing anywhere noticed;
      * `replays/src/*.gdxs` must author NO `shot` lines — placement is derived, and a hand
        `shot` left in the source silently comes back on the next regeneration;
      * SHOT ticks must be unique, ascending, and land on ticks the recording actually has;
      * no unsampled stretch wider than the floor the schedules were derived at. That one is
        the static proxy for "the replay grew and the schedule did not": extend a recording
        and its tail gap opens immediately, with no guest run needed to see it.
    """
    import glob
    import os
    bad, checked = [], 0
    for gdxs in sorted(glob.glob(os.path.join(args.replays, "src", "*.gdxs"))):
        for n, raw in enumerate(open(gdxs), 1):
            if raw.split()[:1] == ["shot"]:
                bad.append("%s:%d authors a `shot` — placement is DERIVED "
                           "(covsched.py derive --rewrite), not authored here" % (gdxs, n))
    for rec in sorted(glob.glob(os.path.join(args.replays, "*.gdxrec"))):
        name = os.path.basename(rec)[:-len(".gdxrec")]
        if any(name.startswith(pre) for pre in args.skip.split(",") if pre):
            continue
        checked += 1
        ticks, shots = [], []
        for raw in open(rec):
            parts = raw.split()
            if not parts or not parts[0].isdigit():
                continue
            ticks.append(int(parts[0]))
            if "SHOT" in parts[1:]:
                shots.append(int(parts[0]))
        if not ticks:
            bad.append("%s has no tick stream" % rec)
            continue
        if not shots:
            bad.append("%s carries NO SHOTs — the parity gate would hash nothing" % rec)
            continue
        if shots != sorted(set(shots)):
            bad.append("%s SHOT ticks are not unique and ascending" % rec)
        have = set(ticks)
        stray = [t for t in shots if t not in have]
        if stray:
            bad.append("%s has %d SHOT tick(s) off the recording's tick stream: %s"
                       % (rec, len(stray), stray[:5]))
        lo, hi = ticks[0], ticks[-1]
        worst, a, b, prev = 0, lo, lo, lo
        for cur in shots + [hi]:
            if cur - prev > worst:
                worst, a, b = cur - prev, prev, cur
            prev = cur
        if args.max_gap and worst > args.max_gap:
            bad.append("%s leaves %d ticks unsampled (%d..%d) > --max-gap %d — re-derive it "
                       "(the recording has probably grown)" % (rec, worst, a, b, args.max_gap))
        print("%-32s %3d SHOTs  %5d ticks  worst gap %4d" % (name, len(shots), hi - lo + 1, worst))
    for line in bad:
        print("LINT FAIL  %s" % line)
    if bad:
        print("schedule invariants: %d problem(s) over %d recording(s)" % (len(bad), checked))
        return 1
    print("schedule invariants: OK (%d recording(s))" % checked)
    return 0



def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("cover", help="what fraction of the features a given schedule covers")
    p.add_argument("log")
    p.add_argument("--frames", help="comma-separated [fh] frame numbers")
    p.add_argument("--ticks", help="comma-separated .gdxrec ticks (frame = tick + 1)")
    p.add_argument("--gdxrec", help="read the committed schedule's SHOT ticks from this .gdxrec")
    p.add_argument("--gaps", action="store_true", help="list the features it misses")
    p.add_argument("--require", type=float, metavar="PCT",
                   help="ASSERT: exit 1 below this coverage percentage, naming every "
                        "unsampled feature (the drift check -- NEXT.md B48)")
    p.add_argument("--max-gap", type=int, default=0, metavar="N",
                   help="ASSERT: exit 1 if any unsampled stretch exceeds N frames")
    p.add_argument("--name-limit", type=int, default=25, metavar="N",
                   help="name at most N unsampled features individually (default 25)")
    p.set_defaults(fn=cmd_cover)

    p = sub.add_parser("derive", help="derive a schedule; optionally rewrite the .gdxrec")
    p.add_argument("log")
    p.add_argument("--edge", type=int, default=1, help="frames each side of a mode change (default 1)")
    p.add_argument("--budget", type=int, default=None, help="stop the cover at N frames")
    p.add_argument("--min-frame", type=int, default=120, help="no sample before this frame (boot window)")
    p.add_argument("--avoid", help="frames never to sample -- documented non-deterministic "
                                   "transients, e.g. the canonical replay's 1702/1703 glyph frame")
    p.add_argument("--max-gap", type=int, default=300,
                   help="floor sampling rate: never leave more than N frames unsampled "
                        "(0 = pure set cover). A mitigation, not a guarantee -- see fill_gaps()")
    p.add_argument("--sig-budget", type=int, default=0, metavar="N",
                   help="OPT-IN (default 0 = off, the shipped algorithm unchanged): after the "
                        "key cover and the gap floor, spend N FURTHER SHOTs on the most "
                        "distinctive frame SIGNATURES. A greedy set cover covers keys, not "
                        "key-SETS, so a frame whose every individual feature appears elsewhere "
                        "is never selected however unique its combination is -- NEXT.md B55. "
                        "The cost is exactly N SHOTs. Do not turn this on for the committed "
                        "corpus without re-deriving and re-gating every replay.")
    p.add_argument("--sig-rule", default="farthest", choices=("farthest", "unique", "rarity"),
                   help="how the signature budget is spent (default farthest: maximin "
                        "symmetric difference against the frames already scheduled). "
                        "`unique`/`rarity` rank by a property of the RUN instead and spend the "
                        "budget on one-frame transients -- kept for comparison, see "
                        "signature_spend()")
    p.add_argument("--sig-family", metavar="HEX",
                   help="restrict the signature to one census family (e.g. 0x0C for batch). "
                        "Measured and NOT recommended -- a one-family signature is too narrow "
                        "to separate frames; see signature_spend()")
    p.add_argument("--rewrite", nargs=2, metavar=("IN.gdxrec", "OUT.gdxrec"))
    p.add_argument("--verbose", action="store_true")
    p.set_defaults(fn=cmd_derive)

    p = sub.add_parser("report", help="old schedule vs derived schedule, per family")
    p.add_argument("log")
    p.add_argument("--old", help="comma-separated OLD .gdxrec SHOT ticks")
    p.add_argument("--edge", type=int, default=1)
    p.add_argument("--budget", type=int, default=None)
    p.add_argument("--min-frame", type=int, default=120)
    p.add_argument("--avoid")
    p.add_argument("--max-gap", type=int, default=300)
    p.add_argument("--sig-budget", type=int, default=0, metavar="N",
                   help="OPT-IN (default 0 = off, the shipped algorithm unchanged): after the "
                        "key cover and the gap floor, spend N FURTHER SHOTs on the most "
                        "distinctive frame SIGNATURES. A greedy set cover covers keys, not "
                        "key-SETS, so a frame whose every individual feature appears elsewhere "
                        "is never selected however unique its combination is -- NEXT.md B55. "
                        "The cost is exactly N SHOTs. Do not turn this on for the committed "
                        "corpus without re-deriving and re-gating every replay.")
    p.add_argument("--sig-rule", default="farthest", choices=("farthest", "unique", "rarity"),
                   help="how the signature budget is spent (default farthest: maximin "
                        "symmetric difference against the frames already scheduled). "
                        "`unique`/`rarity` rank by a property of the RUN instead and spend the "
                        "budget on one-frame transients -- kept for comparison, see "
                        "signature_spend()")
    p.add_argument("--sig-family", metavar="HEX",
                   help="restrict the signature to one census family (e.g. 0x0C for batch). "
                        "Measured and NOT recommended -- a one-family signature is too narrow "
                        "to separate frames; see signature_spend()")
    p.add_argument("--gaps", type=int, default=0, help="list N features the old schedule missed")
    p.set_defaults(fn=cmd_report)

    p = sub.add_parser("lint", help="static schedule invariants over replays/ (no census)")
    p.add_argument("replays", nargs="?", default="replays", help="the replays/ directory")
    p.add_argument("--max-gap", type=int, default=300, metavar="N",
                   help="fail if a recording leaves more than N ticks unsampled (default 300, "
                        "the floor the shipped schedules were derived at)")
    p.add_argument("--skip", default="probe",
                   help="comma-separated name prefixes to skip (default `probe`: the bisect "
                        "probes are deliberately hand-placed truncations, not gate entries)")
    p.set_defaults(fn=cmd_lint)

    p = sub.add_parser("frames", help="which frames carry a named feature")
    p.add_argument("log")
    p.add_argument("feature", help="substring of a feature name, e.g. `fx:explosion`")
    p.set_defaults(fn=cmd_frames)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
