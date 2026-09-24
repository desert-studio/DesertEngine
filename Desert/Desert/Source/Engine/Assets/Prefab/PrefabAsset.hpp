#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AssetEvents.hpp>
#include <Engine/ECS/Entity.hpp>

#include "PrefabData.hpp"

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Assets
{
    class PrefabAsset : public AssetBase
    {
    public:
        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::Prefab;
        }

        explicit PrefabAsset( const AssetPriority priority, const Common::Filepath& filepath )
             : AssetBase( priority, filepath, AssetTypeID::Prefab )
        {
        }

        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        Common::ResultStr<std::string> Serialize() const;
        // Serialize, then the atomic replace: a failure of either refuses and leaves the file as it was.
        Common::BoolResultStr
        SaveTo( const std::filesystem::path& file ) const; // fails if the writer produced text that is not JSON

        bool IsReadyForUse() const override
        {
            return m_IsLoaded;
        }

        // A prefab captured from a live entity has not been written yet — see CreateFromEntity below and
        // Unload's refusal.
        bool IsReloadableFromFile() const override
        {
            return !m_CapturedInMemory;
        }
        const std::vector<EntityData>& GetEntities() const
        {
            return m_EntityData;
        }

        void CreateFromEntity( ECS::Entity rootEntity, const AssetManager& assetManager );

        // Place one instance of this prefab in @p scene, under @p parent (a null entity means the scene
        // root), optionally moving its root to @p position.
        //
        // IT ANSWERS WITH A REASON WHEN IT REFUSES, and that is what the bare `ECS::Entity` it used to
        // return could not do. Two of its failures are silent by nature: a null entity looks like "no
        // prefab" whatever went wrong, and a UI prefab placed outside a canvas SUCCEEDS while covering no
        // pixels (see PrefabPlacement.hpp). Both now come back as text naming the file, the target and
        // what to do instead.
        //
        // @p parent IS THE ARGUMENT THAT WAS MISSING. Every caller before Ю19 created the instance at the
        // scene root and, in one case out of six, attached it afterwards — which is fine for a mesh and
        // is the whole defect for a UI element, whose parent decides both whether it is drawn and where.
        [[nodiscard]] Common::ResultStr<ECS::Entity> Instantiate( Core::Scene*        scene,
                                                                  const AssetManager& assetManager,
                                                                  ECS::Entity         parent   = {},
                                                                  const glm::vec3*    position = nullptr ) const;

    private:
        std::vector<EntityData> m_EntityData;
        bool                    m_IsLoaded = false;
        // Set by CreateFromEntity and cleared by Load: this payload came from a live entity and no file
        // holds it yet, so releasing it destroys the only copy.
        bool m_CapturedInMemory = false;
    };
} // namespace Desert::Assets