/* gdx3ds_fx_shim_begin.h — [native] fx: the macro redefinitions for the effects bracket of
 * Racer_Draw. Preprocessor directives only (it is #included INSIDE the function body, right
 * before GDXFX_BEGIN); gdx3ds_fx_shim_end.h pops every macro back. The packet variable of the
 * bracket is Racer_Draw's `gfx` (every site is `gfx++`). See gdx3ds_fx_shim.h. */
#pragma push_macro("gSPDisplayList")
#pragma push_macro("gSPVertex")
#pragma push_macro("gSP1Triangle")
#pragma push_macro("gSP2Triangles")
#pragma push_macro("gDPSetPrimColor")
#pragma push_macro("gDPSetEnvColor")
#pragma push_macro("gSPTexture")
#pragma push_macro("gDPPipeSync")
#pragma push_macro("gDPSetTextureImage")

#undef gSPDisplayList
#define gSPDisplayList(pkt, dl)                                                         \
    {                                                                                   \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        const void* _gdxD = (const void*)(dl);                                          \
        if (gdxfx_active && gdxfx_setup(_gdxG, _gdxD) == 2) {                           \
            _gdxG = gfx++;                                                              \
        }                                                                               \
        gdxfx_gbi_dl(_gdxG, _gdxD);                                                     \
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
        int _gdxR = gdxfx_active ? gdxfx_vertex(_gdxG, _gdxV, _gdxN, _gdxV0, _gdxAdv) : 0; \
        if (_gdxR == 1) {                                                               \
            if (_gdxAdv) {                                                              \
                gfx = _gdxG;                                                            \
            }                                                                           \
        } else {                                                                        \
            if (_gdxR == 2) {                                                           \
                _gdxG = gfx++;                                                          \
            }                                                                           \
            gdxfx_gbi_vertex(_gdxG, _gdxV, _gdxN, _gdxV0);                              \
        }                                                                               \
    }

#undef gSP1Triangle
#define gSP1Triangle(pkt, v0, v1, v2, flag)                                             \
    {                                                                                   \
        Gfx* _gdxB = gfx;                                                               \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        int _gdxA = (int)(v0), _gdxBv = (int)(v1), _gdxC = (int)(v2), _gdxF = (int)(flag); \
        int _gdxAdv = (_gdxB != gfx);                                                   \
        int _gdxR = gdxfx_active ? gdxfx_tri(_gdxG, _gdxA, _gdxBv, _gdxC, _gdxAdv) : 0;  \
        if (_gdxR == 1) {                                                               \
            if (_gdxAdv) {                                                              \
                gfx = _gdxG;                                                            \
            }                                                                           \
        } else {                                                                        \
            if (_gdxR == 2) {                                                           \
                _gdxG = gfx++;                                                          \
            }                                                                           \
            gdxfx_gbi_tri1(_gdxG, _gdxA, _gdxBv, _gdxC, _gdxF);                         \
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
        int _gdxR = gdxfx_active ? gdxfx_tri2(_gdxG, _gdxA, _gdxBv, _gdxC, _gdxD, _gdxE, _gdxFv, _gdxAdv) : 0; \
        if (_gdxR == 1) {                                                               \
            if (_gdxAdv) {                                                              \
                gfx = _gdxG;                                                            \
            }                                                                           \
        } else {                                                                        \
            if (_gdxR == 2) {                                                           \
                _gdxG = gfx++;                                                          \
            }                                                                           \
            gdxfx_gbi_tri2(_gdxG, _gdxA, _gdxBv, _gdxC, _gdxF0, _gdxD, _gdxE, _gdxFv, _gdxF1); \
        }                                                                               \
    }

#undef gDPSetPrimColor
#define gDPSetPrimColor(pkt, m, l, r, g, b, a)                                          \
    {                                                                                   \
        Gfx* _gdxB = gfx;                                                               \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        int _gdxM = (int)(m), _gdxL = (int)(l);                                         \
        int _gdxR_ = (int)(r), _gdxG_ = (int)(g), _gdxB_ = (int)(b), _gdxA_ = (int)(a); \
        int _gdxAdv = (_gdxB != gfx);                                                   \
        int _gdxR = gdxfx_active ? gdxfx_prim(_gdxG, _gdxM, _gdxL, _gdxR_, _gdxG_, _gdxB_, _gdxA_, _gdxAdv) : 0; \
        if (_gdxR == 1) {                                                               \
            if (_gdxAdv) {                                                              \
                gfx = _gdxG;                                                            \
            }                                                                           \
        } else {                                                                        \
            if (_gdxR == 2) {                                                           \
                _gdxG = gfx++;                                                          \
            }                                                                           \
            gdxfx_gbi_prim(_gdxG, _gdxM, _gdxL, _gdxR_, _gdxG_, _gdxB_, _gdxA_);        \
        }                                                                               \
    }

#undef gDPSetEnvColor
#define gDPSetEnvColor(pkt, r, g, b, a)                                                 \
    {                                                                                   \
        Gfx* _gdxB = gfx;                                                               \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        int _gdxR_ = (int)(r), _gdxG_ = (int)(g), _gdxB_ = (int)(b), _gdxA_ = (int)(a); \
        int _gdxAdv = (_gdxB != gfx);                                                   \
        int _gdxR = gdxfx_active ? gdxfx_env(_gdxG, _gdxR_, _gdxG_, _gdxB_, _gdxA_, _gdxAdv) : 0; \
        if (_gdxR == 1) {                                                               \
            if (_gdxAdv) {                                                              \
                gfx = _gdxG;                                                            \
            }                                                                           \
        } else {                                                                        \
            if (_gdxR == 2) {                                                           \
                _gdxG = gfx++;                                                          \
            }                                                                           \
            gdxfx_gbi_env(_gdxG, _gdxR_, _gdxG_, _gdxB_, _gdxA_);                       \
        }                                                                               \
    }

#undef gSPTexture
#define gSPTexture(pkt, s, t, level, tile, on)                                          \
    {                                                                                   \
        Gfx* _gdxB = gfx;                                                               \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        int _gdxS = (int)(s), _gdxT = (int)(t), _gdxLv = (int)(level), _gdxTi = (int)(tile), _gdxOn = (int)(on); \
        int _gdxAdv = (_gdxB != gfx);                                                   \
        int _gdxR = gdxfx_active ? gdxfx_texture(_gdxG, _gdxS, _gdxT, _gdxLv, _gdxTi, _gdxOn, _gdxAdv) : 0; \
        if (_gdxR == 1) {                                                               \
            if (_gdxAdv) {                                                              \
                gfx = _gdxG;                                                            \
            }                                                                           \
        } else {                                                                        \
            if (_gdxR == 2) {                                                           \
                _gdxG = gfx++;                                                          \
            }                                                                           \
            gdxfx_gbi_texture(_gdxG, _gdxS, _gdxT, _gdxLv, _gdxTi, _gdxOn);             \
        }                                                                               \
    }

#undef gDPPipeSync
#define gDPPipeSync(pkt)                                                                \
    {                                                                                   \
        Gfx* _gdxB = gfx;                                                               \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        int _gdxAdv = (_gdxB != gfx);                                                   \
        int _gdxR = gdxfx_active ? gdxfx_pipesync(_gdxG, _gdxAdv) : 0;                  \
        if (_gdxR == 1) {                                                               \
            if (_gdxAdv) {                                                              \
                gfx = _gdxG;                                                            \
            }                                                                           \
        } else {                                                                        \
            if (_gdxR == 2) {                                                           \
                _gdxG = gfx++;                                                          \
            }                                                                           \
            gdxfx_gbi_pipesync(_gdxG);                                                  \
        }                                                                               \
    }

#undef gDPSetTextureImage
#define gDPSetTextureImage(pkt, f, s, w, i)                                             \
    {                                                                                   \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        if (gdxfx_active && gdxfx_texload(_gdxG) == 2) {                                \
            _gdxG = gfx++;                                                              \
        }                                                                               \
        gdxfx_gbi_settimg(_gdxG, (int)(f), (int)(s), (int)(w), (const void*)(i));       \
    }
