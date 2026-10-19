// GDX (c5submit): the P3a submission diet, its verify mode and its profiler.
// Control surface and bit meanings: gdx/c3d_gdx.h. Background: docs/research/native/
// c5submit-progress.md.
//
// Every function here that emits commands is a copy of a stock citro3d 1.7.1 function (named
// beside it) that differs only in HOW it appends the same words: inline stores instead of the
// GPUCMD_Add call chain (C3DGDX_DIET_EMIT), the word-wise uniform scan (C3DGDX_DIET_UNIFSCAN),
// and one early test of the dirty flags (C3DGDX_DIET_FLAGSKIP). C3DGDX_VERIFY proves the
// equivalence per draw at run time.
#include <string.h>
#include "gdx_submit.h"
#include <c3d/base.h>
#include "../../../gfx/gdx3ds_hotcold.h" // GDX (hotcold): GDX_COLD on the verify/profile-only paths

// base.c's weak hooks (strong definitions come from lightenv.c / proctex.c / gas.c if linked).
void C3Di_LightEnvUpdate(C3D_LightEnv* env);
void C3Di_ProcTexUpdate(C3D_Context* ctx);

uint32_t C3Dgdx_mode;

// GDX (n60tail): [native] sky_late's stencil override (c3d_gdx.h).
uint32_t C3Dgdx_stencilMode;
uint32_t C3Dgdx_stencilWords[2];
uint32_t C3Dgdx_stencilCur;
uint32_t C3Dgdx_stencilBad;
uint32_t C3Dgdx_stencilFirst[4];
static uint32_t s_stencilCurFx[4];

void C3Dgdx_StencilDraw(void) {
	if (C3Dgdx_stencilCur != 0 && C3Dgdx_stencilBad == 0) {
		C3Dgdx_stencilFirst[0] = s_stencilCurFx[0];
		C3Dgdx_stencilFirst[1] = s_stencilCurFx[1];
		C3Dgdx_stencilFirst[2] = s_stencilCurFx[2];
		C3Dgdx_stencilFirst[3] = s_stencilCurFx[3];
	}
	C3Dgdx_stencilBad |= C3Dgdx_stencilCur;
}

void C3Dgdx_StencilCheck(uint32_t fragOpMode, uint32_t alphaBlend, uint32_t logicOp, uint32_t* fx4) {
	const uint32_t depth = fx4[3];
	uint32_t bad = (fx4[1] & 1u) ? C3DGDX_STB_STENCIL : 0u;
	if (C3Dgdx_stencilMode == C3DGDX_STENCIL_MARK) {
		// Every fragment that writes colour must overwrite all of it from its own value alone, so that
		// "stencil written" == "the sky under it was invisible".
		if (fragOpMode & 0x100u) {
			if (alphaBlend != 0x01010000u) { // ADD/ADD, ONE/ZERO for colour and alpha
				bad |= C3DGDX_STB_BLEND;
			}
		} else if (logicOp != GPU_LOGICOP_COPY) {
			bad |= C3DGDX_STB_LOGIC;
		}
		if (((depth >> 8) & 0xFu) != 0xFu) {
			bad |= C3DGDX_STB_MASK;
		}
		if ((fragOpMode & 3u) != GPU_FRAGOPMODE_GL) {
			bad |= C3DGDX_STB_FRAGOP;
		}
	} else if (depth & 0x1000u) { // SKY: a depth write bit, whatever the test enable
		bad |= C3DGDX_STB_SKYZ;
	}
	C3Dgdx_stencilCur = bad;
	s_stencilCurFx[0] = fragOpMode;
	s_stencilCurFx[1] = alphaBlend;
	s_stencilCurFx[2] = logicOp;
	s_stencilCurFx[3] = depth;
	fx4[1] = C3Dgdx_stencilWords[0];
	fx4[2] = C3Dgdx_stencilWords[1];
}

static C3Dgdx_Stats s_stats;
static u32 s_drawIndex;

static inline u64 C3Dgdx_Now(void)
{
	return svcGetSystemTick();
}

// ---------------------------------------------------------------------------------------------
// Diet copies of the per-draw state binds (stock: attribs.c, buffers.c, effect.c, texture.c,
// texenv.c). Same packets, same order.
// ---------------------------------------------------------------------------------------------

static void DietAttrInfoBind(u32 e, C3D_AttrInfo* info) // C3Di_AttrInfoBind
{
	C3Dgdx_WriteN(e, C3DGDX_HDR_INC(GPUREG_ATTRIBBUFFERS_FORMAT_LOW), (u32*)info->flags, sizeof(info->flags)/sizeof(u32));
	C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_VSH_INPUTBUFFER_CONFIG, 0xB), 0xA0000000 | (info->attrCount - 1));
	C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_VSH_NUM_ATTR, 0xF), info->attrCount - 1);
	C3Dgdx_WriteN(e, C3DGDX_HDR_INC(GPUREG_VSH_ATTRIBUTES_PERMUTATION_LOW), (u32*)&info->permutation, 2);
}

static void DietBufInfoBind(u32 e, C3D_BufInfo* info) // C3Di_BufInfoBind
{
	C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_ATTRIBBUFFERS_LOC, 0xF), info->base_paddr >> 3);
	C3Dgdx_WriteN(e, C3DGDX_HDR_INC(GPUREG_ATTRIBBUFFER0_OFFSET), (u32*)info->buffers, sizeof(info->buffers)/sizeof(u32));
}

static void DietEffectBind(u32 e, C3D_Effect* fx) // C3Di_EffectBind
{
	C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_DEPTHMAP_ENABLE, 0xF), fx->zBuffer ? 1 : 0);
	C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_FACECULLING_CONFIG, 0xF), fx->cullMode & 0x3);
	C3Dgdx_WriteN(e, C3DGDX_HDR_INC(GPUREG_DEPTHMAP_SCALE), (u32*)&fx->zScale, 2);
	if (C3Dgdx_stencilMode) { // GDX (n60tail): [native] sky_late's stencil words
		u32 fx4[4] = { fx->alphaTest, fx->stencilMode, fx->stencilOp, fx->depthTest };
		C3Dgdx_StencilCheck(fx->fragOpMode, fx->alphaBlend, (u32)fx->clrLogicOp, fx4);
		C3Dgdx_WriteN(e, C3DGDX_HDR_INC(GPUREG_FRAGOP_ALPHA_TEST), fx4, 4);
	} else {
		C3Dgdx_WriteN(e, C3DGDX_HDR_INC(GPUREG_FRAGOP_ALPHA_TEST), (u32*)&fx->alphaTest, 4);
	}
	C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_GAS_DELTAZ_DEPTH, 0x8), (u32)GPU_MAKEGASDEPTHFUNC((fx->depthTest>>4)&7) << 24);
	C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_BLEND_COLOR, 0xF), fx->blendClr);
	C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_BLEND_FUNC, 0xF), fx->alphaBlend);
	C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_LOGIC_OP, 0xF), fx->clrLogicOp);
	C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_COLOR_OPERATION, 7), fx->fragOpMode);
	C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_FRAGOP_SHADOW, 0xF), fx->fragOpShadow);
	C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_EARLYDEPTH_TEST1, 1), fx->earlyDepth ? 1 : 0);
	C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_EARLYDEPTH_TEST2, 0xF), fx->earlyDepth ? 1 : 0);
	C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_EARLYDEPTH_FUNC, 1), fx->earlyDepthFunc);
	C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_EARLYDEPTH_DATA, 0x7), fx->earlyDepthRef);
}

// C3Di_SetTex for a 2D texture; cube maps go to the stock function.
static void DietSetTex(u32 e, int unit, C3D_Tex* tex)
{
	static const u16 borderReg[3] = { GPUREG_TEXUNIT0_BORDER_COLOR, GPUREG_TEXUNIT1_BORDER_COLOR, GPUREG_TEXUNIT2_BORDER_COLOR };
	static const u16 typeReg[3] = { GPUREG_TEXUNIT0_TYPE, GPUREG_TEXUNIT1_TYPE, GPUREG_TEXUNIT2_TYPE };
	u32 reg[5];

	if (!C3Di_TexIs2D(tex) || unit < 0 || unit > 2)
	{
		C3Di_SetTex(unit, tex);
		return;
	}
	reg[0] = tex->border;
	reg[1] = tex->dim;
	reg[2] = tex->param;
	reg[3] = tex->lodParam;
	reg[4] = osConvertVirtToPhys(tex->data) >> 3;
	C3Dgdx_WriteN(e, C3DGDX_HDR_INC(borderReg[unit]), reg, 5);
	C3Dgdx_Write1(e, C3DGDX_HDR(typeReg[unit], 0xF), tex->fmt);
}

static void DietTexEnvBind(u32 e, int id, C3D_TexEnv* env) // C3Di_TexEnvBind
{
	if (id >= 4)
	{
		id += 2;
	}
	C3Dgdx_WriteN(e, C3DGDX_HDR_INC(GPUREG_TEXENV0_SOURCE + id*8), (u32*)env, sizeof(C3D_TexEnv)/sizeof(u32));
}

// ---------------------------------------------------------------------------------------------
// C3Di_UpdateContext, diet form. The state part is stock's sequence with the diet binds; the
// uniform part is C3Di_GdxUpdateUniforms (uniforms.c) under UNIFSCAN.
// ---------------------------------------------------------------------------------------------

// Every ctx->flags bit C3Di_UpdateContext acts on.
#define C3DGDX_CTX_WORK_FLAGS (C3DiF_FrameBuf | C3DiF_Viewport | C3DiF_Scissor | C3DiF_Program \
	| C3DiF_AttrInfo | C3DiF_BufInfo | C3DiF_Effect | C3DiF_TexAll | C3DiF_TexStatus \
	| C3DiF_ProcTex | C3DiF_ProcTexColorLut | C3DiF_ProcTexLutAll | C3DiF_TexEnvBuf \
	| C3DiF_FogLut | C3DiF_TexEnvAll | C3DiF_LightEnv)

// True when stock C3Di_UpdateContext would do anything before its uniform updates.
static inline bool ContextHasWork(C3D_Context* ctx)
{
	return (ctx->flags & C3DGDX_CTX_WORK_FLAGS) || (ctx->texEnvBuf&7) == GPU_GAS
		|| ctx->lightEnv || ctx->fixedAttribDirty;
}

static void DietUpdateTextures(u32 e, C3D_Context* ctx)
{
	int i;
	if (ctx->flags & C3DiF_TexAll)
	{
		u32 units = 0;
		for (i = 0; i < 3; i ++)
		{
			if (ctx->tex[i])
			{
				units |= BIT(i);
				if (ctx->flags & C3DiF_Tex(i))
				{
					DietSetTex(e, i, ctx->tex[i]);
				}
			}
		}

		// Enable texture units and clear texture cache
		ctx->texConfig &= ~7;
		ctx->texConfig |= units | BIT(16);
		ctx->flags &= ~C3DiF_TexAll;
		ctx->flags |= C3DiF_TexStatus;
	}

	if (ctx->flags & C3DiF_TexStatus)
	{
		ctx->flags &= ~C3DiF_TexStatus;
		C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_TEXUNIT_CONFIG, 0xB), ctx->texConfig);
		// Clear texture cache if requested *after* configuring texture units
		if (ctx->texConfig & BIT(16))
		{
			ctx->texConfig &= ~BIT(16);
			C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_TEXUNIT_CONFIG, 0x4), BIT(16));
		}
		C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_TEXUNIT0_SHADOW, 0xF), ctx->texShadow);
	}
}

static void DietUpdateCombiners(u32 e, C3D_Context* ctx)
{
	int i;
	if (ctx->flags & (C3DiF_ProcTex | C3DiF_ProcTexColorLut | C3DiF_ProcTexLutAll))
	{
		C3Di_ProcTexUpdate(ctx);
	}

	if (ctx->flags & C3DiF_TexEnvBuf)
	{
		ctx->flags &= ~C3DiF_TexEnvBuf;
		C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_TEXENV_UPDATE_BUFFER, 0x7), ctx->texEnvBuf);
		C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_TEXENV_BUFFER_COLOR, 0xF), ctx->texEnvBufClr);
		C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_FOG_COLOR, 0xF), ctx->fogClr);
	}

	if ((ctx->flags & C3DiF_FogLut) && (ctx->texEnvBuf&7) != GPU_NO_FOG)
	{
		ctx->flags &= ~C3DiF_FogLut;
		if (ctx->fogLut)
		{
			C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_FOG_LUT_INDEX, 0xF), 0);
			C3Dgdx_WriteN(e, C3DGDX_HDR(GPUREG_FOG_LUT_DATA0, 0xF), ctx->fogLut->data, 128);
		}
	}

	if ((ctx->texEnvBuf&7) == GPU_GAS)
	{
		C3Di_GasUpdate(ctx);
	}

	if (ctx->flags & C3DiF_TexEnvAll)
	{
		for (i = 0; i < 6; i ++)
		{
			if (!(ctx->flags & C3DiF_TexEnv(i)))
			{
				continue;
			}
			DietTexEnvBind(e, i, &ctx->texEnv[i]);
		}
		ctx->flags &= ~C3DiF_TexEnvAll;
	}
}

static void DietUpdateLightAndFixed(u32 e, C3D_Context* ctx)
{
	int i;
	C3D_LightEnv* env = ctx->lightEnv;

	if (ctx->flags & C3DiF_LightEnv)
	{
		u32 enable = env != NULL;
		C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_LIGHTING_ENABLE0, 0xF), enable);
		C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_LIGHTING_ENABLE1, 0xF), !enable);
		ctx->flags &= ~C3DiF_LightEnv;
	}

	if (env)
	{
		C3Di_LightEnvUpdate(env);
	}

	if (ctx->fixedAttribDirty)
	{
		for (i = 0; i < 12; i ++)
		{
			if (!(ctx->fixedAttribDirty & BIT(i)))
			{
				continue;
			}
			C3D_FVec* v = &ctx->fixedAttribs[i];

			C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_FIXEDATTRIB_INDEX, 0xF), i);
			C3D_ImmSendAttrib(v->x, v->y, v->z, v->w);
		}
		ctx->fixedAttribDirty = 0;
	}
}

static void DietUpdateState(u32 e, C3D_Context* ctx)
{
	if (ctx->flags & C3DiF_FrameBuf)
	{
		ctx->flags &= ~C3DiF_FrameBuf;
		if (ctx->flags & C3DiF_DrawUsed)
		{
			ctx->flags &= ~C3DiF_DrawUsed;
			C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_FRAMEBUFFER_FLUSH, 0xF), 1);
			C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_EARLYDEPTH_CLEAR, 0xF), 1);
		}
		C3Di_FrameBufBind(&ctx->fb);
	}

	if (ctx->flags & C3DiF_Viewport)
	{
		ctx->flags &= ~C3DiF_Viewport;
		C3Dgdx_WriteN(e, C3DGDX_HDR_INC(GPUREG_VIEWPORT_WIDTH), ctx->viewport, 4);
		C3Dgdx_Write1(e, C3DGDX_HDR(GPUREG_VIEWPORT_XY, 0xF), ctx->viewport[4]);
	}

	if (ctx->flags & C3DiF_Scissor)
	{
		ctx->flags &= ~C3DiF_Scissor;
		C3Dgdx_WriteN(e, C3DGDX_HDR_INC(GPUREG_SCISSORTEST_MODE), ctx->scissor, 3);
	}

	if (ctx->flags & C3DiF_Program)
	{
		shaderProgramConfigure(ctx->program, (ctx->flags & C3DiF_VshCode) != 0, (ctx->flags & C3DiF_GshCode) != 0);
		ctx->flags &= ~(C3DiF_Program | C3DiF_VshCode | C3DiF_GshCode);
	}

	if (ctx->flags & C3DiF_AttrInfo)
	{
		ctx->flags &= ~C3DiF_AttrInfo;
		DietAttrInfoBind(e, &ctx->attrInfo);
	}

	if (ctx->flags & C3DiF_BufInfo)
	{
		ctx->flags &= ~C3DiF_BufInfo;
		DietBufInfoBind(e, &ctx->bufInfo);
	}

	if (ctx->flags & C3DiF_Effect)
	{
		ctx->flags &= ~C3DiF_Effect;
		DietEffectBind(e, &ctx->effect);
	}

	DietUpdateTextures(e, ctx);
	DietUpdateCombiners(e, ctx);
	DietUpdateLightAndFixed(e, ctx);
}

static void DietUpdateContext(u32 mode)
{
	C3D_Context* ctx = C3Di_GetContext();
	u32 e = mode & C3DGDX_DIET_EMIT;

	if (!(mode & C3DGDX_DIET_FLAGSKIP) || ContextHasWork(ctx))
	{
		DietUpdateState(e, ctx);
	}

	if (mode & C3DGDX_DIET_UNIFSCAN)
	{
		C3Di_GdxUpdateUniforms(GPU_VERTEX_SHADER, mode);
		C3Di_GdxUpdateUniforms(GPU_GEOMETRY_SHADER, mode);
	}
	else
	{
		C3D_UpdateUniforms(GPU_VERTEX_SHADER);
		C3D_UpdateUniforms(GPU_GEOMETRY_SHADER);
	}
}

// ---------------------------------------------------------------------------------------------
// GDX (n60core2): the NRR direct emitter's hand-off sync (gdx/c3d_gdx.h).
// ---------------------------------------------------------------------------------------------

// ContextHasWork, less a fog LUT that stays dirty while the fog unit is off (C3Di_UpdateContext
// emits it only once fog is on, so with fog off the next draw emits nothing for it).
int C3Dgdx_StatePending(void)
{
	C3D_Context* ctx = C3Di_GetContext();
	u32 work = ctx->flags & C3DGDX_CTX_WORK_FLAGS;
	if ((ctx->texEnvBuf&7) == GPU_NO_FOG)
	{
		work &= ~C3DiF_FogLut;
	}
	return work || (ctx->texEnvBuf&7) == GPU_GAS || ctx->lightEnv || ctx->fixedAttribDirty
		|| C3Di_GdxUniformsPending(GPU_VERTEX_SHADER) || C3Di_GdxUniformsPending(GPU_GEOMETRY_SHADER);
}

// Everything a context update changes (the context, the uniform module's statics, ELIDE's
// shadow): the NRR verify runs the direct emitter as a shadow between a save and a load.
typedef struct
{
	C3D_Context ctx;
	C3Di_GdxUnifState unif;
	C3Di_GdxUnifShadow shadow;
} C3Dgdx_SavedState;


/* n60lead (hardware freeze, RUN V): the context's FIRST member is libctru's GX command queue,
 * whose counters (numEntries/curEntry/lastEntry) the GSP interrupt thread advances
 * asynchronously while the queue is live (overlap kick). A whole-context memcpy save/restore
 * can roll a completion back and leave the main thread waiting forever on an idle GPU (the
 * emulator's HLE GSP completes synchronously and never shows it). Save/restore everything
 * AFTER the queue; the queue is never ours to restore. */
_Static_assert(offsetof(C3D_Context, gxQueue) == 0, "gxQueue must be the first C3D_Context member");
#define GDX_CTX_KEEP_OFF offsetof(C3D_Context, cmdBuf)
/* n60hwdiag: the copy's (unused) queue words hold the live queue's counters at the save, so the
 * load can count what the GPU completed inside the window (C3Dgdx_QueueWindowStats). */
static unsigned s_qWindows, s_qCompleted, s_qChanged;
static void GdxCtxSaveNoQueue(C3D_Context* dst)
{
	const volatile gxCmdQueue_s* q = &C3Di_GetContext()->gxQueue;
	memcpy((u8*)dst + GDX_CTX_KEEP_OFF, (const u8*)C3Di_GetContext() + GDX_CTX_KEEP_OFF,
	       sizeof(C3D_Context) - GDX_CTX_KEEP_OFF);
	dst->gxQueue.numEntries = q->numEntries;
	dst->gxQueue.lastEntry = q->lastEntry;
}
static void GdxCtxNoteWindow(const C3D_Context* src)
{
	const volatile gxCmdQueue_s* q = &C3Di_GetContext()->gxQueue;
	const u16 num = q->numEntries;
	const u16 last = q->lastEntry;
	s_qWindows++;
	if (num == src->gxQueue.numEntries && last == src->gxQueue.lastEntry)
		return;
	if (num == src->gxQueue.numEntries && last > src->gxQueue.lastEntry)
		s_qCompleted++;
	else
		s_qChanged++;
}
static void GdxCtxLoadNoQueue(const C3D_Context* src)
{
	GdxCtxNoteWindow(src);
	memcpy((u8*)C3Di_GetContext() + GDX_CTX_KEEP_OFF, (const u8*)src + GDX_CTX_KEEP_OFF,
	       sizeof(C3D_Context) - GDX_CTX_KEEP_OFF);
}

void C3Dgdx_QueueWindowStats(unsigned out[3])
{
	out[0] = s_qWindows;
	out[1] = s_qCompleted;
	out[2] = s_qChanged;
}

/* n60hwdiag: [debug] gxq_selftest -- the next infinite wait, once the queue really finished, reports
 * two synthetic empty slices through the stuck hook (exercises the reporting path; the emulator's
 * synchronous GSP never produces a real one). */
static int s_qSelftest = 0;
void C3Dgdx_QueueSelftestArm(void)
{
	s_qSelftest = 1;
}

/* n60hwdiag: the bounded GX queue wait (c3d_gdx.h). */
int C3Dgdx_QueueWatchMs = 0;
void (*C3Dgdx_QueueStuckHook)(const char* where, const struct tag_gxCmdQueue_s* q, unsigned waitedMs,
                              int forcing) = NULL;

bool C3Dgdx_QueueWait(gxCmdQueue_s* q, long long timeout, const char* where)
{
	if (timeout >= 0 || C3Dgdx_QueueWatchMs <= 0)
		return gxCmdQueueWait(q, timeout);
	/* Never forced: libctru's gxCmdQueueStop only REQUESTS a stop while a command is marked running,
	   and gxCmdQueueClear svcBreaks on the bound queue while it is running, so there is no safe way
	   to abandon a wedged queue. The watch only reports; the wait goes on (stock semantics). */
	const s64 slice = (s64)C3Dgdx_QueueWatchMs * 1000000LL;
	for (unsigned n = 1;; n++)
	{
		if (gxCmdQueueWait(q, slice))
			break;
		if (C3Dgdx_QueueStuckHook && (n <= 5 || n % 15 == 0))
			C3Dgdx_QueueStuckHook(where, q, n * (unsigned)C3Dgdx_QueueWatchMs, 0);
	}
	if (s_qSelftest)
	{
		s_qSelftest = 0;
		if (C3Dgdx_QueueStuckHook)
		{
			C3Dgdx_QueueStuckHook("SELFTEST (synthetic, the queue had finished)", q, 1u * (unsigned)C3Dgdx_QueueWatchMs, 0);
			C3Dgdx_QueueStuckHook("SELFTEST (synthetic, the queue had finished)", q, 2u * (unsigned)C3Dgdx_QueueWatchMs, 0);
		}
	}
	return true;
}

unsigned C3Dgdx_StateBytes(void)
{
	return sizeof(C3Dgdx_SavedState);
}

void C3Dgdx_StateSave(void* buf)
{
	C3Dgdx_SavedState* s = (C3Dgdx_SavedState*)buf;
	GdxCtxSaveNoQueue(&s->ctx);
	C3Di_GdxUnifSave(&s->unif);
	C3Di_GdxShadowSave(&s->shadow);
}

void C3Dgdx_StateLoad(const void* buf)
{
	const C3Dgdx_SavedState* s = (const C3Dgdx_SavedState*)buf;
	GdxCtxLoadNoQueue(&s->ctx);
	C3Di_GdxUnifLoad(&s->unif);
	C3Di_GdxShadowLoad(&s->shadow);
}

int C3Dgdx_UniformsPending(int shaderType)
{
	return C3Di_GdxUniformsPending(shaderType ? GPU_GEOMETRY_SHADER : GPU_VERTEX_SHADER) ? 1 : 0;
}

void C3Dgdx_FlushState(void)
{
	u32 diet = C3Dgdx_mode & C3DGDX_DIET_MASK;
	if (diet)
	{
		DietUpdateContext(diet);
	}
	else
	{
		C3Di_UpdateContext();
	}
}

// ---------------------------------------------------------------------------------------------
// The draw calls' own packets (stock: drawElements.c / drawArrays.c after C3Di_UpdateContext).
// ---------------------------------------------------------------------------------------------

static inline void PutPair(u32* p, int k, u32 header, u32 param)
{
	p[2*k] = param;
	p[2*k + 1] = header;
}

static void ElementsPacketsStockCalls(GPU_Primitive_t primitive, int count, u32 indexConfig)
{
	GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 2, primitive != GPU_TRIANGLES ? primitive : GPU_GEOMETRY_PRIM);
	GPUCMD_AddWrite(GPUREG_RESTART_PRIMITIVE, 1);
	GPUCMD_AddWrite(GPUREG_INDEXBUFFER_CONFIG, indexConfig);
	GPUCMD_AddWrite(GPUREG_NUMVERTICES, count);
	GPUCMD_AddWrite(GPUREG_VERTEX_OFFSET, 0);
	if (primitive == GPU_TRIANGLES)
	{
		GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 2, 0x100);
		GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 2, 0x100);
	}
	GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 0);
	GPUCMD_AddWrite(GPUREG_DRAWELEMENTS, 1);
	GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 1);
	if (primitive == GPU_TRIANGLES)
	{
		GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 2, 0);
		GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 2, 0);
	}
	GPUCMD_AddWrite(GPUREG_VTX_FUNC, 1);
	GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
	GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
}

// The same 11 (or 15, for triangles) single-parameter packets, one bounds check.
static void ElementsPacketsInline(GPU_Primitive_t primitive, int count, u32 indexConfig)
{
	bool tris = primitive == GPU_TRIANGLES;
	u32* p = C3Dgdx_Reserve(tris ? 30 : 22);
	int k = 0;
	PutPair(p, k++, C3DGDX_HDR(GPUREG_PRIMITIVE_CONFIG, 2), tris ? GPU_GEOMETRY_PRIM : primitive);
	PutPair(p, k++, C3DGDX_HDR(GPUREG_RESTART_PRIMITIVE, 0xF), 1);
	PutPair(p, k++, C3DGDX_HDR(GPUREG_INDEXBUFFER_CONFIG, 0xF), indexConfig);
	PutPair(p, k++, C3DGDX_HDR(GPUREG_NUMVERTICES, 0xF), count);
	PutPair(p, k++, C3DGDX_HDR(GPUREG_VERTEX_OFFSET, 0xF), 0);
	if (tris)
	{
		PutPair(p, k++, C3DGDX_HDR(GPUREG_GEOSTAGE_CONFIG, 2), 0x100);
		PutPair(p, k++, C3DGDX_HDR(GPUREG_GEOSTAGE_CONFIG2, 2), 0x100);
	}
	PutPair(p, k++, C3DGDX_HDR(GPUREG_START_DRAW_FUNC0, 1), 0);
	PutPair(p, k++, C3DGDX_HDR(GPUREG_DRAWELEMENTS, 0xF), 1);
	PutPair(p, k++, C3DGDX_HDR(GPUREG_START_DRAW_FUNC0, 1), 1);
	if (tris)
	{
		PutPair(p, k++, C3DGDX_HDR(GPUREG_GEOSTAGE_CONFIG, 2), 0);
		PutPair(p, k++, C3DGDX_HDR(GPUREG_GEOSTAGE_CONFIG2, 2), 0);
	}
	PutPair(p, k++, C3DGDX_HDR(GPUREG_VTX_FUNC, 0xF), 1);
	PutPair(p, k++, C3DGDX_HDR(GPUREG_PRIMITIVE_CONFIG, 0x8), 0);
	PutPair(p, k++, C3DGDX_HDR(GPUREG_PRIMITIVE_CONFIG, 0x8), 0);
}

static void ArraysPacketsStockCalls(GPU_Primitive_t primitive, int first, int size)
{
	GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 2, primitive);
	GPUCMD_AddWrite(GPUREG_RESTART_PRIMITIVE, 1);
	GPUCMD_AddWrite(GPUREG_INDEXBUFFER_CONFIG, 0x80000000);
	GPUCMD_AddWrite(GPUREG_NUMVERTICES, size);
	GPUCMD_AddWrite(GPUREG_VERTEX_OFFSET, first);
	GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 1, 1);
	GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 0);
	GPUCMD_AddWrite(GPUREG_DRAWARRAYS, 1);
	GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 1);
	GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 1, 0);
	GPUCMD_AddWrite(GPUREG_VTX_FUNC, 1);
}

static void ArraysPacketsInline(GPU_Primitive_t primitive, int first, int size)
{
	u32* p = C3Dgdx_Reserve(22);
	PutPair(p, 0, C3DGDX_HDR(GPUREG_PRIMITIVE_CONFIG, 2), primitive);
	PutPair(p, 1, C3DGDX_HDR(GPUREG_RESTART_PRIMITIVE, 0xF), 1);
	PutPair(p, 2, C3DGDX_HDR(GPUREG_INDEXBUFFER_CONFIG, 0xF), 0x80000000);
	PutPair(p, 3, C3DGDX_HDR(GPUREG_NUMVERTICES, 0xF), size);
	PutPair(p, 4, C3DGDX_HDR(GPUREG_VERTEX_OFFSET, 0xF), first);
	PutPair(p, 5, C3DGDX_HDR(GPUREG_GEOSTAGE_CONFIG2, 1), 1);
	PutPair(p, 6, C3DGDX_HDR(GPUREG_START_DRAW_FUNC0, 1), 0);
	PutPair(p, 7, C3DGDX_HDR(GPUREG_DRAWARRAYS, 0xF), 1);
	PutPair(p, 8, C3DGDX_HDR(GPUREG_START_DRAW_FUNC0, 1), 1);
	PutPair(p, 9, C3DGDX_HDR(GPUREG_GEOSTAGE_CONFIG2, 1), 0);
	PutPair(p, 10, C3DGDX_HDR(GPUREG_VTX_FUNC, 0xF), 1);
}

// Stock: (pa - base) | (type << 31), with type an int (0 or 1: C3D_UNSIGNED_BYTE / _SHORT).
static inline u32 IndexConfig(u32 pa, u32 base, int type)
{
	return (pa - base) | ((u32)type << 31);
}

static void DietDrawElements(u32 mode, GPU_Primitive_t primitive, int count, int type, const void* indices)
{
	C3D_Context* ctx = C3Di_GetContext();
	u32 pa = osConvertVirtToPhys(indices);
	u32 base = ctx->bufInfo.base_paddr;
	if (pa < base)
	{
		return;
	}

	DietUpdateContext(mode);

	if (mode & C3DGDX_DIET_EMIT)
	{
		ElementsPacketsInline(primitive, count, IndexConfig(pa, base, type));
	}
	else
	{
		ElementsPacketsStockCalls(primitive, count, IndexConfig(pa, base, type));
	}
	ctx->flags |= C3DiF_DrawUsed;
}

static void DietDrawArrays(u32 mode, GPU_Primitive_t primitive, int first, int size)
{
	DietUpdateContext(mode);

	if (mode & C3DGDX_DIET_EMIT)
	{
		ArraysPacketsInline(primitive, first, size);
	}
	else
	{
		ArraysPacketsStockCalls(primitive, first, size);
	}
	C3Di_GetContext()->flags |= C3DiF_DrawUsed;
}

// ---------------------------------------------------------------------------------------------
// C3DGDX_PROF_SPLIT: stock C3Di_UpdateContext (base.c), verbatim apart from a tick read and a
// word count at each section boundary. It calls the stock binds and C3D_UpdateUniforms.
// ---------------------------------------------------------------------------------------------

typedef struct
{
	u64 last;
	u32 lastOffset;
} SplitClock;

static void SplitMark(SplitClock* c, int sec)
{
	u64 now = C3Dgdx_Now();
	u32 words = gpuCmdBufOffset - c->lastOffset;
	s_stats.secTicks[sec] += now - c->last;
	s_stats.secWords[sec] += words;
	if (words)
	{
		s_stats.secFired[sec] ++;
	}
	c->last = now;
	c->lastOffset = gpuCmdBufOffset;
}

static void SplitClockStart(SplitClock* c)
{
	c->lastOffset = gpuCmdBufOffset;
	c->last = C3Dgdx_Now();
}

static void SplitUpdateContext(SplitClock* clk)
{
	int i;
	C3D_Context* ctx = C3Di_GetContext();

	if (ctx->flags & C3DiF_FrameBuf)
	{
		ctx->flags &= ~C3DiF_FrameBuf;
		if (ctx->flags & C3DiF_DrawUsed)
		{
			ctx->flags &= ~C3DiF_DrawUsed;
			GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
			GPUCMD_AddWrite(GPUREG_EARLYDEPTH_CLEAR, 1);
		}
		C3Di_FrameBufBind(&ctx->fb);
	}

	if (ctx->flags & C3DiF_Viewport)
	{
		ctx->flags &= ~C3DiF_Viewport;
		GPUCMD_AddIncrementalWrites(GPUREG_VIEWPORT_WIDTH, ctx->viewport, 4);
		GPUCMD_AddWrite(GPUREG_VIEWPORT_XY, ctx->viewport[4]);
	}

	if (ctx->flags & C3DiF_Scissor)
	{
		ctx->flags &= ~C3DiF_Scissor;
		GPUCMD_AddIncrementalWrites(GPUREG_SCISSORTEST_MODE, ctx->scissor, 3);
	}
	SplitMark(clk, C3DGDX_SEC_FB);

	if (ctx->flags & C3DiF_Program)
	{
		shaderProgramConfigure(ctx->program, (ctx->flags & C3DiF_VshCode) != 0, (ctx->flags & C3DiF_GshCode) != 0);
		ctx->flags &= ~(C3DiF_Program | C3DiF_VshCode | C3DiF_GshCode);
	}
	SplitMark(clk, C3DGDX_SEC_PROGRAM);

	if (ctx->flags & C3DiF_AttrInfo)
	{
		ctx->flags &= ~C3DiF_AttrInfo;
		C3Di_AttrInfoBind(&ctx->attrInfo);
	}
	SplitMark(clk, C3DGDX_SEC_ATTR);

	if (ctx->flags & C3DiF_BufInfo)
	{
		ctx->flags &= ~C3DiF_BufInfo;
		C3Di_BufInfoBind(&ctx->bufInfo);
	}
	SplitMark(clk, C3DGDX_SEC_BUF);

	if (ctx->flags & C3DiF_Effect)
	{
		ctx->flags &= ~C3DiF_Effect;
		C3Di_EffectBind(&ctx->effect);
	}
	SplitMark(clk, C3DGDX_SEC_EFFECT);

	if (ctx->flags & C3DiF_TexAll)
	{
		u32 units = 0;
		for (i = 0; i < 3; i ++)
		{
			if (ctx->tex[i])
			{
				units |= BIT(i);
				if (ctx->flags & C3DiF_Tex(i))
				{
					C3Di_SetTex(i, ctx->tex[i]);
				}
			}
		}

		// Enable texture units and clear texture cache
		ctx->texConfig &= ~7;
		ctx->texConfig |= units | BIT(16);
		ctx->flags &= ~C3DiF_TexAll;
		ctx->flags |= C3DiF_TexStatus;
	}

	if (ctx->flags & C3DiF_TexStatus)
	{
		ctx->flags &= ~C3DiF_TexStatus;
		GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0xB, ctx->texConfig);
		// Clear texture cache if requested *after* configuring texture units
		if (ctx->texConfig & BIT(16))
		{
			ctx->texConfig &= ~BIT(16);
			GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0x4, BIT(16));
		}
		GPUCMD_AddWrite(GPUREG_TEXUNIT0_SHADOW, ctx->texShadow);
	}
	SplitMark(clk, C3DGDX_SEC_TEX);

	if (ctx->flags & (C3DiF_ProcTex | C3DiF_ProcTexColorLut | C3DiF_ProcTexLutAll))
	{
		C3Di_ProcTexUpdate(ctx);
	}

	if (ctx->flags & C3DiF_TexEnvBuf)
	{
		ctx->flags &= ~C3DiF_TexEnvBuf;
		GPUCMD_AddMaskedWrite(GPUREG_TEXENV_UPDATE_BUFFER, 0x7, ctx->texEnvBuf);
		GPUCMD_AddWrite(GPUREG_TEXENV_BUFFER_COLOR, ctx->texEnvBufClr);
		GPUCMD_AddWrite(GPUREG_FOG_COLOR, ctx->fogClr);
	}

	if ((ctx->flags & C3DiF_FogLut) && (ctx->texEnvBuf&7) != GPU_NO_FOG)
	{
		ctx->flags &= ~C3DiF_FogLut;
		if (ctx->fogLut)
		{
			GPUCMD_AddWrite(GPUREG_FOG_LUT_INDEX, 0);
			GPUCMD_AddWrites(GPUREG_FOG_LUT_DATA0, ctx->fogLut->data, 128);
		}
	}

	if ((ctx->texEnvBuf&7) == GPU_GAS)
	{
		C3Di_GasUpdate(ctx);
	}
	SplitMark(clk, C3DGDX_SEC_TEXENVBUF);

	if (ctx->flags & C3DiF_TexEnvAll)
	{
		for (i = 0; i < 6; i ++)
		{
			if (!(ctx->flags & C3DiF_TexEnv(i)))
			{
				continue;
			}
			C3Di_TexEnvBind(i, &ctx->texEnv[i]);
		}
		ctx->flags &= ~C3DiF_TexEnvAll;
	}
	SplitMark(clk, C3DGDX_SEC_TEXENV);

	C3D_LightEnv* env = ctx->lightEnv;

	if (ctx->flags & C3DiF_LightEnv)
	{
		u32 enable = env != NULL;
		GPUCMD_AddWrite(GPUREG_LIGHTING_ENABLE0, enable);
		GPUCMD_AddWrite(GPUREG_LIGHTING_ENABLE1, !enable);
		ctx->flags &= ~C3DiF_LightEnv;
	}

	if (env)
	{
		C3Di_LightEnvUpdate(env);
	}

	if (ctx->fixedAttribDirty)
	{
		for (i = 0; i < 12; i ++)
		{
			if (!(ctx->fixedAttribDirty & BIT(i)))
			{
				continue;
			}
			C3D_FVec* v = &ctx->fixedAttribs[i];

			GPUCMD_AddWrite(GPUREG_FIXEDATTRIB_INDEX, i);
			C3D_ImmSendAttrib(v->x, v->y, v->z, v->w);
		}
		ctx->fixedAttribDirty = 0;
	}
	SplitMark(clk, C3DGDX_SEC_LIGHTFIX);

	C3D_UpdateUniforms(GPU_VERTEX_SHADER);
	SplitMark(clk, C3DGDX_SEC_VSUNIF);
	C3D_UpdateUniforms(GPU_GEOMETRY_SHADER);
	SplitMark(clk, C3DGDX_SEC_GSUNIF);
}

GDX_COLD static void SplitDrawElements(GPU_Primitive_t primitive, int count, int type, const void* indices)
{
	SplitClock clk;
	C3D_Context* ctx = C3Di_GetContext();
	SplitClockStart(&clk);
	u32 pa = osConvertVirtToPhys(indices);
	u32 base = ctx->bufInfo.base_paddr;
	if (pa < base)
	{
		return;
	}
	SplitMark(&clk, C3DGDX_SEC_DRAW);
	SplitUpdateContext(&clk);
	ElementsPacketsStockCalls(primitive, count, IndexConfig(pa, base, type));
	ctx->flags |= C3DiF_DrawUsed;
	SplitMark(&clk, C3DGDX_SEC_DRAW);
	s_stats.splitDraws ++;
}

GDX_COLD static void SplitDrawArrays(GPU_Primitive_t primitive, int first, int size)
{
	SplitClock clk;
	SplitClockStart(&clk);
	SplitMark(&clk, C3DGDX_SEC_DRAW); // two DRAW intervals per draw, like elements
	SplitUpdateContext(&clk);
	ArraysPacketsStockCalls(primitive, first, size);
	C3Di_GetContext()->flags |= C3DiF_DrawUsed;
	SplitMark(&clk, C3DGDX_SEC_DRAW);
	s_stats.splitDraws ++;
}

// ---------------------------------------------------------------------------------------------
// Uniform reuse census (profile modes only): how many of the vertex-shader vec4s about to be
// uploaded hold the value the last upload of that register carried. Sizes the "skip identical
// uniform writes" idea; shader-constant uploads at program bind are not tracked.
// ---------------------------------------------------------------------------------------------

static C3D_FVec s_lastUpload[C3D_FVUNIF_COUNT];
static u8 s_lastUploadValid[C3D_FVUNIF_COUNT];

GDX_COLD static void CountUniformReuse(void)
{
	const u8* dirty = (const u8*)C3D_FVUnifDirty[GPU_VERTEX_SHADER];
	int i = C3Dgdx_NextDirty(dirty, 0, C3D_FVUNIF_COUNT);
	while (i < C3D_FVUNIF_COUNT)
	{
		int j = C3Dgdx_RunEnd(dirty, i, C3D_FVUNIF_COUNT);
		int k;
		for (k = i; k < j; k ++)
		{
			const C3D_FVec* v = &C3D_FVUnif[GPU_VERTEX_SHADER][k];
			if (s_lastUploadValid[k] && memcmp(v, &s_lastUpload[k], sizeof(*v)) == 0)
			{
				s_stats.unifSame ++;
			}
			s_lastUpload[k] = *v;
			s_lastUploadValid[k] = 1;
		}
		s_stats.unifVec4 += j - i;
		i = C3Dgdx_NextDirty(dirty, j, C3D_FVUNIF_COUNT);
	}
}

// ---------------------------------------------------------------------------------------------
// C3DGDX_VERIFY: run the stock draw and the diet draw from the same citro3d state; compare the
// words each appended and the state each left. The stock result is kept.
// ---------------------------------------------------------------------------------------------

#define C3DGDX_VERIFY_MAX_WORDS 2048
#define C3DGDX_SELFTEST_PERIOD 997

typedef struct
{
	C3D_Context ctx;
	C3Di_GdxUnifState unif;
} Snapshot;

static Snapshot s_snapBefore, s_snapStock, s_snapDiet;
static C3Di_GdxUnifShadow s_shadowBefore;
static u32 s_stockWords[C3DGDX_VERIFY_MAX_WORDS];

static void SnapshotSave(Snapshot* s)
{
	memset(s, 0, sizeof(*s));
	GdxCtxSaveNoQueue(&s->ctx);
	C3Di_GdxUnifSave(&s->unif);
}

static void SnapshotLoad(const Snapshot* s)
{
	GdxCtxLoadNoQueue(&s->ctx);
	C3Di_GdxUnifLoad(&s->unif);
}

// State the snapshot does not hold: the light environment, procedural texture and gas LUT
// objects that their update hooks change. Draws with any of them bound are not compared.
static bool VerifySupported(void)
{
	C3D_Context* ctx = C3Di_GetContext();
	return !ctx->lightEnv && !ctx->procTex && (ctx->texEnvBuf&7) != GPU_GAS;
}

static void NoteWordMismatch(const u32* diet, u32 dietWords, u32 stockWords)
{
	u32 n = dietWords < stockWords ? dietWords : stockWords;
	u32 k = 0;
	while (k < n && diet[k] == s_stockWords[k])
	{
		k ++;
	}
	s_stats.mismatchWords ++;
	if (s_stats.firstBadDraw == 0)
	{
		s_stats.firstBadDraw = s_drawIndex;
		s_stats.firstBadWord = k;
		s_stats.firstBadStock = k < stockWords ? s_stockWords[k] : 0xFFFFFFFF;
		s_stats.firstBadDiet = k < dietWords ? diet[k] : 0xFFFFFFFF;
	}
}

// ---- ELIDE's semantic compare: a model of the VS float-uniform register file ----

#define UNIF_UNKNOWN 0
#define UNIF_F32     1
#define UNIF_F24     2

typedef struct
{
	u32 word[C3D_FVUNIF_COUNT][4];
	u8 kind[C3D_FVUNIF_COUNT];
	u32 index, component;
	bool f32;
} UnifModel;

typedef struct
{
	UnifModel model;
	UnifModel atDraw;   // the file when the first draw trigger is seen
	bool sawDraw;
	u32 other[C3DGDX_VERIFY_MAX_WORDS]; // every packet that is not a VS float-uniform write
	u32 otherWords;
	bool malformed;
} StreamView;

static StreamView s_viewStock, s_viewDiet;

static void ModelFromShadow(UnifModel* m, const C3Di_GdxUnifShadow* sh)
{
	int i;
	memset(m, 0, sizeof(*m));
	for (i = 0; i < C3D_FVUNIF_COUNT; i ++)
	{
		if (sh->valid[i])
		{
			memcpy(m->word[i], &sh->value[i], sizeof(m->word[i]));
			m->kind[i] = UNIF_F32;
		}
	}
}

static void ModelWrite(UnifModel* m, u32 reg, u32 value)
{
	if (reg == GPUREG_VSH_FLOATUNIFORM_CONFIG)
	{
		m->f32 = (value & 0x80000000) != 0;
		m->index = value & 0xFF;
		m->component = 0;
		return;
	}
	// GPUREG_VSH_FLOATUNIFORM_DATA .. +7 all feed the same data port
	u32 perVec = m->f32 ? 4 : 3;
	if (m->index >= C3D_FVUNIF_COUNT)
	{
		return; // out of range: the PICA ignores it, so does the model
	}
	m->word[m->index][m->component] = value;
	m->component ++;
	if (m->component == perVec)
	{
		if (!m->f32)
		{
			m->word[m->index][3] = 0;
		}
		m->kind[m->index] = m->f32 ? UNIF_F32 : UNIF_F24;
		m->index ++;
		m->component = 0;
	}
}

static bool IsVsFloatUniformReg(u32 reg)
{
	return reg >= GPUREG_VSH_FLOATUNIFORM_CONFIG && reg <= GPUREG_VSH_FLOATUNIFORM_DATA + 7;
}

static bool IsDrawTrigger(u32 reg)
{
	return reg == GPUREG_DRAWARRAYS || reg == GPUREG_DRAWELEMENTS;
}

// Walk one packet stream (GPUCMD_AddInternal layout). VS float-uniform packets update the model;
// every other packet is copied to `other` whole, header included.
static void ViewStream(StreamView* v, const u32* words, u32 n, const C3Di_GdxUnifShadow* before)
{
	u32 p = 0;
	ModelFromShadow(&v->model, before);
	v->sawDraw = false;
	v->otherWords = 0;
	v->malformed = false;
	while (p + 1 < n)
	{
		u32 header = words[p + 1];
		u32 reg = header & 0x3FF;
		u32 count = ((header >> 20) & 0xFF) + 1;
		bool incremental = (header >> 31) != 0;
		u32 packetWords = count + 1 + ((count - 1) & 1);
		u32 k;
		if (p + packetWords > n)
		{
			v->malformed = true;
			return;
		}
		u32 lastReg = incremental ? reg + count - 1 : reg;
		if (IsVsFloatUniformReg(reg) || IsVsFloatUniformReg(lastReg))
		{
			if (!IsVsFloatUniformReg(reg) || !IsVsFloatUniformReg(lastReg))
			{
				v->malformed = true; // straddles the uniform block: not a shape citro3d emits
				return;
			}
			for (k = 0; k < count; k ++)
			{
				u32 value = k == 0 ? words[p] : words[p + 1 + k];
				ModelWrite(&v->model, incremental ? reg + k : reg, value);
			}
		}
		else
		{
			if (v->otherWords + packetWords > C3DGDX_VERIFY_MAX_WORDS)
			{
				v->malformed = true;
				return;
			}
			memcpy(&v->other[v->otherWords], &words[p], packetWords * sizeof(u32));
			v->otherWords += packetWords;
			if (!v->sawDraw && (IsDrawTrigger(reg) || (incremental && reg <= GPUREG_DRAWELEMENTS && lastReg >= GPUREG_DRAWARRAYS)))
			{
				v->atDraw = v->model;
				v->sawDraw = true;
			}
		}
		p += packetWords;
	}
	if (p != n)
	{
		v->malformed = true;
	}
}

static bool ModelsEqual(const UnifModel* a, const UnifModel* b)
{
	int i;
	for (i = 0; i < C3D_FVUNIF_COUNT; i ++)
	{
		if (a->kind[i] != b->kind[i])
		{
			return false;
		}
		if (a->kind[i] != UNIF_UNKNOWN && memcmp(a->word[i], b->word[i], sizeof(a->word[i])) != 0)
		{
			return false;
		}
	}
	return true;
}

// Same GPU effect: identical non-uniform packets in the same order, and the same VS float
// uniform file at the first draw trigger and at the end of the draw's words.
static bool StreamsEquivalent(const u32* diet, u32 dietWords, const u32* stock, u32 stockWords)
{
	ViewStream(&s_viewStock, stock, stockWords, &s_shadowBefore);
	ViewStream(&s_viewDiet, diet, dietWords, &s_shadowBefore);
	if (s_viewStock.malformed || s_viewDiet.malformed)
	{
		return false;
	}
	if (s_viewStock.otherWords != s_viewDiet.otherWords
		|| memcmp(s_viewStock.other, s_viewDiet.other, s_viewStock.otherWords * sizeof(u32)) != 0)
	{
		return false;
	}
	if (s_viewStock.sawDraw != s_viewDiet.sawDraw)
	{
		return false;
	}
	if (s_viewStock.sawDraw && !ModelsEqual(&s_viewStock.atDraw, &s_viewDiet.atDraw))
	{
		return false;
	}
	return ModelsEqual(&s_viewStock.model, &s_viewDiet.model);
}

typedef enum { VERIFY_ELEMENTS, VERIFY_ARRAYS } VerifyKind;

typedef struct
{
	VerifyKind kind;
	GPU_Primitive_t primitive;
	int a, b; // count/type (elements) or first/size (arrays)
	const void* indices;
} VerifyDraw;

static void RunStock(const VerifyDraw* d)
{
	if (d->kind == VERIFY_ELEMENTS)
	{
		C3Di_DrawElementsStock(d->primitive, d->a, d->b, d->indices);
	}
	else
	{
		C3Di_DrawArraysStock(d->primitive, d->a, d->b);
	}
}

static void RunDiet(u32 mode, const VerifyDraw* d)
{
	if (d->kind == VERIFY_ELEMENTS)
	{
		DietDrawElements(mode, d->primitive, d->a, d->b, d->indices);
	}
	else
	{
		DietDrawArrays(mode, d->primitive, d->a, d->b);
	}
}

static void SelfTestCorrupt(u32 mode, u32* words, u32 n)
{
	if ((mode & C3DGDX_VERIFY_SELFTEST) && n && (s_drawIndex % C3DGDX_SELFTEST_PERIOD) == 0)
	{
		words[n / 2] ^= 0x00000100; // flip a bit mid-stream: the detector must see it
	}
}

// With ELIDE the diet's words are what stays (the shadow then describes the GPU exactly); without
// it the two are byte-identical when they match, and stock's are kept either way.
GDX_COLD static void VerifyOne(u32 mode, const VerifyDraw* d)
{
	u32 diet = mode & C3DGDX_DIET_MASK;
	bool elide = (diet & C3DGDX_DIET_ELIDE) && (diet & C3DGDX_DIET_UNIFSCAN);
	if (!VerifySupported())
	{
		s_stats.unverifiable ++;
		RunStock(d);
		C3Dgdx_InvalidateUniformShadow();
		return;
	}

	u32* buf = gpuCmdBuf;
	u32 start = gpuCmdBufOffset;
	SnapshotSave(&s_snapBefore);
	C3Di_GdxShadowSave(&s_shadowBefore);
	RunStock(d);
	u32 stockWords = gpuCmdBufOffset - start;
	if (stockWords > C3DGDX_VERIFY_MAX_WORDS)
	{
		s_stats.unverifiable ++;
		C3Dgdx_InvalidateUniformShadow();
		return;
	}
	memcpy(s_stockWords, &buf[start], stockWords * sizeof(u32));
	SnapshotSave(&s_snapStock);

	SnapshotLoad(&s_snapBefore);
	gpuCmdBufOffset = start;
	RunDiet(diet, d);
	u32 dietWords = gpuCmdBufOffset - start;
	SnapshotSave(&s_snapDiet);
	SelfTestCorrupt(mode, &buf[start], dietWords);

	s_stats.verified ++;
	bool wordsEqual;
	if (elide)
	{
		wordsEqual = StreamsEquivalent(&buf[start], dietWords, s_stockWords, stockWords);
	}
	else
	{
		wordsEqual = dietWords == stockWords && memcmp(&buf[start], s_stockWords, stockWords * sizeof(u32)) == 0;
	}
	bool stateEqual = memcmp(&s_snapDiet, &s_snapStock, sizeof(Snapshot)) == 0;
	if (!wordsEqual)
	{
		NoteWordMismatch(&buf[start], dietWords, stockWords);
	}
	if (!stateEqual)
	{
		s_stats.mismatchState ++;
	}
	if (!wordsEqual || !stateEqual || !elide)
	{
		// Keep what stock citro3d would have left.
		memcpy(&buf[start], s_stockWords, stockWords * sizeof(u32));
		gpuCmdBufOffset = start + stockWords;
		SnapshotLoad(&s_snapStock);
		if (elide)
		{
			C3Dgdx_InvalidateUniformShadow();
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Entry points (drawElements.c / drawArrays.c call these when C3Dgdx_mode != 0).
// ---------------------------------------------------------------------------------------------

static void Dispatch(u32 mode, const VerifyDraw* d)
{
	u32 diet = mode & C3DGDX_DIET_MASK;
	u32 start = gpuCmdBufOffset;
	u32* buf = gpuCmdBuf;

	s_drawIndex ++;
	s_stats.draws ++;
	if (!diet)
	{
		s_stats.drawsStock ++;
	}
	if (GDX_UNLIKELY(mode & (C3DGDX_PROF_SPLIT | C3DGDX_PROF_TOTAL)))
	{
		CountUniformReuse();
	}

	if (GDX_UNLIKELY(mode & C3DGDX_VERIFY))
	{
		VerifyOne(mode, d);
	}
	else if (mode & C3DGDX_PROF_SPLIT)
	{
		if (d->kind == VERIFY_ELEMENTS)
		{
			SplitDrawElements(d->primitive, d->a, d->b, d->indices);
		}
		else
		{
			SplitDrawArrays(d->primitive, d->a, d->b);
		}
	}
	else if (GDX_UNLIKELY(mode & C3DGDX_PROF_TOTAL))
	{
		u64 t0 = C3Dgdx_Now();
		if (diet)
		{
			RunDiet(diet, d);
		}
		else
		{
			RunStock(d);
		}
		s_stats.totTicks += C3Dgdx_Now() - t0;
		s_stats.totDraws ++;
	}
	else if (diet)
	{
		RunDiet(diet, d);
	}
	else
	{
		RunStock(d);
	}

	if (gpuCmdBuf == buf)
	{
		s_stats.words += gpuCmdBufOffset - start;
	}
	s_stats.unifElided += C3Di_GdxTakeElided();
}

void C3Dgdx_DrawElements(GPU_Primitive_t primitive, int count, int type, const void* indices)
{
	VerifyDraw d = { VERIFY_ELEMENTS, primitive, count, type, indices };
	Dispatch(C3Dgdx_mode, &d);
}

void C3Dgdx_DrawArrays(GPU_Primitive_t primitive, int first, int size)
{
	VerifyDraw d = { VERIFY_ARRAYS, primitive, first, size, NULL };
	Dispatch(C3Dgdx_mode, &d);
}

uint32_t C3Dgdx_Calibrate(void)
{
	u64 best = ~0ull;
	int i;
	for (i = 0; i < 256; i ++)
	{
		u64 t0 = C3Dgdx_Now();
		u64 t1 = C3Dgdx_Now();
		if (t1 - t0 < best)
		{
			best = t1 - t0;
		}
	}
	return (uint32_t)best;
}

void C3Dgdx_GetStats(C3Dgdx_Stats* out, int reset)
{
	*out = s_stats;
	if (reset)
	{
		u32 firstBadDraw = s_stats.firstBadDraw;
		u32 firstBadWord = s_stats.firstBadWord;
		u32 firstBadStock = s_stats.firstBadStock;
		u32 firstBadDiet = s_stats.firstBadDiet;
		memset(&s_stats, 0, sizeof(s_stats));
		// The first mismatch is sticky: it names the draw since arming.
		s_stats.firstBadDraw = firstBadDraw;
		s_stats.firstBadWord = firstBadWord;
		s_stats.firstBadStock = firstBadStock;
		s_stats.firstBadDiet = firstBadDiet;
	}
}
