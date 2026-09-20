/* port/3ds/gfx/n64/gdx_cc.cpp -- see gdx_cc.h. GenerateCC is Interpreter::GenerateCC with the
 * LUS constants spelled from gdx_gbi.h; nothing else changed. */
#include "gdx_cc.h"

#include <string.h>

#include "gdx_gbi.h"

namespace gdxn {

void GenerateCC(Combiner* comb, const CombinerKey& key) {
    const bool is2Cyc = (key.options & SHOPT_2CYC) != 0;

    uint8_t c[2][2][4];
    uint64_t shaderId0 = 0;
    uint64_t shaderId1 = key.options;
    uint8_t shaderInputMapping[2][7] = { { 0 } };
    bool usedTextures[2]{};
    for (uint32_t i = 0; i < 2 && (i == 0 || is2Cyc); i++) {
        uint32_t rgbA = (key.combine_mode >> (i * 28)) & 0xf;
        uint32_t rgbB = (key.combine_mode >> (i * 28 + 4)) & 0xf;
        uint32_t rgbC = (key.combine_mode >> (i * 28 + 8)) & 0x1f;
        uint32_t rgbD = (key.combine_mode >> (i * 28 + 13)) & 7;
        uint32_t alphaA = (key.combine_mode >> (i * 28 + 16)) & 7;
        uint32_t alphaB = (key.combine_mode >> (i * 28 + 16 + 3)) & 7;
        uint32_t alphaC = (key.combine_mode >> (i * 28 + 16 + 6)) & 7;
        uint32_t alphaD = (key.combine_mode >> (i * 28 + 16 + 9)) & 7;

        if (rgbA >= 8) {
            rgbA = CCMUX_0;
        }
        if (rgbB >= 8) {
            rgbB = CCMUX_0;
        }
        if (rgbC >= 16) {
            rgbC = CCMUX_0;
        }
        if (rgbD == 7) {
            rgbD = CCMUX_0;
        }

        if (rgbA == rgbB || rgbC == CCMUX_0) {
            rgbA = CCMUX_0;
            rgbB = CCMUX_0;
            rgbC = CCMUX_0;
        }
        if (alphaA == alphaB || alphaC == ACMUX_0) {
            alphaA = ACMUX_0;
            alphaB = ACMUX_0;
            alphaC = ACMUX_0;
        }
        if (i == 1) {
            if (rgbA != CCMUX_COMBINED && rgbB != CCMUX_COMBINED && rgbC != CCMUX_COMBINED && rgbD != CCMUX_COMBINED) {
                c[0][0][0] = c[0][0][1] = c[0][0][2] = c[0][0][3] = CCMUX_0;
            }
            if (rgbC != CCMUX_COMBINED_ALPHA && alphaA != ACMUX_COMBINED && alphaB != ACMUX_COMBINED &&
                alphaD != ACMUX_COMBINED) {
                c[0][1][0] = c[0][1][1] = c[0][1][2] = c[0][1][3] = ACMUX_0;
            }
        }

        c[i][0][0] = static_cast<uint8_t>(rgbA);
        c[i][0][1] = static_cast<uint8_t>(rgbB);
        c[i][0][2] = static_cast<uint8_t>(rgbC);
        c[i][0][3] = static_cast<uint8_t>(rgbD);
        c[i][1][0] = static_cast<uint8_t>(alphaA);
        c[i][1][1] = static_cast<uint8_t>(alphaB);
        c[i][1][2] = static_cast<uint8_t>(alphaC);
        c[i][1][3] = static_cast<uint8_t>(alphaD);
    }
    if (!is2Cyc) {
        for (uint32_t i = 0; i < 2; i++) {
            for (uint32_t k = 0; k < 4; k++) {
                c[1][i][k] = static_cast<uint8_t>(i == 0 ? CCMUX_0 : ACMUX_0);
            }
        }
        for (uint32_t k = 0; k < 4; k++) {
            if (c[0][0][k] == CCMUX_TEXEL1) c[0][0][k] = CCMUX_TEXEL0;
            if (c[0][0][k] == CCMUX_TEXEL1_ALPHA) c[0][0][k] = CCMUX_TEXEL0_ALPHA;
            if (c[0][1][k] == ACMUX_TEXEL1) c[0][1][k] = ACMUX_TEXEL0;
        }
    }
    {
        uint8_t inputNumber[32] = { 0 };
        uint32_t nextInputNumber = SHADER_INPUT_1;
        for (uint32_t i = 0; i < 2 && (i == 0 || is2Cyc); i++) {
            for (uint32_t j = 0; j < 4; j++) {
                if (j == 1 && c[i][0][j] == CCMUX_CENTER) {
                    c[i][0][j] = CCMUX_KEY_CENTER;
                } else if (j == 1 && c[i][0][j] == CCMUX_K4) {
                    c[i][0][j] = CCMUX_CONVERT_K4;
                } else if (j == 2 && c[i][0][j] == CCMUX_SCALE) {
                    c[i][0][j] = CCMUX_KEY_SCALE;
                } else if (j == 2 && c[i][0][j] == CCMUX_K5) {
                    c[i][0][j] = CCMUX_CONVERT_K5;
                }
                uint32_t val = 0;
                switch (c[i][0][j]) {
                    case CCMUX_0:
                        val = SHADER_0;
                        break;
                    case CCMUX_1:
                        val = SHADER_1;
                        break;
                    case CCMUX_TEXEL0:
                        val = SHADER_TEXEL0;
                        usedTextures[i == 0 ? 0 : 1] = true;
                        break;
                    case CCMUX_TEXEL1:
                        val = SHADER_TEXEL1;
                        usedTextures[i == 0 ? 1 : 0] = true;
                        break;
                    case CCMUX_TEXEL0_ALPHA:
                        val = SHADER_TEXEL0A;
                        usedTextures[i == 0 ? 0 : 1] = true;
                        break;
                    case CCMUX_TEXEL1_ALPHA:
                        val = SHADER_TEXEL1A;
                        usedTextures[i == 0 ? 1 : 0] = true;
                        break;
                    case CCMUX_NOISE:
                        val = SHADER_NOISE;
                        break;
                    case CCMUX_PRIMITIVE:
                    case CCMUX_PRIMITIVE_ALPHA:
                    case CCMUX_PRIM_LOD_FRAC:
                    case CCMUX_SHADE:
                    case CCMUX_SHADE_ALPHA:
                    case CCMUX_ENVIRONMENT:
                    case CCMUX_ENV_ALPHA:
                    case CCMUX_LOD_FRACTION:
                    case CCMUX_KEY_CENTER:
                    case CCMUX_KEY_SCALE:
                    case CCMUX_CONVERT_K4:
                    case CCMUX_CONVERT_K5:
                        if (inputNumber[c[i][0][j]] == 0) {
                            shaderInputMapping[0][nextInputNumber - 1] = c[i][0][j];
                            inputNumber[c[i][0][j]] = static_cast<uint8_t>(nextInputNumber++);
                        }
                        val = inputNumber[c[i][0][j]];
                        break;
                    case CCMUX_COMBINED:
                        val = SHADER_COMBINED;
                        break;
                    default:
                        break;
                }
                shaderId0 |= static_cast<uint64_t>(val) << (i * 32 + j * 4);
            }
        }
    }
    {
        uint8_t inputNumber[16] = { 0 };
        uint32_t nextInputNumber = SHADER_INPUT_1;
        for (uint32_t i = 0; i < 2; i++) {
            for (uint32_t j = 0; j < 4; j++) {
                uint32_t val = 0;
                switch (c[i][1][j]) {
                    case ACMUX_0:
                        val = SHADER_0;
                        break;
                    case ACMUX_TEXEL0:
                        val = SHADER_TEXEL0;
                        usedTextures[i == 0 ? 0 : 1] = true;
                        break;
                    case ACMUX_TEXEL1:
                        val = SHADER_TEXEL1;
                        usedTextures[i == 0 ? 1 : 0] = true;
                        break;
                    case ACMUX_LOD_FRACTION: /* == ACMUX_COMBINED */
                        if (j != 2) {
                            val = SHADER_COMBINED;
                            break;
                        }
                        c[i][1][j] = CCMUX_LOD_FRACTION;
                        [[fallthrough]];
                    case ACMUX_1: /* == ACMUX_PRIM_LOD_FRAC */
                        if (j != 2) {
                            val = SHADER_1;
                            break;
                        }
                        [[fallthrough]];
                    case ACMUX_PRIMITIVE:
                    case ACMUX_SHADE:
                    case ACMUX_ENVIRONMENT:
                        if (inputNumber[c[i][1][j]] == 0) {
                            shaderInputMapping[1][nextInputNumber - 1] = c[i][1][j];
                            inputNumber[c[i][1][j]] = static_cast<uint8_t>(nextInputNumber++);
                        }
                        val = inputNumber[c[i][1][j]];
                        break;
                    default:
                        break;
                }
                shaderId0 |= static_cast<uint64_t>(val) << (i * 32 + 16 + j * 4);
            }
        }
    }
    comb->shader_id0 = shaderId0;
    comb->shader_id1 = shaderId1;
    comb->usedTextures[0] = usedTextures[0];
    comb->usedTextures[1] = usedTextures[1];
    memcpy(comb->shader_input_mapping, shaderInputMapping, sizeof(shaderInputMapping));
}

CombinerPool::CombinerPool() {
    Clear();
}

void CombinerPool::Clear() {
    memset(mSlots, 0, sizeof(mSlots));
    mPrev = nullptr;
    mCount = 0;
}

Combiner* CombinerPool::Lookup(const CombinerKey& key, bool* created) {
    *created = false;
    if (mPrev != nullptr && mPrev->key == key) {
        return &mPrev->comb;
    }
    uint64_t h = key.combine_mode ^ (key.options * 0x9E3779B97F4A7C15ull);
    h ^= h >> 29;
    unsigned idx = static_cast<unsigned>(h) & (kSlots - 1);
    for (unsigned probe = 0; probe < kSlots; probe++) {
        Slot& s = mSlots[idx];
        if (!s.used) {
            s.used = true;
            s.key = key;
            s.comb = Combiner();
            GenerateCC(&s.comb, key);
            mCount++;
            mPrev = &s;
            *created = true;
            return &s.comb;
        }
        if (s.key == key) {
            mPrev = &s;
            return &s.comb;
        }
        idx = (idx + 1) & (kSlots - 1);
    }
    return nullptr;
}

} // namespace gdxn
