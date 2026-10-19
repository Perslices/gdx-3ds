#!/usr/bin/env python3
"""tools/audiocrk_parse.py -- turn [audiocrk] windowed telemetry (a hardware filelog or an
emulator run's log.txt, [debug] audiocrk=1 or 2) into a per-second/per-window CSV table plus a
summary, for the audiofix crackle investigation (docs/research/native/audiofix-progress.md).

Each [audiocrk] "w" window is already a per-window (not cumulative) snapshot
(port/3ds/audio/gdx3ds_audio_crk.c: gdx_crk_window_reset at every rollover), so no delta math is
needed -- this just regexes the three "w" lines (sched/content/mean) into one row per window,
plus the one-shot [audioprime]/[audioprime2] "under edge" lines and the [audiocrk] pstep#
detail lines, and prints a CSV + a summary (pad ms, under windows, interior vs chunk-boundary vs
push-boundary big-step rate, worst producer tick / drain poll interval).

Usage: tools/audiocrk_parse.py <log.txt> [log2.txt ...]

A SECOND mode (card n60-3d9 hardware-PCM round) computes the boundary-vs-interior
|second-difference| discontinuity metric directly from a [debug] pcm_dump_s capture (a WAV,
optionally cross-checked against its .csv sidecar for chunk_index gaps / synthesized-silence
rows -- see gdx3ds_audio_pcmdump.c). This is what proved the hardware captures' headline ~5 Hz
"boundary click" signal was a pcmdump-ring-drop splice artifact, not a real ring/drain bug:

  tools/audiocrk_parse.py --wav pcm-main.wav --csv pcm-main.csv
"""
import re
import sys
import wave
import array
import struct

SCHED_RE = re.compile(
    r"\[audiocrk\] w t=(?P<t>\d+)ms chunks=(?P<chunks>\d+) under=(?P<under>\d+) dry=(?P<dry>\d+) "
    r"pad=(?P<pad>\d+) drop=(?P<drop>\d+) rmin=(?P<rmin>-?\d+) \| polls=(?P<polls>\d+) "
    r"late=(?P<late>\d+) pmax=(?P<pmax>\d+)us dspdry=(?P<dspdry>\d+) stop=(?P<stop>\d+) ndrop=(?P<ndrop>\d+)"
)
CONTENT_RE = re.compile(
    r"\[audiocrk\] w pcm rail=(?P<rail>\d+) hot=(?P<hot>\d+) peak=(?P<peak>\d+) "
    r"step=(?P<stepIn>\d+)/(?P<pairsIn>\d+) dmax=(?P<dmaxIn>\d+) "
    r"cstep=(?P<stepChunk>\d+)/(?P<pairsChunk>\d+) cdmax=(?P<dmaxChunk>\d+) "
    r"pstep=(?P<stepPush>\d+)/(?P<pairsPush>\d+) pdmax=(?P<dmaxPush>\d+) \| "
    r"ticks=(?P<ticks>\d+) tmax=(?P<tmax>\d+)us bufmin=(?P<bufmin>-?\d+)"
)
PSTEP_RE = re.compile(
    r"\[audiocrk\] pstep#(?P<idx>\d+) t=(?P<t>\d+)ms push=(?P<push>\d+) frames=(?P<frames>\d+) "
    r"tick=(?P<tick>\d+) L=(?P<L>\S+) R=(?P<R>\S+)"
)
UNDER_RE = re.compile(
    r"\[audioprime\] under edge #(?P<n>\d+): t=(?P<t>\d+)ms sub=(?P<sub>\d+) real=(?P<real>\d+) "
    r"ring=(?P<ring>\d+) inflight=(?P<inflight>\d+) push=(?P<push>\S+) prime=(?P<prime>\d+)"
)
UNDER2_RE = re.compile(
    r"\[audioprime2\] under edge #(?P<n>\d+): t=(?P<t>\d+)ms real=(?P<real>\d+) "
    r"target=(?P<target>\d+) boot=(?P<boot>\d+) "
    r"tick\{in=(?P<in_>\d+) for=(?P<for_>\d+)us last=(?P<last>\d+)us max=(?P<max_>\d+)us n=(?P<n2>\d+)\}"
)


def parse(path):
    rows = []
    pending = {}
    pstep_events = []
    under_events = []
    with open(path, errors="replace") as fh:
        for line in fh:
            m = SCHED_RE.search(line)
            if m:
                pending = {k: int(v) for k, v in m.groupdict().items()}
                continue
            m = CONTENT_RE.search(line)
            if m and pending:
                pending.update({k: int(v) for k, v in m.groupdict().items()})
                rows.append(pending)
                pending = {}
                continue
            m = PSTEP_RE.search(line)
            if m:
                pstep_events.append(m.groupdict())
                continue
            m = UNDER_RE.search(line)
            if m:
                d = m.groupdict()
                d["src"] = "prime"
                under_events.append(d)
                continue
            m = UNDER2_RE.search(line)
            if m:
                d = m.groupdict()
                d["src"] = "prime2"
                under_events.append(d)
                continue
    return rows, pstep_events, under_events


CSV_FIELDS = [
    "t", "chunks", "under", "dry", "pad", "drop", "rmin", "dspdry", "stop", "ndrop",
    "pmax", "rail", "hot", "peak", "stepIn", "pairsIn", "dmaxIn", "stepChunk", "pairsChunk",
    "dmaxChunk", "stepPush", "pairsPush", "dmaxPush", "ticks", "tmax", "bufmin",
]


def dump_one(path):
    rows, pstep_events, under_events = parse(path)
    print(f"# {path}")
    print(f"# windows={len(rows)} pstep-detail-events={len(pstep_events)} under-edges={len(under_events)}")
    print(",".join(CSV_FIELDS))
    for r in rows:
        print(",".join(str(r[f]) for f in CSV_FIELDS))

    print("# --- windows with ring/DSP-queue activity (under|dry|pad|drop|dspdry|stop) ---")
    for r in rows:
        if r["under"] or r["dry"] or r["pad"] or r["drop"] or r["dspdry"] or r["stop"]:
            print(
                f"#  t={r['t']}ms under={r['under']} dry={r['dry']} pad={r['pad']} drop={r['drop']} "
                f"dspdry={r['dspdry']} stop={r['stop']} rmin={r['rmin']} tmax={r['tmax']}us pmax={r['pmax']}us"
            )

    print("# --- windows with elevated interior content steps (stepIn > 2x the run mean rate) ---")
    tot_stepIn = sum(r["stepIn"] for r in rows) or 0
    tot_pairsIn = sum(r["pairsIn"] for r in rows) or 1
    mean_rate = tot_stepIn / tot_pairsIn
    for r in rows:
        rate = r["stepIn"] / max(r["pairsIn"], 1)
        if r["stepIn"] >= 20 and mean_rate > 0 and rate > 2 * mean_rate:
            print(
                f"#  t={r['t']}ms stepIn={r['stepIn']}/{r['pairsIn']} ({rate*100:.3f}%, run mean "
                f"{mean_rate*100:.3f}%) dmaxIn={r['dmaxIn']} peak={r['peak']} rail={r['rail']} "
                f"hot={r['hot']} under={r['under']} dry={r['dry']} pad={r['pad']}"
            )

    print("# --- under-edge one-shots ---")
    for e in under_events:
        print(f"#  {e}")

    if rows:
        tot_chunks = sum(r["chunks"] for r in rows)
        tot_pad = sum(r["pad"] for r in rows)
        tot_under = sum(r["under"] for r in rows)
        tot_stepChunk = sum(r["stepChunk"] for r in rows)
        tot_pairsChunk = sum(r["pairsChunk"] for r in rows) or 1
        tot_stepPush = sum(r["stepPush"] for r in rows)
        tot_pairsPush = sum(r["pairsPush"] for r in rows) or 1
        max_tmax = max(r["tmax"] for r in rows)
        max_pmax = max(r["pmax"] for r in rows)
        dur = rows[-1]["t"] / 1000.0
        chunk_rate = tot_stepChunk / tot_pairsChunk
        push_rate = tot_stepPush / tot_pairsPush
        print("# === SUMMARY ===")
        print(f"# duration_s={dur:.1f} windows={len(rows)} chunks_total={tot_chunks}")
        print(f"# pad_frames_total={tot_pad} pad_ms={tot_pad/32.0:.1f} under_windows={tot_under}")
        print(f"# interior step-rate      = {tot_stepIn}/{tot_pairsIn} = {mean_rate*100:.5f}%")
        print(
            f"# chunk-boundary step-rate = {tot_stepChunk}/{tot_pairsChunk} = {chunk_rate*100:.5f}% "
            f"({chunk_rate/mean_rate if mean_rate else float('nan'):.2f}x interior)"
        )
        print(
            f"# push-boundary  step-rate = {tot_stepPush}/{tot_pairsPush} = {push_rate*100:.5f}% "
            f"({push_rate/mean_rate if mean_rate else float('nan'):.2f}x interior)"
        )
        print(f"# worst producer tick tmax={max_tmax}us   worst drain poll interval pmax={max_pmax}us")
    print()


PCMDUMP_SILENCE_CHUNK_INDEX = 0xFFFFFFFF  # gdx3ds_audio_pcmdump.c: synthesized-gap CSV sentinel


def load_csv_spans(path):
    """Returns a list of (frame_start, frame_end, chunk_index, is_gap) sorted by frame_start.
    is_gap is True for a synthesized-silence row (post AUDIOFIX-round-5 pcmdump fix) OR, for an
    older/pre-fix capture that has no such rows, a row whose chunk_index is not exactly the
    previous real row's chunk_index + 1 (the pre-fix signature of a ring-drop splice point)."""
    spans = []
    prev_real_chunk = None
    with open(path, newline="") as fh:
        next(fh, None)  # header
        for line in fh:
            line = line.strip()
            if not line:
                continue
            frame_offset, chunk_index, t_ms, frames = (int(x) for x in line.split(","))
            is_silence_row = chunk_index == PCMDUMP_SILENCE_CHUNK_INDEX
            is_gap = is_silence_row
            if not is_silence_row:
                if prev_real_chunk is not None and chunk_index != prev_real_chunk + 1:
                    is_gap = True
                prev_real_chunk = chunk_index
            spans.append((frame_offset, frame_offset + frames, chunk_index, is_gap))
    spans.sort(key=lambda s: s[0])
    return spans


def span_is_gap_at(spans, frame_index):
    """Linear-ish scan (spans is sorted; captures are at most a few thousand rows, fine)."""
    for start, end, _chunk, is_gap in spans:
        if start <= frame_index < end:
            return is_gap
    return None  # frame_index fell in no known span (shouldn't happen on a sane capture)


def read_wav_left_channel(path):
    with wave.open(path, "rb") as wf:
        channels = wf.getnchannels()
        sampwidth = wf.getsampwidth()
        rate = wf.getframerate()
        nframes = wf.getnframes()
        raw = wf.readframes(nframes)
    if sampwidth != 2:
        raise ValueError(f"{path}: expected 16-bit PCM, got {sampwidth*8}-bit")
    samples = array.array("h")
    samples.frombytes(raw)
    if sys.byteorder == "big":
        samples.byteswap()  # WAV is always little-endian; array is native-endian
    left = samples[0::channels]
    return left, rate, channels, nframes


def second_difference(samples):
    """|x[i+1] - 2*x[i] + x[i-1]| for i in [1, len-2]; index i in the returned list is i+1 in
    `samples` (i.e. result[0] corresponds to samples[1])."""
    n = len(samples)
    out = array.array("i", [0]) * (n - 2) if n > 2 else array.array("i")
    for i in range(1, n - 1):
        out[i - 1] = abs(samples[i + 1] - 2 * samples[i] + samples[i - 1])
    return out


def analyze_wav(wav_path, csv_path=None, chunk_frames=512, thresholds=(3000, 6000)):
    left, rate, channels, nframes = read_wav_left_channel(wav_path)
    d2 = second_difference(left)  # d2[k] is the score AT sample index k+1

    spans = load_csv_spans(csv_path) if csv_path else None

    print(f"# {wav_path}  rate={rate} channels={channels} frames={nframes}  "
          f"(chunk_frames={chunk_frames})")
    if spans:
        print(f"# {csv_path}  rows={len(spans)}  "
              f"gap_rows={sum(1 for s in spans if s[3])}")

    for thresh in thresholds:
        boundary_hits = boundary_total = interior_hits = interior_total = 0
        gap_hits = clean_hits = unknown_hits = 0
        for k, score in enumerate(d2):
            idx = k + 1
            is_boundary = (idx % chunk_frames) == 0
            if is_boundary:
                boundary_total += 1
            else:
                interior_total += 1
            if score > thresh:
                if is_boundary:
                    boundary_hits += 1
                else:
                    interior_hits += 1
                if spans is not None:
                    g = span_is_gap_at(spans, idx)
                    if g is True:
                        gap_hits += 1
                    elif g is False:
                        clean_hits += 1
                    else:
                        unknown_hits += 1
        bpct = 100.0 * boundary_hits / boundary_total if boundary_total else float("nan")
        ipct = 100.0 * interior_hits / interior_total if interior_total else float("nan")
        ratio = (bpct / ipct) if ipct else float("inf")
        print(f"# thresh>{thresh}: boundary {boundary_hits}/{boundary_total} ({bpct:.3f}%)  "
              f"interior {interior_hits}/{interior_total} ({ipct:.4f}%)  "
              f"ratio={ratio:.1f}x")
        if spans is not None:
            total_hits = boundary_hits + interior_hits
            print(f"#   of {total_hits} total hits: at-known-gap={gap_hits} "
                  f"at-clean-boundary={clean_hits} unresolved={unknown_hits}")
    print()


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    if argv[1] == "--wav":
        wav_path = None
        csv_path = None
        chunk_frames = 512
        i = 2
        while i < len(argv):
            if argv[i] == "--wav":
                wav_path = argv[i + 1]
                i += 2
            elif argv[i] == "--csv":
                csv_path = argv[i + 1]
                i += 2
            elif argv[i] == "--chunk-frames":
                chunk_frames = int(argv[i + 1])
                i += 2
            else:
                i += 1
        if wav_path is None:
            wav_path = argv[2]
        analyze_wav(wav_path, csv_path, chunk_frames)
        return 0
    for path in argv[1:]:
        dump_one(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
