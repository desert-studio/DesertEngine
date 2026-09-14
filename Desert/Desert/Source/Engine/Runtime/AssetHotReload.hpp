#pragma once

#include <Common/Core/Timestep.hpp>

#include <filesystem>
#include <memory>
#include <unordered_map>

namespace Desert::Assets
{
    class AssetManager;
}
namespace Desert::Core
{
    class Scene;
}

namespace Desert::Runtime
{
    // Asset hot-reload: polls the mtimes of editable asset files and pushes changes into the
    // running engine without a restart.
    //
    //   .demat  — re-parses the material asset and re-applies it onto its runtime material
    //             (PBR fast path or DataDrivenMaterial); a SHADER change in the file rebuilds
    //             the runtime material and refreshes every mesh component using that slot.
    //   .dcnv   — re-reads the cloud noise volume and re-uploads it, so a bake in the Cloud Noise Volume
    //             panel is visible in the sky the next frame without a restart. Nothing needs telling:
    //             the cloud renderer resolves its volume through the service every frame.
    //   .decloudtype — re-parses the cloud type and re-registers it, so an edit in the Cloud Type panel
    //             is visible in the sky the next frame. The layer's HANDLE has not changed, so the
    //             service's generation counter is what tells the renderer to rebuild its profile table.
    //   .dcmv   — re-reads a sculpted hero-cloud body and re-uploads it, so a re-bake is visible in the
    //             sky the next frame. Nothing needs telling here either: the cloud renderer resolves the
    //             body through the service every frame.
    //   .shader — recompiles the program (errors land in the log / Logs panel, the old
    //             pipelines keep drawing); on success the pipeline cache entries for that
    //             shader are dropped after a device-idle wait, so the next frame draws with
    //             the new code. Renderer-owned specialized pipelines (batched PBR, shadows)
    //             still need a restart — logged when they're affected.
    //
    // Polling (not FS events) keeps it portable (macOS/Windows/Linux) and cheap: one stat()
    // per watched file per interval.
    class AssetHotReload
    {
    public:
        // Call once per frame. Cheap between poll intervals (just accumulates time).
        void Tick( const Common::Timestep& ts, Assets::AssetManager& assetManager, Core::Scene* scene );

    private:
        void PollMaterials( Assets::AssetManager& assetManager, Core::Scene* scene );
        void PollShaders( Assets::AssetManager& assetManager, Core::Scene* scene );
        // No scene argument: a volume is not referenced by any component the way a material is — the
        // renderer looks its own up by handle every frame — so there is nothing in the scene to refresh.
        void PollCloudNoiseVolumes( Assets::AssetManager& assetManager );
        // No scene argument either, and for the same reason: a cloud type is resolved by handle through
        // Runtime::CloudTypeService every frame, so nothing in the scene holds a copy to refresh.
        void PollCloudTypes( Assets::AssetManager& assetManager );
        // And the same again for a sculpted body: the renderer resolves it by handle every frame, so a
        // re-upload is the whole of the reload.
        void PollCloudModellingVolumes( Assets::AssetManager& assetManager );
        // And the same again for a UI theme: the canvas walk asks UIThemeService for it by handle every
        // frame, so re-registering the flattened table IS the whole of the reload — no scene to refresh.
        void PollUIThemes( Assets::AssetManager& assetManager );

        // Records @p path's mtime and reports whether it MOVED since the last poll. A file seen for the
        // first time returns false: the first sighting is a baseline, not an edit.
        bool TouchWatched( const std::filesystem::path& path );

        using Clock = std::filesystem::file_time_type;
        std::unordered_map<std::string, Clock> m_KnownTimes;
        float                                  m_Accum       = 0.0f;
        bool                                   m_FirstScan   = true;
        static constexpr float                 kPollInterval = 0.7f; // seconds
    };
} // namespace Desert::Runtime
