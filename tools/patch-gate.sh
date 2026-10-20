#!/usr/bin/env bash
# patch-gate.sh — verify/apply the decomp patch stack, and say WHICH patches are missing.
#
#   usage: patch-gate.sh <repo-root> <patch-dir> <sub:patch> [<sub:patch> ...]
#   env:   SKIP_PATCH_GATE=1   downgrade a drift FAIL to a warning and force-apply
#                              whatever individual patch still applies
#   exit:  0 = stack is applied (and the roundtrip proves it), 1 = drift
#   stdout: human lines, plus one machine line:  PATCHGATE: already=N applied=M
#
# Extracted from tools/ci-3ds.sh so the gate is unit-testable against a throwaway
# git repo (tools/dev/tests/test_gdxdev.py::PatchGateTest). ci-3ds.sh still owns
# PATCH_LIST — this script has no knowledge of which patches exist.
#
# ---------------------------------------------------------------------------
# The gate is verified PER SUBMODULE against the FULL ordered stack, not
# per-patch. Order-dependent stacks are the reason: patches later in a series are
# generated against the tree the earlier ones produce, so their hunks are
# addressed AT line numbers that only exist post-application. A per-patch check
# against a fully patched tree then matches neither forward NOR reverse, and the
# old gate false-aborted on a correctly patched checkout. (The worst offenders
# were in the libultraship series, now vendored; the decomp stack keeps the same
# whole-stack treatment because the property is not specific to it.)
#
# Why not a combined `git apply --check`? `--check` with several patch files
# validates every file against the SAME starting tree, so an order-dependent
# stack fails the combined check even when it applies fine. A real `git apply`
# (no --check), by contrast, applies the listed patches SEQUENTIALLY, which is
# what an order-dependent stack needs. We exploit exactly that:
#   * combined forward apply succeeds       -> clean tree, now patched. Done.
#   * else combined reverse apply succeeds   -> tree WAS fully applied; we then
#     re-apply forward to restore it (a proven forward<->reverse roundtrip).
#   * else                                   -> see MISSING SUFFIX below, then
#     real drift, FAIL.
# Sequential real applies are immune to the inter-patch context overlap that
# broke the per-patch check.
#
# ATOMICITY, which the old comment here got wrong. `git apply` is atomic within
# ONE patch file, not across a LIST of them: given `git apply --reverse p3 p2 p1`
# it can revert p3, fail on p2, and leave the tree half-reverted — the probe then
# reports "no" having already damaged the tree it was only supposed to inspect.
# On a drifted tree that silently deleted a patch's changes. Every probe below
# therefore snapshots the submodule's working-tree diff first and restores it
# byte-for-byte when the probe fails, so a probe is a question and never an edit.
# (Found by PatchGateTest::test_drift_leaves_the_tree_as_it_found_it, not by
# reading — which is the whole reason the gate was made testable.)
#
# MISSING SUFFIX (NEXT.md B50). The two branches above are exhaustive only for a
# tree that is fully applied or fully clean. A tree with the first M of N applied
# — the NORMAL state immediately after a merge that ADDS patches to the end of
# the stack — matches neither: forward-all dies on patch 0 (already applied),
# reverse-all dies on patch N-1 (never applied). The old gate then printed
# "partial application / conflicting local edits" and pointed at `git -C decomp
# diff`, which shows the healthy drift of a CORRECTLY patched tree. That
# misdirection cost a full archaeology pass across every worktree before the real
# cause (two never-applied new patches) surfaced.
#
# So before failing we look for the suffix. For every suffix of the ordered
# stack, shortest first, try a real sequential forward apply of just that suffix:
# it is atomic, so a suffix that does not fit changes nothing. The shortest one
# that lands is exactly the set of unapplied patches, and we name them. The
# full-stack roundtrip is then run as the correctness check — it is NOT replaced
# by the suffix probe, and if it fails the suffix is reversed back out and the
# gate fails with the per-patch classification below. A suffix that applies by
# coincidence on a genuinely drifted tree therefore cannot be mistaken for a
# repair.
#
# Only on real drift do we classify per patch, and the report says the
# classification is advisory: against a partially applied, order-dependent stack
# a patch can read as "broken" purely because an earlier one is missing. The
# first UNAPPLIED entry is the one to trust.
set -euo pipefail

if [ "$#" -lt 3 ]; then
    echo "usage: patch-gate.sh <repo-root> <patch-dir> <sub:patch> [...]" >&2
    exit 2
fi
ROOT="$1"; shift
PATCH_DIR="$1"; shift
ENTRIES=("$@")

SKIP_PATCH_GATE="${SKIP_PATCH_GATE:-0}"
PATCHES_OK=1
APPLIED_NOW=0
ALREADY=0

# Distinct submodules in first-seen order (bash 3.2: no associative arrays).
SUBMODULES=()
for entry in "${ENTRIES[@]}"; do
    sub="${entry%%:*}"
    seen=0
    for s in "${SUBMODULES[@]:-}"; do
        if [ "$s" = "$sub" ]; then seen=1; break; fi
    done
    if [ "$seen" -eq 0 ]; then SUBMODULES+=("$sub"); fi
done

# Ordered absolute patch paths for one submodule, on stdout (one per line).
patches_for_sub() { # sub
    local want="$1" entry
    for entry in "${ENTRIES[@]}"; do
        if [ "${entry%%:*}" = "$want" ]; then
            printf '%s\n' "${PATCH_DIR}/${entry#*:}"
        fi
    done
}

# ---------------------------------------------------------------- snapshots --
# The snapshot is the submodule's working-tree diff against its HEAD. That is an
# exact record for a stack of pure content patches; it does NOT record files a
# patch creates or deletes, so we refuse to guess if one ever does.
snap_take() { # sub file
    git -C "${ROOT}/$1" diff --binary > "$2"
}

snap_restore() { # sub file
    git -C "${ROOT}/$1" checkout -- . >/dev/null 2>&1 || true
    if [ -s "$2" ]; then
        if ! git -C "${ROOT}/$1" apply --binary "$2" >/dev/null 2>&1; then
            echo "FATAL: could not restore ${1} to the state the gate found it in." >&2
            echo "       Its snapshot is at $2 — apply it by hand before rebuilding." >&2
            exit 3
        fi
    fi
}

# A probe, not an edit: apply and keep, or restore and say no.
apply_atomic() { # sub snapfile [--reverse] patch...
    local sub="$1" snap="$2"; shift 2
    if git -C "${ROOT}/${sub}" apply "$@" >/dev/null 2>&1; then
        return 0
    fi
    snap_restore "$sub" "$snap"
    return 1
}

# One patch's state against the CURRENT tree. Advisory on an order-dependent
# stack — see the MISSING SUFFIX note above.
classify_patch() { # sub patch-path
    local sub="$1" p="$2" fwd=1 rev=1
    git -C "${ROOT}/${sub}" apply --check "$p" >/dev/null 2>&1 || fwd=0
    git -C "${ROOT}/${sub}" apply --reverse --check "$p" >/dev/null 2>&1 || rev=0
    if [ "$rev" -eq 1 ] && [ "$fwd" -eq 1 ]; then
        echo "ambiguous"     # empty/context-free patch: both directions fit
    elif [ "$rev" -eq 1 ]; then
        echo "applied"
    elif [ "$fwd" -eq 1 ]; then
        echo "unapplied"
    else
        echo "broken"
    fi
}

# The correctness check, unchanged in what it proves: reverse the WHOLE ordered
# stack, then put it back. Succeeds only on a tree that is exactly and fully
# patched. What changed is that both halves go through apply_atomic, so a probe
# that says no has also left the tree exactly as it found it.
roundtrip_ok() { # sub snapfile fwd...
    local sub="$1" snap="$2"
    shift 2
    local -a fwd=("$@")
    local -a rev=()
    local idx=$(( ${#fwd[@]} - 1 ))
    while [ "$idx" -ge 0 ]; do rev+=("${fwd[$idx]}"); idx=$((idx - 1)); done
    if apply_atomic "$sub" "$snap" --reverse "${rev[@]}"; then
        if apply_atomic "$sub" "$snap" "${fwd[@]}"; then
            return 0
        fi
        # Reversed cleanly but would not re-apply. apply_atomic has already put the
        # tree back; this is drift that only the forward direction can see.
        return 1
    fi
    return 1
}

# Advisor fix (native-review-2026-10-15.md follow-up): a rapid reset -> manual-apply ->
# reset -> rebuild cycle (regenerating a patch's CONTENT under the same filename, then
# letting ci-3ds.sh reapply it) can land a patched file with the SAME mtime, to the
# second, as the stale .o ninja/ccache already built from its earlier content -- ninja's
# dependency check is mtime-based and sees "not newer", so it serves the stale object
# despite `git apply` having written the new text. Bumping every patched file's mtime to
# "now" right after confirming the stack is applied costs nothing on the normal path (the
# mtimes are about to be current anyway) and removes the one-second race on the rapid
# edit/rebuild path every agent on this stack was hitting by hand. The sentinel is the
# submodule's own git state: whatever differs from HEAD was put there by the patch stack
# (or is a submodule-local edit the gate has already decided is fine to touch).
touch_patched_files() { # sub
    local sub="$1"
    git -C "${ROOT}/${sub}" diff --name-only HEAD 2>/dev/null | xargs -r -I{} touch -- "${ROOT}/${sub}/{}"
}

report_drift() { # sub  fwd...
    local sub="$1"; shift
    local -a fwd=("$@")
    local p state base
    local -a unapplied=() broken=()
    echo "FAIL: ${sub} patch stack neither applies nor reverses cleanly, and no suffix of" >&2
    echo "      the stack explains it either." >&2
    echo "      Per-patch state against the tree as it stands (ADVISORY: the stack is" >&2
    echo "      order-dependent, so a patch that IS applied can still read 'broken' purely" >&2
    echo "      because a neighbour is missing. Trust the 'unapplied' lines first):" >&2
    for p in "${fwd[@]}"; do
        state="$(classify_patch "$sub" "$p")"
        base="$(basename "$p")"
        printf '        %-12s %s\n' "$state" "$base" >&2
        if [ "$state" = "unapplied" ]; then unapplied+=("$p"); fi
        if [ "$state" = "broken" ];    then broken+=("$p"); fi
    done
    echo "      ${#unapplied[@]} of ${#fwd[@]} apply cleanly right now, i.e. were never applied." >&2
    if [ "${#unapplied[@]}" -gt 0 ]; then
        echo "      To apply just those (and nothing else):" >&2
        for p in "${unapplied[@]}"; do
            echo "        git -C ${sub} apply ${p#${ROOT}/}" >&2
        done
    fi
    if [ "${#broken[@]}" -gt 0 ]; then
        echo "      ${#broken[@]} read as 'broken' (neither direction fits alone). On an" >&2
        echo "      order-dependent stack that is usually a consequence of the lines above," >&2
        echo "      not damage to the patch itself — re-run the gate after applying them." >&2
    fi
    echo "      If instead the submodule has local edits of its own:" >&2
    echo "        git -C ${sub} diff" >&2
    echo "      Or re-run with SKIP_PATCH_GATE=1 to force past a known-dirty tree." >&2
}

# A stack that creates or deletes files cannot be snapshotted by a working-tree
# diff, so say so rather than restoring something subtly wrong later.
for entry in "${ENTRIES[@]}"; do
    f="${PATCH_DIR}/${entry#*:}"
    if [ -f "$f" ] && grep -qE '^(new file mode|deleted file mode|rename from) ' "$f"; then
        echo "WARN: $(basename "$f") creates/deletes/renames a file. The gate's probe" >&2
        echo "      snapshot is a working-tree diff and does not cover that; a failed" >&2
        echo "      probe may not restore the tree exactly." >&2
    fi
done

for sub in "${SUBMODULES[@]}"; do
    fwd=()
    while IFS= read -r line; do fwd+=("$line"); done < <(patches_for_sub "$sub")
    n=${#fwd[@]}

    SNAP="$(mktemp "${TMPDIR:-/tmp}/gdx-patchgate-snap.XXXXXX")"
    snap_take "$sub" "$SNAP"

    # 1. Clean tree? A real sequential forward apply lands the whole stack.
    if apply_atomic "$sub" "$SNAP" "${fwd[@]}"; then
        echo "applied:         ${sub} (${n} patches, clean tree)"
        APPLIED_NOW=$((APPLIED_NOW + n))
        touch_patched_files "$sub"
        rm -f "$SNAP"; continue
    fi

    # 2. Already fully applied? Proven by the full-stack roundtrip.
    if roundtrip_ok "$sub" "$SNAP" "${fwd[@]}"; then
        echo "already applied: ${sub} (${n} patches, full-stack roundtrip)"
        ALREADY=$((ALREADY + n))
        rm -f "$SNAP"; continue
    fi

    # 3. First M of N applied, the rest never applied (B50). Shortest suffix first.
    #    A SHORT suffix can land by coincidence — on the real decomp stack, with the
    #    last TWO patches missing the last one still applies on its own — so a suffix
    #    is only a repair once the full-stack roundtrip agrees. When it does not, the
    #    tree is restored and the probe continues with a longer suffix; it does NOT
    #    give up, which is the difference between diagnosing "1 missing" and the true
    #    "2 missing".
    repaired=0
    k=1
    while [ "$k" -lt "$n" ]; do
        suffix=("${fwd[@]:$((n - k))}")
        if apply_atomic "$sub" "$SNAP" "${suffix[@]}"; then
            if roundtrip_ok "$sub" "$SNAP" "${fwd[@]}"; then
                echo "unapplied:       ${sub} — ${k} of ${n} patches were never applied:"
                for p in "${suffix[@]}"; do echo "                   $(basename "$p")"; done
                echo "applied (missing tail): ${sub} (${k} applied now, all ${n} verified by roundtrip)"
                APPLIED_NOW=$((APPLIED_NOW + k))
                ALREADY=$((ALREADY + n - k))
                touch_patched_files "$sub"
                repaired=1
                break
            fi
            # roundtrip_ok restored to the snapshot on the way out; the suffix that
            # fitted is gone again, so the next probe starts where this one did.
            snap_restore "$sub" "$SNAP"
        fi
        k=$((k + 1))
    done
    if [ "$repaired" -eq 1 ]; then rm -f "$SNAP"; continue; fi

    # 4. Real drift.
    if [ "$SKIP_PATCH_GATE" = "1" ]; then
        echo "WARN: ${sub} stack neither applies nor reverses as a whole." >&2
        echo "      SKIP_PATCH_GATE=1 set — force-applying still-applicable patches." >&2
        for p in "${fwd[@]}"; do
            if git -C "${ROOT}/${sub}" apply --check "$p" >/dev/null 2>&1; then
                git -C "${ROOT}/${sub}" apply "$p"
                echo "applied (forced): ${sub} <- $(basename "$p")"
                APPLIED_NOW=$((APPLIED_NOW + 1))
                touch_patched_files "$sub"
            else
                echo "skipped (already/blocked): ${sub} <- $(basename "$p")" >&2
            fi
        done
    else
        report_drift "$sub" "${fwd[@]}"
        PATCHES_OK=0
    fi
    rm -f "$SNAP"
done

echo "PATCHGATE: already=${ALREADY} applied=${APPLIED_NOW}"
[ "$PATCHES_OK" -eq 1 ]
