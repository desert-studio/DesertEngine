#pragma once

#include <Engine/Assets/AssetRef.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/CloudLayoutAsset.hpp>

#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace Desert::Runtime
{
    /**
     * @brief The painted cloud layouts a scene can point at, resolved once for every viewport.
     *
     * The same shape as CloudTypeService next door and it exists for the same two reasons: the renderer
     * must not know how to read a file, and an editor with three viewports must not resolve the same asset
     * three times. Like that service it owns NO GPU resource, and here that is not a detail but the point —
     * a layout never reaches a sampler at all. It is consumed on the CPU by the placement bake, which is
     * what makes a painting cost the march nothing.
     *
     * THE EMPTY SLOT IS THE DEFAULT AND IT IS NOT AN ERROR. A layer with no layout bound places its clouds
     * exactly as it did before this asset existed: the procedural patch field decides which parts of the
     * sky are busy, and Get() answers with a null pointer that the bake reads as "there is no painting".
     * That is the phase's whole acceptance criterion — an empty slot must render the frame it rendered
     * before — so the absence has to be expressible rather than approximated.
     *
     * A handle that names a layout nobody registered IS an error and is logged as one, once, with the
     * handle in the message. Falling through to "no painting" silently would render a sky that is merely
     * not the one the artist painted, which is the least diagnosable thing this subsystem can do.
     */
    class CloudLayoutService
    {
    public:
        /// THE PROJECT HAS THIS PAINTING. Records the handle and the (unread) asset; opens no file.
        ///
        /// This is what `AssetPreloader::PreloadCloudLayouts` does now. Reading every `.dclayout` at boot
        /// cost a measured **689.0 ms of a 5707.0 ms boot** on this machine for ten files totalling
        /// 10.3 MiB, and every scene in this repository leaves both layout slots EMPTY — so all of it was
        /// spent on paintings nothing in the project points at.
        void Announce( const Assets::Asset<Assets::CloudLayoutAsset>& asset );

        /// Caches @p asset under its handle, from bytes already in hand. Called again for the same asset
        /// after a hot reload; a changed content hash replaces the entry, an unchanged one is a no-op.
        Common::BoolResultStr Register( const std::shared_ptr<Assets::CloudLayoutAsset>& asset );

        /**
         * @brief The painting a layer's slot resolves to, or NULL when there is none.
         *
         * A POINTER AND NOT A REFERENCE, unlike CloudTypeService::GetShape, and the difference is the
         * difference between the two slots. A cloud TYPE has a built-in default because a sky must exist;
         * a LAYOUT has no default because the absence of a painting is itself a meaningful, shipped state —
         * the one every scene in the repository is in. Returning a reference would have forced an empty
         * CloudLayoutData to stand for "none", and a table of zeros is not nothing: under the zero-mean
         * pattern rule it would push every cell's coverage the same way.
         *
         * SHARED AND NOT BORROWED, through the aliasing constructor: the pointer owns a share of the ASSET
         * and addresses its layout member. The bake's parameters are CACHED by the renderer across frames
         * and re-used whenever the region shifts, so a borrowed pointer would outlive an asset the user
         * unloaded — and the failure would be a read of freed pixels inside a bake, which is the least
         * diagnosable crash this subsystem could have.
         */
        /**
         * @brief THE THREE-STATE ANSWER, and the only thing that starts a read.
         *
         * - **Null** — no painting. An EMPTY handle is Null, and that is the shipped state of every scene
         *   in this repository: the bake reads it as "there is no painting" and places the sky exactly as
         *   it did before these fields existed. A handle naming a layout the scan never found is also
         *   Null, and is logged once.
         * - **Pending** — the painting exists and is being read. The bake MUST NOT run: falling through
         *   to "no painting" here would place the clouds procedurally for a layer the artist painted, and
         *   then silently correct itself a few frames later. That is the quiet degradation this whole
         *   change is about, and it is why the two states cannot share a null pointer.
         * - **Ready** — the pixels are here.
         *
         * SHARED AND NOT BORROWED, through the aliasing constructor, exactly as the pointer this replaces
         * was: the payload owns a share of the ASSET and addresses its layout member. The bake's
         * parameters are cached across frames, so a borrowed pointer would outlive an unloaded asset and
         * the failure would be a read of freed pixels inside a bake.
         */
        Assets::AssetRef<const Assets::CloudLayoutData> Require( const Assets::AssetHandle& handle );

        /// The answer as it stands: never reads, never requests, never logs. Not `const` for the reason
        /// CloudNoiseService::Peek states: it shares one walk with `Require`, and a cast to keep the
        /// signature pretty would be a promise the body does not keep.
        [[nodiscard]] Assets::AssetRef<const Assets::CloudLayoutData> Peek( const Assets::AssetHandle& handle );

        /// How many announced layouts have actually been read. Used to be the number of `.dclayout` files
        /// on disk by construction; now it is the number something asked for.
        [[nodiscard]] size_t ResidentCount() const;

        void Clear();

    private:
        struct Entry
        {
            /// Announced, possibly unread. The source of the request: a handle cannot be read.
            std::shared_ptr<Assets::CloudLayoutAsset> Asset;
            uint32_t                                  ContentHash = 0;
            /// SET BY `Register`, AND THE READINESS THIS SERVICE ANSWERS ON — deliberately not
            /// `Asset->IsReadyForUse()`. `AssetEviction` may `Unload()` an unreachable layout after it
            /// was registered, and asking the asset would then flip this entry back to Pending and start
            /// a fresh read on every sweep: a file re-read forever, at the frame rate of the sweep. The
            /// pointer handed out is the same one the version before this change handed out under the
            /// same conditions, so this flag preserves that behaviour rather than inventing one.
            bool Loaded = false;
            /// Live while a worker is reading. See CloudNoiseService for why it is cancelled rather than
            /// dropped in `Clear()`.
            Assets::LoadRequest Request;
            /// Read and failed. Latched: a retry per frame on a corrupt file is a frame-rate defect.
            bool Failed = false;
        };

        Assets::AssetRef<const Assets::CloudLayoutData> Resolve( const Assets::AssetHandle& handle,
                                                                 bool                       mayRequest );
        void BeginRead( const Assets::AssetHandle& handle, Entry& entry );

        std::unordered_map<Assets::AssetHandle, Entry> m_Layouts;
        // Handles already complained about. A missing layout is a permanent state of the scene, so without
        // this the error would be logged every frame of every viewport and bury everything else.
        std::unordered_set<Assets::AssetHandle> m_Reported;
    };
} // namespace Desert::Runtime
