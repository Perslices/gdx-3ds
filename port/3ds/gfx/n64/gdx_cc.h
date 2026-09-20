/* port/3ds/gfx/n64/gdx_cc.h -- colour combiner -> LUS shader ids, lifted verbatim from
 * Interpreter::GenerateCC (interpreter.cpp:827) so the backend's MapCombiner sees identical
 * (id0,id1) pairs and input mappings (plan §3.8). The pool is a small open-addressed table
 * with a `prev` fast path (lus-cc-key-uninit-shader-id: the key is value-initialised). */
#ifndef GDX_CC_H
#define GDX_CC_H

#include <stdint.h>

namespace gdxn {

struct CombinerKey {
    uint64_t combine_mode = 0;
    uint64_t options = 0;
    bool operator==(const CombinerKey& o) const {
        return combine_mode == o.combine_mode && options == o.options;
    }
};

struct Combiner {
    uint64_t shader_id0 = 0;
    uint64_t shader_id1 = 0;
    bool usedTextures[2] = { false, false };
    void* prg[16] = {};
    uint8_t shader_input_mapping[2][7] = {};
};

void GenerateCC(Combiner* comb, const CombinerKey& key);

class CombinerPool {
  public:
    CombinerPool();
    /* Returns the pooled combiner; *created = true when GenerateCC just ran (the caller
     * flushes, as LookupOrCreateColorCombiner does). Null only when the pool is full. */
    Combiner* Lookup(const CombinerKey& key, bool* created);
    void Clear();
    unsigned Count() const {
        return mCount;
    }

  private:
    static constexpr unsigned kSlots = 256;
    struct Slot {
        CombinerKey key;
        Combiner comb;
        bool used;
    };
    Slot mSlots[kSlots];
    Slot* mPrev = nullptr;
    unsigned mCount = 0;
};

} // namespace gdxn

#endif /* GDX_CC_H */
