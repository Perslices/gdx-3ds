#!/usr/bin/env bash
# gdx-refrun — run one already-staged G-Diffuser run directory headlessly inside the container.
#
# /run is the staged run dir (exe + o2r + cfg + script), bind-mounted read-write so the autotest
# BMPs land on the host. Everything about WHAT to run comes from the environment
# tools/dev/refrun.py sets; this script only owns the X server and the exit code.
#
#   docker run --rm -v <rundir>:/run -e GDX_INPUT_SCRIPT=/run/script.txt gdx-ref gdx-refrun
set -euo pipefail

RUN=${GDX_REF_RUNDIR:-/run}
SCREEN=${GDX_REF_SCREEN:-640x480x24}
EXE=${GDX_REF_EXE:-./G-Diffuser}

cd "$RUN"
mkdir -p autotest

# NOT `exec`: that makes xvfb-run PID 1, where it hangs after the game exits instead of tearing
# down Xvfb and returning — a run that had already written its BMP looked like a 300 s timeout.
# Keeping bash as PID 1 gives the container a process that reaps and a real exit code.
#
# stdbuf -oL, because the port writes its whole boot log through stdio: into a pipe (which is
# what `docker run` gives it) that log is block-buffered and a killed run loses every line of it,
# which is exactly when the log is worth having.
#
# -a picks a free display; -s passes the screen geometry. The window itself is sized by
# gdiffuser.cfg.json (Window.Width/Height), not by the X screen — the screen only has to be at
# least as large.
set +e
stdbuf -oL -eL xvfb-run -a -s "-screen 0 $SCREEN -nolisten tcp" "$EXE"
rc=$?
set -e
echo "[gdx-refrun] $EXE exited rc=$rc"
exit "$rc"
