#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/Serialization/ControlRig.hpp>

namespace Desert::Assets
{
    /**
     * @brief A control rig on disk (`.derig`), in the engine's ONE asset system.
     *
     * The same `AssetBase` / `AssetManager` as a texture, a material or a UI theme, so it gets the Content
     * Browser, the drag-and-drop payload, the scene reference and the hot reload for free — and, the part
     * this tier actually needed, an `AssetHandle` a component can hold.
     *
     * A PATH STRING WOULD HAVE BEEN CHEAPER AND IS THE WRONG ANSWER. This engine already has exactly one
     * way for a component to name content, and it is `AssetHandle` + `Asset<T>` on the property; a second
     * addressing scheme would mean the eviction walk cannot see a rig (`SceneAssetRoots`), the Details
     * page cannot offer a picker, and a moved file breaks silently instead of being re-resolved.
     *
     * WHAT IT DOES NOT DO: resolve its names to indices. The parsed file is skeleton-independent by
     * construction (see Serialization/ControlRig.hpp — everything is a name), and the same rig is legally
     * shared by two entities with different skeletons. `AnimationECSSystem` is what builds a
     * `ControlRigStage` per entity, against THAT entity's skeleton. Putting the built stage here would
     * hand the second entity the first one's bone indices.
     */
    class ControlRigAsset final : public AssetBase
    {
    public:
        ControlRigAsset( AssetPriority priority, const Common::Filepath& filepath );

        /// Reads and parses the file. A file that is missing, malformed, from an unknown format version or
        /// describing a rig the loader cannot honour is an ERROR carrying the reason — never a quietly
        /// substituted empty rig, which would attach as a stage that cannot change the pose.
        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        [[nodiscard]] bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        [[nodiscard]] const Serialization::ControlRigData& GetData() const
        {
            return m_Data;
        }

        /// What to show in a slot. The file's `Name` when it has one, the file's stem when it does not.
        [[nodiscard]] const std::string& GetDisplayName() const
        {
            return m_DisplayName;
        }

        /// Bumped by every successful Load. The ECS system holds the revision it last built a stage from,
        /// so a hot-reloaded rig is rebuilt and an unchanged one is not — which is what stops the per-frame
        /// sync from re-resolving every name sixty times a second.
        [[nodiscard]] uint32_t GetRevision() const
        {
            return m_Revision;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::ControlRig;
        }

        /// Writes a rig to disk, creating the directory if needed. Static because saving is what CREATES
        /// an asset: writing through an instance would mean an instance had to exist for a file that does
        /// not.
        static Common::BoolResultStr Save( const Common::Filepath&              filepath,
                                           const Serialization::ControlRigData& data );

    private:
        Serialization::ControlRigData m_Data;
        std::string                   m_DisplayName;
        bool                          m_Ready    = false;
        uint32_t                      m_Revision = 0;
    };
} // namespace Desert::Assets
