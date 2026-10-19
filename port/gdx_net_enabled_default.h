/* port/gdx_net_enabled_default.h -- pure resolution logic for [net] enabled's DEFAULT
 * (n60lead hwlink round).
 *
 * Split out of port/3ds/net/gdx3ds_net.c on purpose, same reason port/gdx_hud_placement.c
 * was split out of port/input_bridge.c: that TU pulls in <3ds.h> (sockets, svc calls,
 * threads) and cannot compile for the host, so the one piece of real DECISION logic in
 * gdx3ds_net_init -- "what should [net] enabled default to when the ini says nothing
 * about it" -- lives here instead, where tools/ci-3ds.sh can compile and run it directly
 * against the host compiler (port/tests/gdx_net_enabled_default_host_tests.c).
 *
 * The rule (gdx3ds_net.h's own [net] enabled doc has the full rationale): an EXPLICIT
 * `enabled = 0` or `= 1` in gdiffuser.ini always wins. Only when the ini has no such key
 * (or an unparsable one) does the launch method get a vote: ON when hbmenu's netloader
 * (3dslink) launched us, OFF otherwise (hbmenu/SD/CIA launch, no netloader).
 */
#ifndef GDX_NET_ENABLED_DEFAULT_H
#define GDX_NET_ENABLED_DEFAULT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Pass this as gdx3ds_config_get_bool("net", "enabled", GDX_NET_ENABLED_SENTINEL)'s
 * fallback. It is never a value get_bool can parse out of an ini line (only 0/1 and the
 * 1/true/yes/on, 0/false/no/off spellings are), so seeing it back means the key is absent
 * or unparsable -- the only case where the launch method gets to pick. */
#define GDX_NET_ENABLED_SENTINEL (-1)

/* explicitVal: gdx3ds_config_get_bool("net", "enabled", GDX_NET_ENABLED_SENTINEL)'s result.
 * launchedViaNetloader: nonzero iff __3dslink_host.s_addr != 0 at gdx3ds_net_init time.
 * Returns the resolved 0/1 enabled state. */
int gdx_net_enabled_resolve(int explicitVal, int launchedViaNetloader);

/* 1 iff `explicitVal` is what GDX_NET_ENABLED_SENTINEL looks like after a round trip
 * through gdx3ds_config_get_bool -- i.e. the ini had no usable key, so the resolved value
 * above came from the launch method rather than the ini. Exposed separately so a caller
 * (the boot receipt) can say WHY it is on without re-deriving the sentinel test itself. */
int gdx_net_enabled_was_defaulted(int explicitVal);

#ifdef __cplusplus
}
#endif

#endif /* GDX_NET_ENABLED_DEFAULT_H */
