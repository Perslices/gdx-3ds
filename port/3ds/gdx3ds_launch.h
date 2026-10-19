/* port/3ds/gdx3ds_launch.h -- N60LAUNCH: launch variants, so ONE card ini covers several runs, each
 * logging to its own file (docs/research/native/n60launch-progress.md; the ini keys and the
 * selection rules are in gdx3ds_launch_core.h).
 *
 * Boot order (main_3ds.cpp):
 *   gdx3ds_config_load
 *   gdx3ds_launch_boot_early()      force / cycle: pick + apply NOW (before every lever latch);
 *                                   hold mode: only arm the window and defer the file log
 *   if (!gdx3ds_launch_defers_filelog()) gdx3ds_filelog_init()
 *   ... gdx3ds_os_window_init
 *   gdx3ds_launch_boot_window()     hold mode: the on-screen window (HID polled every vblank),
 *                                   pick + apply, then the deferred gdx3ds_filelog_init
 *   gdx3ds_fps_hud_init ...         every lever the variants touch latches after this point
 *
 * [debug] launch_variants = 0 (the default): every call returns at its first test. */
#ifndef GDX3DS_LAUNCH_H
#define GDX3DS_LAUNCH_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*GdxLaunchLogFn)(const char* line);

void gdx3ds_launch_boot_early(GdxLaunchLogFn log);
int gdx3ds_launch_defers_filelog(void);
void gdx3ds_launch_boot_window(void);
/* The fps-row tag: "[N]" / "[main]" once a launch-variant boot has picked, "" otherwise. */
const char* gdx3ds_launch_tag(void);
/* AUDIOFIX: the bare variant name ("main" / "q" / "c" / "g" / ...), for filenames. */
const char* gdx3ds_launch_variant_name(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_LAUNCH_H */
