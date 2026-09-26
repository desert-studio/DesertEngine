#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/CloudTypeAsset.hpp>

#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace Desert::Runtime
{
    /**
     * @brief The cloud types a scene can point at, resolved once for every viewport.
     *
     * The same shape as CloudNoiseService next door, and it exists for the same two reasons: the renderer
     * must not know how to read a file, and an editor with three viewports must not resolve the same asset
     * three times. Unlike that service it owns NO GPU resource — a type is twelve numbers, and the one
     * device object built from them (the 256 x 64 profile table) belongs to the renderer that samples it,
     * because that image is per-view state under Docs/RENDERER_FRAME_STATE.md.
     *
     * THE EMPTY SLOT IS NOT AN ERROR. A layer with no type chosen renders as the built-in cumulus congestus
     * (Assets::CloudTypeDefaultShape), because a scene nobody has authored a type for still has to have a
     * sky. That is a load-bearing requirement of the whole programme rather than a convenience, and it is
     * why Get() returns a reference and never a null.
     *
     * A handle that names a type nobody registered IS an error and is logged as one, with the handle in the
     * message. Falling through to the default silently would render a sky that is merely not the one the
     * artist chose, which is the least diagnosable thing this subsystem can do.
     */
    class CloudTypeService
    {
    public:
        // The manager a handle nothing registered is created in, from its content-registry row (AL1-7).
        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets );

        /// Caches @p asset under its handle. Called again for the same asset after a hot reload; a changed
        /// revision replaces the entry, an unchanged one is a no-op.
        Common::BoolResultStr Register( const std::shared_ptr<Assets::CloudTypeAsset>& asset );

        /// The shape a layer's slot resolves to. Never fails: an empty handle and an unknown one both end
        /// at the built-in default, and only the second says anything.
        const Graphic::CloudTypeShape& GetShape( const Assets::AssetHandle& handle );

        /// The noise volume the type in @p handle names, or a null handle for "the built-in default
        /// volume" — which is what an empty slot resolves to as well.
        Assets::AssetHandle GetNoiseVolume( const Assets::AssetHandle& handle );

        /**
         * @brief Bumped whenever any registered type changes.
         *
         * The renderer holds the generation it last built its profile table under. Comparing the two is
         * what makes a hot reload of a `.decloudtype` show up in the viewport without a restart — the
         * handle in the slot has not changed, so nothing else would tell the renderer to rebuild.
         */
        uint32_t GetGeneration() const
        {
            return m_Generation;
        }

        void Clear();

    private:
        struct Entry
        {
            Graphic::CloudTypeShape Shape;
            Assets::AssetHandle     NoiseVolume;
            uint32_t                Revision = 0;
        };

        // The entry of @p handle, discovered and read now when no one registered it yet; null when the
        // handle is empty or cannot be resolved (said once, with the reason).
        const Entry* FindOrDiscover( const Assets::AssetHandle& handle );

        std::weak_ptr<Assets::AssetManager>            m_Assets;
        std::unordered_map<Assets::AssetHandle, Entry> m_Types;
        // Handles already complained about. A missing type is a permanent state of the scene, so without
        // this the error would be logged every frame of every viewport and bury everything else.
        std::unordered_set<Assets::AssetHandle> m_Reported;
        uint32_t                                m_Generation = 0;
    };
} // namespace Desert::Runtime
