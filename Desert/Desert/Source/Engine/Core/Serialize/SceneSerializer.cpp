#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Common/Json/Document.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Engine/Core/Serialize/ComponentRegistry.hpp>
#include <Engine/Core/Serialize/EntitySerializer.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/ForeignKeys.hpp>
#include <Engine/Core/Serialize/SceneStitchRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>
#include <Engine/Core/Serialize/PrefabInstanceOverrides.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Runtime/Factory/PrefabFactory.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>
#include <Engine/Core/SceneSettings.hpp>
#include <Engine/Graphic/Clouds/CloudTypeShape.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>
#include <Engine/World/Landscape/LandscapeTileFiles.hpp>
#include <Common/Core/Units.hpp>
#include <Common/Content/CanonicalText.hpp>

#include <rflcpp/rfl/json.hpp>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <memory>
#include <unordered_set>

namespace Desert::Core
{
    namespace
    {
        // The entity's place among its siblings as the scene holds it: its position in the parent's
        // Children, or - for a root - the running count of roots saved so far. The scene's root order IS
        // its entity order, so the count is that order restated.
        uint32_t SiblingIndexOf( const ECS::Entity& entity, uint32_t& nextRootIndex )
        {
            if ( entity.HasComponent<ECS::RelationshipComponent>() )
            {
                const entt::entity parent = entity.GetComponent<ECS::RelationshipComponent>().Parent;
                if ( parent != entt::null )
                {
                    const auto& siblings =
                         entity.GetRegistry()->get<ECS::RelationshipComponent>( parent ).Children;
                    const auto found = std::find( siblings.begin(), siblings.end(), entity.GetHandle() );
                    return static_cast<uint32_t>( found - siblings.begin() );
                }
            }
            return nextRootIndex++;
        }

        // "Is this key one an ENTITY RECORD's writer states whenever it has one?" — the meta members
        // EntitySerializer fills in, plus every component key the registry holds.
        //
        // It is derived from the same two tables the writer enumerates and is not a list: the meta
        // members come out of rfl::fields<EntityData>() (which is what rfl::json writes them from) and
        // the component keys out of ComponentRegistry itself. A third statement of either would be the
        // fork this whole change exists to remove.
        Serialize::KeyIsOurs EntityRecordKeyIsOurs()
        {
            auto names = std::make_shared<std::unordered_set<std::string>>();
            for ( const auto& field : rfl::fields<Assets::EntityData>() )
                names->insert( std::string( field.name() ) );
            for ( const auto& serializer : Serialize::ComponentRegistry::Get().All() )
                names->insert( serializer.Key );
            return [names]( const std::string& key ) { return names->count( key ) != 0; };
        }

        Serialize::KeyIsOurs NamesIn( const std::vector<std::string>& names )
        {
            auto set = std::make_shared<std::unordered_set<std::string>>( names.begin(), names.end() );
            return [set]( const std::string& key ) { return set->count( key ) != 0; };
        }

        // THE ROOT A TILE NAMES, as the frame LandscapeLayout computes with. nullopt when the id names no
        // entity with a LandscapeComponent — an observation that did not resolve, which the caller reports.
        std::optional<World::Landscape::LandscapeRoot> FindTileRoot( const Scene& scene, const Common::UUID& id )
        {
            const auto found = scene.FindEntityByID( id );
            if ( !found.has_value() || !found->get().HasComponent<ECS::LandscapeComponent>() )
                return std::nullopt;

            return ECS::LandscapeRootOf( found->get() );
        }

        // AFTER A LOAD: every tile's heights against the root it names. A tile of the wrong size, or under
        // a root that cannot be tiled, is REFUSED — its heights are dropped and the reason is logged with
        // the tile's coordinate — rather than kept at a size its neighbours do not share. A root that is
        // rotated or scaled is said out loud: the frame has neither (Components.hpp, LandscapeComponent),
        // and a landscape that silently ignores its own gizmo is the dead-setting shape.
        void CheckLandscapeTiles( const Scene& scene, std::string_view sceneName )
        {
            for ( const auto& entity : scene.GetAllEntities() )
            {
                if ( entity.HasComponent<ECS::LandscapeComponent>() )
                {
                    const glm::mat4 world = entity.GetWorldTransform();
                    const glm::vec3 x( world[0] );
                    const glm::vec3 y( world[1] );
                    const glm::vec3 z( world[2] );
                    const bool      axisAligned = std::abs( x.y ) + std::abs( x.z ) + std::abs( y.x ) +
                                                  std::abs( y.z ) + std::abs( z.x ) + std::abs( z.y ) <
                                             1e-5f;
                    const bool unitScale =
                         std::abs( x.x - 1.0f ) + std::abs( y.y - 1.0f ) + std::abs( z.z - 1.0f ) < 1e-5f;
                    if ( !axisAligned || !unitScale )
                        LOG_WARN( "[Landscape] '{0}': a landscape root is rotated or scaled; its tiles are placed "
                                  "by its position alone, because the landscape frame has no rotation or scale.",
                                  sceneName );
                }

                if ( !entity.HasComponent<ECS::LandscapeTileComponent>() )
                    continue;
                auto& tile = entity.GetComponent<ECS::LandscapeTileComponent>();
                if ( !tile.Heights )
                    continue;

                const auto root = FindTileRoot( scene, tile.Landscape );
                if ( !root.has_value() )
                {
                    LOG_WARN( "[Landscape] '{0}': tile ({1}, {2}) names landscape {3}, which this scene does not "
                              "contain; the tile keeps its heights and has no place in the world.",
                              sceneName, tile.TileX, tile.TileZ, static_cast<uint64_t>( tile.Landscape ) );
                    continue;
                }

                auto refused = World::Landscape::ValidateLandscapeRoot( *root );
                if ( refused )
                    refused = World::Landscape::CheckTileMatchesRoot( *tile.Heights, *root );
                if ( !refused )
                {
                    LOG_ERROR( "[Landscape] '{0}': tile ({1}, {2}) was refused: {3}", sceneName, tile.TileX,
                               tile.TileZ, refused.GetError() );
                    tile.Heights.reset();
                }
            }
        }

        // BEFORE A SAVE: every tile's heights, into files derived from the destination. The first failure
        // stops the save and names the file, so the scene is never written pointing at a tile file that
        // did not land. A tile holding no heights keeps whatever file it named — this save did not lose
        // them, and writing nothing over that file is the only answer that does not invent terrain.
        Common::BoolResultStr WriteLandscapeTiles( Scene& scene, const std::filesystem::path& scenePath )
        {
            // The path the .desce records is spelled the way the scene's other files are: relative to the
            // working directory, so a scene saved on one machine opens on another.
            const std::filesystem::path relative =
                 scenePath.is_absolute() ? scenePath.lexically_proximate( std::filesystem::current_path() )
                                         : scenePath;

            for ( const auto& entity : scene.GetAllEntities() )
            {
                if ( !entity.HasComponent<ECS::LandscapeTileComponent>() )
                    continue;
                auto& tile = entity.GetComponent<ECS::LandscapeTileComponent>();
                if ( !tile.Heights )
                {
                    LOG_WARN( "[Landscape] tile ({0}, {1}) holds no heights; the scene keeps naming '{2}'.",
                              tile.TileX, tile.TileZ, tile.HeightFile );
                    continue;
                }

                // The file is named by the entity's id, so an entity without one has no file name that
                // could not collide with another's; refused rather than numbered zero.
                if ( !entity.HasComponent<ECS::UUIDComponent>() )
                    return Common::MakeFormattedError( "tile ({}, {}) has no entity id to name its file by",
                                                       tile.TileX, tile.TileZ );
                const auto id = static_cast<uint64_t>( entity.GetComponent<ECS::UUIDComponent>().UUID );
                const std::filesystem::path file = World::Landscape::LandscapeTileBlobPath( relative, id );
                if ( const auto written = World::Landscape::WriteLandscapeTileFile( file, *tile.Heights );
                     !written )
                    return Common::MakeFormattedError( "tile ({}, {}): {}", tile.TileX, tile.TileZ,
                                                       written.GetError() );
                tile.HeightFile = file.generic_string();
            }
            return BOOLSUCCESS;
        }
    } // namespace

    SceneSerializer::SceneSerializer( const Scene* scene, const Assets::AssetManager* assetManager )
         : m_Scene( (Scene*)scene ), m_AssetManager( (Assets::AssetManager*)assetManager )
    {
    }

    std::string SceneSerializer::SerializeToJson() const
    {
        SceneSerialized scene;
        // The GUID survives the save (StampTextHeader keeps the loaded one); a scene that never had one gets
        // it minted here, and remembers it, so the next save states the same identity.
        scene.Header = Assets::StampTextHeader( m_Scene->GetAssetHeader(), Common::Content::ContentKind::Scene,
                                                SceneTextSubsystems() );
        m_Scene->SetAssetHeader( scene.Header );
        scene.SceneName = m_Scene->GetSceneName();
        // STATED BY THE WRITER, not left to the foreign-key merge below. Until the scene held the block
        // itself, this key was written by nobody and survived only as an unknown key the merge copied
        // across — which meant a partition the editor CREATED (Rules::ConvertToWorldPartition) had
        // nowhere to be written to, because the source document had no such key to preserve.
        scene.WorldPartition = m_Scene->GetWorldPartition();

        // Helper to check if any ancestor has a PrefabComponent
        auto isPrefabChild = [&]( ECS::Entity entity ) -> bool
        {
            entt::entity current = entity.GetHandle();
            auto* registry = entity.GetRegistry();
            
            while ( registry->has<ECS::RelationshipComponent>( current ) )
            {
                const auto& rel = registry->get<ECS::RelationshipComponent>( current );
                if ( rel.Parent == entt::null ) break;
                
                current = rel.Parent;
                if ( registry->has<ECS::PrefabComponent>( current ) )
                {
                    return true;
                }
            }
            return false;
        };

        uint32_t nextRootIndex = 0;
        for ( const auto& entity : m_Scene->GetAllEntities() )
        {
            if ( isPrefabChild( const_cast<ECS::Entity&>(entity) ) )
            {
                continue;
            }
            Assets::EntityData data = Serialize::EntitySerializer::SerializeEntity( entity, *m_AssetManager );
            data.siblingIndex       = SiblingIndexOf( entity, nextRootIndex );

            // A PREFAB INSTANCE IS A LINK PLUS ITS DIFFERENCES, AND NOTHING ELSE.
            //
            // The loop above skips every entity under an instance root, so before Ю19 a `.desce` recorded
            // an instance as its root's payload alone — and the loader applied only the root's
            // translation, rotation and scale from it. Everything else a user had touched inside an
            // instance, including the root's own components, was written or not written and then
            // discarded on load without a word. That is the difference between a prefab and a copy,
            // deleted silently.
            //
            // So the root's own values are STRIPPED here and re-stated as override records covering the
            // whole instance, root included. One value lives in one place: either the prefab file says
            // it, or the override does. Leaving the record's copy in place as well would be the same
            // value written twice with a reader that prefers one of them — the shape §4.2 of the contract
            // forbids, and the reason the material mirror lost its `Path` field.
            if ( entity.HasComponent<ECS::PrefabComponent>() && data.PrefabPath.has_value() )
            {
                const Serialize::PrefabInstanceCapture capture =
                     Serialize::CapturePrefabInstance( entity, *m_AssetManager );

                data.Tag = std::nullopt;
                data.Translation.reset();
                data.Rotation.reset();
                data.Scale.reset();
                data.Components.clear();

                if ( !capture.Overrides.empty() )
                {
                    data.PrefabOverrides = capture.Overrides;
                }

                // The three things an override cannot express, said out loud with the instance's name and
                // the counts. They used to be indistinguishable from "nothing was changed here".
                if ( capture.AddedEntities > 0 || capture.RemovedComponents > 0 ||
                     capture.UnaddressableEntities > 0 )
                {
                    LOG_WARN( "[Prefab] instance of '{0}': {1} entity(ies) added inside it, {2} component(s) "
                              "removed from its entities and {3} entity(ies) with no addressable source "
                              "record are NOT saved with the scene. Unpack the prefab to keep them, or "
                              "apply them to the source file.",
                              *data.PrefabPath, capture.AddedEntities, capture.RemovedComponents,
                              capture.UnaddressableEntities );
                }

                // A SEPARATE SENTENCE, because it is a different failure: nothing is lost here, something
                // is PINNED. An override can only follow the source for a field the source actually
                // states, so a hand-written `.deprefab` that names three fields of twenty freezes the
                // other seventeen on every instance of it — quietly, until now.
                if ( capture.PinnedUnstatedFields > 0 )
                {
                    LOG_WARN( "[Prefab] instance of '{0}': {1} field(s) are pinned on this instance because "
                              "the prefab file does not state them. This engine's own writer states every "
                              "field, so that file was written by something else — re-save it from the "
                              "editor (Apply Instance Changes to Prefab) and those fields will follow it "
                              "again.",
                              *data.PrefabPath, capture.PinnedUnstatedFields );
                }
            }

            scene.Entities.push_back( std::move( data ) );
        }

        // Records sorted by id (scene v25): the file order is then a function of the SET of entities, not
        // of the order they were created or last reparented in, so adding one entity changes only its own
        // lines and the siblingIndex of its later siblings - never the position of every record after it.
        std::stable_sort( scene.Entities.begin(), scene.Entities.end(),
                          []( const Assets::EntityData& a, const Assets::EntityData& b )
                          {
                              return static_cast<uint64_t>( a.id.value_or( Common::UUID( 0 ) ) ) <
                                     static_cast<uint64_t>( b.id.value_or( Common::UUID( 0 ) ) );
                          } );

        // Scene-wide settings via the generic reflection serializer (no hand-written mirror struct).
        //
        // WITH THE ASSET RESOLVER, on the same terms every component gets one. SceneSettings owns one
        // AssetHandle — SplashSprite, the image the standalone Runtime shows while this scene loads — and
        // without a resolver here it was written as a raw 64-bit number and read back through a double,
        // which rounds every handle above 2^53. It was the last field in the engine still taking that
        // path.
        if ( const auto* st = Reflection::ReflectionRegistry::Get().Find( "SceneSettings" ) )
        {
            auto resolver = Serialize::MakeAssetResolver( *m_AssetManager );
            scene.Settings =
                 rfl::Generic( Reflection::SerializeReflected( *st, &m_Scene->GetSettings(), &resolver ) );
        }

        // WHAT THIS BUILD KNOWS, MERGED ONTO WHAT THE FILE SAID. Everything above enumerates a
        // REGISTRY — SceneSettings' 51 reflected fields, ComponentRegistry's 45 keys — so up to this
        // line a key that is in the file and not in a registry has simply ceased to exist. The merge
        // is what makes the enumeration go by the FILE as well: this build's answer wins for every key
        // it states, and every other key the file carried is kept where it was.
        //
        // A scene that was never loaded from a file has an empty document and the merge is the
        // identity, so File → New costs nothing.
        const auto& source = m_Scene->GetLoadedDocument();
        if ( source.empty() )
            return rfl::json::write( scene );

        auto written = rfl::json::read<rfl::Generic>( rfl::json::write( scene ) );
        if ( !written.has_value() )
        {
            // Cannot happen through a parser that has just produced the text — but a silent fallback
            // here would be the whole defect back again, so it is named rather than assumed away.
            LOG_ERROR( "[SceneSerializer] '{0}': the scene this build wrote could not be re-read as a "
                       "tree, so the keys the file carries that this build does not declare could not "
                       "be merged back in. THEY ARE ABOUT TO BE LOST.",
                       m_Scene->GetSceneName() );
            return rfl::json::write( scene );
        }
        const auto asObject = written.value().to_object();
        if ( !asObject.has_value() )
            return rfl::json::write( scene );

        return rfl::json::write(
             Serialize::MergeSceneDocument( asObject.value(), source, EntityRecordKeyIsOurs() ) );
    }

    Common::BoolResultStr SceneSerializer::DeserializeFromJson( const std::string& json,
                                                                std::string_view   source ) const
    {
        // THE VERSION GATE, AND WHY IT REFUSES INSTEAD OF REPAIRING.
        //
        // Eight schema migrations used to run right here, on every load of every scene, forever. Each was
        // written to be deleted "once no v<n> file remains" and not one ever was, because a migration that
        // runs at LOAD never writes its result back and so can never reach that condition - which is the
        // expiry DEV_CONTRACT §4.6 requires and the reason §4.3 ends "the runtime knows nothing about the
        // old format". This runtime now knows exactly one: the current one.
        //
        // It also does not SUBSTITUTE (§1.4). An old file is not loaded on defaults, not partially loaded
        // and not half-migrated: the gate is before the scene name, before the settings and before a single
        // entity is made, so a refusal creates nothing at all and the error says what to run. The
        // conversion still exists, in full, in Tools/SceneMigrator - it runs once, over the file, and
        // writes it back, which is the only shape of migration that can ever be finished.
        //
        // Callers ask ParseLoadableScene the same question BEFORE they clear the scene they are replacing;
        // this is the second, authoritative asking, so that a caller which forgets still cannot get an old
        // file past here.
        SceneLoadPhases phases( fmt::format( "'{}'", source ) );

        auto loadable = ParseLoadableScene( source, json );
        if ( !loadable )
        {
            LOG_ERROR( "{0}", loadable.GetError() );
            return Common::MakeError( loadable.GetError() );
        }

        const SceneSerialized scene = loadable.ExtractValue();
        m_Scene->SetAssetHeader( scene.Header );
        // The block the file states, kept on the scene rather than only reported below: it is what the
        // editor's World Partition panel edits and what the next save writes back.
        m_Scene->SetWorldPartition( scene.WorldPartition );
        phases.Lap( "parse the file into typed records (version gate)", scene.Entities.size() );

        LOG_INFO( "Loading scene: {0}", scene.SceneName );

        // THE FILE, KEPT AS IT WAS PARSED, and every key in it this build cannot name, SAID OUT LOUD.
        //
        // The two halves are one decision. Preserving without naming is what UE does — an unknown
        // property is skipped by its own byte length with no log at any verbosity — and the price is
        // that a typo, a deleted field and a genuine schema divergence are indistinguishable at
        // runtime. Naming without preserving is the defect. So: the data survives, and the developer
        // hears about it.
        //
        // GROUPED, because a key on forty entities is one finding and not forty lines: the count is by
        // key NAME across the whole file, and the load says it once.
        //
        // Component payload INTERIORS are not walked. Each level of a .desce answers to a different
        // registry, and the key → reflected-type map for component blocks lives in ComponentRegistry's
        // custom serializers rather than as data — so a foreign key inside a light's block is
        // PRESERVED by the merge on save but is not named here. That is a gap in the diagnostic, not
        // in the guarantee, and it is written down rather than left to be discovered.
        {
            std::map<std::string, int> foreign;
            if ( const auto document = rfl::json::read<rfl::Generic>( json ); document.has_value() )
                if ( const auto object = document.value().to_object(); object.has_value() )
                {
                    m_Scene->SetLoadedDocument( object.value() );

                    std::vector<std::string> topLevel;
                    for ( const auto& field : rfl::fields<SceneSerialized>() )
                        topLevel.push_back( std::string( field.name() ) );
                    Serialize::CountForeignKeysAtLevel( object.value(), NamesIn( topLevel ), foreign );

                    if ( const auto settings = object.value().get( "Settings" ); settings.has_value() )
                        if ( const auto block = settings.value().to_object(); block.has_value() )
                            if ( const auto* st = Reflection::ReflectionRegistry::Get().Find( "SceneSettings" ) )
                            {
                                std::vector<std::string> fields;
                                for ( const auto& field : st->Fields )
                                    fields.push_back( field.Name );
                                Serialize::CountForeignKeysAtLevel( block.value(), NamesIn( fields ), foreign );
                            }

                    if ( const auto entities = object.value().get( "Entities" ); entities.has_value() )
                        if ( const auto array = entities.value().to_array(); array.has_value() )
                        {
                            const auto ours = EntityRecordKeyIsOurs();
                            for ( const auto& record : array.value() )
                                if ( const auto fields = record.to_object(); fields.has_value() )
                                    Serialize::CountForeignKeysAtLevel( fields.value(), ours, foreign );
                        }
                }

            if ( const std::string named = Serialize::DescribeForeignKeys( foreign ); !named.empty() )
                LOG_WARN( "[SceneSerializer] '{0}' states {1} key(s) this build does not declare: {2}. "
                          "They are KEPT — the next save writes them back untouched — but nothing in "
                          "this build reads them. If one is a typo it will never take effect; if one "
                          "was retired on purpose, retire it in Tools/SceneMigrator so the files stop "
                          "carrying it.",
                          scene.SceneName, foreign.size(), named );
        }
        phases.Lap( "parse the file again as a generic document, count undeclared keys", scene.Entities.size() );

        // Restore the scene name (was only logged before — so a renamed+saved scene reverted on load).
        if ( !scene.SceneName.empty() )
            m_Scene->SetSceneName( scene.SceneName );

        // Restore scene-wide settings (reflected). Missing keys keep their defaults (forward-compatible).
        if ( scene.Settings.has_value() )
        {
            if ( const auto* st = Reflection::ReflectionRegistry::Get().Find( "SceneSettings" ) )
            {
                // The wrong-type rule: a bad value keeps its default and is named, the load continues.
                Common::Json::Issues issues;
                auto                 resolver = Serialize::MakeAssetResolver( *m_AssetManager );
                Reflection::DeserializeReflected(
                     *st, &m_Scene->GetSettings(),
                     Common::Json::Root( *scene.Settings, Common::Json::Path().Key( "Settings" ) ), issues,
                     &resolver );
                Common::Json::ReportIssues( issues, "scene settings" );
            }
        }

        // IF THIS WORLD SAYS IT IS PARTITIONED, SAY WHAT THE PARTITION IS AND WHAT IT COST.
        //
        // Nothing here can stop a load: a composite wider than a cell goes up a level, and one no level
        // holds is always-loaded (owner decision O1, 2026-09-23). So the facts worth a line are how the
        // world spread over the levels and what stays loaded everywhere - the numbers a streamer will pay
        // in residency. Stated at load because that is where somebody who has just moved an entity will
        // see it. The work is one walk of records the loader has already parsed, and an unpartitioned
        // world (every `.desce` in the repository today) does none of it.
        if ( scene.WorldPartition.has_value() )
        {
            const Rules::WorldPartitionPlan partition =
                 Rules::PlanWorldPartition( scene.Entities, *scene.WorldPartition, RegistryMeshBounds() );

            if ( !partition.Dangling.empty() )
            {
                // Said separately because it is a different fact: a composite that is
                // smaller than its author thinks, because one of its parts names an entity this file
                // does not contain.
                LOG_WARN( "[WorldPartition] '{0}': {1} containment reference(s) name an entity this file "
                          "does not contain, so whatever they were meant to hold together is partitioned "
                          "as separate composites.",
                          scene.SceneName, partition.Dangling.size() );
            }

            if ( !partition.UnplacedPrefabInstances.empty() )
            {
                // A THIRD, DIFFERENT FACT, and it is a limitation rather than a defect in the world: a
                // prefab instance's transform is not in this file at all, so the partitioner put it at
                // the origin (and it widens no composite's footprint). Said out loud because "in cell (0,0)" is
                // otherwise indistinguishable from a correct answer.
                LOG_WARN( "[WorldPartition] '{0}': {1} prefab instance(s) state no transform of their "
                          "own in this file, so they are partitioned AT THE ORIGIN. Placing them needs "
                          "the prefab's own bounds, which are not stored in the asset yet.",
                          scene.SceneName, partition.UnplacedPrefabInstances.size() );
            }

            if ( !partition.UnplacedLandscapeTiles.empty() )
            {
                // The same limitation from a different cause: a tile is placed by its root's frame, and
                // these name a root the file does not contain or one that cannot be tiled.
                LOG_WARN( "[WorldPartition] '{0}': {1} landscape tile(s) have no root this file can place "
                          "them by, so they contribute no footprint.",
                          scene.SceneName, partition.UnplacedLandscapeTiles.size() );
            }

            if ( partition.UnusedGrids > 0 )
            {
                LOG_WARN( "[WorldPartition] '{0}': the world states {1} grid(s) beyond the first. Every "
                          "composite is partitioned on grid 0 until a record can choose its grid.",
                          scene.SceneName, partition.UnusedGrids );
            }

            LOG_INFO( "[WorldPartition] '{0}': {1}.", scene.SceneName,
                      Rules::SummarisePartition( partition, *scene.WorldPartition ) );
            if ( partition.MaxLevelComposite != Rules::kNoRecord && partition.MaxLevel > 0 )
            {
                // The worst promotion by name, because "level 3" is only actionable next to WHAT went there.
                const auto& anchor = scene.Entities[partition.Composites[partition.MaxLevelComposite].Anchor];
                LOG_INFO( "[WorldPartition] '{0}': the widest composite went up to level {1}: '{2}' (id {3}).",
                          scene.SceneName, partition.MaxLevel, anchor.Tag.value_or( "Entity" ),
                          anchor.id.has_value() ? static_cast<std::uint64_t>( *anchor.id ) : 0u );
            }
        }

        phases.Lap( "scene settings and world partition plan", scene.Entities.size() );

        if ( auto made = InstantiateRecords( scene.Entities, scene.SceneName, &phases ); !made )
            return made;

        // After every pass: a tile is checked against its root, and a root may be in a prefab instance.
        CheckLandscapeTiles( *m_Scene, scene.SceneName );
        phases.Lap( "check landscape tiles", scene.Entities.size() );
        phases.LogSummary();

        return BOOLSUCCESS;
    }

    Common::BoolResultStr SceneSerializer::InstantiateRecords( std::span<const Assets::EntityData> records,
                                                               std::string_view                    sceneName,
                                                               SceneLoadPhases*                    phases ) const
    {
        // WHICH entity each record becomes, which one its payload lands on and what it hangs off is a pure
        // function of the parsed tree, and it lives in Rules::PlanSceneStitch so a test can call it: this
        // file cannot be compiled without the renderer, so for as long as the stitch was written out here
        // it was unreachable by every suite in the repository and a defect planted in it stayed green.
        // What remains below is the part only the loader can do — make the entities and feed the payloads.
        // InstantiatedLater: in a .desce a PrefabPath record names another FILE, and the entity it becomes
        // is made by pass 3 below out of that file - so it is listed here, not created.
        const Rules::StitchPlan plan = Rules::PlanSceneStitch( records, &Common::UUID::Generate,
                                                               Rules::PrefabRecordPolicy::InstantiatedLater );

        // DC 1.4: a file that names one id twice, or names a parent that is not in it, loads as a scene
        // that is quietly missing pieces. Say which, once, instead of leaving it to be found in the viewport.
        if ( plan.Shadowed > 0 || plan.UnresolvedParents > 0 )
        {
            LOG_WARN( "[SceneSerializer] '{0}': {1} entity record(s) claim an id another record already "
                      "claimed (their payload is written onto the first claimant and their own entity stays "
                      "bare), and {2} parent link(s) name an entity this file does not contain. {3} id(s) "
                      "were minted for records that carried none.",
                      sceneName, plan.Shadowed, plan.UnresolvedParents, plan.Minted );
        }

        if ( phases != nullptr )
            phases->Lap( "stitch identities and parents", records.size() );

        std::unordered_map<Common::UUID, ECS::Entity> entityMap;

        // Pass 1 — create normal entities
        std::vector<ECS::Entity> created;
        created.reserve( plan.Created.size() );
        for ( const auto& plannedEntity : plan.Created )
        {
            const Assets::EntityData& entityData = records[plannedEntity.Record];
            ECS::Entity               entity =
                 m_Scene->CreateEntityWithUUID( plannedEntity.Id, entityData.Tag.value_or( "Entity" ) );
            created.push_back( entity );
            entityMap.insert( { plannedEntity.Id, entity } );
        }
        if ( phases != nullptr )
            phases->Lap( "create entities", plan.Created.size() );

        // Pass 2 — deserialize normal entities; their parent links are collected, and made after pass 3
        std::vector<Rules::PendingAttach<ECS::Entity>> attaches;
        for ( size_t slot = 0; slot < plan.Loads.size(); ++slot )
        {
            const auto& load   = plan.Loads[slot];
            ECS::Entity entity = created[load.Target];
            Serialize::EntitySerializer::DeserializeEntity( records[load.Record], entity, *m_AssetManager );

            // A root is an attach to no parent; a shadowed record's root is its target's, placed once.
            if ( load.Parent != Rules::kNoSlot )
                attaches.push_back(
                     { created[load.Parent], entity, Rules::SiblingIndexOf( records[load.Record] ) } );
            else if ( load.Target == slot )
                attaches.push_back( { ECS::Entity{}, entity, Rules::SiblingIndexOf( records[load.Record] ) } );
        }
        if ( phases != nullptr )
            phases->Lap( "deserialize components and attach", plan.Loads.size() );

        // Pass 3 — instantiate prefab roots and apply their saved transforms
        for ( const auto& plannedPrefab : plan.PrefabRecords )
        {
            const Assets::EntityData* entityData = &records[plannedPrefab.Record];

            auto prefabAsset = m_AssetManager->FindByPath<Assets::PrefabAsset>( *entityData->PrefabPath );
            if ( !prefabAsset )
            {
                prefabAsset = m_AssetManager->CreateAsset<Assets::PrefabAsset>(
                    Assets::AssetPriority::High, *entityData->PrefabPath );
            }

            if ( !prefabAsset )
            {
                LOG_ERROR( "SceneSerializer: could not load prefab '{0}'", *entityData->PrefabPath );
                continue;
            }

            // A prefab whose FILE is gone (deleted, renamed, not packaged) must not take the rest of
            // the scene down with it: name the failure and skip THIS record only. The result used to
            // be discarded here, so the log never said which prefab was missing — and before the read
            // primitives went soft the question never even came back, because a missing file aborted
            // the process inside ReadFileContent.
            if ( !prefabAsset->IsReadyForUse() )
            {
                if ( const auto loaded = prefabAsset->Load(); !loaded )
                {
                    LOG_ERROR( "SceneSerializer: prefab '{0}' named by the scene did not load: {1}",
                               *entityData->PrefabPath, loaded.GetError() );
                    continue;
                }
            }

            std::unordered_set<Common::UUID> stack;
            // WITH THE ID THE FILE STATES. The factory mints a fresh uuid for every entity of an instance,
            // and for the root that meant this scene was written with a different id for the same instance
            // on every save — the only difference between two consecutive saves of a scene holding one.
            ECS::Entity prefabRoot = Runtime::Factory::PrefabFactory::Instantiate(
                 *prefabAsset, *m_Scene, *m_AssetManager, stack, {}, entityData->id );

            if ( !prefabRoot )
                continue;

            // WHAT THIS INSTANCE HOLDS OF ITS OWN, put back. This used to be three transform fields and
            // nothing else, which is why every other edit inside an instance disappeared on reload; the
            // root's transform is now one override among the rest and takes the same path as they do.
            if ( entityData->PrefabOverrides.has_value() && !entityData->PrefabOverrides->empty() )
            {
                const auto        indexed = Runtime::Factory::PrefabFactory::IndexInstance( prefabRoot );
                const std::size_t missed  = Runtime::Factory::PrefabFactory::ApplyOverrides(
                     *entityData->PrefabOverrides, indexed, {}, *m_AssetManager );
                if ( missed > 0 )
                {
                    LOG_WARN( "SceneSerializer: {0} of {1} override(s) saved for the instance of '{2}' name "
                              "an entity that prefab no longer contains. They were NOT applied.",
                              missed, entityData->PrefabOverrides->size(), *entityData->PrefabPath );
                }
            }

            // Register in map under the original saved UUID so parent links resolve. A prefab root saved
            // without an id has nothing for a child's `parent` to name, so there is nothing to register:
            // the map entry would only shadow whatever else lacked an id.
            if ( entityData->id.has_value() && !entityData->id->IsNull() )
                entityMap[*entityData->id] = prefabRoot;

            // Attach to parent if one exists (e.g. prefab nested under a regular entity); otherwise it is a
            // root, and takes its place among the other roots in pass 4.
            ECS::Entity parent;
            if ( entityData->parent.has_value() && !entityData->parent->IsNull() )
                if ( const auto parentIt = entityMap.find( *entityData->parent ); parentIt != entityMap.end() )
                    parent = parentIt->second;
            attaches.push_back( { parent, prefabRoot, Rules::SiblingIndexOf( *entityData ) } );
        }
        if ( phases != nullptr )
            phases->Lap( "instantiate prefabs", plan.PrefabRecords.size() );

        // Pass 4 — the hierarchy, every parent link at once in sibling order (Rules::OrderAttaches).
        Rules::OrderAttaches( attaches );
        std::vector<ECS::Entity> roots;
        for ( const auto& attach : attaches )
        {
            if ( attach.Parent )
                m_Scene->Attach( attach.Parent, attach.Child );
            else
                roots.push_back( attach.Child );
        }
        m_Scene->ArrangeRoots( roots );
        if ( phases != nullptr )
            phases->Lap( "attach in sibling order", attaches.size() );

        return BOOLSUCCESS;
    }

    Rules::AssetBoundsSource RegistryMeshBounds()
    {
        return []( const Common::Content::AssetGuid& guid,
                   std::string_view                  path ) -> std::optional<Common::Math::AABB>
        {
            const Common::Utils::AssetRegistryEntry* row =
                 Assets::ContentRegistry::Get().FindByGuidReference( guid, path );
            return row != nullptr ? row->Bounds : std::nullopt;
        };
    }

    Common::BoolResultStr SceneSerializer::SaveToFile( const Common::Filepath& path ) const
    {
        // A DESTINATION IS REQUIRED AND CANNOT BE INVENTED. The name-derived fallback that used to stand
        // here is the whole of this defect: it made "the caller forgot to say where" indistinguishable
        // from "the caller said here", and the second file it produced looked like a successful save.
        if ( path.empty() )
            return Common::MakeFormattedError( "no destination was given for '{}'; the scene was not "
                                               "written anywhere",
                                               m_Scene->GetSceneName() );

        // The directory is created here rather than assumed: an ofstream silently writes NOTHING when
        // the parent is missing, and a project whose Scene/ folder has never existed is the ordinary
        // case on the first save after "New Project".
        std::error_code ec;
        std::filesystem::create_directories( path.parent_path(), ec );
        if ( ec )
            return Common::MakeFormattedError( "could not create the directory {} for '{}': {}",
                                               path.parent_path().string(), m_Scene->GetSceneName(),
                                               ec.message() );

        // The tiles first: the scene records the files they were written to, so it is serialized after.
        if ( const auto tiles = WriteLandscapeTiles( *m_Scene, path ); !tiles )
            return Common::MakeFormattedError( "could not save the landscape of '{}': {}", m_Scene->GetSceneName(),
                                               tiles.GetError() );

        // The file is the canonical text (AF6), not rfl's single line: one field per line is what makes a
        // scene's git diff name the fields that changed and two edits to different entities merge.
        const auto text = Common::Content::CanonicalJsonText( SerializeToJson() );
        if ( !text )
            return Common::MakeFormattedError( "could not lay out '{}' as text: {}", m_Scene->GetSceneName(),
                                               text.GetError() );
        if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( path, text.GetValue() );
             !written )
            return Common::MakeFormattedError( "could not write {}: {}", path.string(), written.GetError() );

        return BOOLSUCCESS;
    }

} // namespace Desert::Core