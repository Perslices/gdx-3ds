/* port/3ds/gfx/n64/gdx_dlsrc.h -- command sources for the owned interpreter.
 *
 * Three source classes exist (plan §2.1): host-built wide lists (16-byte packets, GDX1-tagged
 * host pointers), host-endian N64 lists inside fixup-swapped segment images (8-byte packets)
 * and untagged big-endian ROM slices (8-byte packets, byte-swapped on read). A list's class,
 * endianness and dialect are fixed at list entry; operands resolve through the bridge's
 * resolver seam (port/gdx_dlsrc_bridge.h) against the running job's segment view.
 *
 * Host-compilable: the seam is plain C; the host harness supplies its own definitions. */
#ifndef GDX_DLSRC_H
#define GDX_DLSRC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gdx_gbi.h"

typedef struct GdxGfxJob GdxGfxJob;

namespace gdxn {

struct ListRef {
    const uint8_t* base = nullptr;
    size_t limit = 0; /* commands readable from base (a bound, not a terminator scan) */
    uint8_t stride = 8;
    bool isBig = false;
    bool isF3d = false;
};

enum class SrcClass : uint8_t { Wide = 0, HostN64 = 1, RawBE = 2 };

static inline SrcClass ClassOf(const ListRef& l) {
    if (l.stride == 16) {
        return SrcClass::Wide;
    }
    return l.isBig ? SrcClass::RawBE : SrcClass::HostN64;
}

/* Read command i of a list. Wide: w0 @0, w1lo @8, w1hi @12 (host-endian). Narrow: w0 @0,
 * w1 @4, byte-swapped when the list is big-endian; w1hi is 0 (never a host tag). */
static inline Cmd ReadCmd(const ListRef& l, size_t i) {
    Cmd c;
    const uint8_t* p = l.base + i * l.stride;
    if (l.stride == 16) {
        memcpy(&c.w0, p, 4);
        memcpy(&c.w1, p + 8, 4);
        memcpy(&c.w1hi, p + 12, 4);
    } else {
        memcpy(&c.w0, p, 4);
        memcpy(&c.w1, p + 4, 4);
        if (l.isBig) {
            c.w0 = Bswap32(c.w0);
            c.w1 = Bswap32(c.w1);
        }
        c.w1hi = 0;
    }
    return c;
}

class DlSrc {
  public:
    void Begin(GdxGfxJob* job) {
        mJob = job;
    }
    GdxGfxJob* Job() const {
        return mJob;
    }

    /* The GDX1 decision (plan §3.2): a tagged, non-null low32 that is not a placeholder stub
     * or BSS alias is the host pointer; everything else goes through the resolver. */
    static bool IsHostPtr(const Cmd& c);

    bool Root(ListRef& out) const;
    bool ResolveList(const Cmd& c, ListRef& out) const;
    /* Data operand; 0 = unresolved (caller applies its fallback). */
    uintptr_t ResolveData(const Cmd& c, size_t requiredBytes, bool preferPhysical = false,
                          uintptr_t sourceHint = 0) const;
    /* SETTIMG operand with the wide-stub hop for tagged pointers. */
    uintptr_t ResolveTimg(const Cmd& c, size_t requiredBytes) const;
    uintptr_t SetSegment(unsigned segment, const Cmd& c) const;
    /* G_LOAD_UCODE: 1 = known (variant 0/1/2, isLine), 0 = unknown load. */
    static int UcodeVariant(const Cmd& c, int* variant, int* isLine);
    static bool TexelsAreNativeRgba16(uintptr_t src, size_t size);
    /* LUS's "resource-backed" SETTIMG class (o2r-keyed asset texture): legacy decode path and
     * same-content skip eligibility. */
    static bool TimgIsResource(const Cmd& c, uintptr_t resolved, size_t loadBytes);
    static size_t ReadableBytes(uintptr_t address);

  private:
    GdxGfxJob* mJob = nullptr;
};

} // namespace gdxn

#endif /* GDX_DLSRC_H */
