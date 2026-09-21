#pragma once

#include <Engine/Assets/AssetRef.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/CloudModellingVolumeAsset.hpp>
#include <Engine/Graphic/Image.hpp>

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Desert::Runtime
{
    /// The image the march samples every hero cloud of a frame through, and how many bodies are in it.
    /// A `Volume` of nullptr means no body was asked for; the caller binds the fallback image, which it
    /// must do in that case rather than binding nothing — a declared `sampler3D` with no image is an
    /// INVALID descriptor set, and this engine's compute path answers one by skipping the whole dispatch.
    struct CloudModellingAtlasBinding
    {
        // CO-OWNED, and it has to be. This service is PROCESS-WIDE while SceneRenderers are not: six
        // renderer slots may be live at once (Docs/RENDERER_FRAME_STATE.md), and the moment a second one
        // asks EnsureAtlas for a different set of bodies the service replaces `m_Atlas` and the previous
        // image is destroyed — under the first renderer, which is still holding it and still marching
        // through it. A handle instead of a pointer makes that impossible: the old atlas survives until
        // the last renderer that took it lets go. A8-2.
        std::shared_ptr<Graphic::Image3D> Volume;
        uint32_t                          SlabCount = 0;
    };

    /**
     * @brief Owns the GPU side of the sculpted cloud bodies: ONE `Image3D` holding the bodies a frame
     *        actually needs, laid end to end along the depth axis.
     *
     * The same shape as CloudNoiseService next door, and it exists for the same reason: the asset layer
     * must not know about Vulkan, and the renderer must not know how to read a file.
     *
     * THERE IS NO DEFAULT HERE, and that is the difference from CloudNoiseService. An empty noise slot
     * must resolve to something because every scene with clouds needs a shape; an empty HERO CLOUD slot
     * means the artist has not chosen a body, and the honest answer is that there is no hero cloud —
     * inventing one would put a cloud in the sky nobody authored. `HasBody( 0 )` is therefore false
     * without a word, and a handle that names a volume nobody registered is false WITH one.
     *
     * WHY AN ATLAS AND NOT ONE IMAGE PER BODY. A0 bound a single `sampler3D` and could draw one body per
     * frame; several DIFFERENT bodies need several volumes reachable from one dispatch, and this engine's
     * shader reflection refuses arrays of descriptors in so many words (VulkanShaderReflection.cpp). The
     * version that compiles without touching the descriptor machinery is one image, addressed by
     * arithmetic — see the note beside CLOUD_MODELLING_ATLAS_MAX_SLABS in Common/CloudAuthored.glslh.
     *
     * IT IS BUILT ON DEMAND AND HOLDS ONLY WHAT THE SCENE USES. A project may carry fifty `.dcmv` files;
     * a frame pays 4.00 MiB for each body an entity actually names, not for the library. A fixed
     * eight-slab atlas would cost 32.00 MiB in every scene with one hero cloud in it, and against decision
     * D-9's 64 MiB that is the difference between a subsystem that fits at 1920x1080 and one that does
     * not.
     */
    class CloudModellingService
    {
    public:
        /// THE PROJECT HAS THIS BODY. Records the handle and the (unread) asset; opens no file.
        ///
        /// Reading every `.dcmv` at boot cost a measured **913.0 ms of a 5707.0 ms boot** on this machine
        /// for three sculpted bodies of 4 MiB each, and a scene with no hero cloud in it paid all of it.
        /// The header's own promise above — *"a frame pays 4.00 MiB for each body an entity actually
        /// names, not for the library"* — was true of the ATLAS and false of the boot; it is true of both
        /// now.
        void Announce( const Assets::Asset<Assets::CloudModellingVolumeAsset>& asset );

        /// Keeps @p asset so its voxels can be laid into an atlas, from bytes already in hand. The bytes
        /// are NOT copied: the asset holds 4 MiB of them already and a second copy would be a second
        /// thing to keep in step.
        Common::BoolResultStr Register( const std::shared_ptr<Assets::CloudModellingVolumeAsset>& asset );

        /**
         * @brief THE THREE-STATE ANSWER, and the only thing that starts a read.
         *
         * - **Null** — an EMPTY slot (silence: there is no built-in hero cloud and there must not be one),
         *   or a handle the scan never found, or one whose read failed. Logged once per handle for the
         *   latter two; `HasBody`, which this replaces, logged once per FRAME.
         * - **Pending** — the body is being read. The caller must not build an atlas without it: the
         *   atlas would then be rebuilt — twelve megabytes of upload — on the frame it arrived.
         * - **Ready** — the voxels are here and `GetSizeKm` can answer.
         */
        Assets::AssetRef<Assets::CloudModellingVolumeAsset> RequireBody( const Assets::AssetHandle& handle );

        /// The answer as it stands: never reads, never requests, never logs. Not `const` for the reason
        /// CloudNoiseService::Peek states.
        [[nodiscard]] Assets::AssetRef<Assets::CloudModellingVolumeAsset>
        Peek( const Assets::AssetHandle& handle );

        /// How many announced bodies have actually been read.
        [[nodiscard]] size_t ResidentCount() const;

        /// The authored SIZE of that volume in kilometres, which the renderer needs to build the
        /// instance's transform and its bounds. Returned beside the image rather than looked up from the
        /// asset again, because the two must describe the same file and a second lookup is a second
        /// chance to describe a different one.
        glm::vec3 GetSizeKm( const Assets::AssetHandle& handle );

        /**
         * @brief The atlas holding exactly @p bodies, in that order — built if the request has changed
         *        since the last one, reused otherwise.
         *
         * @param bodies  distinct, registered handles, at most Graphic::kCloudModellingAtlasMaxSlabs of
         *                them. The caller owns the de-duplication because the caller is what knows which
         *                entity asked for what, and slab i is bodies[i].
         *
         * REBUILT ONLY WHEN THE REQUEST OR A REVISION CHANGES. Uploading 4 MiB per body every frame would
         * cost more than the march does; uploading when the scene's set of hero bodies changes costs it
         * once, where the scene is already being loaded.
         */
        CloudModellingAtlasBinding EnsureAtlas( const std::vector<Assets::AssetHandle>& bodies );

        // `GetGeneration()` STOOD HERE AND ITS OWN COMMENT NAMED THE READER IT NEVER HAD: "the renderer
        // compares it to decide whether the descriptor it bound last frame still points at the same
        // image". No renderer ever called it — the guard was written, documented, and not connected, which
        // is the shape Г12 spent a day removing elsewhere and which costs more here than it did there.
        // Co-ownership answers the question the counter was for, and answers it without anyone having to
        // remember to ask: a renderer holding the binding IS holding that image, so "is it still the same
        // one" cannot be got wrong. A8-2.

        void Clear();

    private:
        struct Entry
        {
            /// Announced, possibly unread. The source of the request: a handle cannot be read.
            std::shared_ptr<Assets::CloudModellingVolumeAsset> Asset;
            /// FILLED BY `Register`, NOT BY `Announce`, because the authored size is inside the file. An
            /// announced-but-unread body therefore reports zero — which is why nothing may ask for it
            /// before `RequireBody` says Ready.
            glm::vec3           SizeKm{ 0.0f };
            uint32_t            Revision = 0;
            bool                Loaded   = false;
            Assets::LoadRequest Request;
            bool                Failed = false;
        };

        Assets::AssetRef<Assets::CloudModellingVolumeAsset> Resolve( const Assets::AssetHandle& handle,
                                                                     bool                       mayRequest );
        void BeginRead( const Assets::AssetHandle& handle, Entry& entry );

        /// Handles already named in a "referenced but not announced" error. `HasBody` had no such set and
        /// logged its error EVERY FRAME for a scene with a stale reference; with a pending state in the
        /// mix that would have become a log nobody can read at all.
        std::unordered_set<Assets::AssetHandle> m_Reported;

        std::unordered_map<Assets::AssetHandle, Entry> m_Volumes;

        std::shared_ptr<Graphic::Image3D> m_Atlas;

        /// What the live atlas was built from: slab i is m_AtlasSlabs[i] at revision m_AtlasRevisions[i].
        /// Both are compared against the next request, and a difference in EITHER is a rebuild — a hot
        /// reload changes the second without touching the first.
        std::vector<Assets::AssetHandle> m_AtlasSlabs;
        std::vector<uint32_t>            m_AtlasRevisions;
    };
} // namespace Desert::Runtime
