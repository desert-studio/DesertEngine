#include "PrefabFactory.hpp"
#include <Engine/Assets/Prefab/PrefabOverrides.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Core/Serialize/EntitySerializer.hpp>
#include <Engine/Core/Serialize/SceneStitchRules.hpp>

namespace Desert::Runtime::Factory
{
    // A prefab nests prefabs; the depth is the file's nesting, and `stack` is what bounds it (a cycle is
    // refused by name above). NOLINTNEXTLINE must be the LAST comment line before the statement.
    // NOLINTNEXTLINE(misc-no-recursion)
    ECS::Entity PrefabFactory::Instantiate( const Assets::PrefabAsset& prefab, Core::Scene& scene,
                                            const Assets::AssetManager&       assetManager,
                                            std::unordered_set<Common::UUID>& stack,
                                            const std::vector<Common::UUID>&  pathPrefix )
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
        // CreatedInPlace: unlike a .desce, a record here that carries a PrefabPath is still an entity of
        // THIS prefab — one with another prefab nested underneath it — so it is created and stitched like
        // any other, and the nested body is hung off it below.
        const Core::Rules::StitchPlan plan = Core::Rules::PlanSceneStitch(
             records, &Common::UUID::Generate, Core::Rules::PrefabRecordPolicy::CreatedInPlace );

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
            const ECS::Entity entity = scene.CreateEntityWithUUID(
                 Common::UUID::Generate(), records[plannedEntity.Record].Tag.value_or( "PrefabEntity" ) );

            std::vector<Common::UUID> path = pathPrefix;
            path.push_back( plannedEntity.Id );
            entity.AddComponent<ECS::PrefabInstanceComponent>().SourcePath = std::move( path );

            created.push_back( entity );
        }

        // 2. Nested prefab bodies, hung off the entity the nesting record became.
        for ( const auto& plannedPrefab : plan.PrefabRecords )
        {
            const Assets::EntityData& data = records[plannedPrefab.Record];

            auto nested = assetManager.FindByPath<Assets::PrefabAsset>( *data.PrefabPath );
            if ( !nested )
                continue;

            // The nested instance's entities are addressed BELOW the nesting record: its own path is the
            // prefix for everything inside it, which is what makes two nested copies of one prefab
            // distinguishable at all.
            std::vector<Common::UUID> nestedPrefix =
                 created[plannedPrefab.Slot].GetComponent<ECS::PrefabInstanceComponent>().SourcePath;

            const ECS::Entity nestedRoot = Instantiate( *nested, scene, assetManager, stack, nestedPrefix );
            scene.Attach( created[plannedPrefab.Slot], nestedRoot );

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

            // THE SAME FUNCTION A RECORD IS LOADED WITH. An override is a partial entity record, so
            // applying one is loading one — writing a second applier here is how the two would come to
            // disagree about, say, what an absent field means.
            Core::Serialize::EntitySerializer::DeserializeEntity( Assets::OverrideAsEntityData( over ),
                                                                  found->second, assetManager );
        }
        return missed;
    }

} // namespace Desert::Runtime::Factory
