#include "internal.h"
#include "gdx_submit.h" // GDX (c5submit)

void C3Di_DrawArraysStock(GPU_Primitive_t primitive, int first, int size)
{
	C3Di_UpdateContext();

	// Set primitive type
	GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 2, primitive);
	// Start a new primitive (breaks off a triangle strip/fan)
	GPUCMD_AddWrite(GPUREG_RESTART_PRIMITIVE, 1);
	// The index buffer is not used, but this command is still required
	GPUCMD_AddWrite(GPUREG_INDEXBUFFER_CONFIG, 0x80000000);
	// Number of vertices
	GPUCMD_AddWrite(GPUREG_NUMVERTICES, size);
	// First vertex
	GPUCMD_AddWrite(GPUREG_VERTEX_OFFSET, first);
	// Enable array drawing mode
	GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 1, 1);
	// Enable drawing mode
	GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 0);
	// Trigger array drawing
	GPUCMD_AddWrite(GPUREG_DRAWARRAYS, 1);
	// Go back to configuration mode
	GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 1);
	// Disable array drawing mode
	GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 1, 0);
	// Clear the post-vertex cache
	GPUCMD_AddWrite(GPUREG_VTX_FUNC, 1);

	C3Di_GetContext()->flags |= C3DiF_DrawUsed;
}

// GDX (c5submit): the stock body above is unchanged (renamed); C3Dgdx_mode != 0 routes the draw
// through the P3a diet / verify / profile in gdx_submit.c.
void C3D_DrawArrays(GPU_Primitive_t primitive, int first, int size)
{
	if (C3Dgdx_mode)
	{
		C3Dgdx_DrawArrays(primitive, first, size);
	}
	else
	{
		C3Di_DrawArraysStock(primitive, first, size);
	}
	if (C3Dgdx_stencilMode) { // GDX (n60tail): [native] sky_late accounts the effect each draw used
		C3Dgdx_StencilDraw();
	}
}
