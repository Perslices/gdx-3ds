/* port/3ds/gfx/n64/gdx_log.h -- logging shim for the owned interpreter. On device the glue
 * routes to the svc debug channel + the sdmc filelog (the [ninterp]/[nverify] receipts); on
 * the host harness it is printf. Never call from a per-vertex path. */
#ifndef GDX_N64_LOG_H
#define GDX_N64_LOG_H

#ifdef __cplusplus
extern "C" {
#endif
/* hotcold: cold -- every call site's path is predicted unlikely and laid out off the hot blocks. */
void gdxn_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2), cold));
#ifdef __cplusplus
}
#endif

#define GDXN_LOG(...) gdxn_logf(__VA_ARGS__)

#endif /* GDX_N64_LOG_H */
