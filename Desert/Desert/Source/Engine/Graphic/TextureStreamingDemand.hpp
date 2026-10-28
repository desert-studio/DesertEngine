#pragma once

#include <Common/Core/AssetHandle.hpp>
#include <Engine/Geometry/LODSelection.hpp>

#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Desert::Graphic
{
    class Material;

    /**
     * WHAT THE FRAME ASKED OF THE TEXTURES (TEX-STREAMING). UE FRenderAssetStreamingManager reads, per texture,
     * the largest screen size any primitive drawing it covers (FRenderAssetInstanceView: bounds + texel factor)
     * and streams the mip that size needs. Here the renderer notes every submitted draw: its material and the
     * screen size of its bounds, measured the way UE measures it -- the projection's focal scale over the
     * distance from the view to the bounds, so a narrow field of view asks for more (the LOD rule, WP-FAR-2).
     * Submission happens before the frustum cull, so a texture behind the camera keeps its mips (UE's static
     * instances are distance-only too): turning around never meets a soft wall.
     *
     * Main thread only: the submit path and Runtime::TextureService::UpdateStreaming both run there.
     */
    class TextureStreamingDemand
    {
    public:
        static TextureStreamingDemand& Get();

        /// The screen diameter, in pixels, of a world-space sphere seen through @p view on a target
        /// @p viewHeight pixels tall. The distance is to the sphere's SURFACE: a camera inside it asks for all.
        [[nodiscard]] static float ScreenPixels( const glm::vec3& center, float radius,
                                                 const Geometry::LODView& view, uint32_t viewHeight );

        /// One draw of @p material covering @p screenPixels: every 2D texture the material binds that an asset
        /// owns (Image::GetOwningAsset) is asked for at least that size this frame.
        void Note( const Material* material, float screenPixels );

        /// The frame's demand per texture asset (largest screen size in pixels), handed over and cleared.
        [[nodiscard]] std::unordered_map<Common::AssetHandle, float> TakeFrame();

        /// THE POOL'S ROW IN THE MEMORY CENSUS. Written by the texture service each frame, read by the
        /// allocator's census (any thread), so the census can say what the pool holds against its budget.
        struct PoolStats
        {
            std::atomic<uint64_t> ResidentBytes{ 0 };
            std::atomic<uint64_t> WantedBytes{ 0 };
            std::atomic<uint64_t> BudgetBytes{ 0 };
            std::atomic<uint64_t> FullChainBytes{ 0 };
            std::atomic<uint32_t> Textures{ 0 };
            std::atomic<uint32_t> Streaming{ 0 };
        };
        [[nodiscard]] PoolStats& Pool()
        {
            return m_Pool;
        }

    private:
        std::unordered_map<Common::AssetHandle, float> m_Demand;
        /// The asset textures each material binds, resolved once per frame per material.
        std::unordered_map<const Material*, std::vector<Common::AssetHandle>> m_MaterialTextures;
        PoolStats                                                             m_Pool;
    };
} // namespace Desert::Graphic
