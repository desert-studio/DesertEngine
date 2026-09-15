#include <Engine/Core/Serialize/PrefabInstanceOverrides.hpp>

#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/Assets/Prefab/PrefabOverrides.hpp>
#include <Engine/Core/Serialize/EntitySerializer.hpp>
#include <Engine/ECS/Components.hpp>

#include <unordered_map>
#include <unordered_set>

namespace Desert::Core::Serialize
{
    namespace
    {
        using BaseIndex = std::unordered_map<std::string, Assets::EntityData>;

        // Every record of @p prefab and of everything nested under it, keyed by the same path
        // PrefabFactory stamps onto the entities it creates. The two walks mirror each other on purpose:
        // if they ever disagree an override addresses nothing, and that is the failure this whole
        // mechanism is meant to make impossible.
        // NOLINTNEXTLINE(misc-no-recursion) -- a prefab nests prefabs; the depth is the file's nesting
        void IndexBaseRecords( const Assets::PrefabAsset& prefab, const Assets::AssetManager& assetManager,
                               const std::vector<Common::UUID>& prefix, BaseIndex& out,
                               std::unordered_set<Common::UUID>& stack )
        {
            const auto& prefabID = prefab.GetMetadata().Handle;
            if ( stack.contains( prefabID ) )
            {
                return;
            }
            stack.insert( prefabID );

            for ( const Assets::EntityData& record : prefab.GetEntities() )
            {
                if ( !record.id.has_value() )
                {
                    // No stable address — counted by the caller when a live entity lands on it.
                    continue;
                }

                std::vector<Common::UUID> path = prefix;
                path.push_back( *record.id );
                out[Assets::PrefabPathKey( path )] = record;

                if ( !record.PrefabPath.has_value() )
                {
                    continue;
                }

                auto nested = assetManager.FindByPath<Assets::PrefabAsset>( *record.PrefabPath );
                if ( !nested || !nested->IsReadyForUse() )
                {
                    continue;
                }

                IndexBaseRecords( *nested, assetManager, path, out, stack );

                if ( record.PrefabOverrides.has_value() )
                {
                    for ( const Assets::PrefabOverrideData& over : *record.PrefabOverrides )
                    {
                        std::vector<Common::UUID> full = path;
                        full.insert( full.end(), over.Path.begin(), over.Path.end() );
                        const auto found = out.find( Assets::PrefabPathKey( full ) );
                        if ( found != out.end() )
                        {
                            Assets::LayerOverrideOntoRecord( found->second, over );
                        }
                    }
                }
            }

            stack.erase( prefabID );
        }
    } // namespace

    PrefabInstanceCapture CapturePrefabInstance( ECS::Entity                 instanceRoot,
                                                 const Assets::AssetManager& assetManager )
    {
        PrefabInstanceCapture capture;
        if ( !instanceRoot || !instanceRoot.HasComponent<ECS::PrefabComponent>() )
        {
            return capture;
        }

        auto source = assetManager.FindByHandle<Assets::PrefabAsset>(
             instanceRoot.GetComponent<ECS::PrefabComponent>().Prefab );
        if ( !source || !source->IsReadyForUse() )
        {
            return capture;
        }

        BaseIndex                        base;
        std::unordered_set<Common::UUID> stack;
        IndexBaseRecords( *source, assetManager, {}, base, stack );

        entt::registry* registry = instanceRoot.GetRegistry();

        // The walk is the LIVE subtree and not the record list, because what is being measured is what the
        // user has in front of them. Iterative for the same reason PrefabFactory::IndexInstance is.
        std::vector<entt::entity> pending{ instanceRoot.GetHandle() };
        while ( !pending.empty() )
        {
            const entt::entity handle = pending.back();
            pending.pop_back();

            const ECS::Entity entity{ handle, *registry };

            if ( entity.HasComponent<ECS::RelationshipComponent>() )
            {
                for ( const entt::entity child : entity.GetComponent<ECS::RelationshipComponent>().Children )
                {
                    pending.push_back( child );
                }
            }

            if ( !entity.HasComponent<ECS::PrefabInstanceComponent>() )
            {
                ++capture.AddedEntities;
                continue;
            }

            const std::vector<Common::UUID>& path = entity.GetComponent<ECS::PrefabInstanceComponent>().SourcePath;
            const auto                       found = base.find( Assets::PrefabPathKey( path ) );
            if ( found == base.end() )
            {
                ++capture.UnaddressableEntities;
                continue;
            }

            const Assets::EntityData live = EntitySerializer::SerializeEntity( entity, assetManager );

            Assets::PrefabDiffReport report;
            if ( auto over = Assets::DiffPrefabEntity( found->second, live, path, &report ) )
            {
                capture.Overrides.push_back( std::move( *over ) );
            }
            capture.RemovedComponents += report.RemovedComponents;
            capture.PinnedUnstatedFields += report.UnstatedFields;
        }

        return capture;
    }
} // namespace Desert::Core::Serialize
