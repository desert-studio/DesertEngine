#pragma once

#include <atomic>
#include <cstdint>

// TAA1 step 3 — WHICH WORLD A SCENE OBJECT HOLDS. One process-wide monotonic counter: every Scene takes a value
// when it is constructed and again when Clear() empties it for a load / reload / new scene, so no two worlds the
// process ever held share a generation — not even a reload into the same object or a new Scene at a freed
// address. Views key their history by it (Graphic::MakeViewCameraIdentity), never by an address.
namespace Desert::Core
{
    // Never 0 (0 is "no scene" in Graphic::ViewInputs::SceneIdentity).
    [[nodiscard]] inline uint64_t NextSceneGeneration()
    {
        static std::atomic<uint64_t> counter{ 0 };
        return counter.fetch_add( 1, std::memory_order_relaxed ) + 1;
    }
} // namespace Desert::Core
