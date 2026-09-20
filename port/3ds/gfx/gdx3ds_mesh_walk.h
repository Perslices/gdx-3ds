/* gdx3ds_mesh_walk.h — F3DEX2 walker for the game's BUILT machine display lists
 * (racer.c D_800CDDB0[slot*6+lod], 126 x Gfx each), shared by the host differential
 * harness (tools/native/machines_harness.sh) and the device baker (gdx3ds_mesh.cpp).
 *
 * Input is the port's host-built Gfx packet stream: 16 bytes per command on EVERY host,
 * w0 a little-endian u32 at byte 0 and the pointer word a u64 at byte 8 (decomp gbi.h
 * under PORT; on 32-bit hosts the high half carries GDX_GFXW1_HOST_TAG and the pointer
 * is the low half — see port/n64_gfx_bridge.cpp kHostBuiltGfxStride). Vertex pointers
 * are handed to the caller's resolver so the device can apply the bridge's host-pointer
 * rules and the harness can just cast.
 *
 * Output: material runs in emission order, vertex chunks referencing the game's Vtx
 * arrays IN PLACE, and mesh-global u16 triangle indices. Anything outside the nine
 * opcodes the emitters use (G_TEXTURE, G_SETPRIMCOLOR, G_SETENVCOLOR, G_SETCOMBINE,
 * G_VTX, G_TRI1, G_TRI2, G_RDPPIPESYNC, G_ENDDL) is a bake failure (status < 0) and the
 * caller keeps the list on the interpreter.
 *
 * Pure C, no libultraship/decomp includes: compiled on macOS by the harness and on the
 * 3DS inside gdx3ds_gfx. */
#ifndef GDX3DS_MESH_WALK_H
#define GDX3DS_MESH_WALK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX3DS_MESH_MAX_CMDS 126   /* racer.c: Gfx[126] per slot/LOD */
#define GDX3DS_MESH_MAX_RUNS 64
#define GDX3DS_MESH_MAX_CHUNKS 64
#define GDX3DS_MESH_MAX_TRIS 256   /* <= 2 per G_TRI2 x 126 commands */
#define GDX3DS_MESH_MAX_VERTS 1024 /* sum of (deduplicated) chunk counts */
#define GDX3DS_MESH_SLOTS 64       /* F3DEX2 vertex buffer */

enum {
    GDX3DS_MESH_COLOR_INHERIT = 0, /* never set in this list: interpreter state at hook time */
    GDX3DS_MESH_COLOR_CONST = 1    /* set by the list before this run's first triangle */
};

/* One material run: the RDP/RSP state in force for triCount consecutive triangles. */
typedef struct Gdx3dsMeshRun {
    uint32_t combineW0; /* G_SETCOMBINE w0 low 24 bits (raw, as emitted) */
    uint32_t combineW1; /* G_SETCOMBINE w1 */
    uint32_t primRgba;  /* r<<24|g<<16|b<<8|a (CONST only) */
    uint32_t envRgba;
    uint16_t texS, texT; /* G_TEXTURE scale */
    uint16_t firstTri, triCount;
    uint8_t tile;        /* G_TEXTURE tile (0..7) */
    uint8_t texOn;       /* G_TEXTURE on bit */
    uint8_t primKind, envKind;
    uint8_t primMin, primLod; /* G_SETPRIMCOLOR minlevel / lod fraction bytes */
    uint8_t combineSet;  /* 0 = no G_SETCOMBINE before this run (inherit) */
    uint8_t texSet;      /* 0 = no G_TEXTURE before this run (inherit) */
} Gdx3dsMeshRun;

/* A G_VTX load: `count` host-endian 16-byte Vtx starting at `src`, mesh-global vertex
 * index base `baseVertex`. Deduplicated on (src, count, texS, texT). */
typedef struct Gdx3dsMeshChunk {
    uint64_t srcW1;  /* raw pointer word as written by the emitter */
    const void* src; /* resolver's answer */
    uint16_t count;
    uint16_t baseVertex;
    uint16_t texS, texT; /* texture_scaling_factor at the G_VTX (GfxSpVertex bakes U/V with it) */
} Gdx3dsMeshChunk;

typedef struct Gdx3dsMeshWalkOut {
    Gdx3dsMeshRun runs[GDX3DS_MESH_MAX_RUNS];
    Gdx3dsMeshChunk chunks[GDX3DS_MESH_MAX_CHUNKS];
    uint16_t tris[GDX3DS_MESH_MAX_TRIS * 3]; /* mesh-global vertex indices */
    uint16_t nRuns, nChunks, nTris, nVerts;
    uint16_t nCmds;       /* commands consumed including G_ENDDL */
    uint16_t nVtxCmds;    /* G_VTX commands seen (before dedup) */
    uint32_t opcodeMask[8]; /* bit per opcode value seen (256 bits) */
    int status;           /* 0 ok, else GDX3DS_MESH_WALK_E_* */
    uint16_t errCmd;      /* command index of the failure */
    uint8_t errOpcode;
    uint8_t v0NonZero;    /* any G_VTX with v0 != 0 (census says never) */
} Gdx3dsMeshWalkOut;

enum {
    GDX3DS_MESH_WALK_OK = 0,
    GDX3DS_MESH_WALK_E_OPCODE = -1,     /* unexpected opcode */
    GDX3DS_MESH_WALK_E_NOEND = -2,      /* no G_ENDDL within maxCmds */
    GDX3DS_MESH_WALK_E_VTXRANGE = -3,   /* G_VTX count/v0 outside the 64-slot buffer */
    GDX3DS_MESH_WALK_E_VTXPTR = -4,     /* resolver refused the vertex pointer */
    GDX3DS_MESH_WALK_E_SLOT = -5,       /* triangle references an unloaded slot */
    GDX3DS_MESH_WALK_E_CAPACITY = -6,   /* runs/chunks/tris/verts overflow */
    GDX3DS_MESH_WALK_E_NULL = -7,       /* null list */
    GDX3DS_MESH_WALK_E_TEXOFF = -8      /* G_TEXTURE with on == 0 */
};

/* Vertex pointer resolver: raw w1 word -> host address of `bytes` readable bytes, or
 * NULL to refuse (the walk then fails with E_VTXPTR). */
typedef const void* (*Gdx3dsMeshResolveFn)(uint64_t w1, size_t bytes, void* user);

/* Walk `gfx` (host-built 16-byte packets) for at most maxCmds commands. Returns
 * out->status. `out` is fully written on success; on failure err* name the culprit. */
int gdx3ds_mesh_walk(const void* gfx, size_t maxCmds, Gdx3dsMeshResolveFn resolve, void* user,
                     Gdx3dsMeshWalkOut* out);

/* Host-built packet accessors (shared with the harness's independent checks). */
static inline uint32_t gdx3ds_mesh_gfx_w0(const void* gfx, size_t idx) {
    const uint8_t* p = (const uint8_t*)gfx + idx * 16u;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t gdx3ds_mesh_gfx_w1(const void* gfx, size_t idx) {
    const uint8_t* p = (const uint8_t*)gfx + idx * 16u + 8u;
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

const char* gdx3ds_mesh_walk_strerror(int status);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MESH_WALK_H */
