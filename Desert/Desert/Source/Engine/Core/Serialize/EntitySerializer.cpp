#include "EntitySerializer.hpp"
#include "ComponentRegistry.hpp"
#include <Common/Json/Document.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>

namespace Desert::Core::Serialize
{
    // EntitySerializer is now a thin orchestrator:
    //   - "meta" (id / parent / tag / transform / prefab link) is structural and consumed directly by the
    //     scene & prefab loaders, so it stays here.
    //   - every actual component is (de)serialized generically through the ComponentRegistry, so adding a
    //     component (or a field to a reflected component) no longer touches this file.

    Assets::EntityData EntitySerializer::SerializeEntity( ECS::Entity entity, const Assets::AssetManager& assetManager )
    {
        Assets::EntityData data;

        // ---- Meta ----
        if ( entity.HasComponent<ECS::UUIDComponent>() )
            data.id = entity.GetComponent<ECS::UUIDComponent>().UUID;
        else
            data.id = Common::UUID::Generate(); // no UUIDComponent: mint one so the record is addressable

        if ( entity.HasComponent<ECS::RelationshipComponent>() )
        {
            entt::entity parentHandle = entity.GetComponent<ECS::RelationshipComponent>().Parent;
            if ( parentHandle != entt::null )
            {
                ECS::Entity parentEntity{ parentHandle, *entity.GetRegistry() };
                if ( parentEntity.HasComponent<ECS::UUIDComponent>() )
                    data.parent = parentEntity.GetComponent<ECS::UUIDComponent>().UUID;
            }
        }

        if ( entity.HasComponent<ECS::TagComponent>() )
            data.Tag = entity.GetComponent<ECS::TagComponent>().Tag;

        if ( entity.HasComponent<ECS::PrefabComponent>() )
        {
            auto handle = entity.GetComponent<ECS::PrefabComponent>().Prefab;
            if ( handle )
            {
                auto asset = assetManager.FindByHandle<Assets::PrefabAsset>( handle );
                if ( asset )
                    data.PrefabPath = asset->GetMetadata().Filepath.string();
            }
        }

        if ( entity.HasComponent<ECS::TransformComponent>() )
        {
            const auto& tc  = entity.GetComponent<ECS::TransformComponent>();
            data.Translation = tc.Translation;
            data.Rotation    = tc.Rotation;
            data.Scale       = tc.Scale;
        }

        // ---- Components (generic, registry-driven) ----
        for ( const auto& serializer : ComponentRegistry::Get().All() )
        {
            if ( serializer.Has( entity ) )
                data.Components[serializer.Key] = serializer.Serialize( entity, assetManager );
        }

        return data;
    }

    void EntitySerializer::DeserializeEntity( const Assets::EntityData& data, ECS::Entity entity, const Assets::AssetManager& assetManager )
    {
        // ---- Meta ----
        // Tag — entity is always created with TagComponent; avoid double AddComponent
        if ( data.Tag )
        {
            if ( entity.HasComponent<ECS::TagComponent>() )
                entity.GetComponent<ECS::TagComponent>().Tag = *data.Tag;
            else
                entity.AddComponent<ECS::TagComponent>().Tag = *data.Tag;
        }

        if ( data.Translation || data.Rotation || data.Scale )
        {
            auto& tc = entity.GetComponent<ECS::TransformComponent>();
            if ( data.Translation ) tc.Translation = *data.Translation;
            if ( data.Rotation )    tc.Rotation    = *data.Rotation;
            if ( data.Scale )       tc.Scale       = *data.Scale;
        }

        // ---- Components (generic, registry-driven) ----
        // ExtraFields holds component payloads keyed by registry key — for both new saves and legacy
        // files where the same keys lived directly at the entity's top level.
        // Each block is read at its place in the scene, so a wrong-typed value names its entity, component and
        // field ("Entities[id=4127].Light.Intensity"); the entity's issues are reported in one line.
        const Common::Json::Path entities = Common::Json::Path().Key( "Entities" );
        const Common::Json::Path record =
             entity.HasComponent<ECS::UUIDComponent>()
                  ? entities.Record( entity.GetComponent<ECS::UUIDComponent>().UUID.ToString() )
                  : entities;
        Common::Json::Issues issues;
        for ( const auto& serializer : ComponentRegistry::Get().All() )
        {
            auto found = data.Components.get( serializer.Key );
            if ( found.has_value() )
                serializer.Deserialize( entity, Common::Json::Root( found.value(), record.Key( serializer.Key ) ),
                                        assetManager, issues );
        }
        if ( !issues.empty() )
            Common::Json::ReportIssues( issues, "scene component" );

        // ---- Prefab link (meta) ----
        if ( data.PrefabPath )
        {
            auto asset = assetManager.FindByPath<Assets::PrefabAsset>( *data.PrefabPath );
            if ( asset )
                entity.AddComponent<ECS::PrefabComponent>().Prefab = asset->GetMetadata().Handle;
        }
    }
} // namespace Desert::Core::Serialize
