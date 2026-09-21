#pragma once

#include <Common/Utilities/AssetRegistry.hpp>

#include <filesystem>
#include <mutex>
#include <string>

// THE SHARED INTERNALS OF THE CONTENT REGISTRY, between its two translation units.
//
// WHY THERE ARE TWO. `ContentRegistry.cpp` is on the path every asset takes — `AssetManager::
// CreateAsset` calls `NoteAsset` — and it is listed by three test suites that deliberately compile a
// hand-picked set of engine sources so that a registry can be built without linking Vulkan. The COOK
// (`Refresh`) is the one function that reads dependency edges, and it reads them through
// `AssetEviction`, which reaches `Graphic::ResourceLedger` and through it the renderer.
//
// Keeping both in one file made every suite that touches an asset link the renderer — measured
// immediately: `TextureSlotRoundTrip` stopped linking with an undefined `AssetEviction::EdgesOf`. So
// the cook lives in `ContentRegistryCook.cpp`, which only the editor's own build lists, and the shared
// state is declared here rather than duplicated. That is the same split, for the same reason, as
// `TextureSlot.cpp` being carved out of `ComponentRegistry.cpp`.
namespace Desert::Assets::ContentRegistry::Detail
{
    struct State
    {
        std::mutex                   Mutex;
        Common::Utils::AssetRegistry Registry;
        bool                         Dirty = false;
    };

    State& Get_();

    std::string LowerExtension( const std::filesystem::path& file );
} // namespace Desert::Assets::ContentRegistry::Detail
