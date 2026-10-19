/* gdx3ds_gad_shim_begin.h -- [native] gadget_batch: the macro redefinitions for the gadget
 * bracket of Course_GadgetsDraw. Preprocessor directives only (it is #included INSIDE the
 * function body, right before GDXGAD_BEGIN); gdx3ds_gad_shim_end.h pops every macro back. See
 * gdx3ds_gad_shim.h. The bracket's packet variable is `gfx`. */
#pragma push_macro("gSPDisplayList")
#pragma push_macro("gSPVertex")
#pragma push_macro("gSP1Triangle")
#pragma push_macro("gSP2Triangles")
#pragma push_macro("gSPModifyVertex")
#pragma push_macro("gSPTexture")
#pragma push_macro("gDPSetTextureImage")

/* The setup lists stay real GBI; a pending run is drawn before them. */
#undef gSPDisplayList
#define gSPDisplayList(pkt, dl)                                                         \
    {                                                                                   \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        const void* _gdxD = (const void*)(dl);                                          \
        if (gdxgad_active && gdxgad_boundary(_gdxG, 1) == 2) {                          \
            _gdxG = gfx++;                                                              \
        }                                                                               \
        gdxgad_gbi_dl(_gdxG, _gdxD);                                                    \
    }

#undef gSPVertex
#define gSPVertex(pkt, v, n, v0)                                                        \
    {                                                                                   \
        Gfx* _gdxB = gfx;                                                               \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        const void* _gdxV = (const void*)(v);                                           \
        int _gdxN = (int)(n);                                                           \
        int _gdxV0 = (int)(v0);                                                         \
        int _gdxAdv = (_gdxB != gfx);                                                   \
        int _gdxR = gdxgad_active ? gdxgad_vertex(_gdxG, _gdxV, _gdxN, _gdxV0, _gdxAdv) : 0; \
        if (_gdxR == 1) {                                                               \
            if (_gdxAdv) {                                                              \
                gfx = _gdxG;                                                            \
            }                                                                           \
        } else {                                                                        \
            if (_gdxR == 2) {                                                           \
                _gdxG = gfx++;                                                          \
            }                                                                           \
            gdxgad_gbi_vertex(_gdxG, _gdxV, _gdxN, _gdxV0);                             \
        }                                                                               \
    }

#undef gSP1Triangle
#define gSP1Triangle(pkt, v0, v1, v2, flag)                                             \
    {                                                                                   \
        Gfx* _gdxB = gfx;                                                               \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        int _gdxA = (int)(v0), _gdxBv = (int)(v1), _gdxC = (int)(v2), _gdxF = (int)(flag); \
        int _gdxAdv = (_gdxB != gfx);                                                   \
        int _gdxR = gdxgad_active ? gdxgad_tri(_gdxG, _gdxA, _gdxBv, _gdxC, _gdxAdv) : 0; \
        if (_gdxR == 1) {                                                               \
            if (_gdxAdv) {                                                              \
                gfx = _gdxG;                                                            \
            }                                                                           \
        } else {                                                                        \
            if (_gdxR == 2) {                                                           \
                _gdxG = gfx++;                                                          \
            }                                                                           \
            gdxgad_gbi_tri1(_gdxG, _gdxA, _gdxBv, _gdxC, _gdxF);                        \
        }                                                                               \
    }

#undef gSP2Triangles
#define gSP2Triangles(pkt, v00, v01, v02, flag0, v10, v11, v12, flag1)                  \
    {                                                                                   \
        Gfx* _gdxB = gfx;                                                               \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        int _gdxA = (int)(v00), _gdxBv = (int)(v01), _gdxC = (int)(v02), _gdxF0 = (int)(flag0); \
        int _gdxD = (int)(v10), _gdxE = (int)(v11), _gdxFv = (int)(v12), _gdxF1 = (int)(flag1); \
        int _gdxAdv = (_gdxB != gfx);                                                   \
        int _gdxR = gdxgad_active ? gdxgad_tri2(_gdxG, _gdxA, _gdxBv, _gdxC, _gdxD, _gdxE, _gdxFv, _gdxAdv) : 0; \
        if (_gdxR == 1) {                                                               \
            if (_gdxAdv) {                                                              \
                gfx = _gdxG;                                                            \
            }                                                                           \
        } else {                                                                        \
            if (_gdxR == 2) {                                                           \
                _gdxG = gfx++;                                                          \
            }                                                                           \
            gdxgad_gbi_tri2(_gdxG, _gdxA, _gdxBv, _gdxC, _gdxF0, _gdxD, _gdxE, _gdxFv, _gdxF1); \
        }                                                                               \
    }

#undef gSPModifyVertex
#define gSPModifyVertex(pkt, vtx, where, val)                                           \
    {                                                                                   \
        Gfx* _gdxB = gfx;                                                               \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        int _gdxVtx = (int)(vtx);                                                       \
        int _gdxWh = (int)(where);                                                      \
        uint32_t _gdxVal = (uint32_t)(val);                                             \
        int _gdxAdv = (_gdxB != gfx);                                                   \
        int _gdxR = gdxgad_active ? gdxgad_modify(_gdxG, _gdxVtx, _gdxWh, _gdxVal, _gdxAdv) : 0; \
        if (_gdxR == 1) {                                                               \
            if (_gdxAdv) {                                                              \
                gfx = _gdxG;                                                            \
            }                                                                           \
        } else {                                                                        \
            if (_gdxR == 2) {                                                           \
                _gdxG = gfx++;                                                          \
            }                                                                           \
            gdxgad_gbi_modify(_gdxG, _gdxVtx, _gdxWh, _gdxVal);                         \
        }                                                                               \
    }

#undef gSPTexture
#define gSPTexture(pkt, s, t, level, tile, on)                                          \
    {                                                                                   \
        Gfx* _gdxB = gfx;                                                               \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        int _gdxS = (int)(s), _gdxT = (int)(t), _gdxLv = (int)(level), _gdxTi = (int)(tile), _gdxOn = (int)(on); \
        int _gdxAdv = (_gdxB != gfx);                                                   \
        int _gdxR = gdxgad_active ? gdxgad_texture(_gdxG, _gdxS, _gdxT, _gdxLv, _gdxTi, _gdxOn, _gdxAdv) : 0; \
        if (_gdxR == 1) {                                                               \
            if (_gdxAdv) {                                                              \
                gfx = _gdxG;                                                            \
            }                                                                           \
        } else {                                                                        \
            if (_gdxR == 2) {                                                           \
                _gdxG = gfx++;                                                          \
            }                                                                           \
            gdxgad_gbi_texture(_gdxG, _gdxS, _gdxT, _gdxLv, _gdxTi, _gdxOn);            \
        }                                                                               \
    }

/* The head of gDPLoadTextureBlock: a pending run is drawn before the load; the rest of the
 * load's commands follow as real GBI. */
#undef gDPSetTextureImage
#define gDPSetTextureImage(pkt, f, s, w, i)                                             \
    {                                                                                   \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        if (gdxgad_active && gdxgad_boundary(_gdxG, 0) == 2) {                          \
            _gdxG = gfx++;                                                              \
        }                                                                               \
        gdxgad_gbi_settimg(_gdxG, (int)(f), (int)(s), (int)(w), (const void*)(i));      \
    }
