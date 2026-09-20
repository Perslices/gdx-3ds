# Hardware dev loop over WiFi

One page. Do steps 1-6 once per power-on; after that the Mac drives the console on its own.

Console side: `port/3ds/net/gdx3ds_net.c` (killswitch `[net] enabled`, **default off**).
Mac side: `tools/dev/hw.py` (stdlib only). Design + rationale: `docs/research/native/02-hwlink-plan.md`.

## 1. Console setup (once per power-on)

1. **Luma3DS ≥ 10.1.1** (13.x recommended — the Rosalina menu items below are 10.x+).
   Same requirement as `docs/3DS-HARDWARE.md` §1.
2. **On the charger, lid open, auto-sleep OFF** (System Settings → Power Saving).
   Sleep drops SOC sockets; the app reconnects, but every sleep costs you a relink.
   Screen dim is fine.
3. **WiFi on the same LAN/subnet as the Mac.** Find the console's IP: it is printed on
   hbmenu's netloader screen, or System Settings → Internet Settings → Connection → status.
4. **Homebrew Launcher → press Y once.** It says "waiting for 3dslink".
5. **Rosalina menu (L + Down + Select):**
   - *Miscellaneous options* → **Enable input redirection** — needed for `hw.py input`.
   - *Debugger options* → **Enable debugger** — needed for `hw.py gdb`. Leave it off if you
     are not debugging, and turn it off before powering down.
6. On the SD card, `sdmc:/3ds/gdiffuser/gdiffuser.ini` needs:

   ```ini
   [net]
   enabled = 1
   ```

   Everything else has a working default (see the header of `port/3ds/net/gdx3ds_net.h`).
   `[debug] filelog = 1` is also worth having: it keeps a copy of everything on the card,
   so a crash that kills the link still leaves evidence.

## 2. Mac setup (once)

```ini
; tools/dev/hw.ini   — GITIGNORED, your LAN address never gets committed
[hw]
ip = 192.168.1.42
```

or `export GDX_3DS_IP=192.168.1.42`, or pass `--ip` every time.

`hw.py push` needs devkitPro's `3dslink`
(`/opt/devkitpro/tools/bin/3dslink`, package `3dstools`).

## 3. The loop

```
hw.py push --build --log     # first build of the session: netloader push + live log tail
hw.py reload --build         # EVERY build after: no console interaction at all
```

`push` uploads to hbmenu's netloader, which writes `sdmc:/3ds/G-Diffuser-3DS.3dsx` and
launches it. `reload` streams the new image straight into the running game, which verifies
its CRC, renames it over the target, exits through the normal `[exit]` chain, and asks
Luma's `hb:ldr` to relaunch it. **Keep the basename `G-Diffuser-3DS.3dsx`** — the netloader
writes `sdmc:/3ds/<basename>`, and reload targets that exact path.

Everything else:

| Command | What it does |
|---|---|
| `hw.py log` | Serve the log link on 17491 and tail it. Runs forever, reconnects forever. |
| `hw.py ping` | `OK PONG <build-id> <frame> <uptime_ms>` — liveness + which build is actually running. |
| `hw.py ini <section> <key> <value> [--save]` | Change a config key at runtime; `--save` writes `gdiffuser.ini` back. |
| `hw.py capture <tag>` | Arm the oracle track's frame capture for the next frame. |
| `hw.py replay <file.gdxrec>` | Push a `.gdxrec` to the card and start the in-app tick-exact replay. |
| `hw.py input --demo` / `--script F` | Drive Rosalina InputRedirection over UDP 4950. |
| `hw.py quit` | Clean programmatic exit through the whole `[exit]` chain. |
| `hw.py gdb` | Batch backtrace against the Rosalina GDB stub on port 4000. |

## 4. Two input lanes, and why they are not interchangeable

- **`hw.py input` (UDP 4950)** patches HID shared memory from outside the process. It has
  **no frame clock**, so it can never reproduce a run exactly. Use it to explore, drive
  menus, and poke at something by hand. It is not a parity oracle.
- **`hw.py replay` (`.gdxrec`)** pushes a recording to the SD card and starts the app's own
  injector, which consumes exactly one pad state per `gdx_controller_poll()`. That is the
  tick-exact lane, and the only one a parity claim may cite. The oracle track owns the
  format and the injector.

InputRedirection patches HID **globally** — it drives the HOME menu too — and a stuck
stream outlives the driver. `hw.py input` always sends a neutral packet on exit, including
on Ctrl-C. If a console is ever left with a stuck pad, run `hw.py input --demo` and let it
finish, or toggle input redirection off in Rosalina.

## 5. Ports

| Port | Proto | Who listens | What |
|---|---|---|---|
| 17491 | TCP | **the Mac** (`hw.py log`) | log link; the console dials out |
| 17491 | TCP | the console (hbmenu) | netloader, while hbmenu waits after Y |
| 17492 | TCP | the console (us) | control commands |
| 4950 | UDP | the console (Rosalina) | input redirection |
| 4000 | TCP | the console (Rosalina) | GDB stub |

`hw.py push` deliberately does not pass `3dslink -s`: that would make `3dslink` occupy
17491 and fight our own log server for the console's connection.

## 6. When it goes wrong

| Symptom | Cause | Do this |
|---|---|---|
| `3dslink` fails to connect | hbmenu is not on the netloader screen | press Y in hbmenu |
| No `[net] link up` after a push | `[net] enabled = 0`, or the console cannot reach the Mac | check the ini; check both are on the same subnet; check the Mac firewall allows inbound 17491 |
| `[net] hbldr unavailable rc=…` in the log after a reload | this loader is not Luma's Rosalina | the new image is already on the card — press A in hbmenu |
| Reload times out | the new build crashes at boot | **do not push again.** Read the log tail `hw.py reload` printed, fix, then push from hbmenu |
| Log goes silent, game fine | WiFi dropped | the app retries every 2 s; up to 32 KiB of missed lines are re-sent from the ring, then counted as `[net] dropped=N` |
| Pad stuck after Ctrl-C | InputRedirection kept the last packet | see §4 |
| `[watchdog] … net=0` | link down but the game is alive | distinguishes a silent host from a wedged game |

## 7. What still needs a human

Power on, one Y press in hbmenu, two Rosalina toggles, driving reference laps for the
replay files, and subjective calls (feel, judder, stereo comfort). There is no remote
power-on. Keep the console docked on the charger with auto-sleep off.
