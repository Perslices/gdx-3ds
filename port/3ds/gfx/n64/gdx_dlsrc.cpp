/* port/3ds/gfx/n64/gdx_dlsrc.cpp -- see gdx_dlsrc.h. Every resolution delegates to the
 * bridge seam so the owned path classifies operands byte-for-byte as the bridge walk does
 * (deviation from plan §3.2 recorded in interp-progress.md: bodies move at M5, not now). */
#include "gdx_dlsrc.h"

#include "gdx_dlsrc_bridge.h"

namespace gdxn {

bool DlSrc::IsHostPtr(const Cmd& c) {
    if (c.w1hi != kHostTag || c.w1 == 0) {
        return false;
    }
    return gdx_gfx_low32_needs_resolver(c.w1) == 0;
}

static void FillRef(ListRef& out, const GdxDlSrcList& l) {
    out.base = static_cast<const uint8_t*>(l.ptr);
    out.limit = l.limit;
    out.stride = l.stride;
    out.isBig = l.isBig != 0;
    out.isF3d = l.isF3d != 0;
}

bool DlSrc::Root(ListRef& out) const {
    GdxDlSrcList l = {};
    if (gdx_gfx_job_root_list(mJob, &l) == 0) {
        return false;
    }
    FillRef(out, l);
    return true;
}

bool DlSrc::ResolveList(const Cmd& c, ListRef& out) const {
    GdxDlSrcList l = {};
    const bool host = IsHostPtr(c);
    if (gdx_gfx_job_resolve_list(mJob, c.w1, host ? 1 : 0, host ? c.w1 : 0, &l) == 0) {
        return false;
    }
    FillRef(out, l);
    return true;
}

uintptr_t DlSrc::ResolveData(const Cmd& c, size_t requiredBytes, bool preferPhysical, uintptr_t sourceHint) const {
    const bool host = IsHostPtr(c);
    return gdx_gfx_job_resolve_data(mJob, c.w1, host ? 1 : 0, host ? c.w1 : 0, requiredBytes,
                                    preferPhysical ? 1 : 0, sourceHint);
}

uintptr_t DlSrc::ResolveTimg(const Cmd& c, size_t requiredBytes) const {
    if (IsHostPtr(c)) {
        return gdx_gfx_job_resolve_timg_host(mJob, c.w1, requiredBytes);
    }
    return gdx_gfx_job_resolve_data(mJob, c.w1, 0, 0, requiredBytes, 0, 0);
}

uint32_t DlSrc::TimgClassEpoch() const {
    return gdx_gfx_timg_class_epoch();
}

uintptr_t DlSrc::SetSegment(unsigned segment, const Cmd& c) const {
    const bool host = IsHostPtr(c);
    return gdx_gfx_job_set_segment(mJob, segment, c.w1, host ? 1 : 0, host ? c.w1 : 0);
}

int DlSrc::UcodeVariant(const Cmd& c, int* variant, int* isLine) {
    const bool host = IsHostPtr(c);
    return gdx_gfx_ucode_variant(c.w1, host ? 1 : 0, host ? c.w1 : 0, variant, isLine);
}

bool DlSrc::TimgIsResource(const Cmd& c, uintptr_t resolved, size_t loadBytes) {
    return gdx_gfx_timg_is_resource(c.w1, IsHostPtr(c) ? 1 : 0, resolved, loadBytes) != 0;
}

bool DlSrc::TexelsAreNativeRgba16(uintptr_t src, size_t size) {
    return gdx_gfx_texels_are_native_rgba16(src, size) != 0;
}

size_t DlSrc::ReadableBytes(uintptr_t address) {
    return gdx_gfx_readable_bytes(address);
}

} // namespace gdxn
