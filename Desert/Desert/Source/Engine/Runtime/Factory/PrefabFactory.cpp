#include "PrefabFactory.hpp"
#include <Engine/Assets/Prefab/PrefabOverrides.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Core/Serialize/ComponentRegistry.hpp>
#include <Engine/Core/Serialize/EntitySerializer.hpp>
#include <Engine/Core/Serialize/SceneStitchRules.hpp>

namespace Desert::Runtime::Factory
{
    namespace
    {
        // ComponentRegistry is a vector keyed by string, and an override names its component by that key.
        // A linear scan over ~50 entries, run once per overridden component at load, is not worth a second
        // index that would have to be kept in step with the registry's own.
        [[nodiscard]] const Core::Serialize::ComponentSerializer* FindComponentSerializer( const std::string& key )
        {
            for ( const auto& serializer : Core::Serialize::ComponentRegistry::Get().All() )
            {
                if ( serializer.Key == key )
                {
                    return &serializer;
                }
            }
            return nullptr;
        }
    } // namespace

    // A prefab nests prefabs; the depth is the file's nesting, and `stack` is what bounds it (a cycle is
    // refused by name above). NOLINTNEXTLINE must be the LAST comment line before the statement.
    // NOLINTNEXTLINE(misc-no-recursion)
    ECS::Entity PrefabFactory::Instantiate( const Assets::PrefabAsset& prefab, Core::Scene& scene,
                                            const Assets::AssetManager&       assetManager,
                                            std::unordered_set<Common::UUID>& stack,
                                            const std::vector<Common::UUID>&  pathPrefix,
                                            std::optional<Common::UUID>       rootId )
    {
        if ( prefab.GetEntities().empty() )
            return {};

        const auto& prefabID = prefab.GetMetadata().Handle;

        if ( stack.contains( prefabID ) )
        {
            LOG_ERROR( "Prefab cyclic dependency detected for asset: {0}", prefab.GetMetadata().Filepath.string() );
            return {};
        }

        stack.insert( prefabID );

        const std::vector<Assets::EntityData>& records = prefab.GetEntities();

        // The identity stitch is Rules::PlanSceneStitch, the same function the scene loader plans with —
        // this used to be a hand-written copy of it, and the copy had drifted: it registered records with
        // `entityMap[id] = e`, so when two records in one prefab claimed one id the LAST one silently took
        // it over, while the scene loader's copy gave it to the FIRST. Nothing compiled either file, so
        // neither answer was ever anybody's decision. It is one decision now, and it is documented where
        // it is taken.
        //
        // InstantiatedLater, AND IT USED TO BE CreatedInPlace. While a nested prefab was copied into the
        // outer file, the nesting record was an entity of this file and creating it was right. Ю19 made
        // it a LINK, and creating it then produced a component-less entity sitting between the outer
        // prefab and the nested body — one extra level per capture-and-reinstantiate, invisible on screen
        // because a UI element with no UILayout is laid out as its parent. The nested root now hangs off
        // the nesting record's PARENT, which the stitch resolves with the same id map it resolves every
        // other parent link with (PlannedPrefab::Parent).
        const Core::Rules::StitchPlan plan = Core::Rules::PlanSceneStitch(
             records, &Common::UUID::Generate, Core::Rules::PrefabRecordPolicy::InstantiatedLater );

        // DC §1.4: no silent fallback. Two records of one prefab claiming one id means the second one's
        // components land on the first one's entity and its own entity stays bare — say which prefab and
        // how many, once.
        //
        // UnresolvedParents is deliberately NOT reported: a prefab's first record keeps the `parent` it had
        // in the scene it was cut from, and that entity is by definition not in the prefab. Here "the
        // parent is not in this file" is the normal spelling of "this record is a root of the instance",
        // and warning about it would fire on every well-formed prefab in existence.
        if ( plan.Shadowed > 0 )
        {
            LOG_WARN( "[PrefabFactory] '{0}': {1} record(s) claim an id another record in the same prefab "
                      "already claimed. Their components are applied to the first claimant and their own "
                      "entities are left bare.",
                      prefab.GetMetadata().Filepath.string(), plan.Shadowed );
        }

        // 1. Create all entities with FRESH UUIDs to avoid collisions when multiple instances exist. The id
        // the record carries is a LINK KEY inside this file only — PlannedEntity::Id is what resolved the
        // parent links above, and it never reaches the scene.
        //
        // IT REACHES THE ENTITY AS AN ADDRESS, THOUGH, and that is not the same thing: PrefabInstanceComponent
        // records which RECORD this entity came from, so an override written for that record can find it
        // again after a reload that minted every uuid afresh. Stamped here, for every entity, because here
        // is the only place the correspondence exists.
        std::vector<ECS::Entity> created;
        created.reserve( plan.Created.size() );
        for ( const auto& plannedEntity : plan.Created )
        {
            // The root keeps the identity the caller named (the scene file's, on load); everything else
            // is minted, because two instances of one prefab must not share an id.
            const bool         isRoot = created.empty();
            const Common::UUID id     = ( isRoot && rootId.has_value() ) ? *rootId : Common::UUID::Generate();

            const ECS::Entity entity =
                 scene.CreateEntityWithUUID( id, records[plannedEntity.Record].Tag.value_or( "PrefabEntity" ) );

            std::vector<Common::UUID> path = pathPrefix;
            path.push_back( plannedEntity.Id );
            entity.AddComponent<ECS::PrefabInstanceComponent>().SourcePath = std::move( path );

            created.push_back( entity );
        }

        // 2. Nested prefab bodies, hung off the parent the nesting record named.
        for ( const auto& plannedPrefab : plan.PrefabRecords )
        {
            const Assets::EntityData& data = records[plannedPrefab.Record];

            // A NESTED PREFAB HAS TO BE REGISTERED AND LOADED HERE, AND THIS USED TO BE `continue`.
            //
            // Nothing registers a nested prefab as an asset: the scene loader creates the asset for the
            // prefab the SCENE names, and the nested file is named only from inside that file. So the
            // lookup missed on every scene load, and the body silently did not appear — nested prefabs
            // have never worked through a `.desce` in this engine, and the bare `continue` is why nobody
            // could see that they did not. Measured on the witness scene this task added: 1 of 2
            // overrides "named an entity that prefab no longer contains", because the entity was never
            // created.
            //
            // The const_cast is the idiom this tree already uses for exactly this — resolving an asset
            // reference found inside a file being read (ComponentRegistry.cpp's AssetResolver::FromPath,
            // same reason, same shape). Changing the signature would push non-constness through seven
            // call sites to reach the two lines that need it.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
            auto& mutableAssets = const_cast<Assets::AssetManager&>( assetManager );

            auto nested = assetManager.FindByPath<Assets::PrefabAsset>( *data.PrefabPath );
            if ( !nested )
            {
                nested = mutableAssets.CreateAsset<Assets::PrefabAsset>( Assets::AssetPriority::High,
                                                                         *data.PrefabPath );
            }
            if ( !nested )
            {
                LOG_ERROR( "[PrefabFactory] '{0}' nests '{1}', and no asset could be made for that path. "
                           "That part of the prefab is MISSING from the instance.",
                           prefab.GetMetadata().Filepath.string(), *data.PrefabPath );
                continue;
            }
            if ( !nested->IsReadyForUse() )
            {
                if ( const auto loaded = nested->Load(); !loaded )
                {
                    LOG_ERROR( "[PrefabFactory] '{0}' nests '{1}', which did not load: {2}. That part of "
                               "the prefab is MISSING from the instance.",
                               prefab.GetMetadata().Filepath.string(), *data.PrefabPath, loaded.GetError() );
                    continue;
                }
            }

            // The nested instance's entities are addressed BELOW the nesting record: its id is the prefix
            // for everything inside it, which is what makes two nested copies of one prefab
            // distinguishable at all. The record's own id and not a created entity's, because under
            // InstantiatedLater the record becomes no entity — the nested file's root IS the entity.
            //
            // A nesting record with no id cannot be addressed: PlanSceneStitch mints one per load for
            // every record that lacks one, so an override written against it would miss on the next
            // open. EntitySerializer writes an id for every entity it has ever serialized, so this can
            // only be a hand-edited file — and it is named rather than silently given a random address.
            if ( !data.id.has_value() )
            {
                LOG_WARN( "[PrefabFactory] '{0}': the record nesting '{1}' states no id, so nothing inside "
                          "that nested prefab can carry a per-instance override. It is instantiated, but "
                          "any override addressed at it will miss.",
                          prefab.GetMetadata().Filepath.string(), *data.PrefabPath );
            }

            std::vector<Common::UUID> nestedPrefix = pathPrefix;
            nestedPrefix.push_back( data.id.value_or( Common::UUID::Null() ) );

            const ECS::Entity nestedRoot = Instantiate( *nested, scene, assetManager, stack, nestedPrefix );
            if ( plannedPrefab.Parent != Core::Rules::kNoSlot )
            {
                scene.Attach( created[plannedPrefab.Parent], nestedRoot );
            }

            // A nested instance may itself have been edited when this prefab was captured. Those edits
            // live on the NESTING RECORD (not in the nested file, which is shared by every other instance
            // of it), so they are applied here, against the body that was just built.
            if ( nestedRoot && data.PrefabOverrides.has_value() && !data.PrefabOverrides->empty() )
            {
                const auto        indexed = IndexInstance( nestedRoot );
                const std::size_t missed =
                     ApplyOverrides( *data.PrefabOverrides, indexed, nestedPrefix, assetManager );
                if ( missed > 0 )
                {
                    LOG_WARN( "[PrefabFactory] '{0}': {1} of {2} override(s) recorded for the nested prefab "
                              "'{3}' name an entity that prefab no longer contains. They were NOT applied.",
                              prefab.GetMetadata().Filepath.string(), missed, data.PrefabOverrides->size(),
                              *data.PrefabPath );
                }
            }
        }

        // 3. Apply components and set up hierarchy.
        for ( const auto& load : plan.Loads )
        {
            const ECS::Entity entity = created[load.Target];
            Core::Serialize::EntitySerializer::DeserializeEntity( records[load.Record], entity, assetManager );

            if ( load.Parent != Core::Rules::kNoSlot )
                scene.Attach( created[load.Parent], entity );
        }

        // The root is the FIRST record, because PrefabAsset::CreateFromEntity writes the subtree pre-order
        // and so the first record is the entity the prefab was cut from.
        ECS::Entity rootEntity = created.front();

        // Ensure the root entity is tagged as a prefab instance
        if ( rootEntity && prefabID )
        {
            if ( !rootEntity.HasComponent<ECS::PrefabComponent>() )
                rootEntity.AddComponent<ECS::PrefabComponent>().Prefab = prefabID;
        }

        stack.erase( prefabID );
        return rootEntity;
    }

    std::unordered_map<std::string, ECS::Entity> PrefabFactory::IndexInstance( ECS::Entity instanceRoot )
    {
        std::unordered_map<std::string, ECS::Entity> indexed;
        if ( !instanceRoot )
        {
            return indexed;
        }

        entt::registry* registry = instanceRoot.GetRegistry();

        // Iterative, with an explicit stack: a prefab of a few thousand entities is a legal thing to save,
        // and a recursive walk over one is a stack overflow rather than an error message.
        std::vector<entt::entity> pending{ instanceRoot.GetHandle() };
        while ( !pending.empty() )
        {
            const entt::entity handle = pending.back();
            pending.pop_back();

            const ECS::Entity entity{ handle, *registry };
            if ( entity.HasComponent<ECS::PrefabInstanceComponent>() )
            {
                indexed.emplace(
                     Assets::PrefabPathKey( entity.GetComponent<ECS::PrefabInstanceComponent>().SourcePath ),
                     entity );
            }

            if ( entity.HasComponent<ECS::RelationshipComponent>() )
            {
                for ( const entt::entity child : entity.GetComponent<ECS::RelationshipComponent>().Children )
                {
                    pending.push_back( child );
                }
            }
        }
        return indexed;
    }

    std::size_t PrefabFactory::ApplyOverrides( const std::vector<Assets::PrefabOverrideData>&      overrides,
                                               const std::unordered_map<std::string, ECS::Entity>& indexed,
                                               std::span<const Common::UUID>                       prefix,
                                               const Assets::AssetManager&                         assetManager )
    {
        std::size_t missed = 0;
        for ( const Assets::PrefabOverrideData& over : overrides )
        {
            std::vector<Common::UUID> full( prefix.begin(), prefix.end() );
            full.insert( full.end(), over.Path.begin(), over.Path.end() );

            const auto found = indexed.find( Assets::PrefabPathKey( full ) );
            if ( found == indexed.end() )
            {
                ++missed;
                continue;
            }

            const ECS::Entity entity = found->second;

            // META (tag, transform) THROUGH THE SAME FUNCTION A RECORD IS LOADED WITH. Those fields are
            // already field-level — the record states each one or does not — so nothing has to be merged.
            Core::Serialize::EntitySerializer::DeserializeEntity( Assets::OverrideMetaAsEntityData( over ), entity,
                                                                  assetManager );

            // COMPONENTS ARE MERGED, NOT WRITTEN (Ю20). An override now carries the FIELDS that differ,
            // and the entity at this moment holds exactly what the prefab said — it was deserialized from
            // the base record a few lines ago and nothing has touched it since. So the value to merge
            // onto is the component itself, read back through the same serializer that wrote it, and no
            // caller has to carry the base records down here to find it.
            for ( const auto& [key, fields] : over.Components )
            {
                const Core::Serialize::ComponentSerializer* serializer = FindComponentSerializer( key );
                if ( serializer == nullptr )
                {
                    // A key no build knows: the file was written by a newer engine, or a component was
                    // retired. Dropping it silently is how an override comes to mean nothing.
                    LOG_WARN( "[PrefabFactory] an override names component '{0}', which this build does "
                              "not register. It was NOT applied.",
                              key );
                    ++missed;
                    continue;
                }

                if ( serializer->Has( entity ) )
                {
                    serializer->Deserialize(
                         entity, Assets::MergePayload( serializer->Serialize( entity, assetManager ), fields ),
                         assetManager );
                }
                else
                {
                    // The instance has a component the prefab does not. There is nothing to merge onto,
                    // and the diff recorded the whole payload for exactly this case.
                    serializer->Deserialize( entity, fields, assetManager );
                }
            }
        }
        return missed;
    }

} // namespace Desert::Runtime::Factory
