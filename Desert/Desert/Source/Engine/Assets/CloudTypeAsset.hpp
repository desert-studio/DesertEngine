#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/CloudTypeData.hpp>

namespace Desert::Assets
{
    /**
     * @brief A cloud type on disk (`.decloudtype`), in the engine's ONE asset system.
     *
     * The same `AssetBase` / `AssetManager` as a texture, a material or a noise volume, so it gets the
     * Content Browser, the drag-and-drop payload, the scene reference and the hot reload for free. A second
     * asset system for one file type would have been a week of plumbing and a permanent second place to
     * look (§2.2 of the contract).
     *
     * WHAT IT HOLDS. Twelve numbers, a name, and the path of the noise volume this type's edge is cut from.
     * Not the profile table: that is generated from the numbers by Graphic::CloudBuildProfileTable whenever
     * the renderer needs it (decision D-13), so there is no baked artefact here that can go stale against
     * the maths that produced it.
     *
     * NO GPU HERE, and no AssetManager either beyond ResolveDependencies. The asset layer knows nothing
     * about `Image3D`; `Runtime::CloudTypeService` is what hands a resolved type to the renderer. That is
     * the layer rule, and it is also what lets this class be covered by a GPU-free test.
     *
     * ITS HANDLE IS DERIVED FROM ITS PATH, like a mesh's and unlike the random uuid AssetBase hands out.
     * That matters because a scene does NOT store the handle: reflected asset fields go through
     * Core::MakeAssetResolver and are written as PATHS (this is how every mesh, material and skybox
     * reference in a `.desce` already works), so the handle only has to be stable and unique WITHIN a
     * session. The random id the base class would have given it is neither — a second launch would hand
     * the same file a different handle, which is the defect the noise volume slot next door still carries
     * and which nothing has noticed only because no scene has ever had a volume in it.
     */
    class CloudTypeAsset final : public AssetBase
    {
    public:
        CloudTypeAsset( AssetPriority priority, const Common::Filepath& filepath );

        /// Reads and parses the file. A file that is missing, malformed, from an unknown format version or
        /// carrying numbers the generator cannot honour is an ERROR carrying the reason and the offending
        /// value — never a quietly built-in default, because a type that silently became the default one
        /// renders as a sky that merely looks like somebody else's.
        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        /// Binds NoiseVolume (a project-relative path in the file) to the volume asset it names. Called by
        /// AssetManager after this asset is created, which is why the noise volumes are preloaded first.
        void ResolveDependencies( AssetManager& manager ) override;

        bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        const CloudTypeData& GetData() const
        {
            return m_Data;
        }

        const Graphic::CloudTypeShape& GetShape() const
        {
            return m_Data.Shape;
        }

        /// The volume this type's edge is cut from, or a null handle for "the built-in default volume".
        /// Null is also what an unresolvable path leaves behind — Load has already logged which path.
        const AssetHandle& GetNoiseVolume() const
        {
            return m_NoiseVolume;
        }

        /// What to show in a slot. The file's DisplayName when it has one, the file's stem when it does not.
        const std::string& GetDisplayName() const
        {
            return m_DisplayName;
        }

        /**
         * @brief Bumped by every successful Load.
         *
         * The renderer holds the last revision it built a profile table for. Comparing the two is the whole
         * rebuild decision, and it answers the right question where a cached handle would answer the wrong
         * one the moment the file changed underneath a slot that did not.
         */
        uint32_t GetRevision() const
        {
            return m_Revision;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::CloudType;
        }

        /**
         * @brief Writes a type to disk, creating the directory if needed.
         *
         * Static because saving is what CREATES an asset — the Cloud Type panel authors numbers and writes
         * them, and only then does the AssetManager get asked to load the file back. Writing through an
         * instance would mean an instance had to exist for a file that does not.
         *
         * Refuses a shape Validate rejects, so an unusable file is never written in the first place.
         */
        static Common::BoolResultStr Save( const Common::Filepath& filepath, const CloudTypeData& data );

    private:
        CloudTypeData m_Data;
        AssetHandle   m_NoiseVolume;
        std::string   m_DisplayName;
        bool          m_Ready    = false;
        uint32_t      m_Revision = 0;
    };
} // namespace Desert::Assets
