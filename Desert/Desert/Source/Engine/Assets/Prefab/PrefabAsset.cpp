#include "PrefabAsset.hpp"
#include "PrefabFormat.hpp"
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Core/Serialize/EntitySerializer.hpp>
#include <Engine/ECS/Components.hpp>
#include <functional>
#include <Engine/Core/Scene.hpp>
#include <Engine/Runtime/Factory/PrefabFactory.hpp>
#include <unordered_set>

namespace Desert::Assets
{
    Common::BoolResultStr PrefabAsset::Load()
    {
        const auto raw = Common::Utils::FileSystem::ReadFileContent( m_Metadata.Filepath );
        if ( !raw )
        {
            return Common::MakeError<bool>( raw.GetError() );
        }
        if ( raw.GetValue().empty() )
        {
            return Common::MakeError<bool>( "Prefab file is empty: " + m_Metadata.Filepath.string() );
        }

        // The version gate (Д28): a prefab is the scene's own entity payloads in a different container,
        // so it is held to the same generation the scene loader requires. An old file is refused BY NAME
        // — file, found versions, required versions, and the SceneMigrator command that converts it —
        // instead of being read into whatever the current parser happens to make of it.
        //
        // The argument is raw.GetValue() and not raw because Ф3 made the read primitive return a
        // ResultStr: both halves of this merge are kept, and the unwrap is safe precisely because the
        // two guards above already refused the miss and the empty file by name.
        auto loadable = ParseLoadablePrefab( m_Metadata.Filepath.string(), raw.GetValue() );
        if ( !loadable )
        {
            return Common::MakeError<bool>( loadable.GetError() );
        }

        m_EntityData = std::move( loadable.GetValue().Entities );
        m_IsLoaded   = true;
        // The file IS the payload now, whatever this asset held before.
        m_CapturedInMemory = false;

        return BOOLSUCCESS;
    }

    Common::BoolResultStr PrefabAsset::Unload()
    {
        // A CAPTURE IS NOT AN EVICTION CANDIDATE. CreateFromEntity fills this asset from a live entity and
        // sets m_IsLoaded WITHOUT any file existing yet — "Create Prefab" then "save" is two steps, and
        // between them the only copy of the payload is this vector.
        if ( !IsReloadableFromFile() )
        {
            return Common::MakeFormattedError<bool>(
                 "'{}' holds a prefab captured from a live entity and not yet written to disk. Releasing it "
                 "would destroy the capture and Load() would fail or read an older file. The asset stays "
                 "resident.",
                 m_Metadata.Filepath.string() );
        }

        m_EntityData.clear();
        // `clear()` alone kept the whole buffer — EntityData is a fat record and a prefab of a few hundred
        // entities keeps every byte of its capacity. Every other clearing implementation in this directory
        // shrinks; this one did not.
        m_EntityData.shrink_to_fit();
        m_IsLoaded = false;
        return BOOLSUCCESS;
    }

    std::string PrefabAsset::Serialize() const
    {
        PrefabData data;
        data.Name = m_Metadata.Filepath.stem().string();
        data.Entities = m_EntityData;
        // Root names the first record BY ID, so it can only be written when that record has one. It used to
        // fall back to a default-constructed UUID, which was a random number — a root id pointing at no
        // entity, baked into the file.
        if ( !m_EntityData.empty() )
        {
            const auto& firstId = m_EntityData.front().id;
            if ( firstId.has_value() )
                data.Root = *firstId;
        }

        // The one writer: stamps both generation integers, so every file this engine saves is one its
        // own gate accepts. Writing rfl::json directly here would be a prefab the loader refuses.
        return WritePrefabJson( std::move( data ) );
    }

    void PrefabAsset::CreateFromEntity( ECS::Entity rootEntity, const AssetManager& assetManager )
    {
        if ( !rootEntity ) return;

        m_EntityData.clear();
        
        std::function<void( ECS::Entity )> traverse = [&]( ECS::Entity e )
        {
            if ( !e )
                return;

            // Serialize current entity
            m_EntityData.push_back( Core::Serialize::EntitySerializer::SerializeEntity( e, assetManager ) );

            // Traverse children
            if ( e.HasComponent<ECS::RelationshipComponent>() )
            {
                const auto& rel = e.GetComponent<ECS::RelationshipComponent>();
                for ( auto childHandle : rel.Children )
                {
                    traverse( ECS::Entity{ childHandle, *e.GetRegistry() } );
                }
            }
        };

        traverse( rootEntity );

        // Root entity's own PrefabComponent points to this very file — strip it from the
        // serialized data so that re-instantiating doesn't recurse into itself.
        if ( !m_EntityData.empty() )
            m_EntityData[0].PrefabPath = std::nullopt;

        m_IsLoaded = true;
        // Nothing on disk holds this yet — see IsReloadableFromFile and Unload's refusal.
        m_CapturedInMemory = true;
    }

    // Placing an instance in the world. The BUILDING of the instance is not here and must not be: it is
    // PrefabFactory::Instantiate, and this used to be a second copy of it that had drifted in three ways at
    // once — it resolved duplicate ids the other way round (`insert` against the factory's `operator[]`),
    // it ignored PrefabPath entirely so a prefab nested inside a prefab silently did not appear, and it
    // guessed the root by looking for the first record with no `parent`. That guess is wrong for every
    // prefab cut from an entity that HAD a parent: the first record then carries a parent id naming an
    // entity outside the file, no record is parentless at all, and the function returned a null entity
    // while leaving its entities in the scene — the editor's "Instantiate Prefab" appeared to do nothing.
    // The factory takes the first record, which is the entity the prefab was cut from by construction.
    //
    // Everything this adds over the factory is the placement: the position argument the Lua binding and the
    // editor's drag-and-drop use.
    ECS::Entity PrefabAsset::Instantiate( Core::Scene* scene, const AssetManager& assetManager,
                                          const glm::vec3* position ) const
    {
        if ( !scene || m_EntityData.empty() )
            return {};

        std::unordered_set<Common::UUID> stack;
        ECS::Entity                      rootEntity =
             Runtime::Factory::PrefabFactory::Instantiate( *this, *scene, assetManager, stack );

        if ( position && rootEntity )
        {
            if ( rootEntity.HasComponent<ECS::TransformComponent>() )
            {
                auto& tc       = rootEntity.GetComponent<ECS::TransformComponent>();
                tc.Translation = *position;
            }
            else
            {
                rootEntity.AddComponent<ECS::TransformComponent>().Translation = *position;
            }
        }

        return rootEntity;
    }

} // namespace Desert::Assets