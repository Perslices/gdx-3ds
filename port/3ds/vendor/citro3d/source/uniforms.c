#include "internal.h"
#include <c3d/uniforms.h>
#include "../gdx/c3d_gdx.h" // GDX (c5submit)

C3D_FVec C3D_FVUnif[2][C3D_FVUNIF_COUNT];
C3D_IVec C3D_IVUnif[2][C3D_IVUNIF_COUNT];
u16      C3D_BoolUnifs[2];

// GDX (c5submit): 4-byte aligned for the word-at-a-time scan (gdx/c3d_gdx_scan.h).
bool C3D_FVUnifDirty[2][C3D_FVUNIF_COUNT] __attribute__((aligned(4)));
bool C3D_IVUnifDirty[2][C3D_IVUNIF_COUNT] __attribute__((aligned(4)));
bool C3D_BoolUnifsDirty[2];
u32 C3Dgdx_unifTouch = 1; // GDX (n60draw): include/c3d/uniforms.h

static struct
{
	bool dirty;
	int count;
	float24Uniform_s* data;
} C3Di_ShaderFVecData[2];

static bool C3Di_FVUnifEverDirty[2][C3D_FVUNIF_COUNT] __attribute__((aligned(4))); // GDX (c5submit)
static bool C3Di_IVUnifEverDirty[2][C3D_IVUNIF_COUNT];

void C3D_UpdateUniforms(GPU_SHADER_TYPE type)
{
	int offset = type == GPU_GEOMETRY_SHADER ? (GPUREG_GSH_BOOLUNIFORM-GPUREG_VSH_BOOLUNIFORM) : 0;
	int i = 0;

	// Update FVec uniforms that come from shader constants
	if (C3Di_ShaderFVecData[type].dirty)
	{
		while (i < C3Di_ShaderFVecData[type].count)
		{
			float24Uniform_s* u = &C3Di_ShaderFVecData[type].data[i++];
			GPUCMD_AddIncrementalWrites(GPUREG_VSH_FLOATUNIFORM_CONFIG+offset, (u32*)u, 4);
			C3D_FVUnifDirty[type][u->id] = false;
		}
		C3Di_ShaderFVecData[type].dirty = false;
		i = 0;
	}

	// Update FVec uniforms
	while (i < C3D_FVUNIF_COUNT)
	{
		if (!C3D_FVUnifDirty[type][i])
		{
			i ++;
			continue;
		}

		// Find the number of consecutive dirty uniforms
		int j;
		for (j = i; j < C3D_FVUNIF_COUNT && C3D_FVUnifDirty[type][j]; j ++);

		// Upload the uniforms
		GPUCMD_AddWrite(GPUREG_VSH_FLOATUNIFORM_CONFIG+offset, 0x80000000|i);
		GPUCMD_AddWrites(GPUREG_VSH_FLOATUNIFORM_DATA+offset, (u32*)&C3D_FVUnif[type][i], (j-i)*4);

		// Clear the dirty flag
		int k;
		for (k = i; k < j; k ++)
		{
			C3D_FVUnifDirty[type][k] = false;
			C3Di_FVUnifEverDirty[type][k] = true;
		}

		// Advance
		i = j;
	}

	// Update IVec uniforms
	for (i = 0; i < C3D_IVUNIF_COUNT; i ++)
	{
		if (!C3D_IVUnifDirty[type][i]) continue;

		GPUCMD_AddWrite(GPUREG_VSH_INTUNIFORM_I0+offset+i, C3D_IVUnif[type][i]);
		C3D_IVUnifDirty[type][i] = false;
		C3Di_IVUnifEverDirty[type][i] = false;
	}

	// Update bool uniforms
	if (C3D_BoolUnifsDirty[type])
	{
		GPUCMD_AddWrite(GPUREG_VSH_BOOLUNIFORM+offset, 0x7FFF0000 | C3D_BoolUnifs[type]);
		C3D_BoolUnifsDirty[type] = false;
	}
}

void C3Di_DirtyUniforms(GPU_SHADER_TYPE type)
{
	int i;
	C3Dgdx_unifTouch = 1; // GDX (n60draw)
	if (type == GPU_VERTEX_SHADER)
	{
		C3Dgdx_InvalidateUniformShadow(); // GDX (c5submit): the GPU lost what ELIDE's shadow says it holds
	}
	C3D_BoolUnifsDirty[type] = true;
	if (C3Di_ShaderFVecData[type].count)
		C3Di_ShaderFVecData[type].dirty = true;
	for (i = 0; i < C3D_FVUNIF_COUNT; i ++)
		C3D_FVUnifDirty[type][i] = C3D_FVUnifDirty[type][i] || C3Di_FVUnifEverDirty[type][i];
	for (i = 0; i < C3D_IVUNIF_COUNT; i ++)
		C3D_IVUnifDirty[type][i] = C3D_IVUnifDirty[type][i] || C3Di_IVUnifEverDirty[type][i];
}

void C3Di_LoadShaderUniforms(shaderInstance_s* si)
{
	GPU_SHADER_TYPE type = si->dvle->type;
	C3Dgdx_unifTouch = 1; // GDX (n60draw)
	if (si->boolUniformMask)
	{
		C3D_BoolUnifs[type] &= ~si->boolUniformMask;
		C3D_BoolUnifs[type] |= si->boolUniforms;
	}

	if (type == GPU_GEOMETRY_SHADER)
		C3D_BoolUnifs[type] &= ~BIT(15);
	C3D_BoolUnifsDirty[type] = true;

	if (si->intUniformMask)
	{
		int i;
		for (i = 0; i < 4; i ++)
		{
			if (si->intUniformMask & BIT(i))
			{
				C3D_IVUnif[type][i] = si->intUniforms[i];
				C3D_IVUnifDirty[type][i] = true;
			}
		}
	}
	C3Di_ShaderFVecData[type].dirty = true;
	C3Di_ShaderFVecData[type].count = si->numFloat24Uniforms;
	C3Di_ShaderFVecData[type].data = si->float24Uniforms;
}

void C3Di_ClearShaderUniforms(GPU_SHADER_TYPE type)
{
	C3Di_ShaderFVecData[type].dirty = false;
	C3Di_ShaderFVecData[type].count = 0;
	C3Di_ShaderFVecData[type].data = NULL;
}

// ---------------------------------------------------------------------------------------------
// GDX (c5submit): the P3a diet's C3D_UpdateUniforms, plus the verify mode's state save/restore.
// ---------------------------------------------------------------------------------------------
#include <string.h>
#include "gdx_submit.h"
#include "../../../gfx/gdx3ds_hotcold.h" // GDX (hotcold): GDX_COLD on the non-default ELIDE path

static C3Di_GdxUnifShadow C3Di_GdxShadow;
static u32 C3Di_GdxElided;

void C3Dgdx_InvalidateUniformShadow(void)
{
	memset(C3Di_GdxShadow.valid, 0, sizeof(C3Di_GdxShadow.valid));
}

// Bitwise equality (a float compare would call -0 == +0 and NaN != NaN wrongly).
static inline bool C3Di_GdxGpuHolds(int id)
{
	const u32* held = (const u32*)&C3Di_GdxShadow.value[id];
	const u32* want = (const u32*)&C3D_FVUnif[GPU_VERTEX_SHADER][id];
	return C3Di_GdxShadow.valid[id]
		&& held[0] == want[0] && held[1] == want[1] && held[2] == want[2] && held[3] == want[3];
}

static void C3Di_GdxSendRun(GPU_SHADER_TYPE type, int offset, int i, int j, u32 emitInline)
{
	C3Dgdx_Write1(emitInline, C3DGDX_HDR(GPUREG_VSH_FLOATUNIFORM_CONFIG+offset, 0xF), 0x80000000|i);
	C3Dgdx_WriteN(emitInline, C3DGDX_HDR(GPUREG_VSH_FLOATUNIFORM_DATA+offset, 0xF), (u32*)&C3D_FVUnif[type][i], (j-i)*4);
}

// ELIDE: send the parts of the dirty run [i,j) the GPU does not already hold. Held registers at
// either end are dropped; inside the run only a gap of two or more is cut (a one-register gap
// costs as many words to cut as it saves). The shadow records what was sent.
GDX_COLD static void C3Di_GdxSendRunElided(int i, int j, u32 emitInline)
{
	int k = i;
	while (k < j)
	{
		while (k < j && C3Di_GdxGpuHolds(k))
		{
			k ++;
			C3Di_GdxElided ++;
		}
		if (k >= j)
		{
			break;
		}
		int end = k + 1;
		while (end < j)
		{
			if (!C3Di_GdxGpuHolds(end))
			{
				end ++;
				continue;
			}
			if (end + 1 < j && !C3Di_GdxGpuHolds(end + 1))
			{
				end += 2; // absorb a one-register gap
				continue;
			}
			break;
		}
		C3Di_GdxSendRun(GPU_VERTEX_SHADER, 0, k, end, emitInline);
		memcpy(&C3Di_GdxShadow.value[k], &C3D_FVUnif[GPU_VERTEX_SHADER][k], (end-k)*sizeof(C3D_FVec));
		memset(&C3Di_GdxShadow.valid[k], true, end-k);
		k = end;
	}
}

// C3D_UpdateUniforms with the dirty runs found four flags per load (C3Dgdx_NextDirty /
// C3Dgdx_RunEnd return exactly the run boundaries of the stock byte loop) and, under
// C3DGDX_DIET_EMIT, the packets written straight into gpuCmdBuf. Without ELIDE the packets,
// their order and every flag update are the stock function's, including stock's
// IVUnifEverDirty = false. With ELIDE (vertex shader only) the flag updates are still stock's;
// only the words sent for a run shrink (C3Di_GdxSendRunElided).
void C3Di_GdxUpdateUniforms(GPU_SHADER_TYPE type, u32 mode)
{
	int offset = type == GPU_GEOMETRY_SHADER ? (GPUREG_GSH_BOOLUNIFORM-GPUREG_VSH_BOOLUNIFORM) : 0;
	u32 emitInline = mode & C3DGDX_DIET_EMIT;
	bool elide = (mode & C3DGDX_DIET_ELIDE) && type == GPU_VERTEX_SHADER;
	int i = 0;

	// Update FVec uniforms that come from shader constants
	if (C3Di_ShaderFVecData[type].dirty)
	{
		while (i < C3Di_ShaderFVecData[type].count)
		{
			float24Uniform_s* u = &C3Di_ShaderFVecData[type].data[i++];
			C3Dgdx_WriteN(emitInline, C3DGDX_HDR_INC(GPUREG_VSH_FLOATUNIFORM_CONFIG+offset), (u32*)u, 4);
			C3D_FVUnifDirty[type][u->id] = false;
			if (type == GPU_VERTEX_SHADER)
			{
				C3Di_GdxShadow.valid[u->id] = false; // a float24 constant, not a shadowed value
			}
		}
		C3Di_ShaderFVecData[type].dirty = false;
	}

	// Update FVec uniforms, one dirty run at a time
	const u8* dirty = (const u8*)C3D_FVUnifDirty[type];
	i = C3Dgdx_NextDirty(dirty, 0, C3D_FVUNIF_COUNT);
	while (i < C3D_FVUNIF_COUNT)
	{
		int j = C3Dgdx_RunEnd(dirty, i, C3D_FVUNIF_COUNT);

		if (elide)
		{
			C3Di_GdxSendRunElided(i, j, emitInline);
		}
		else
		{
			C3Di_GdxSendRun(type, offset, i, j, emitInline);
		}

		memset(&C3D_FVUnifDirty[type][i], false, j-i);
		memset(&C3Di_FVUnifEverDirty[type][i], true, j-i);

		i = C3Dgdx_NextDirty(dirty, j, C3D_FVUNIF_COUNT);
	}

	// Update IVec uniforms (the four flags are one aligned word)
	if (C3Dgdx_LoadFlags4((const u8*)C3D_IVUnifDirty[type], 0))
	{
		for (i = 0; i < C3D_IVUNIF_COUNT; i ++)
		{
			if (!C3D_IVUnifDirty[type][i])
			{
				continue;
			}
			C3Dgdx_Write1(emitInline, C3DGDX_HDR(GPUREG_VSH_INTUNIFORM_I0+offset+i, 0xF), C3D_IVUnif[type][i]);
			C3D_IVUnifDirty[type][i] = false;
			C3Di_IVUnifEverDirty[type][i] = false;
		}
	}

	// Update bool uniforms
	if (C3D_BoolUnifsDirty[type])
	{
		C3Dgdx_Write1(emitInline, C3DGDX_HDR(GPUREG_VSH_BOOLUNIFORM+offset, 0xF), 0x7FFF0000 | C3D_BoolUnifs[type]);
		C3D_BoolUnifsDirty[type] = false;
	}
}

// GDX (n60core2): true when C3D_UpdateUniforms(type) would emit anything (the NRR direct
// emitter's hand-off sync, C3Dgdx_StatePending).
bool C3Di_GdxUniformsPending(GPU_SHADER_TYPE type)
{
	const u32* fv = (const u32*)C3D_FVUnifDirty[type];
	u32 any = C3Dgdx_LoadFlags4((const u8*)C3D_IVUnifDirty[type], 0);
	int i;
	for (i = 0; i < C3D_FVUNIF_COUNT/4; i ++)
	{
		any |= fv[i];
	}
	return any || C3D_BoolUnifsDirty[type] || C3Di_ShaderFVecData[type].dirty;
}

void C3Di_GdxShadowSave(C3Di_GdxUnifShadow* s)
{
	memcpy(s, &C3Di_GdxShadow, sizeof(*s));
}

void C3Di_GdxShadowLoad(const C3Di_GdxUnifShadow* s)
{
	memcpy(&C3Di_GdxShadow, s, sizeof(*s));
}

u32 C3Di_GdxTakeElided(void)
{
	u32 n = C3Di_GdxElided;
	C3Di_GdxElided = 0;
	return n;
}

void C3Di_GdxUnifSave(C3Di_GdxUnifState* s)
{
	int t;
	memcpy(s->fvDirty, C3D_FVUnifDirty, sizeof(s->fvDirty));
	memcpy(s->ivDirty, C3D_IVUnifDirty, sizeof(s->ivDirty));
	memcpy(s->boolDirty, C3D_BoolUnifsDirty, sizeof(s->boolDirty));
	memcpy(s->boolUnifs, C3D_BoolUnifs, sizeof(s->boolUnifs));
	memcpy(s->fvEverDirty, C3Di_FVUnifEverDirty, sizeof(s->fvEverDirty));
	memcpy(s->ivEverDirty, C3Di_IVUnifEverDirty, sizeof(s->ivEverDirty));
	for (t = 0; t < 2; t ++)
	{
		s->shaderFVecDirty[t] = C3Di_ShaderFVecData[t].dirty;
		s->shaderFVecCount[t] = C3Di_ShaderFVecData[t].count;
		s->shaderFVecData[t] = C3Di_ShaderFVecData[t].data;
	}
}

void C3Di_GdxUnifLoad(const C3Di_GdxUnifState* s)
{
	int t;
	memcpy(C3D_FVUnifDirty, s->fvDirty, sizeof(s->fvDirty));
	memcpy(C3D_IVUnifDirty, s->ivDirty, sizeof(s->ivDirty));
	memcpy(C3D_BoolUnifsDirty, s->boolDirty, sizeof(s->boolDirty));
	memcpy(C3D_BoolUnifs, s->boolUnifs, sizeof(s->boolUnifs));
	memcpy(C3Di_FVUnifEverDirty, s->fvEverDirty, sizeof(s->fvEverDirty));
	memcpy(C3Di_IVUnifEverDirty, s->ivEverDirty, sizeof(s->ivEverDirty));
	for (t = 0; t < 2; t ++)
	{
		C3Di_ShaderFVecData[t].dirty = s->shaderFVecDirty[t];
		C3Di_ShaderFVecData[t].count = s->shaderFVecCount[t];
		C3Di_ShaderFVecData[t].data = s->shaderFVecData[t];
	}
}
