/* port/3ds/gfx/n64/gdx_nrr_menu.inc -- nimenu [native] ni_menu: the 2D screen runner (nointerp S2).
 * docs/research/native/nointerp-plan.md §2.1-2.6, docs/research/native/nimenu-progress.md.
 *
 * Included at the end of gdx_nrr_hud.cpp (it shares that TU's private types: NrrHud, NrrProg, the op
 * kinds); its own code lives here.
 *
 * WHAT RUNS HERE. With [native] ni_menu != 0 the game-side capture compacts every non-race mode's draw
 * (gdx3ds_menu_capture.c, kind MENU): the runs of RDP-state / texture / rectangle words sit in the
 * per-GfxPool record behind 0x5E markers, exactly as the race HUD's runs do. HudReplayRunOne hands a
 * MENU run to NrrMenuHotRun, which executes it through the NRR HUD's program compiler -- the run's
 * PROGRAM (ST masked register stores, LD virtual loads, RT / FL rectangles from memoised state blocks
 * drawn through the NRR core, SL compiled setup lists, EX the per-word decoder), keyed on the run's
 * skeleton and the entry registers it reads, each op re-checking its own word. Per-frame operand
 * changes (a fading prim / env colour: COL ops take any colour; a moved rectangle: RT / FL ops check
 * only the opcode and tile, the corners come from the live words; a scissor: late-bound variants)
 * keep their program: nothing is recompiled for them. The exactness argument is the HUD's: every op
 * makes the stock handler's register writes, every rectangle the stock floats (nrr_hud=2 / ni_menu=2
 * check both).
 *
 * WHY A SEPARATE RUNNER. The race HUD phase is at the 16 KB L1-I limit (core + NrrHudHot* + citro3d =
 * 16,164 B). A menu frame never runs a race HUD run, so the menu phase has its own hot set: the loop
 * below (NrrMenuHot*: the program lookup and op dispatch, inlined here, without the race HUD's run
 * replay `nrr_hud_rr`) plus the out-of-line op bodies it shares with the HUD (NrrHudHotRect / Load /
 * Reserve / Sl / ..., listed under `menu` in tools/native/nrr_hot.list). Nothing is added to NrrHudHot*.
 *
 * Levers: [native] ni_menu 0 off (default) / 1 on / 2 verify (every menu run executes stock and the
 * compiled ops are checked against it: `[nimenu] ... vfy ... bad=` must be 0). Needs the NRR HUD
 * ([native] nrr=1 nrr_hud=1); without it the MENU runs replay through the walker's handlers
 * (HudReplayRunPlain), which is the stock execution. */

namespace gdxn {

/* A MENU-kind run. Returns false when the NRR cannot take it (the caller replays it stock).
 *
 * VOLATILE RUNS. A run whose words change every frame (the machine settings' and machine select's
 * animated elements) never finds a stored program: the per-word decoder runs it every frame and pays
 * the compile of every new register word -- measured ~2x the stock walk of the same words. Per run
 * ordinal in the task (mNiMenuVol), a run that took the decoder kNiVolTrip times in a row replays
 * stock (the walker's handlers: what the screen cost without the lever) and is offered to the decoder
 * again every kNiVolRetry tasks (a screen that settles after a slide-in gets its programs back within
 * ~16 frames; a run that keeps changing pays one decode in 17). A run that finds its program resets the
 * count. ni_menu=2 verifies every run. */
bool Interp::NrrMenuHotRun(const void* runWords, uint32_t n) {
    if ((mNrr == nullptr && !NrrEnsure()) || gdx3ds_replay_interp != nullptr || mVerifyActive || n == 0) {
        return false;
    }
    uint8_t& vol = mNiMenuVol[mNiMenuOrd++ & (kNiVolSlots - 1u)];
    if (GDX_UNLIKELY(vol >= kNiVolTrip) && mNiMenu == 1) {
        vol = vol >= kNiVolTrip + kNiVolRetry ? static_cast<uint8_t>(kNiVolTrip - 1u) : static_cast<uint8_t>(vol + 1u);
        if (vol != kNiVolTrip - 1u) {
            mNiMenuWin.vol++;
            return false; /* volatile: the stock replay (the caller) */
        }
    }
    const GdxHudWord* w = static_cast<const GdxHudWord*>(runWords);
    mNiMenuWin.runs++;
    mNiMenuWin.words += n;
    mNiMenuRun = true;
    bool prog;
    if (GDX_UNLIKELY(mNiMenu == 2 || mNrr->clock != 0)) {
        prog = NrrMenuRunCold(w, n);
    } else {
        prog = NrrMenuHotBody(w, n);
    }
    mNiMenuRun = false;
    vol = prog ? 0u : static_cast<uint8_t>(vol + (vol < kNiVolTrip ? 1u : 0u));
    return true;
}

/* NrrHudHotBody without the race HUD's run replay (nrr_hud_rr): the program, or the per-word decoder
 * that records one. */
inline __attribute__((always_inline)) bool Interp::NrrMenuHotBody(const GdxHudWord* w, uint32_t n) {
    NrrHud& N = *mNrr;
    N.w.runs++;
    N.w.words += n;
    N.runWords = w;
    N.runN = n;
    N.drew = false;
    if (GDX_UNLIKELY(N.atlasEpoch != *N.atlasPtr || N.envSeq != N.seq || N.envEgm != mRsp.extra_geometry_mode)) {
        NrrRunEpochs();
    }
    if (GDX_UNLIKELY(mBufNumTris != 0) && (mNrrLate & kLateFlush) != 0 && mTexOv == nullptr) {
        NrrLateFlush();
    }
    const NrrProg* p = (mBufNumTris == 0 && mTexOv == nullptr) ? NrrHudHotProg(w, n) : nullptr;
    if (p != nullptr) {
        mNiMenuWin.prog++;
        NrrHudHotExec(*p, w, n, N.pool + p->ops, 0);
    } else {
        NrrMenuDec(w, n);
    }
    if (N.drew) {
        mRdp.viewport_or_scissor_changed = true; /* == DrawRectangle's exit: the 3D viewport is re-checked */
    }
    return p != nullptr;
}

/* No program fits: the per-word decoder runs the run and records its program (cold). */
GDX_COLD void Interp::NrrMenuDec(const GdxHudWord* w, uint32_t n) {
    mNiMenuWin.dec++;
    NrrDecRun(w, n);
}

/* ni_menu=2 (verify) or a diagnostic clock: the run executes stock and every compiled op is checked
 * (NrrVerifyRun, the nrr_hud=2 machinery). */
GDX_COLD bool Interp::NrrMenuRunCold(const GdxHudWord* w, uint32_t n) {
    NrrHud& N = *mNrr;
    if (mNiMenu == 2) {
        NrrVerifyRun(w, n);
        return true;
    }
    const uint64_t t0 = static_cast<uint64_t>(gdx_ninterp_now());
    const bool prog = NrrMenuHotBody(w, n);
    if (N.clock == -1) {
        NrrHudHotClose();
    }
    N.w.ticks += static_cast<uint64_t>(gdx_ninterp_now()) - t0;
    return prog;
}

/* A LOADTILE in a MENU run (the title's tiled images): the load stays real (the per-word decoder ran
 * its handler), and the TMEM words it wrote get an identity -- the SETTIMG token and the LOADTILE
 * words, which fix the bytes DpLoadTile mirrors (source pointer, image width, size, the window) --
 * so the rectangles that sample it draw from a state block instead of the stock way (and do not
 * leave an interpreter batch open for every later run). The range is DpLoadTile's: tile_height rows
 * of the 8-byte-rounded row size from the load tile's TMEM address. A segmented operand gets none
 * (its base moves with the segment table): those rectangles stay stock, as before. */
GDX_COLD void Interp::NrrNoteLoadTile(const Cmd& c) {
    const auto& tl = mRdp.texture_to_load;
    if (tl.cmd.w1hi != kHostTag || tl.cmd.w1 == 0) {
        return;
    }
    const uint32_t uls = C0(c.w0, 12, 12), ult = C0(c.w0, 0, 12);
    const uint32_t lrs = C1(c.w1, 12, 12), lrt = C1(c.w1, 0, 12);
    if (lrs < uls || lrt < ult) {
        return;
    }
    const uint32_t shift = tl.siz == IM_SIZ_16b ? 1u : tl.siz == IM_SIZ_32b ? 2u : 0u;
    const uint32_t rowBytes = (((lrs - uls) >> 2) + 1u) << shift;
    const uint32_t stride = (rowBytes + 7u) & ~7u;
    const uint32_t rows = ((lrt - ult) >> 2) + 1u;
    const uint32_t lo = mRdp.texture_tile[C1(c.w1, 24, 3)].tmem_index;
    if (lo >= 512u) {
        return;
    }
    uint32_t hi = lo + (rows * stride + 7u) / 8u;
    hi = hi > 512u ? 512u : hi;
    const uint32_t r[4] = { tl.cmd.w0, tl.cmd.w1, c.w0, c.w1 };
    NrrHudHotSlot(lo, hi, r);
    mNiMenuWin.tileIds++;
}

/* [nimenu] receipt (every 64 tasks, before [nrrhud] reads and clears the decoder's window): the game
 * mode at the receipt, MENU runs / words per task, runs that took a stored program, runs the per-word
 * decoder ran (no program for the entry state: a first sight or a changed word), runs replayed stock
 * (the NRR refused), and the decoder's fallbacks in the window (stock rectangles, words through Exec);
 * with ni_menu=2 the verify counters. */
GDX_COLD int Interp::NiMenuFormat(char* buf, size_t cap, uint32_t tasks, int gameMode) {
    if (mNiMenu == 0 || cap == 0 || tasks == 0) {
        return 0;
    }
    NiMenuWin& m = mNiMenuWin;
    if (m.runs == 0 && m.stockRuns == 0) {
        return 0;
    }
    const double inv = 1.0 / static_cast<double>(tasks);
    uint32_t stockRects = 0, interp = 0, draws = 0, progMiss = 0, decRuns = 0;
    uint32_t noHash = 0, depMiss = 0, recFails = 0, regCold = 0, regBuilds = 0, slWalks = 0, progEx = 0;
    if (mNrr != nullptr) {
        stockRects = mNrr->w.stockRects;
        interp = mNrr->w.interp;
        draws = mNrr->w.draws;
        progMiss = mNrr->w.progMiss;
        decRuns = mNrr->w.decRuns;
        noHash = mNrr->w.progNoHash;
        depMiss = mNrr->w.progDepMiss;
        recFails = mNrr->w.recFails;
        regCold = mNrr->w.regCold;
        regBuilds = mNrr->w.regBuilds;
        slWalks = mNrr->w.slWalks;
        progEx = mNrr->w.progEx;
    }
    int n = snprintf(buf, cap,
                     "[nimenu] tasks=%u mode=%d ni_menu=%d runs=%.2f words=%.1f compiled=%.2f dec=%.2f stock=%.2f "
                     "stockwords=%.1f vol=%.2f tileids=%.2f | nrr stockrects=%.2f interp=%.2f draws=%.2f progmiss=%.2f decruns=%.2f "
                     "nohash=%.2f depmiss=%.2f recfail=%.2f ex=%.2f regcold=%.2f regbuilds=%.2f slwalk=%.2f",
                     static_cast<unsigned>(tasks), gameMode, mNiMenu, m.runs * inv, m.words * inv, m.prog * inv,
                     m.dec * inv, m.stockRuns * inv, m.stockWords * inv, m.vol * inv, m.tileIds * inv, stockRects * inv, interp * inv, draws * inv,
                     progMiss * inv, decRuns * inv, noHash * inv, depMiss * inv, recFails * inv, progEx * inv,
                     regCold * inv, regBuilds * inv, slWalks * inv);
    if (mNiMenu == 2 && mNrr != nullptr && n > 0 && static_cast<size_t>(n) < cap) {
        const NrrHud::Vfy& v = mNrr->vfy;
        n += snprintf(buf + n, cap - static_cast<size_t>(n),
                      " | vfy prog=%u reg ok=%u bad=%u new=%u ld ok=%u bad=%u rect ok=%u bad=%u new=%u skip=%u aux "
                      "ok=%u bad=%u | ses_bad reg=%u ld=%u rect=%u aux=%u",
                      v.progRuns, v.regOk, v.regBad, v.regNew, v.ldOk, v.ldBad, v.rectOk, v.rectBad, v.rectNew,
                      v.rectSkip, v.auxOk, v.auxBad, mNrr->vfySes.regBad, mNrr->vfySes.ldBad, mNrr->vfySes.rectBad,
                      mNrr->vfySes.auxBad);
        if (mNrrHud != 2) {
            memset(&mNrr->vfy, 0, sizeof(mNrr->vfy));
        }
    }
    memset(&m, 0, sizeof(m));
    return n;
}

} // namespace gdxn
