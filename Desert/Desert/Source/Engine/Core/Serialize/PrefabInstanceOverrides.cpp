#include <Engine/Core/Serialize/PrefabInstanceOverrides.hpp>

#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/Assets/Prefab/PrefabOverrides.hpp>
#include <Engine/Core/Serialize/EntitySerializer.hpp>
#include <Engine/Core/Serialize/EntityReferenceRemap.hpp>

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <Engine/ECS/PrefabComponent.hpp>
#include <Engine/ECS/PrefabInstanceComponent.hpp>
#include <Engine/ECS/RelationshipComponent.hpp>
#include <Engine/ECS/UUIDComponent.hpp>

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

                // The nesting record's own transform places the nested root (PrefabFactory, before the overrides),
                // so the base the live root is diffed against carries it - else every unedited link reads as
                // moved.
                const auto& nestedRecords = nested->GetEntities();
                if ( ( record.Translation.has_value() || record.Rotation.has_value() ||
                       record.Scale.has_value() ) &&
                     !nestedRecords.empty() && nestedRecords.front().id.has_value() )
                {
                    std::vector<Common::UUID> rootPath = path;
                    rootPath.push_back( *nestedRecords.front().id );
                    if ( const auto root = out.find( Assets::PrefabPathKey( rootPath ) ); root != out.end() )
                    {
                        if ( record.Translation.has_value() )
                            root->second.Translation = record.Translation;
                        if ( record.Rotation.has_value() )
                            root->second.Rotation = record.Rotation;
                        if ( record.Scale.has_value() )
                            root->second.Scale = record.Scale;
                    }
                }

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

        // THE RECORD EVERY LIVE ENTITY OF THE INSTANCE CAME FROM. PrefabFactory points a body's entity references
        // (Rules::kEntityReferences) at the uuids it mints, while the base record states the RECORD id - so a
        // reference to an entity of the same scope is compared, and written, as that record id. Measured raw, every
        // instance holding a SocketAttachment inside its body carried a "changed" Target naming a uuid minted for
        // this session only: saved as an override, it pointed the attachment at nothing on the next load.
        std::unordered_map<Common::UUID, std::vector<Common::UUID>> sourcePaths;
        {
            std::vector<entt::entity> walk{ instanceRoot.GetHandle() };
            while ( !walk.empty() )
            {
                const ECS::Entity member{ walk.back(), *registry };
                walk.pop_back();
                if ( member.HasComponent<ECS::RelationshipComponent>() )
                    for ( const entt::entity child : member.GetComponent<ECS::RelationshipComponent>().Children )
                        walk.push_back( child );
                if ( member.HasComponent<ECS::PrefabInstanceComponent>() && member.HasComponent<ECS::UUIDComponent>() )
                    sourcePaths.emplace( member.GetComponent<ECS::UUIDComponent>().UUID,
                                         member.GetComponent<ECS::PrefabInstanceComponent>().SourcePath );
            }
        }

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
            if ( handle == instanceRoot.GetHandle() )
            {
                capture.RootPath = path;
            }
            const auto                       found = base.find( Assets::PrefabPathKey( path ) );
            if ( found == base.end() )
            {
                ++capture.UnaddressableEntities;
                continue;
            }

            Assets::EntityData live = EntitySerializer::SerializeEntity( entity, assetManager );
            ::Desert::Core::Rules::RemapEntityReferences(
                 live,
                 [&sourcePaths, &path]( const Common::UUID& stated ) -> std::optional<Common::UUID>
                 {
                     const auto hit = sourcePaths.find( stated );
                     if ( hit == sourcePaths.end() || path.empty() || hit->second.size() != path.size() ||
                          !std::equal( path.begin(), path.end() - 1, hit->second.begin() ) )
                         return std::nullopt; // outside this record's file scope: a scene entity, kept as is
                     return hit->second.back();
                 } );

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
