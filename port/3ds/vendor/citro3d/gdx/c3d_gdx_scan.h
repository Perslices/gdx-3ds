/* port/3ds/vendor/citro3d/gdx/c3d_gdx_scan.h -- GDX (c5submit): the word-at-a-time dirty
 * scan behind C3DGDX_DIET_UNIFSCAN. Pure C, no citro3d or libctru dependency, so the host test
 * (port/3ds/test/host/c3d_gdx_scan_tests.c) compiles exactly this code.
 *
 * citro3d keeps one bool per float-uniform register (C3D_FVUnifDirty[type][96]) and stock
 * C3D_UpdateUniforms walks them a byte at a time: skip clean flags, then extend a run of dirty
 * ones, upload the run, repeat. These helpers find the same run boundaries four flags per load:
 *
 *   C3Dgdx_NextDirty(d, i, n)  first index k >= i with d[k] != 0, or n
 *   C3Dgdx_RunEnd(d, i, n)     first index k >= i with d[k] == 0, or n
 *
 * Contract: d is 4-byte aligned and n is a multiple of 4 (the dirty arrays are 96 flags, and
 * the vendored uniforms.c aligns them). A flag counts as dirty when its byte is non-zero, which
 * is what stock's `if (!C3D_FVUnifDirty[type][i])` tests, so the runs are identical for any
 * byte contents, not only 0/1. */
#ifndef C3D_GDX_SCAN_H
#define C3D_GDX_SCAN_H

#include <stdint.h>
#include <string.h>

/* 0x80 in every byte lane whose byte is non-zero (no carry crosses a lane). */
static inline uint32_t C3Dgdx_NonZeroLanes(uint32_t w)
{
	return (((w & 0x7F7F7F7Fu) + 0x7F7F7F7Fu) | w) & 0x80808080u;
}

/* Lanes at or after byte (i & 3) of its word. */
static inline uint32_t C3Dgdx_LanesFrom(int i)
{
	return 0x80808080u << ((i & 3) * 8);
}

static inline uint32_t C3Dgdx_LoadFlags4(const uint8_t* d, int wordStart)
{
	uint32_t w;
	memcpy(&w, __builtin_assume_aligned(d + wordStart, 4), sizeof(w));
	return w;
}

/* Index of the lowest lane set in a non-zero lane mask (little-endian byte order). */
static inline int C3Dgdx_FirstLane(uint32_t lanes)
{
	return __builtin_ctz(lanes) >> 3;
}

static inline int C3Dgdx_NextDirty(const uint8_t* d, int i, int n)
{
	while (i < n)
	{
		int wordStart = i & ~3;
		uint32_t lanes = C3Dgdx_NonZeroLanes(C3Dgdx_LoadFlags4(d, wordStart)) & C3Dgdx_LanesFrom(i);
		if (lanes)
		{
			return wordStart + C3Dgdx_FirstLane(lanes);
		}
		i = wordStart + 4;
	}
	return n;
}

static inline int C3Dgdx_RunEnd(const uint8_t* d, int i, int n)
{
	while (i < n)
	{
		int wordStart = i & ~3;
		uint32_t lanes = ~C3Dgdx_NonZeroLanes(C3Dgdx_LoadFlags4(d, wordStart)) & C3Dgdx_LanesFrom(i);
		if (lanes)
		{
			return wordStart + C3Dgdx_FirstLane(lanes);
		}
		i = wordStart + 4;
	}
	return n;
}

#endif /* C3D_GDX_SCAN_H */
