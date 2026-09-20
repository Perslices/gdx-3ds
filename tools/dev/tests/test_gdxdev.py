#!/usr/bin/env python3
"""Unit tests for gdxdev. Stdlib unittest only:  python3 tools/dev/tests/test_gdxdev.py

These are the golden-file checks the plan asks for (05-devcli-plan.md §5 steps 1-2):
the manifest round-trips through the schema check, the ini merge provably preserves a
foreign agent's key, and the log parser reproduces hand-computed numbers.
"""

import json
import os
import shutil
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from gdxdev import inifile, prof, replay, runs, util  # noqa: E402
from gdxdev.util import GdxError  # noqa: E402

FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")


class ManifestTest(unittest.TestCase):
    def _full(self):
        return runs.new_manifest(
            run_id="20260907-120000-boot", track="devcli", lane="azahar",
            started="2026-09-07T12:00:00", ended="2026-09-07T12:01:00", wall_s=60.0,
            build={"id": "native/devcli@abcdef12", "dirty": False, "artifact": "x.3dsx",
                   "sha256": "0" * 64, "generator": "Ninja", "configure_s": 1.0,
                   "build_s": 2.0},
            ini={"debug.filelog": "1"}, replay=None, gputrace=False, seconds=60,
            platform={"host": "darwin-arm64", "azahar": "2126.0", "retroarch": None,
                      "model": "emu-new3ds"},
            result={"verdict": "PASS", "fatals": 0, "max_frame": 100,
                    "distinct_frames": 10, "errors": {}},
            artifacts={"log": "log.txt", "emu_log": "emu.log", "shots": []})

    def test_valid_manifest_round_trips(self):
        with tempfile.TemporaryDirectory() as d:
            rd = runs.RunDir(d)
            m = self._full()
            rd.write_manifest(m)
            self.assertEqual(rd.read_manifest(), m)
            self.assertEqual(runs.validate(rd.read_manifest()), [])

    def test_missing_key_is_rejected(self):
        m = self._full()
        del m["gputrace"]
        self.assertTrue(any("gputrace" in p for p in runs.validate(m)))

    def test_gputrace_must_be_bool(self):
        m = self._full()
        m["gputrace"] = "yes"
        self.assertTrue(any("gputrace" in p for p in runs.validate(m)))

    def test_write_refuses_invalid(self):
        with tempfile.TemporaryDirectory() as d:
            m = self._full()
            m["schema"] = 99
            with self.assertRaises(GdxError):
                runs.RunDir(d).write_manifest(m)


class IniMergeTest(unittest.TestCase):
    SAMPLE = (
        "; gdiffuser.ini\n"
        "\n"
        "[debug]\n"
        "console = 1\n"
        "filelog = 1\n"
        "gputrace = 0\n"
        "someone_elses_lever = 1\n"
        "\n"
        "[stereo]\n"
        "enabled = 1\n"
    )

    def _tmp(self):
        f = tempfile.NamedTemporaryFile("w", suffix=".ini", delete=False)
        f.write(self.SAMPLE)
        f.close()
        return f.name

    def test_merge_preserves_foreign_keys(self):
        p = self._tmp()
        inifile.merge(p, {("debug", "gputrace"): "1"})
        got = inifile.read(p)
        # PROTOCOL.md line 17: another agent's lever must survive our write.
        self.assertEqual(got["debug"]["someone_elses_lever"], "1")
        self.assertEqual(got["debug"]["console"], "1")
        self.assertEqual(got["debug"]["gputrace"], "1")
        self.assertEqual(got["stereo"]["enabled"], "1")
        self.assertIn("; gdiffuser.ini", util.read_text(p))  # comments survive too
        os.unlink(p)

    def test_new_key_lands_in_its_section(self):
        p = self._tmp()
        inifile.merge(p, {("debug", "atlas3d"): "1"})
        got = inifile.read(p)
        self.assertEqual(got["debug"]["atlas3d"], "1")
        self.assertEqual(got["stereo"]["enabled"], "1")
        os.unlink(p)

    def test_new_section_is_appended(self):
        p = self._tmp()
        inifile.merge(p, {("native", "tier1"): "1"})
        self.assertEqual(inifile.read(p)["native"]["tier1"], "1")
        os.unlink(p)

    def test_dry_run_changes_nothing(self):
        p = self._tmp()
        inifile.merge(p, {("debug", "gputrace"): "1"}, dry_run=True)
        self.assertEqual(util.read_text(p), self.SAMPLE)
        os.unlink(p)

    def test_spec_parsing(self):
        self.assertEqual(inifile.parse_spec("gputrace=1"), ("debug", "gputrace", "1"))
        self.assertEqual(inifile.parse_spec("native.tier1=2"), ("native", "tier1", "2"))
        with self.assertRaises(GdxError):
            inifile.parse_spec("nope")


class AutoinputTest(unittest.TestCase):
    def _write(self, text):
        f = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False)
        f.write(text)
        f.close()
        return f.name

    def test_valid(self):
        p = self._write("ticks\n# comment\n60 START\n120 A 30\n300 SHOT drive1\n")
        self.assertEqual(replay.validate_autoinput(p), 3)
        os.unlink(p)

    def test_seconds_mode_rejected(self):
        p = self._write("2 START\n")
        with self.assertRaises(GdxError):
            replay.validate_autoinput(p)
        os.unlink(p)

    def test_descending_ticks_rejected(self):
        p = self._write("ticks\n120 A\n60 B\n")
        with self.assertRaises(GdxError):
            replay.validate_autoinput(p)
        os.unlink(p)

    def test_unknown_token_rejected(self):
        p = self._write("ticks\n60 TURBO\n")
        with self.assertRaises(GdxError):
            replay.validate_autoinput(p)
        os.unlink(p)

    def test_too_many_events_rejected(self):
        p = self._write("ticks\n" + "".join("%d A\n" % (i * 10) for i in range(70)))
        with self.assertRaises(GdxError):
            replay.validate_autoinput(p)
        os.unlink(p)

    def test_shot_needs_a_label(self):
        p = self._write("ticks\n60 SHOT\n")
        with self.assertRaises(GdxError):
            replay.validate_autoinput(p)
        os.unlink(p)


class ProfParseTest(unittest.TestCase):
    """Golden numbers computed by hand from tools/dev/tests/fixtures/prof-sample.log."""

    @classmethod
    def setUpClass(cls):
        cls.p = prof.parse_file(os.path.join(FIXTURES, "prof-sample.log"))

    def test_counts(self):
        self.assertEqual(len(self.p.prof), 3)
        self.assertEqual(len(self.p.prof_storm), 1)
        self.assertEqual(len(self.p.profop), 1)
        self.assertEqual(len(self.p.gpu), 1)
        self.assertEqual(len(self.p.fill), 1)
        self.assertEqual(len(self.p.rt), 1)
        self.assertEqual(len(self.p.watchdog), 4)

    def test_prof_fields(self):
        w = self.p.prof[0]
        self.assertEqual(w["frame"], 64)
        self.assertEqual(w["n"], 64)
        self.assertAlmostEqual(w["br"], 3.20)
        self.assertAlmostEqual(w["dsp"], 6.50)
        self.assertEqual(w["nD"], 110)
        self.assertAlmostEqual(w["total"], 3.20 + 6.50 + 0.70 + 3.40 + 1.20 + 2.30 + 0.02 + 0.10)

    def test_section_means(self):
        s = prof.section_summary(self.p.prof)
        self.assertEqual(s["windows"], 3)
        self.assertEqual(s["frames"], 192)
        self.assertAlmostEqual(s["br"]["mean"], (3.20 + 3.50 + 3.80) / 3, places=6)
        self.assertAlmostEqual(s["dsp"]["mean"], (6.50 + 6.90 + 7.30) / 3, places=6)

    def test_nd_bucketing(self):
        buckets = dict((n, len(w)) for n, w in prof.bucket_by_nd(self.p.prof))
        self.assertEqual(buckets, {"crowd (nD 100-inf)": 3})

    def test_fps_percentiles(self):
        # beats: +0 (dropped), +130, +260, +150 -> 26.0, 52.0, 30.0 fps
        f = prof.fps_summary(self.p)
        self.assertEqual(f["beats"], 3)
        self.assertAlmostEqual(f["p50"], 30.0)
        self.assertAlmostEqual(f["min"], 26.0)
        self.assertAlmostEqual(f["max"], 52.0)
        self.assertEqual(f["max_frame"], 540)

    def test_profop(self):
        rows = prof.op_summary(self.p)
        self.assertEqual(rows[0][0], "E4")
        self.assertAlmostEqual(rows[0][1], 3.90)

    def test_errors_counted_once_each(self):
        self.assertEqual(self.p.errors["[fatal]"], 1)
        self.assertEqual(self.p.errors["[gfxfail]"], 1)
        self.assertEqual(self.p.errors["[fatal-trace]"], 0)
        self.assertEqual(self.p.fatals, 1)

    def test_gputrace_detected(self):
        self.assertTrue(self.p.gputrace)

    def test_percentile_is_nearest_rank(self):
        v = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
        self.assertEqual(prof.percentile(v, 10), 1)
        self.assertEqual(prof.percentile(v, 50), 5)
        self.assertEqual(prof.percentile(v, 90), 9)


class VerdictTest(unittest.TestCase):
    def test_fatal_outside_the_slice_is_invisible(self):
        from gdxdev import emu
        out = util.Out(quiet=True)
        mine = ("OutputDebugString: frame 1\nOutputDebugString: frame 2\n"
                "OutputDebugString: frame 3\n")
        r = emu.judge(mine, "", out)
        self.assertEqual(r["verdict"], "PASS")
        self.assertEqual(r["fatals"], 0)
        # The same text WITH someone else's crash appended would fail — which is exactly
        # what ci-3ds.sh does by grepping the whole shared log.
        r2 = emu.judge(mine + "[fatal] someone else's crash\n", "", out)
        self.assertEqual(r2["verdict"], "FAIL")

    def test_no_heartbeat_fails(self):
        from gdxdev import emu
        r = emu.judge("OutputDebugString: frame 1\n", "", util.Out(quiet=True))
        self.assertEqual(r["verdict"], "FAIL")
        self.assertIn("heartbeat", r["detail"])


class SdGuardTest(unittest.TestCase):
    def test_writes_are_confined_to_3ds(self):
        from gdxdev import sd
        with tempfile.TemporaryDirectory() as vol:
            self.assertTrue(sd._safe_dest(vol, "3ds/G-Diffuser-3DS.3dsx")
                            .startswith(os.path.join(vol, "3ds")))
            for escape in ("../evil", "3ds/../../evil", "boot.firm", "/etc/hosts"):
                with self.assertRaises(GdxError, msg=escape):
                    sd._safe_dest(vol, escape)


class ReplayResolutionTest(unittest.TestCase):
    def test_committed_replays_all_validate(self):
        d = replay.replays_dir()
        found = [f for f in sorted(os.listdir(d)) if f.endswith(".txt")]
        self.assertTrue(found, "no committed replays under %s" % d)
        for f in found:
            replay.validate_autoinput(os.path.join(d, f))

    def test_resolve_by_id(self):
        path, kind = replay.resolve("storm2")
        self.assertEqual(kind, "autoinput")
        self.assertTrue(path.endswith("storm2.txt"))

    def test_unknown_id_exits_5(self):
        with self.assertRaises(GdxError) as cm:
            replay.resolve("no-such-replay")
        self.assertEqual(cm.exception.code, util.EXIT_STAGING)


class ReplayStagingTest(unittest.TestCase):
    """Staging must be lane-agnostic: `retro run --replay` regressed because retro.py
    declared the flag but never called stage(), and stage() was hardcoded to the shared
    Azahar SD. Reported by the refbuild track, 2026-09-12."""

    def test_autoinput_lands_at_the_given_sd_root(self):
        with tempfile.TemporaryDirectory() as sd:
            block, ini = replay.stage("boot", sdmc_root=sd,
                                      gdx_dir=os.path.join(sd, "3ds", "gdiffuser"))
            dest = os.path.join(sd, "gdx-autoinput.txt")
            self.assertTrue(os.path.isfile(dest), "not staged to the private SD root")
            self.assertEqual(block["kind"], "autoinput")
            self.assertEqual(block["events"], 2)
            # Must actively DISARM a stale .gdxrec rather than leaving it to win.
            self.assertEqual(ini, {("debug", "input_replay"): ""})
            src = os.path.join(replay.replays_dir(), "boot.txt")
            self.assertEqual(util.read_text(dest), util.read_text(src))

    def test_gdxrec_lands_under_gdiffuser_rec(self):
        """The guest opens the ABSOLUTE path <gdx3ds_fs_base_path()>rec/<name>.gdxrec, i.e.
        sdmc:/3ds/gdiffuser/rec/, so staging is cwd-independent. (Earlier the guest used a
        cwd-relative path and staging chased it to the SD root; both sides are now pinned.)
        Reported by integration-verify, 2026-09-13. The previous version of this very test
        asserted the wrong path, which is how the bug survived."""
        with tempfile.TemporaryDirectory() as sd:
            gdx = os.path.join(sd, "3ds", "gdiffuser")
            os.makedirs(gdx)
            rec_src = os.path.join(replay.replays_dir(), "unittest-tmp.gdxrec")
            with open(rec_src, "w") as f:
                f.write("gdxrec 1\n0 0000 0 0\n1 0001 0 0\n")
            try:
                block, ini = replay.stage("unittest-tmp", sdmc_root=sd, gdx_dir=gdx)
            finally:
                os.unlink(rec_src)
            self.assertTrue(os.path.isfile(os.path.join(gdx, "rec", "unittest-tmp.gdxrec")),
                            "must stage at <sd>/3ds/gdiffuser/rec/")
            self.assertFalse(os.path.exists(os.path.join(sd, "rec")),
                             "must NOT stage at the SD root")
            self.assertEqual(block["kind"], "gdxrec")
            # 03-oracle-plan.md:128 — input_replay wins over autoinput.
            self.assertEqual(ini, {("debug", "input_replay"): "unittest-tmp"})

    def test_a_gdxrec_with_shots_arms_the_capture_path(self):
        """SHOT capture is gated on [debug] fh; with fh=0 the guest logs "capture request
        ignored" and the completion check would fail a good run."""
        with tempfile.TemporaryDirectory() as sd:
            src = os.path.join(replay.replays_dir(), "unittest-shots.gdxrec")
            with open(src, "w") as f:
                f.write("gdxrec 1\n0 0000 0 0\n10 0000 0 0 SHOT title\n")
            try:
                block, ini = replay.stage("unittest-shots", sdmc_root=sd)
            finally:
                os.unlink(src)
            self.assertEqual(block["shots"], ["title"])
            self.assertEqual(ini[("debug", "fh")], "1")
            self.assertFalse(os.path.exists(os.path.join(sd, "gdx-autoinput.txt")))

    def test_rec_path_matches_the_guest_source(self):
        """Pin the staging path to the guest that consumes it, so the two cannot drift."""
        src = os.path.join(util.repo_root(), "port", "3ds", "main_3ds.cpp")
        if not os.path.exists(src):
            self.skipTest("port source not present")
        text = util.read_text(src)
        self.assertIn('"%srec/%s.gdxrec"', text,
                      "the guest no longer builds <base>/rec/<name>.gdxrec — re-check rec_dir()")
        self.assertIn('gdx3ds_fs_base_path()', text)
        with tempfile.TemporaryDirectory() as sd:
            self.assertEqual(replay.rec_dir(sd), os.path.join(sd, "3ds", "gdiffuser", "rec"))

    def test_default_root_is_the_azahar_sd(self):
        self.assertEqual(replay.autoinput_path(), util.AUTOINPUT_SD)
        self.assertEqual(replay.rec_dir(), os.path.join(util.AZAHAR_SDMC, "3ds", "gdiffuser", "rec"))


class ReplayReceiptTest(unittest.TestCase):
    """A staged replay that never loads must FAIL the run. Without this, a mis-staged
    replay boots fine, never replays, and reports a confident PASS."""

    def test_open_failure_is_detected(self):
        ok, why = replay.verify_receipts(
            "gdxrec", "[rec] replay open failed: rec/storm2.gdxrec\n")
        self.assertFalse(ok)
        self.assertIn("rejected", why)

    def test_missing_receipt_is_detected(self):
        ok, why = replay.verify_receipts("gdxrec", "[watchdog] beat=1 frame=10(+10)\n")
        self.assertFalse(ok)
        self.assertIn("never loaded", why)

    def test_armed_receipt_passes(self):
        ok, why = replay.verify_receipts(
            "gdxrec", "[rec] replay armed rec/storm2.gdxrec frames=600\n")
        self.assertTrue(ok, why)

    def test_autoinput_receipt(self):
        self.assertTrue(replay.verify_receipts(
            "autoinput", "[input] autoinput script loaded: 2 events (tick timebase)\n")[0])
        ok, why = replay.verify_receipts("autoinput", "nothing here\n")
        self.assertFalse(ok)

    def test_seconds_timebase_fallback_fails(self):
        ok, why = replay.verify_receipts(
            "autoinput", "[input] autoinput script loaded: 1 events (seconds timebase)\n")
        self.assertFalse(ok)
        self.assertIn("62", why)

    def test_no_replay_is_always_ok(self):
        self.assertTrue(replay.verify_receipts(None, "")[0])

    def test_judge_fails_a_run_whose_replay_never_loaded(self):
        from gdxdev import emu
        beats = "".join("OutputDebugString: frame %d\n" % i for i in range(1, 5))
        r = emu.judge(beats, "", util.Out(quiet=True), replay_kind="gdxrec")
        self.assertEqual(r["verdict"], "FAIL")
        r2 = emu.judge(beats, "[rec] replay armed rec/x.gdxrec frames=10\n",
                       util.Out(quiet=True), replay_kind="gdxrec")
        self.assertEqual(r2["verdict"], "PASS")


class BenignReceiptTest(unittest.TestCase):
    def test_atexit_trace_is_not_a_fatal(self):
        """port/3ds/main_3ds.cpp:815 emits this on the NORMAL exit() path, so counting it
        made every clean run FAIL on "1 [fatal] line(s)" (integration-verify, 2026-09-13)."""
        text = ("OutputDebugString: frame 1\nOutputDebugString: frame 2\n"
                "OutputDebugString: frame 3\n"
                "[fatal-trace] atexit: process exiting via exit()\n")
        p = prof.parse_text(text)
        self.assertEqual(p.errors["[fatal-trace]"], 0)
        self.assertEqual(p.benign, 1)
        self.assertEqual(p.fatals, 0)
        from gdxdev import emu
        self.assertEqual(emu.judge(text, "", util.Out(quiet=True))["verdict"], "PASS")

    def test_a_real_fatal_trace_still_counts(self):
        p = prof.parse_text("[fatal-trace] pc=0x001 lr=0x002 in Machine_Draw\n")
        self.assertEqual(p.errors["[fatal-trace]"], 1)
        self.assertEqual(p.fatals, 1)


class RetroGfxTest(unittest.TestCase):
    def test_gfx_is_case_insensitive(self):
        from gdxdev import retro
        self.assertEqual(retro.normalise_gfx("vulkan"), "Vulkan")
        self.assertEqual(retro.normalise_gfx("OpenGL"), "OpenGL")
        self.assertEqual(retro.normalise_gfx("SOFTWARE"), "Software")
        self.assertIsNone(retro.normalise_gfx(None))

    def test_unknown_gfx_is_a_usage_error(self):
        from gdxdev import retro
        with self.assertRaises(GdxError) as cm:
            retro.normalise_gfx("metal")
        self.assertEqual(cm.exception.code, util.EXIT_USAGE)

    def test_crash_signature_names_the_vulkan_worker(self):
        from gdxdev import retro
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write("thread VulkanWorker\nReceived signal SIGBUS\n")
            path = f.name
        try:
            sig = retro._crash_signature(path)
        finally:
            os.unlink(path)
        self.assertIn("SIGBUS", sig)
        self.assertIn("VulkanWorker", sig)

    def test_no_guest_log_is_retried_not_blamed_on_the_renderer(self):
        """An empty guest log means the boot died before filelog init — transient under
        load, so it must be retried rather than falling straight through to the next
        renderer (measured: one Vulkan run gave up after a single attempt)."""
        import inspect
        from gdxdev import retro
        src = inspect.getsource(retro._attempt)
        i = src.index("if not sd_log.strip():")
        self.assertIn("return False, True", src[i:i + 600],
                      "an empty guest log must be reported as an early crash")

    def test_retry_count_is_three_attempts(self):
        from gdxdev import retro
        self.assertEqual(retro.EARLY_CRASH_ATTEMPTS, 3)   # initial + 2 retries

    def test_retro_run_stages_the_replay(self):
        """retro.do_run must call stage() with the instance's private paths."""
        import inspect
        from gdxdev import retro
        src = inspect.getsource(retro.do_run)
        self.assertIn("replay.stage(", src,
                      "retro run accepts --replay but never stages it")
        self.assertIn("sdmc_root=inst.sdmc()", src)
        self.assertIn("gdx_dir=inst.gdx_dir()", src)
        self.assertIn("replay=replay_block", inspect.getsource(retro.do_run),
                      "the manifest must record the staged replay, not null")


class RetroInstanceLockTest(unittest.TestCase):
    """Orphan RetroArch processes shared private SDs with live runs (reported by the
    coordinator, 2026-09-13: 9 orphans, ppid 1, several on instance names a live run was
    using). These cover the four guards that make that impossible."""

    def setUp(self):
        from gdxdev import retro
        self.retro = retro
        self.tmp = tempfile.mkdtemp()
        self._root = retro.INSTANCE_ROOT
        retro.INSTANCE_ROOT = self.tmp

    def tearDown(self):
        self.retro.INSTANCE_ROOT = self._root
        del self.retro._ACTIVE[:]
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _inst(self, name="t"):
        return self.retro.Instance(name)

    def test_acquire_then_second_acquire_is_refused_with_exit_4(self):
        inst = self._inst()
        out = util.Out(quiet=True)
        self.retro.acquire_instance(inst, {"track": "devcli"}, "/tmp/run1", out)
        self.assertTrue(os.path.isfile(inst.lock))
        # A second driver (this process is alive, so the lock is live) must be refused.
        with self.assertRaises(GdxError) as cm:
            self.retro.acquire_instance(inst, {"track": "other"}, "/tmp/run2", out)
        self.assertEqual(cm.exception.code, util.EXIT_LOCK)
        msg = str(cm.exception)
        self.assertIn(str(os.getpid()), msg, "the refusal must name the owning pid")
        self.assertIn("/tmp/run1", msg, "the refusal must name the owning run dir")

    def test_stale_lock_from_a_dead_driver_is_cleared(self):
        inst = self._inst()
        os.makedirs(inst.dir, exist_ok=True)
        with open(inst.lock, "w") as f:
            json.dump({"driver_pid": 999999, "retro_pid": None,
                       "instance": inst.name, "run_dir": "/tmp/old"}, f)
        self.retro.acquire_instance(inst, {"track": "devcli"}, "/tmp/new", util.Out(quiet=True))
        self.assertEqual(self.retro.read_lock(inst)["driver_pid"], os.getpid())

    def test_release_removes_the_lock(self):
        inst = self._inst()
        self.retro.acquire_instance(inst, {}, "/tmp/r", util.Out(quiet=True))
        self.retro.release_instance(inst)
        self.assertFalse(os.path.exists(inst.lock))

    def test_set_child_records_the_pid_for_reap(self):
        inst = self._inst()
        self.retro.acquire_instance(inst, {}, "/tmp/r", util.Out(quiet=True))
        self.retro.set_child(inst, 4242)
        self.assertEqual(self.retro.read_lock(inst)["retro_pid"], 4242)

    def test_kill_group_refuses_to_kill_the_driver_itself(self):
        """Caught a real self-kill: the 'refuse our own group' branch fell through to
        os.kill(pid), so kill_group(os.getpid()) SIGTERMed the driver. The suicide guard
        must return False and leave us running."""
        self.assertFalse(self.retro.kill_group(os.getpid(), grace=0.2))
        self.assertTrue(util.pid_alive(os.getpid()), "kill_group killed its own driver")

    def test_kill_group_kills_a_real_child_in_its_own_session(self):
        import subprocess
        child = subprocess.Popen(["sleep", "60"], start_new_session=True)
        self.assertNotEqual(os.getpgid(child.pid), os.getpgid(0))
        self.assertTrue(self.retro.kill_group(child.pid, grace=2.0))
        child.wait()
        self.assertFalse(util.pid_alive(child.pid))

    def test_a_zombie_counts_as_gone(self):
        import subprocess
        child = subprocess.Popen(["true"], start_new_session=True)
        while util.pid_alive(child.pid) and not self.retro._gone(child.pid):
            time.sleep(0.05)
        self.assertTrue(self.retro._gone(child.pid),
                        "a terminated-but-unreaped child must read as gone")
        child.wait()

    def test_kill_group_on_a_dead_pid_is_a_noop(self):
        self.assertFalse(self.retro.kill_group(999999))
        self.assertFalse(self.retro.kill_group(None))

    def test_instance_name_parsed_from_the_cfg_argument(self):
        cmd = ("/Applications/RetroArch.app/Contents/MacOS/RetroArch -c "
               "/Users/x/Library/Application Support/gdx-retro/r3/retroarch.cfg -L core.dylib")
        m = self.retro._CFG_RE.search(cmd)
        self.assertIsNotNone(m)
        self.assertEqual(m.group(2), "r3")


class RetroReapClassificationTest(unittest.TestCase):
    """Which processes `reap` kills. The spare-cases matter more than the kill-case: this
    machine runs several tracks at once, and a wrong kill destroys someone's run."""

    def _classify(self, procs, locks):
        from gdxdev import retro
        calls = {"killed": []}
        real_procs, real_lock, real_names, real_kill = (
            retro.our_retro_processes, retro.read_lock,
            retro.instance_names, retro.kill_group)
        retro.our_retro_processes = lambda: procs
        retro.instance_names = lambda: list(locks)
        retro.read_lock = lambda inst: locks.get(inst.name)
        retro.kill_group = lambda pid, **kw: calls["killed"].append(pid) or True
        try:
            args = type("A", (), {"dry_run": False})()
            retro.do_reap(args, util.Out(quiet=True), {"track": "devcli"})
        finally:
            retro.our_retro_processes, retro.read_lock = real_procs, real_lock
            retro.instance_names, retro.kill_group = real_names, real_kill
        return calls["killed"]

    def test_reparented_to_init_is_killed(self):
        procs = [{"pid": 111, "ppid": 1, "etime": "50:00", "instance": "r1"}]
        self.assertEqual(self._classify(procs, {}), [111])

    def test_lock_naming_a_dead_driver_is_killed(self):
        procs = [{"pid": 222, "ppid": 5, "etime": "10:00", "instance": "r2"}]
        locks = {"r2": {"driver_pid": 999999, "retro_pid": 222}}
        self.assertEqual(self._classify(procs, locks), [222])

    def test_live_driver_is_spared(self):
        procs = [{"pid": 333, "ppid": 5, "etime": "01:00", "instance": "r3"}]
        locks = {"r3": {"driver_pid": os.getpid(), "retro_pid": 333}}
        self.assertEqual(self._classify(procs, locks), [])

    def test_no_lock_but_live_parent_is_spared(self):
        """The regression that would have killed other tracks: processes started before
        this fix have no lock file and perfectly live drivers. Absence of a lock is NOT
        evidence of an orphan."""
        procs = [{"pid": 444, "ppid": 5, "etime": "04:00", "instance": "fx2"}]
        self.assertEqual(self._classify(procs, {}), [])


class RetroDefaultInstanceTest(unittest.TestCase):
    def test_run_defaults_the_instance_to_the_unique_run_id(self):
        import inspect
        from gdxdev import cli, retro
        src = inspect.getsource(retro.do_run)
        self.assertIn('getattr(args, "instance", None) or rd.run_id', src)
        self.assertIn("acquire_instance(inst", src)
        # and the parser must not supply a fixed default that hides it
        ap = cli.build_parser()
        args = ap.parse_args(["retro", "run", "x.3dsx"])
        self.assertIsNone(args.instance,
                          "a fixed --instance default lets two agents share a private SD")


class SeedIniHygieneTest(unittest.TestCase):
    """A fresh instance must not inherit another agent's experiment. Measured: a stability
    sweep inherited `input_replay` from the shared Qt SD and every boot logged
    "[rec] replay open failed" for a replay it was never given."""

    SHARED = ("[debug]\nconsole = 1\ninput_replay = someones-run\nreplay_quit = 1\n"
              "fh_every = 1\ndet = 1\ngputrace = 1\ntrectbatch = 0\n"
              "quit_at_tick = 3400\ntmemfast = 1\ntrifast = 1\n"
              "\n[native]\ninterp = 1\nmachines = 1\nblendflush = 1\n"
              "\n[display]\nborder_mode = 2\n"
              "\n[stereo]\nenabled = 1\n"
              "\n[input]\ndeadzone = 18\n")

    def _strip(self, text):
        from gdxdev import retro
        with tempfile.NamedTemporaryFile("w", suffix=".ini", delete=False) as f:
            f.write(text)
            path = f.name
        try:
            return retro.strip_run_control(path), inifile.read(path)
        finally:
            os.unlink(path)

    def test_every_debug_and_native_key_is_dropped(self):
        """By SECTION, not by a key list. A key list has to be chased every time another
        track invents a lever — `trectbatch` was missed exactly that way."""
        removed, got = self._strip(self.SHARED)
        self.assertEqual(got.get("debug", {}), {})
        self.assertEqual(got.get("native", {}), {})
        for key in ("input_replay", "replay_quit", "fh_every", "det", "gputrace",
                    "trectbatch", "quit_at_tick", "tmemfast", "trifast"):
            self.assertIn("debug.%s" % key, removed)
        self.assertIn("native.interp", removed)

    def test_user_preferences_survive(self):
        _removed, got = self._strip(self.SHARED)
        self.assertEqual(got["display"]["border_mode"], "2")
        self.assertEqual(got["input"]["deadzone"], "18")
        self.assertEqual(got["stereo"]["enabled"], "1")

    def test_stripping_an_ini_with_no_levers_is_a_noop(self):
        removed, got = self._strip("[display]\nborder_mode = 1\n")
        self.assertEqual(removed, [])
        self.assertEqual(got["display"]["border_mode"], "1")

    def test_both_lanes_set_the_same_measurement_ini(self):
        """Nothing is inherited any more, so the retro lane must set what it relies on."""
        import inspect
        from gdxdev import emu, retro
        self.assertIn("MEASUREMENT_INI", inspect.getsource(retro.do_run))
        for key in (("debug", "filelog"), ("debug", "verbose")):
            self.assertIn(key, emu.MEASUREMENT_INI)

    def test_apt_signal_lines_are_not_crashes(self):
        """Azahar logs "with signal 00000001" on every healthy boot."""
        from gdxdev import retro
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write("applet_manager.cpp:CancelAndSendParameter: ... with signal 00000001\n")
            path = f.name
        try:
            self.assertEqual(retro._crash_signature(path), "")
        finally:
            os.unlink(path)


class ReplayCompletionTest(unittest.TestCase):
    """A guest core that dies MID-replay still leaves an advancing heartbeat and zero
    [fatal] lines. Reported 2026-09-17: an EXC_BAD_ACCESS at frame ~433, 8 s into a
    6000-frame replay, was reported "PASS heartbeat reached frame 432, 0 [fatal] lines"."""

    def _block(self, kind="gdxrec", shots=("a", "b"), last_tick=6000):
        return {"id": "r", "kind": kind, "sha256": "x", "events": 1, "path": None,
                "shots": list(shots), "last_tick": last_tick}

    def _beats(self, upto):
        return "".join("OutputDebugString: frame %d\n" % i for i in range(1, upto + 1))

    def test_gdxrec_without_end_receipt_is_incomplete(self):
        info = replay.completion(self._block(), "[fh] frame=1 tag=a\n[fh] frame=2 tag=b\n",
                                 max_frame=432, emu_alive_at_end=True)
        self.assertFalse(info["replay_complete"])
        self.assertIn("[rec] end", info["detail"])

    def test_gdxrec_with_end_and_all_shots_is_complete(self):
        log = "[fh] frame=1 tag=a\n[fh] frame=2 tag=b\n[rec] end tick=6000 lines=6001\n"
        info = replay.completion(self._block(), log, max_frame=6000, emu_alive_at_end=True)
        self.assertTrue(info["replay_complete"], info["detail"])
        self.assertEqual((info["shots_seen"], info["shots_expected"]), (2, 2))

    def test_missing_shots_are_named(self):
        log = "[fh] frame=1 tag=a\n[rec] end tick=6000 lines=6001\n"
        info = replay.completion(self._block(), log, max_frame=6000, emu_alive_at_end=True)
        self.assertFalse(info["replay_complete"])
        self.assertEqual(info["missing_shots"], ["b"])
        self.assertIn("b", info["detail"])

    def test_periodic_auto_tag_is_not_a_shot(self):
        log = ("[fh] frame=1 tag=auto\n" * 50) + "[rec] end tick=6000 lines=1\n"
        info = replay.completion(self._block(), log, max_frame=6000, emu_alive_at_end=True)
        self.assertEqual(info["shots_seen"], 0)
        self.assertFalse(info["replay_complete"])

    def test_autotest_receipt_also_counts_a_shot(self):
        log = ("[autotest] SHOT captured -> autotest/a_L.bmp bytes=1 frame=2\n"
               "[autotest] SHOT captured -> autotest/b_L.bmp bytes=1 frame=3\n"
               "[rec] end tick=6000 lines=1\n")
        info = replay.completion(self._block(), log, max_frame=6000, emu_alive_at_end=True)
        self.assertTrue(info["replay_complete"], info["detail"])

    def test_autoinput_short_of_the_last_tick_is_incomplete(self):
        b = self._block(kind="autoinput", shots=(), last_tick=21000)
        info = replay.completion(b, "", max_frame=432, emu_alive_at_end=True)
        self.assertFalse(info["replay_complete"])
        self.assertIn("21000", info["detail"])

    def test_autoinput_reaching_the_last_tick_is_complete(self):
        b = self._block(kind="autoinput", shots=("card",), last_tick=100)
        info = replay.completion(b, "[autotest] SHOT captured -> autotest/card_L.bmp\n",
                                 max_frame=120, emu_alive_at_end=True)
        self.assertTrue(info["replay_complete"], info["detail"])

    def test_emulator_dying_early_is_incomplete_even_with_everything_else(self):
        log = "[fh] frame=1 tag=a\n[fh] frame=2 tag=b\n"
        info = replay.completion(self._block(), log, max_frame=432, emu_alive_at_end=False)
        self.assertFalse(info["replay_complete"])
        self.assertIn("core died", info["detail"])

    def test_replay_quit_exit_is_legitimate(self):
        log = ("[fh] frame=1 tag=a\n[fh] frame=2 tag=b\n"
               "[rec] end tick=6000 lines=6001\n"
               "[oracle] quit requested: [rec] end with replay_quit=1\n")
        info = replay.completion(self._block(), log, max_frame=6000, emu_alive_at_end=False)
        self.assertTrue(info["replay_complete"], info["detail"])

    def test_no_replay_means_no_completion_requirement(self):
        info = replay.completion(None, "", max_frame=10, emu_alive_at_end=True)
        self.assertIsNone(info["replay_complete"])

    def test_judge_fails_the_reported_crash_scenario(self):
        """The regression itself: heartbeat to 432, no [fatal], replay armed, core dead."""
        from gdxdev import emu
        sd = ("[rec] replay armed rec/r.gdxrec frames=6001\n"
              "[fh] frame=1 tag=a\n")
        r = emu.judge(self._beats(432), sd, util.Out(quiet=True),
                      replay_kind="gdxrec", replay_block=self._block(),
                      emu_alive_at_end=False)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertIn("did not complete", r["detail"])
        self.assertEqual(r["shots_seen"], 1)
        self.assertEqual(r["shots_expected"], 2)

    def test_judge_passes_a_genuinely_complete_replay(self):
        from gdxdev import emu
        sd = ("[rec] replay armed rec/r.gdxrec frames=6001\n"
              "[fh] frame=1 tag=a\n[fh] frame=2 tag=b\n"
              "[rec] end tick=6000 lines=6001\n")
        r = emu.judge(self._beats(6000), sd, util.Out(quiet=True),
                      replay_kind="gdxrec", replay_block=self._block(),
                      emu_alive_at_end=True)
        self.assertEqual(r["verdict"], "PASS", r["detail"])
        self.assertTrue(r["replay_complete"])
        self.assertIn("replay complete (2/2 SHOTs)", r["detail"])

    def test_shot_labels_parse_real_files(self):
        gd = os.path.join(util.repo_root(), "replays", "race-mutecity-start.gdxrec")
        if os.path.exists(gd):
            labels = replay.shot_labels(gd, "gdxrec")
            self.assertIn("title", labels)
            self.assertNotIn("label", labels)      # the header comment is not a SHOT
            self.assertEqual(replay.last_tick(gd, "gdxrec"), 6000)
        ai = os.path.join(replay.replays_dir(), "storm2.txt")
        self.assertEqual(replay.shot_labels(ai, "autoinput"),
                         ["card", "drive1", "drive2", "drive3", "drive4", "drive5"])
        self.assertEqual(replay.last_tick(ai, "autoinput"), 21000)


class GuestExitTest(unittest.TestCase):
    """The EMULATOR outliving the GUEST is the normal case: a .3dsx that exits leaves
    Azahar up with a dead window, so `emu run` waited out the whole of --seconds after
    the run was already over (integration-verify-2, item 1)."""

    def test_atexit_receipt_means_the_guest_is_done(self):
        from gdxdev import emu
        self.assertEqual(
            emu.guest_exit_reason("[fatal-trace] atexit: process exiting via exit()\n"),
            "exited via exit()")

    def test_oracle_quit_means_the_guest_is_done(self):
        from gdxdev import emu
        self.assertEqual(
            emu.guest_exit_reason("[oracle] quit requested: [rec] end with replay_quit=1\n"),
            "oracle quit")

    def test_a_healthy_running_log_is_not_an_exit(self):
        from gdxdev import emu
        self.assertIsNone(emu.guest_exit_reason(
            "[watchdog] beat=3 frame=180(+60) stage=5\n[prof] frame=64 n=64 br=1.0\n"))

    def test_the_loop_polls_faster_than_it_snapshots(self):
        from gdxdev import emu
        self.assertLess(emu.POLL_S, emu.SNAPSHOT_S)

    def test_the_wait_loop_checks_the_guest_not_just_the_process(self):
        import inspect
        from gdxdev import emu
        src = inspect.getsource(emu.do_run)
        self.assertIn("guest_exit_reason(", src)


class QuitAtTickTest(unittest.TestCase):
    """A replay cut short deliberately by [debug] quit_at_tick is COMPLETE when the tick
    was reached — same as replay_quit (integration-verify-2, item 2)."""

    INIT = "[fh] init enabled=1 every=0 start=0 eye=L scale=0 quit_at_tick=1000 build=b\n"
    QUIT = "[oracle] quit requested: [debug] quit_at_tick reached\n"

    def _block(self):
        return {"id": "r", "kind": "gdxrec", "sha256": "x", "events": 1, "path": None,
                "shots": ["early", "late"], "last_tick": 6000,
                "schedule": [(100, "early"), (5000, "late")]}

    def test_quit_at_tick_is_complete_without_the_end_receipt(self):
        log = self.INIT + "[fh] frame=1 tag=early\n" + self.QUIT
        info = replay.completion(self._block(), log, max_frame=1000,
                                 emu_alive_at_end=False)
        self.assertTrue(info["replay_complete"], info["detail"])
        self.assertEqual(info["quit_at_tick"], 1000)

    def test_shots_scheduled_after_the_cut_are_not_expected(self):
        log = self.INIT + "[fh] frame=1 tag=early\n" + self.QUIT
        info = replay.completion(self._block(), log, max_frame=1000,
                                 emu_alive_at_end=False)
        self.assertEqual(info["shots_expected"], 1)      # "late" is at tick 5000
        self.assertEqual(info["missing_shots"], [])

    def test_a_shot_before_the_cut_is_still_required(self):
        log = self.INIT + self.QUIT          # "early" never captured
        info = replay.completion(self._block(), log, max_frame=1000,
                                 emu_alive_at_end=False)
        self.assertFalse(info["replay_complete"])
        self.assertEqual(info["missing_shots"], ["early"])

    def test_without_the_quit_receipt_the_end_receipt_is_still_required(self):
        log = self.INIT + "[fh] frame=1 tag=early\n[fh] frame=2 tag=late\n"
        info = replay.completion(self._block(), log, max_frame=6000,
                                 emu_alive_at_end=True)
        self.assertFalse(info["replay_complete"])
        self.assertIn("[rec] end", info["detail"])

    def test_quit_at_tick_parsing(self):
        self.assertEqual(replay.quit_at_tick(self.INIT + self.QUIT), 1000)
        self.assertIsNone(replay.quit_at_tick(self.INIT))   # configured but never reached

    def test_shot_schedule_carries_ticks(self):
        ai = os.path.join(replay.replays_dir(), "storm2.txt")
        sched = replay.shot_schedule(ai, "autoinput")
        self.assertEqual(sched[0], (3400, "card"))
        self.assertEqual([l for _t, l in sched], replay.shot_labels(ai, "autoinput"))


class ErrorCensusTest(unittest.TestCase):
    """ONE classifier for every path. emu.judge used to re-derive its own fatal count,
    which is how `[fatal-trace] atexit` kept failing clean runs after prof.py was fixed
    (integration-verify-2, item 3)."""

    CRASH = "[fatal] assert failed: x (a.c:1 in f)\n"
    ATEXIT = "[fatal-trace] atexit: process exiting via exit()\n"

    def test_one_crash_in_both_sinks_counts_once(self):
        """logFatal (main_3ds.cpp:412) writes the SAME line to the Azahar log AND the SD
        filelog, so summing the sinks reported 2 fatals for one assert."""
        c = prof.error_census(self.CRASH, self.CRASH)
        self.assertEqual(c["fatals"], 1)

    def test_a_crash_in_only_one_sink_still_counts(self):
        self.assertEqual(prof.error_census(self.CRASH, "")["fatals"], 1)

    def test_two_distinct_crashes_count_twice(self):
        both = self.CRASH + "[fatal] second one\n"
        self.assertEqual(prof.error_census(both, both)["fatals"], 2)

    def test_atexit_is_benign_in_the_census(self):
        c = prof.error_census(self.ATEXIT, self.ATEXIT)
        self.assertEqual(c["fatals"], 0)
        self.assertEqual(c["benign"], 2)

    def test_judge_and_prof_agree_through_the_shared_classifier(self):
        from gdxdev import emu
        beats = "".join("OutputDebugString: frame %d\n" % i for i in range(1, 5))
        r = emu.judge(beats + self.ATEXIT, self.ATEXIT, util.Out(quiet=True))
        self.assertEqual(r["verdict"], "PASS")
        self.assertEqual(r["fatals"], 0)
        r2 = emu.judge(beats + self.CRASH, self.CRASH, util.Out(quiet=True))
        self.assertEqual(r2["fatals"], 1)
        self.assertIn("1 [fatal] line(s)", r2["detail"])

    def test_judge_uses_the_census_rather_than_its_own_sum(self):
        import inspect
        from gdxdev import emu
        self.assertIn("prof.error_census(", inspect.getsource(emu.judge))

    def test_is_benign_is_the_single_predicate(self):
        self.assertTrue(prof.is_benign("x [fatal-trace] atexit: process exiting"))
        self.assertFalse(prof.is_benign("[fatal-trace] pc=0x1 lr=0x2 in Machine_Draw"))


class ResultLineTest(unittest.TestCase):
    def test_result_line_carries_sha256_not_just_the_build_id(self):
        """GDX3DS_BUILD_ID is configure-time and can name the wrong commit after a
        merge-then-rebuild, so the artifact hash is what a report must quote."""
        from gdxdev import emu
        m = {"build": {"id": "native/devcli@deadbeef", "sha256": "a" * 64},
             "replay": {"id": "r", "kind": "gdxrec"}, "wall_s": 8.2}
        r = {"verdict": "FAIL", "max_frame": 432, "fatals": 0, "replay_complete": False,
             "shots_seen": 1, "shots_expected": 8}
        line = emu.result_line(m, r)
        self.assertTrue(line.startswith("RESULT FAIL"))
        self.assertIn("sha256=" + "a" * 16, line)
        self.assertIn("wall_s=8.2", line)
        self.assertIn("complete=False", line)
        self.assertIn("shots=1/8", line)


class ReapPruneTest(unittest.TestCase):
    def setUp(self):
        from gdxdev import retro
        self.retro = retro
        self.tmp = tempfile.mkdtemp()
        self._root = retro.INSTANCE_ROOT
        retro.INSTANCE_ROOT = self.tmp

    def tearDown(self):
        self.retro.INSTANCE_ROOT = self._root
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _mk(self, name, age_days):
        d = os.path.join(self.tmp, name)
        os.makedirs(d, exist_ok=True)
        f = os.path.join(d, "retroarch.cfg")
        open(f, "w").write("x\n")
        old = time.time() - age_days * 86400
        os.utime(f, (old, old))
        os.utime(d, (old, old))
        return d

    def _prune(self, days=2, dry=False, claimed=None, live=()):
        real = self.retro.our_retro_processes
        self.retro.our_retro_processes = lambda: [{"instance": n} for n in live]
        try:
            args = type("A", (), {"days": days, "dry_run": dry})()
            return self.retro._prune_instances(args, util.Out(quiet=True), claimed or {})
        finally:
            self.retro.our_retro_processes = real

    def test_old_idle_dirs_are_pruned(self):
        self._mk("old", 5)
        self._mk("fresh", 0)
        self.assertEqual(self._prune(), ["old"])
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "old")))
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "fresh")))

    def test_default_threshold_is_two_days(self):
        self._mk("d1", 1)
        self._mk("d3", 3)
        self.assertEqual(self._prune(), ["d3"])

    def test_a_running_instance_is_never_pruned(self):
        self._mk("busy", 9)
        self.assertEqual(self._prune(live=["busy"]), [])
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "busy")))

    def test_a_live_driver_lock_protects_an_old_dir(self):
        self._mk("held", 9)
        claimed = {"held": {"driver_pid": os.getpid()}}
        self.assertEqual(self._prune(claimed=claimed), [])

    def test_a_future_dated_asset_does_not_block_pruning(self):
        """seed_sd copies the user's dspfirm.cdc, which is dated 2035 on this machine.
        A newest-file-in-tree heuristic therefore put every instance nine years in the
        future and pruned nothing, ever."""
        d = self._mk("old", 9)
        sd = os.path.join(d, "saves", "Azahar", "Azahar", "sdmc", "3ds")
        os.makedirs(sd, exist_ok=True)
        f = os.path.join(sd, "dspfirm.cdc")
        open(f, "w").write("x")
        future = time.time() + 9 * 365 * 86400
        os.utime(f, (future, future))
        self.assertEqual(self._prune(), ["old"])

    def test_last_used_reads_a_file_gdx_dev_writes(self):
        d = self._mk("x", 3)
        inst = self.retro.Instance("x")
        self.assertAlmostEqual(self.retro._last_used(inst),
                               os.path.getmtime(os.path.join(d, "retroarch.cfg")),
                               places=3)

    def test_dry_run_deletes_nothing(self):
        self._mk("old", 9)
        self.assertEqual(self._prune(dry=True), ["old"])
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "old")))


class DelegateTest(unittest.TestCase):
    def test_missing_peer_exits_7(self):
        from gdxdev import delegate
        if os.path.exists(delegate.peer_path("framediff.py")):
            self.skipTest("oracle track has merged; the absence path is untestable here")
        with self.assertRaises(GdxError) as cm:
            delegate.require("framediff.py")
        self.assertEqual(cm.exception.code, util.EXIT_NODELEGATE)
        self.assertIn("oracle", str(cm.exception))


class StdlibOnlyTest(unittest.TestCase):
    def test_no_third_party_imports(self):
        """05-devcli-plan.md §6: stdlib-only drift. Import with -S (no site-packages)."""
        import subprocess
        pkg = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
        code = ("import sys; sys.path.insert(0, %r); "
                "import gdxdev.cli, gdxdev.emu, gdxdev.prof, gdxdev.sd, gdxdev.retro"
                % pkg)
        p = subprocess.run([sys.executable, "-S", "-c", code],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.assertEqual(p.returncode, 0, p.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
