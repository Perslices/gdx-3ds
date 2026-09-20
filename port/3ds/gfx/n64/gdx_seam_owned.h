/* port/3ds/gfx/n64/gdx_seam_owned.h -- GdxInterpSeam over the owned interpreter
 * (port/3ds/gfx/gdx3ds_interp_seam.h). Created by gdx_rt_glue.cpp next to the Interp and
 * handed to the walker's G_NOOP / G_DL hooks, so the track and fx marker replays and the
 * machine meshes run under [native] interp=1 through the same interpreter-neutral bodies
 * they run through under the vendored LUS interpreter. */
#ifndef GDX_SEAM_OWNED_H
#define GDX_SEAM_OWNED_H

#include <stddef.h>
#include <stdint.h>

class GdxInterpSeam;
namespace gdxn {
class Interp;
}

GdxInterpSeam* GdxOwnedSeamCreate(gdxn::Interp* interp);
/* The G_DL target being considered (sourceHint for the bake's vertex resolution). */
void GdxOwnedSeamSetListHint(GdxInterpSeam* seam, const void* listPtr);
/* Gdx3dsMeshResolveFn for a lazy bake: the walker's own G_VTX resolution (DlSrc::ResolveData
 * with preferPhysical + full-payload readability), user = the seam. */
const void* GdxOwnedSeamResolveVtx(uint64_t w1word, size_t bytes, void* user);

#endif /* GDX_SEAM_OWNED_H */
