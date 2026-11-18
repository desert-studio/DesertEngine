#pragma once

#include <unordered_map>
#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AssetEvents.hpp>
#include <Engine/ECS/Entity.hpp>

#include "PrefabData.hpp"

#include <Common/Core/Math/AABB.hpp>

#include <optional>
#include <span>
#include <string_view>
#include <vector>

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

        // ITS HANDLE IS HandleForGuid OF ITS HEADER GUID, adopted here from the file (as FoliageTypeAsset
        // and the other header-stating types do), so the handle the content registry states for the row
        // and the one this asset carries are the same number. A file that does not exist yet keeps the
        // path handle - which is why a NEW prefab is written first and created second (SaveNewFromEntity).
        explicit PrefabAsset( const Common::Filepath& filepath );

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

        // How every instance of this prefab streams in a partitioned world unless it overrides it (WP21-WHOLE;
        // PrefabData::RuntimeBehavior, UE ALevelInstance::DesiredRuntimeBehavior). Read from the file by Load and
        // written by Serialize; CreateFromEntity ("Apply to Prefab") takes the captured root's override as the new
        // value, as UE applies an instance's overrides to its level.
        [[nodiscard]] PrefabRuntimeBehavior GetRuntimeBehavior() const
        {
            return m_RuntimeBehavior;
        }

        // THE BODY SET WHOLE, from records a caller already holds - the editor's prefab edit in place keeps the
        // body as it was before a Commit and puts it back on Undo (and the committed one on Redo), so the asset in
        // memory and the file it then writes (SaveTo) are the state the history says. Like CreateFromEntity, the
        // payload is the only copy until it is saved.
        void ReplaceBody( std::vector<EntityData> records );

        // ONE ENTITY OF A PREFAB CUT FROM SEVERAL (CreateFromEntities): the entity, and where it stands in the new
        // prefab's root frame - relative to the pivot the caller chose, which is the file's origin.
        struct Member
        {
            ECS::Entity         Entity;
            ECS::LocalTransform InRoot;
        };

        // UE's ULevelInstanceSubsystem::CreateLevelInstanceFrom: @p members become the children of a NEW root
        // record named @p rootTag that stands at the file's origin. Each member's subtree is captured by
        // CreateFromEntity's rules (a nested instance is a link carrying its overrides, never a copy of its body);
        // its own transform is InRoot, stated on its record - a nesting record's placement lives there too
        // (PrefabFactory places the nested root by it). Refused, the payload as it was, when there is no member or
        // a member entity is gone.
        [[nodiscard]] Common::BoolResultStr CreateFromEntities( std::string_view        rootTag,
                                                                std::span<const Member> members,
                                                                const AssetManager&     assetManager );

        // The box this prefab's body occupies in its root's frame - the partitioner's rule (Rules::PrefabBounds),
        // the one Serialize states in the file and the registry carries. nullopt when nothing in the body has an
        // extent.
        [[nodiscard]] std::optional<Common::Math::AABB> BodyBounds() const;

        // The GUID the file's header states; null for a prefab no file holds yet. Serialize writes it
        // back, so re-saving a prefab never mints a new identity for it.
        [[nodiscard]] const Common::Content::AssetGuid& Guid() const
        {
            return m_Guid;
        }

        // "Save as Prefab": capture @p root into @p file and answer the asset REGISTERED FROM THAT FILE.
        // A prefab already registered at @p file is overwritten in place (its GUID kept); otherwise a
        // draft is written first and only then created through @p assetManager, so the asset is born with
        // its header identity instead of a path handle the registry disagrees with.
        [[nodiscard]] static Common::ResultStr<Asset<PrefabAsset>>
        SaveNewFromEntity( ECS::Entity root, AssetManager& assetManager, const Common::Filepath& file );

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
        // CreateFromEntity's walk: @p e and its subtree, pre-order, appended to the payload. A nested instance
        // (never
        // @p isRoot) is written as ONE link record that states the instance root's placement on itself.
        // @p liveToRecord gains each written record's live uuid -> record id (RemapBodyLinks rewrites the links).
        void AppendSubtree( ECS::Entity e, bool isRoot, const AssetManager& assetManager,
                            std::unordered_map<Common::UUID, Common::UUID>& liveToRecord );

        Common::Content::AssetGuid m_Guid;
        std::vector<EntityData>    m_EntityData;
        PrefabRuntimeBehavior      m_RuntimeBehavior = PrefabRuntimeBehavior::Partitioned;
        bool                    m_IsLoaded = false;
        // Set by CreateFromEntity and cleared by Load: this payload came from a live entity and no file
        // holds it yet, so releasing it destroys the only copy.
        bool m_CapturedInMemory = false;
    };
} // namespace Desert::Assets