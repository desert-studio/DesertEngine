#pragma once

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Math/AABB.hpp>
#include <Engine/Geometry/LODSelection.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Desert::Graphic
{
    class Material;
    struct TextureDemandBuffer;
    struct InstanceFieldBounds;

    /**
     * WHAT THE FRAME ASKED OF THE TEXTURES (TEX-STREAMING). UE FRenderAssetStreamingManager reads, per texture,
     * the largest screen size any primitive drawing it covers (FRenderAssetInstanceView: bounds + texel factor)
     * and streams the mip that size needs. Here the renderer notes every submitted draw: its material and the
     * screen size of its bounds, measured the way UE measures it -- the projection's focal scale over the
     * distance from the view to the bounds, so a narrow field of view asks for more (the LOD rule, WP-FAR-2).
     * Submission happens before the frustum cull, so a texture behind the camera keeps its mips (UE's static
     * instances are distance-only too): turning around never meets a soft wall.
     *
     * Any thread, with no lock on the note (CPU-STREAM-DEMAND): the submit path notes on the render job's
     * workers (OVERLAP-ON-7) into its own thread's buffer, and Runtime::TextureService::UpdateStreaming merges
     * every buffer once, on the main thread, into one reused map. A take that lands while a frame is still
     * noting hands over what that frame noted so far; the rest goes into the next take. A material's texture
     * list is the material's own (MaterialExecutor::GetStreamedTextureAssets, resolved per slot write), and an
     * instanced field is measured through its cached bounds tree (FieldScreenPixels), not instance by instance.
     */
    class TextureStreamingDemand
    {
    public:
        static TextureStreamingDemand& Get();

        /// The screen diameter, in pixels, of a world-space sphere seen through @p view on a target
        /// @p viewHeight pixels tall. The distance is to the sphere's SURFACE: a camera inside it asks for all.
        [[nodiscard]] static float ScreenPixels( const glm::vec3& center, float radius,
                                                 const Geometry::LODView& view, uint32_t viewHeight );
        /// A draw's world bounding sphere (the LOD rule's: box centre, half-diagonal x largest scale) of
        /// @p local through @p transform.
        [[nodiscard]] static std::pair<glm::vec3, float> WorldSphere( const Common::Math::AABB& local,
                                                                      const glm::mat4&          transform );

        /// THE NEAREST INSTANCE OF A FIELD (UE UInstancedStaticMeshComponent::GetStreamingRenderAssetInfo reports
        /// the cluster tree's bounds, not one entry per instance): the largest ScreenPixels of
        /// WorldSphere(@p local, t) over every t in @p transforms -- the same value, bit for bit, as measuring
        /// every instance. The field's bounds tree is built once per snapshot (a new instance revision is a new
        /// vector) and walked branch-and-bound, nearest cluster first: a cluster whose bound cannot beat the best
        /// so far is not opened. The answer for one view is kept, so the sections and views of a frame measure
        /// the field once.
        [[nodiscard]] float FieldScreenPixels( const std::shared_ptr<const std::vector<glm::mat4>>& transforms,
                                               const Common::Math::AABB& local, const Geometry::LODView& view,
                                               uint32_t viewHeight );

        /// One draw of @p material covering @p screenPixels: every 2D texture the material binds that an asset
        /// owns (Image::GetOwningAsset) is asked for at least that size this frame.
        void Note( const Material* material, float screenPixels );
        /// One draw covering @p screenPixels that samples texture asset @p asset directly (a per-draw texture
        /// override -- MaterialOverrides::Textures -- or a landscape layer). 0 = no asset: nothing.
        void NoteAsset( const Common::AssetHandle& asset, float screenPixels );

        /// The frame's demand per texture asset (largest screen size in pixels): every thread's notes merged
        /// once. The map is this object's own and reused; it holds until the next TakeFrame. One taker (the
        /// texture service's frame loop).
        [[nodiscard]] const std::unordered_map<Common::AssetHandle, float>& TakeFrame();

        TextureStreamingDemand();
        ~TextureStreamingDemand();
        TextureStreamingDemand( const TextureStreamingDemand& )            = delete;
        TextureStreamingDemand& operator=( const TextureStreamingDemand& ) = delete;

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
        /// This thread's buffer, claimed from m_Buffers on its first note (a buffer of an exited thread is
        /// reused by the next new one).
        TextureDemandBuffer& LocalBuffer();
        /// Appends (asset, @p screenPixels) per non-zero asset to this thread's buffer of the current epoch.
        void Append( std::span<const Common::AssetHandle> assets, float screenPixels );

        // THE NOTE/TAKE HAND-OVER. A note publishes the epoch it writes into its buffer's Writing and re-reads
        // m_Epoch; TakeFrame advances m_Epoch, then waits per buffer until no note still writes the taken epoch,
        // and merges that epoch's half. A note never waits and never takes a lock.
        std::atomic<uint64_t>                             m_Epoch{ 0 };
        std::mutex                                        m_BuffersLock; // guards m_Buffers' list; the take
        std::vector<std::unique_ptr<TextureDemandBuffer>> m_Buffers;
        std::unordered_map<Common::AssetHandle, float>    m_Demand; // TakeFrame's, reused (cleared, not dropped)

        /// The instanced fields' bounds trees, by snapshot (weakly held: a dead snapshot's tree is swept on the
        /// next insert). Looked up shared; built once per snapshot.
        std::shared_mutex                                                     m_FieldsLock;
        std::unordered_map<const void*, std::shared_ptr<InstanceFieldBounds>> m_Fields;
        PoolStats                                                             m_Pool;
    };
} // namespace Desert::Graphic
