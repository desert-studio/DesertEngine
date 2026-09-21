#pragma once

#include <Engine/Assets/AssetRef.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/CloudNoiseVolumeAsset.hpp>
#include <Engine/Graphic/Image.hpp>

#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace Desert::Runtime
{
    /**
     * @brief Owns the GPU side of the cloud noise volumes: one `Image3D` per `.dcnv` THAT SOMETHING ASKED
     *        FOR.
     *
     * The same `AssetBase` / `AssetManager` shape as SkyboxService and TextureService, and it exists for
     * the same reason they do: the asset layer must not know about Vulkan, and the renderer must not know
     * how to read a file. It also settles a question the old bake got wrong by construction — the volume
     * is uploaded ONCE and shared by every view, where before each VolumetricCloudRenderer baked its own
     * 8 MiB copy and an editor with two viewports paid for two.
     *
     * ── WHAT CHANGED, AND THE SENTENCE IT ANSWERS ────────────────────────────────────────────────────
     *
     * `AssetPreloader::PreloadCloudNoiseVolumes` used to read and upload every `.dcnv` in the project
     * before the first frame, and it justified that in its own comment: *"Deferring would buy a stall
     * exactly where the sky first appears."* Measured on this machine, that stage cost **1312.7 ms of a
     * 5707.0 ms boot**, of which a single file — `CloudNoise_Default.dcnv`, 8 MiB — was **607.12 ms by
     * its own time**, and a scene with no clouds in it paid all of it.
     *
     * The comment was not wrong; it was incomplete. Deferring a read without somewhere to put "not here
     * yet" moves the cost into a frame, which is worse than a boot because a boot is a loading screen
     * and a frame is a hitch. So the read is deferred AND asynchronous AND its absence is a state:
     * `Announce` records what exists without reading it, and `Require` answers in three values while
     * asking `AsyncAssetLoader` to close the gap.
     *
     * ── `Get()` IS GONE, DELIBERATELY ────────────────────────────────────────────────────────────────
     *
     * It returned `Image3D*` and its null meant two different things — "nothing is registered under this
     * handle" and, now, "the read is in flight". A caller cannot distinguish those from a pointer, and
     * the one caller in the engine took the fallback path and logged a broken reference for both. There
     * is no compatibility overload: the old spelling is removed by the change that replaces it, so a
     * caller that has not thought about the pending case does not compile.
     *
     * THE EMPTY SLOT IS STILL NOT AN ERROR. A component with no volume chosen resolves to the built-in
     * default, because a scene nobody has authored a volume for still has to have a sky.
     */
    class CloudNoiseService
    {
    public:
        /// THE PROJECT HAS THIS VOLUME. Records the handle, the path and the (unread) asset; touches no
        /// file and no device. This is what the preloader's directory walk does now, and it is why the
        /// walk still costs the handle->path index its 368 entries while costing the boot nothing.
        void Announce( const Assets::Asset<Assets::CloudNoiseVolumeAsset>& asset );

        /// Uploads @p asset's voxels into a volume texture and caches it under the asset's handle, from
        /// bytes ALREADY IN HAND. Called by the async completion, by hot reload and by the editor panel
        /// that just baked one; a changed revision re-uploads, an unchanged one is a no-op.
        ///
        /// It refuses an asset that is not loaded rather than reading it — reading here would be the
        /// blocking `Get()` this class just removed, wearing a different name.
        Common::BoolResultStr Register( const std::shared_ptr<Assets::CloudNoiseVolumeAsset>& asset );

        /// Nominates the volume the empty slot resolves to. The preloader calls this for the built-in
        /// default; a project may ship its own by giving it the same file name.
        void SetDefault( const Assets::AssetHandle& handle );

        /**
         * @brief THE THREE-STATE ANSWER, and the only thing that starts a read.
         *
         * - **Ready** — the volume is on the device; draw with it.
         * - **Pending** — it was announced and is being read. The caller must WAIT: not draw this layer,
         *   and not report a missing reference. This state is why the type exists.
         * - **Null** — nothing was announced under this handle, or its read failed, and no default
         *   answers either. The caller's fallback is correct here and the log already names the reason.
         *
         * An empty @p handle resolves to the nominated default, exactly as the old `Get()` did.
         */
        Assets::AssetRef<Graphic::Image3D> Require( const Assets::AssetHandle& handle );

        /// The answer AS IT STANDS: never reads, never requests, never logs. For anything that wants to
        /// know the state without becoming the reason a load starts — a census, a panel drawing a
        /// status, a test.
        [[nodiscard]] Assets::AssetRef<Graphic::Image3D> Peek( const Assets::AssetHandle& handle ) const;

        /// How many announced volumes have been read. The number a boot can now be judged by: it used to
        /// equal the number of `.dcnv` files on disk by construction.
        [[nodiscard]] size_t ResidentCount() const;

        void Clear();

    private:
        struct Entry
        {
            /// The asset, announced and possibly unread. THE SOURCE OF THE REQUEST: without it the
            /// service would have only a handle, and a handle cannot be read.
            Assets::Asset<Assets::CloudNoiseVolumeAsset> Source;
            std::shared_ptr<Graphic::Image3D>            Volume;
            uint32_t                                     Revision = 0;
            /// Live while a worker is reading. Dropping it releases the request, so `Clear()` cannot
            /// leave a completion pointing at a service that has forgotten why it was asked.
            Assets::LoadRequest Request;
            /// The read was tried and failed. Latched so `Require` resolves to Null instead of asking
            /// again every frame forever — a retry loop on a corrupt file is a frame-rate defect that
            /// reads as a rendering one.
            bool Failed = false;
        };

        /// The shared body of `Require` and `Peek`. @p mayRequest is what separates them, and it is a
        /// parameter rather than two copies of the walk because two copies is how the observing path
        /// and the driving path start disagreeing about what "pending" means.
        Assets::AssetRef<Graphic::Image3D> Resolve( const Assets::AssetHandle& handle, bool mayRequest );

        /// Starts the read for @p handle. Split out because it is the one place that binds the two
        /// delegates, and T2.3's rule is about that pair.
        void BeginRead( const Assets::AssetHandle& handle, Entry& entry );

        std::unordered_map<Assets::AssetHandle, Entry> m_Volumes;
        Assets::AssetHandle                            m_Default{ 0 };
        bool                                           m_ReportedMissingDefault = false;
        /// Handles already named in a "referenced but not announced" error. One line per bad reference,
        /// not one per frame.
        std::unordered_set<Assets::AssetHandle> m_ReportedMissing;
    };
} // namespace Desert::Runtime
