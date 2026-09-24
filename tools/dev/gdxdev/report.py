"""`gdx-dev prof`, `gdx-dev runs`, `gdx-dev report` — the reading end of the loop.

Every table an agent currently hand-assembles with grep/awk per progress file
(05-devcli-plan.md §2 items 10-11) lives here, so two reports of the same run agree.
"""

import os

from . import prof, runs, util
from .util import GdxError


def _fmt(v, nd=2):
    if v is None:
        return "-"
    if isinstance(v, float):
        return ("%%.%df" % nd) % v
    return str(v)


def _load(ident, run_root, track):
    """A prof target is a log file, a run dir, or a run id. -> (Parsed, label, manifest)."""
    if os.path.isfile(ident):
        return prof.parse_file(ident), ident, None
    rd = runs.resolve(run_root, track, ident)
    manifest = None
    try:
        manifest = rd.read_manifest()
    except GdxError:
        pass
    # The retro lane names its sinks per instance, so take the names from the manifest
    # and fall back to the Azahar lane's defaults.
    arts = (manifest or {}).get("artifacts") or {}
    names = [arts.get("log") or "log.txt", arts.get("emu_log") or "emu.log"]
    text = ""
    for name in names:
        text += util.read_text(rd.sub(name))
    if not text.strip():
        raise GdxError("run %s has no content in %s" % (rd.run_id, " / ".join(names)),
                       util.EXIT_NODATA)
    return prof.parse_text(text), rd.run_id, manifest


def _sections_table(out, label, windows, storm=None):
    s = prof.section_summary(windows)
    if not s:
        out.say("%s: no [prof] windows" % label)
        return
    header = ["set", "wins", "frames"] + prof.PROF_SECTIONS + ["total", "nD", "vtxN"]
    rows = [["all", s["windows"], s["frames"]]
            + [_fmt(s[k]["mean"]) for k in prof.PROF_SECTIONS]
            + [_fmt(s["total"]["mean"]), _fmt(s["nD"], 0), _fmt(s["vtxN"], 0)]]
    if storm:
        ss = prof.section_summary(storm)
        rows.append(["storm (prof!)", ss["windows"], ss["frames"]]
                    + [_fmt(ss[k]["mean"]) for k in prof.PROF_SECTIONS]
                    + [_fmt(ss["total"]["mean"]), _fmt(ss["nD"], 0), _fmt(ss["vtxN"], 0)])
    out.say("")
    out.say("[prof] sections, mean ms/frame — %s" % label)
    out.table(header, rows)


def _fps_table(out, label, parsed):
    f = prof.fps_summary(parsed)
    if not f:
        out.say("%s: no [watchdog] beats (fps needs at least 2)" % label)
        return None
    out.say("")
    out.say("fps from [watchdog] frame deltas / %.0fs — %s (first beat dropped: it "
            "covers boot)" % (prof.WATCHDOG_PERIOD_S, label))
    rows = [["all", f["beats"], _fmt(f["p10"], 1), _fmt(f["p50"], 1), _fmt(f["p90"], 1),
             _fmt(f["min"], 1), _fmt(f["max"], 1), _fmt(f["mean"], 1), f["max_frame"]]]
    # A10: a beat that carried a replay SHOT is measuring the harness, not the game -- the
    # screenshot's readback and 230-288 KB BMP write are main-thread work, ~2 s per image on
    # an SD card. On hardware those beats alone produced every teens-fps dip, in every lever
    # configuration, which is why the row is printed rather than left to be discovered again.
    if f.get("shot_beats"):
        g = prof.fps_summary(parsed, exclude_shots=True)
        if g:
            rows.append(["no SHOT", g["beats"], _fmt(g["p10"], 1), _fmt(g["p50"], 1),
                         _fmt(g["p90"], 1), _fmt(g["min"], 1), _fmt(g["max"], 1),
                         _fmt(g["mean"], 1), g["max_frame"]])
    out.table(["beats", "n", "p10", "p50", "p90", "min", "max", "mean", "max frame"], rows)
    if f.get("shot_beats"):
        out.say("  %d beat(s) carried a replay SHOT (screenshot readback + BMP write on the "
                "main thread) and are excluded from the second row." % f["shot_beats"])
    return f


def _errors_table(out, parsed):
    nonzero = {k: v for k, v in parsed.errors.items() if v}
    out.say("")
    if not nonzero:
        out.say("errors: none of %s" % " ".join(prof.ERROR_TAGS))
        return
    out.table(["error", "lines"], sorted(nonzero.items(), key=lambda kv: -kv[1]))
    for line in parsed.error_lines[:5]:
        out.say("  " + line[:160])


def do_prof(args, out, ctx):
    if args.ab:
        return _do_ab(args, out, ctx)
    if not args.target:
        raise GdxError("prof needs a log file or a run id (or --ab A B)", util.EXIT_USAGE)
    parsed, label, manifest = _load(args.target, ctx["run_root"], ctx["track"])
    if parsed.is_empty():
        raise GdxError("no [prof]/[gpu]/[watchdog]/[rt] receipts in %s — was "
                       "[debug] filelog/verbose on?" % label, util.EXIT_NODATA)

    windows = parsed.prof[-args.window:] if args.window else parsed.prof
    storm = parsed.prof_storm[-args.window:] if args.window else parsed.prof_storm

    gputrace = manifest.get("gputrace") if manifest else parsed.gputrace
    out.say("%s — %d [prof] window(s), %d [prof!] storm, %d [gpu], %d [rt], %d beats; "
            "gputrace=%s" % (label, len(parsed.prof), len(parsed.prof_storm),
                             len(parsed.gpu), len(parsed.rt), len(parsed.watchdog),
                             gputrace))
    if gputrace:
        # PROTOCOL.md line 22: gputrace inflates profiles; never let a table hide it.
        out.warn("gputrace was ON — these millisecond values are inflated and are not "
                 "comparable with a gputrace=0 run")

    show_all = not (args.sections or args.ops or args.fps or args.by_vtxn
                    or getattr(args, "main", False))
    fps = None
    if args.fps or show_all:
        fps = _fps_table(out, label, parsed)
    if args.sections or show_all:
        _sections_table(out, label, windows, storm)
    if args.by_vtxn:
        out.say("")
        out.say("[prof] sections bucketed by nD (draw count) — %s" % label)
        header = ["bucket", "wins", "frames"] + prof.PROF_SECTIONS + ["total", "vtxN"]
        rows = []
        for name, sel in prof.bucket_by_nd(windows):
            s = prof.section_summary(sel)
            rows.append([name, s["windows"], s["frames"]]
                        + [_fmt(s[k]["mean"]) for k in prof.PROF_SECTIONS]
                        + [_fmt(s["total"]["mean"]), _fmt(s["vtxN"], 0)])
        out.table(header, rows)
    if getattr(args, "main", False) or (show_all and parsed.mprof):
        _main_tables(out, label, parsed, args)
    if args.ops or show_all:
        rows = prof.op_summary(parsed)
        if rows:
            out.say("")
            out.say("[profop] top opcode handlers, mean ms/frame — %s" % label)
            out.table(["op", "ms", "calls", "windows"],
                      [[r[0], _fmt(r[1]), _fmt(r[2], 0), r[3]] for r in rows])
    if parsed.rt and (args.sections or show_all):
        last = parsed.rt[-1]
        out.say("")
        out.say("[rt] last window: mode=%s waitMain=%.2f waitRender=%.2f ovl=%.2f "
                "brMain=%.2f" % (last.get("mode"), last.get("waitMain", 0),
                                 last.get("waitRender", 0), last.get("ovl", 0),
                                 last.get("brMain", 0)))
    _errors_table(out, parsed)

    out.emit({"label": label, "gputrace": gputrace, "fps": fps,
              "sections": prof.section_summary(windows),
              "errors": parsed.errors})
    if args.fail_on_error and (parsed.fatals or any(parsed.errors.values())):
        raise GdxError("error receipts present (--fail-on-error)", util.EXIT_FAIL)
    return util.EXIT_OK


def _main_tables(out, label, parsed, args):
    """[mprof] MAIN-thread breakdown (NEXT.md A8). The render-side [prof] table answers
    "what does the render thread spend the frame on"; this one answers the same question
    for the thread that was actually the bottleneck on hardware ([rt] waitMain 14-16 ms vs
    waitRender < 1 ms). Sections partition main's frame, so the percentages sum to 100."""
    windows = parsed.mprof[-args.window:] if args.window else parsed.mprof
    if not windows:
        out.warn("no [mprof] receipts in %s — was [debug] mainprof=1 set?" % label)
        return
    s = prof.mprof_summary(windows)
    out.say("")
    out.say("[mprof] MAIN-thread sections, ranked, mean ms/frame — %s "
            "(%d window(s), %d frames)" % (label, s["windows"], s["frames"]))
    rows = [[name, _fmt(ms), "%.1f%%" % pct,
             "WAIT" if name in prof.MPROF_WAIT else "work",
             "fiber" if name in prof.MPROF_FIB else
             ("host" if name in prof.MPROF_HOST else "residual")]
            for name, ms, pct in prof.mprof_ranked(s)]
    out.table(["section", "ms", "%frame", "kind", "side"], rows)
    out.say("  total %s ms/frame  (game-fiber %s + host-loop %s + residual %s)"
            % (_fmt(s["total"]["mean"]), _fmt(s["fib"]), _fmt(s["host"]),
               _fmt(s["oth"]["mean"])))
    out.say("  of which WORK %s ms/frame, BLOCKED %s ms/frame (%s)"
            % (_fmt(s["work"]), _fmt(s["wait"]), " ".join(prof.MPROF_WAIT)))
    # The ranking that survives leaving the emulator: host contention inflates the wait
    # columns (core 2 is time-sliced onto the same host thread), the work columns hold.
    out.say("")
    out.say("[mprof] main-thread WORK only (waits excluded), mean ms/frame — %s" % label)
    out.table(["section", "ms", "%work", "side"],
              [[n, _fmt(ms), "%.1f%%" % pct,
                "fiber" if n in prof.MPROF_FIB else
                ("host" if n in prof.MPROF_HOST else "residual")]
               for n, ms, pct in prof.mprof_ranked(s, sections=prof.MPROF_WORK,
                                                   denom="work") if ms > 0.004])
    # The emitted tot= is computed in the firmware from the same accumulators; a drift
    # means a part= line was lost in the log, not that the model is wrong.
    if s["emitted_tot"] and abs(s["emitted_tot"] - s["total"]["mean"]) > 0.05:
        out.warn("summed sections %.2f != emitted tot= %.2f — a part= line is missing "
                 "from the log" % (s["total"]["mean"], s["emitted_tot"]))
    if args.by_vtxn:
        out.say("")
        out.say("[mprof] main-thread sections bucketed by nD (draw count) — %s" % label)
        cols = prof.MPROF_SECTIONS
        header = ["bucket", "wins", "frames"] + cols + ["total"]
        rows = []
        for name, sel in prof.bucket_by_nd(windows):
            b = prof.mprof_summary(sel)
            rows.append([name, b["windows"], b["frames"]]
                        + [_fmt(b[k]["mean"]) for k in cols]
                        + [_fmt(b["total"]["mean"])])
        out.table(header, rows)


def _do_ab(args, out, ctx):
    a_id, b_id = args.ab
    pa, la, ma = _load(a_id, ctx["run_root"], ctx["track"])
    pb, lb, mb = _load(b_id, ctx["run_root"], ctx["track"])

    # An A/B across different builds or different gputrace states is not a measurement.
    if ma and mb:
        if bool(ma.get("gputrace")) != bool(mb.get("gputrace")):
            raise GdxError("refusing to A/B: gputrace differs (%s vs %s) — gputrace "
                           "inflates profiles (PROTOCOL.md line 22)"
                           % (ma.get("gputrace"), mb.get("gputrace")), util.EXIT_USAGE)
        ga = (ma.get("platform") or {}).get("gfx")
        gb = (mb.get("platform") or {}).get("gfx")
        if ga and gb and ga != gb and not args.force:
            raise GdxError("refusing to A/B: renderer differs (%s vs %s). `retro run` falls "
                           "back Vulkan->OpenGL->Software on a boot crash, and both frame "
                           "hashes and timings are renderer-specific, so this would compare "
                           "two renderers. Re-run with --gfx pinned, or --force"
                           % (ga, gb), util.EXIT_USAGE)
        # Contended runs measure the host, not the guest (see tools/dev/README.md).
        for tag, m in (("A", ma), ("B", mb)):
            load = (m.get("host") or {}).get("loadavg_at_launch")
            if load is not None and load > (m.get("host") or {}).get("max_loadavg", 40):
                out.warn("%s launched at load %.1f — treat these ms as host contention"
                         % (tag, load))
        ida = (ma.get("build") or {}).get("id")
        idb = (mb.get("build") or {}).get("id")
        if ida != idb and not args.force:
            raise GdxError("refusing to A/B: build ids differ (%s vs %s); a lever A/B must "
                           "use ONE build — pass --force if this is deliberate"
                           % (ida, idb), util.EXIT_USAGE)

    wa = pa.prof[-args.window:] if args.window else pa.prof
    wb = pb.prof[-args.window:] if args.window else pb.prof

    def compare(sel_a, sel_b, title):
        sa, sb = prof.section_summary(sel_a), prof.section_summary(sel_b)
        if not sa or not sb:
            return
        out.say("")
        out.say(title)
        rows = []
        for k in prof.PROF_SECTIONS + ["total"]:
            va, vb = sa[k]["mean"], sb[k]["mean"]
            d = vb - va
            pct = (100.0 * d / va) if va else None
            rows.append([k, _fmt(va), _fmt(vb), ("%+.2f" % d),
                         ("%+.1f%%" % pct) if pct is not None else "-"])
        out.table(["section", "A %s" % la, "B %s" % lb, "delta", "pct"], rows)

    out.say("A = %s (%d windows)   B = %s (%d windows)" % (la, len(wa), lb, len(wb)))
    compare(wa, wb, "[prof] mean ms/frame, all windows")
    if getattr(args, "main", False):
        ma = pa.mprof[-args.window:] if args.window else pa.mprof
        mb = pb.mprof[-args.window:] if args.window else pb.mprof
        sa, sb = prof.mprof_summary(ma), prof.mprof_summary(mb)
        if sa and sb:
            out.say("")
            out.say("[mprof] MAIN-thread mean ms/frame, all windows")
            rows = []
            for k in prof.MPROF_SECTIONS:
                va, vb = sa[k]["mean"], sb[k]["mean"]
                d = vb - va
                rows.append([k, _fmt(va), _fmt(vb), "%+.2f" % d])
            rows.append(["TOTAL", _fmt(sa["total"]["mean"]), _fmt(sb["total"]["mean"]),
                         "%+.2f" % (sb["total"]["mean"] - sa["total"]["mean"])])
            out.table(["section", "A %s" % la, "B %s" % lb, "delta"], rows)
    if args.by_vtxn:
        # Selection bias: the buckets ARE nD, so a lever that changes the draw count moves
        # windows between buckets and each bucket then compares different scenes. A batch
        # fold measured -22% draws was reported as +5.9% SLOWER in every bucket while the
        # whole-run total was flat. Say so rather than printing a plausible wrong number.
        nda = sum(w["nD"] for w in wa) / max(1, len(wa))
        ndb = sum(w["nD"] for w in wb) / max(1, len(wb))
        if nda and abs(ndb - nda) / nda > 0.05:
            out.warn("--by-vtxn is UNSAFE for this pair: mean nD %.1f vs %.1f (%.0f%%). The "
                     "buckets are nD, so windows migrate between them and each bucket compares "
                     "different scenes. Cut the band by scene tag instead, or read the all-windows "
                     "table above." % (nda, ndb, 100.0 * (ndb - nda) / nda))
        ba = dict(prof.bucket_by_nd(wa))
        bb = dict(prof.bucket_by_nd(wb))
        for name in [n for n, _ in prof.bucket_by_nd(wa)]:
            if name in bb:
                compare(ba[name], bb[name], "[prof] bucket: %s" % name)

    out.say("")
    out.say("error receipts (A / B)")
    rows = [[t, pa.errors[t], pb.errors[t]] for t in prof.ERROR_TAGS
            if pa.errors[t] or pb.errors[t]]
    out.table(["error", "A", "B"], rows or [["(none)", 0, 0]])

    fa, fb = prof.fps_summary(pa), prof.fps_summary(pb)
    if fa and fb:
        out.say("")
        out.table(["fps", "p10", "p50", "p90"],
                  [["A " + la, _fmt(fa["p10"], 1), _fmt(fa["p50"], 1), _fmt(fa["p90"], 1)],
                   ["B " + lb, _fmt(fb["p10"], 1), _fmt(fb["p50"], 1), _fmt(fb["p90"], 1)]])
    out.emit({"a": la, "b": lb,
              "a_sections": prof.section_summary(wa),
              "b_sections": prof.section_summary(wb)})
    return util.EXIT_OK


# ------------------------------------------------------------------- runs --
def do_runs(args, out, ctx):
    if args.action == "list":
        rows = []
        for rd in reversed(runs.listing(ctx["run_root"], ctx["track"])):
            try:
                m = rd.read_manifest()
            except GdxError:
                rows.append([rd.run_id, "?", "no manifest", "", "", ""])
                continue
            r = m.get("result") or {}
            rows.append([rd.run_id, m.get("lane"), r.get("verdict"),
                         r.get("max_frame"), "gputrace" if m.get("gputrace") else "-",
                         (m.get("replay") or {}).get("id") or "-"])
        if not rows:
            out.say("no runs for track %r under %s"
                    % (ctx["track"], runs.track_root(ctx["run_root"], ctx["track"])))
            return util.EXIT_OK
        out.table(["run_id", "lane", "verdict", "max frame", "trace", "replay"], rows)
        out.emit({"runs": [r[0] for r in rows]})
        return util.EXIT_OK

    rd = runs.resolve(ctx["run_root"], ctx["track"], args.ident)
    if args.action == "path":
        print(rd.path)
        return util.EXIT_OK
    m = rd.read_manifest()
    problems = runs.validate(m)
    if problems:
        out.warn("manifest is not schema-%d valid: %s" % (runs.SCHEMA, "; ".join(problems)))
    b = m.get("build") or {}
    r = m.get("result") or {}
    out.table(["field", "value"], [
        ["run_id", m.get("run_id")], ["track", m.get("track")], ["lane", m.get("lane")],
        ["started", m.get("started")], ["wall_s", m.get("wall_s")],
        ["build id", b.get("id")], ["dirty", b.get("dirty")],
        ["artifact sha256", (b.get("sha256") or "")[:16]],
        ["gputrace", m.get("gputrace")], ["seconds", m.get("seconds")],
        ["replay", (m.get("replay") or {}).get("id")],
        ["verdict", r.get("verdict")], ["max frame", r.get("max_frame")],
        ["fatals", r.get("fatals")],
        ["dir", rd.path],
    ])
    out.say("")
    out.say("ini: " + ", ".join("%s=%s" % kv for kv in sorted((m.get("ini") or {}).items())))
    out.emit(m)
    return util.EXIT_OK


def do_report(args, out, ctx):
    rd = runs.resolve(ctx["run_root"], ctx["track"], args.ident)
    m = rd.read_manifest()
    parsed, label, _ = _load(rd.path, ctx["run_root"], ctx["track"])
    b = m.get("build") or {}
    r = m.get("result") or {}
    f = prof.fps_summary(parsed)
    s = prof.section_summary(parsed.prof)

    if not args.markdown:
        return do_runs(_Args(action="show", ident=args.ident), out, ctx)

    L = []
    L.append("### run `%s` — %s" % (m.get("run_id"), r.get("verdict")))
    L.append("")
    L.append("| field | value |")
    L.append("|---|---|")
    L.append("| build | `%s`%s |" % (b.get("id"), " **(dirty tree)**" if b.get("dirty") else ""))
    L.append("| artifact sha256 | `%s` |" % (b.get("sha256") or "")[:16])
    plat = m.get("platform") or {}
    emu = ("azahar %s" % plat.get("azahar")) if m.get("lane") == "azahar" \
        else ("retroarch %s" % plat.get("retroarch"))
    L.append("| lane / platform | %s / %s |" % (m.get("lane"), emu))
    L.append("| gputrace | **%s** |" % m.get("gputrace"))
    L.append("| replay | %s |" % ((m.get("replay") or {}).get("id") or "none"))
    L.append("| seconds | %s (wall %ss) |" % (m.get("seconds"), m.get("wall_s")))
    L.append("| ini | %s |" % ", ".join("`%s=%s`" % kv
                                        for kv in sorted((m.get("ini") or {}).items())))
    L.append("| verdict | %s — max frame %s, %s fatal |"
             % (r.get("verdict"), r.get("max_frame"), r.get("fatals")))
    L.append("| run dir | `%s` |" % rd.path)
    if f:
        L.append("")
        L.append("fps (`[watchdog]` deltas / 5 s, %d beats): p10 **%.1f** · p50 **%.1f** · "
                 "p90 **%.1f**" % (f["beats"], f["p10"], f["p50"], f["p90"]))
    if s:
        L.append("")
        L.append("| " + " | ".join(["set", "wins", "frames"] + prof.PROF_SECTIONS + ["total"]) + " |")
        L.append("|" + "---|" * (3 + len(prof.PROF_SECTIONS) + 1))
        L.append("| all | %d | %d | %s | %s |"
                 % (s["windows"], s["frames"],
                    " | ".join(_fmt(s[k]["mean"]) for k in prof.PROF_SECTIONS),
                    _fmt(s["total"]["mean"])))
    errs = {k: v for k, v in parsed.errors.items() if v}
    L.append("")
    L.append("errors: %s" % (", ".join("`%s`×%d" % kv for kv in sorted(errs.items()))
                             if errs else "none"))
    print("\n".join(L))
    return util.EXIT_OK


class _Args:
    def __init__(self, **kw):
        self.__dict__.update(kw)
