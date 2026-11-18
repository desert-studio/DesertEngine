#include "PrefabAsset.hpp"
#include "PrefabFormat.hpp"
#include "PrefabPlacement.hpp"
#include "PackedPrefab.hpp"
#include <optional>
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>
#include <Engine/Core/Serialize/EntityReferenceRemap.hpp>
#include <Engine/Core/Serialize/EntitySerializer.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>
#include <Engine/Core/Serialize/PrefabInstanceOverrides.hpp>
#include <Engine/ECS/Components.hpp>
#include <functional>
#include <Engine/Core/Scene.hpp>
#include <Engine/UI/Ecs/UICanvasLayoutEcs.hpp>
#include <Engine/Runtime/Factory/PrefabFactory.hpp>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <format>

namespace Desert::Assets
{
    PrefabAsset::PrefabAsset( const Common::Filepath& filepath ) : AssetBase( filepath, AssetTypeID::Prefab )
    {
        if ( const TextAssetIdentity identity = ReadTextAssetIdentity( m_Metadata.Filepath );
             !identity.Guid.IsNull() )
        {
            m_Guid = identity.Guid;
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
        }
    }

    Common::BoolResultStr PrefabAsset::LoadFromFile()
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

        // The identity the file states, which Serialize writes back.
        m_Guid            = PrefabStatedGuid( loadable.GetValue() );
        m_RuntimeBehavior = loadable.GetValue().RuntimeBehavior;
        m_EntityData      = std::move( loadable.GetValue().Entities );
        AdoptOrRepack( std::move( loadable.GetValue().Packed ) );
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
        m_Packed.reset();
        m_IsLoaded = false;
        return BOOLSUCCESS;
    }

    Common::ResultStr<std::string> PrefabAsset::Serialize() const
    {
        PrefabData data;
        data.Name = m_Metadata.Filepath.stem().string();
        data.Entities = m_EntityData;
        data.RuntimeBehavior = m_RuntimeBehavior;
        // Root names the first record BY ID, so it can only be written when that record has one. It used to
        // fall back to a default-constructed UUID, which was a random number — a root id pointing at no
        // entity, baked into the file.
        if ( !m_EntityData.empty() )
        {
            const auto& firstId = m_EntityData.front().id;
            if ( firstId.has_value() )
            {
                data.Root = *firstId;
            }
        }

        // THE BOX THE REGISTRY WILL CARRY FOR THIS FILE (AL1-8a), composed from the body by the partitioner's
        // own rule, with each mesh and nested prefab measured by its registry row - nothing is loaded for it.
        // A block the rule cannot read is named on the log and adds no box; the body itself is saved as is.
        data.Bounds.reset();
        if ( const auto box = BodyBounds(); box.has_value() )
            data.Bounds = PrefabBoundsSer{ box->Min, box->Max };

        // THE PACKED FORM, derived from exactly the body written here (PackedPrefab.hpp), so the file never states
        // a form of another body.
        data.Packed = PackPrefab( data.Entities );

        // The one writer: stamps both generation integers, so every file this engine saves is one its
        // own gate accepts. Writing rfl::json directly here would be a prefab the loader refuses.
        // With the identity this prefab already has: without it the stamp mints a fresh GUID on every save.
        return WritePrefabJson( std::move( data ), m_Guid );
    }

    Common::BoolResultStr PrefabAsset::SaveTo( const std::filesystem::path& file ) const
    {
        const auto text = Serialize();
        if ( !text )
            return Common::MakeFormattedError<bool>( "prefab save of '{}' refused, the file is unchanged: {}",
                                                     file.string(), text.GetError() );
        if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( file, text.GetValue() );
             !written )
            return written;
        // The row is re-described from the file just written, which is what carries the box it states into
        // the registry now rather than at the next scan (the mesh cook's NoteFile, for the same reason).
        ContentRegistry::NoteFile( file );
        return BOOLSUCCESS;
    }

    Common::ResultStr<Asset<PrefabAsset>>
    PrefabAsset::SaveNewFromEntity( ECS::Entity root, AssetManager& assetManager, const Common::Filepath& file )
    {
        if ( !root )
            return Common::MakeFormattedError<Asset<PrefabAsset>>( "prefab '{}' not saved: no entity to capture",
                                                                   file.string() );

        // A path the manager already holds is re-captured in place, so its GUID (and every reference to it)
        // survives; a new path is captured into a draft that only exists to be written.
        Asset<PrefabAsset>         registered = assetManager.FindByPath<PrefabAsset>( file );
        std::optional<PrefabAsset> draft;
        PrefabAsset&               target = registered ? *registered : draft.emplace( file );
        target.CreateFromEntity( root, assetManager );
        if ( const auto written = target.SaveTo( file ); !written )
            return Common::MakeError<Asset<PrefabAsset>>( written.GetError() );
        if ( registered )
            return Common::MakeSuccess( std::move( registered ) );

        Asset<PrefabAsset> created = assetManager.CreateAsset<PrefabAsset>( file );
        if ( !created )
            return Common::MakeFormattedError<Asset<PrefabAsset>>(
                 "prefab '{}' was written but does not load back; see the load error above", file.string() );
        return Common::MakeSuccess( std::move( created ) );
    }

    namespace
    {
        // THE LINKS INSIDE A CAPTURED BODY FOLLOW THE IDS (CreateFromEntity, CreateFromEntities). A record carries
        // its RECORD id, but the serializer wrote its `parent` and its entity references (Rules::kEntityReferences)
        // as the LIVE uuids the factory minted - so a re-instantiated body found no parent for any record below the
        // root and hung them all off the scene root, and an attachment named an entity of the instance it was
        // captured from. Both are rewritten through @p liveToRecord; a link that leaves the body (the root's own
        // parent, a new root CreateFromEntities made) names no record and is kept.
        void RemapBodyLinks( std::vector<EntityData>&                              records,
                             const std::unordered_map<Common::UUID, Common::UUID>& liveToRecord )
        {
            const Core::Rules::EntityIdRemap toRecord =
                 [&liveToRecord]( const Common::UUID& live ) -> std::optional<Common::UUID>
            {
                const auto hit = liveToRecord.find( live );
                if ( hit == liveToRecord.end() )
                    return std::nullopt;
                return hit->second;
            };
            for ( EntityData& record : records )
            {
                if ( record.parent.has_value() )
                    if ( const auto parent = toRecord( *record.parent ); parent.has_value() )
                        record.parent = *parent;
                Core::Rules::RemapEntityReferences( record, toRecord );
            }
        }
    } // namespace

    void PrefabAsset::AppendSubtree( ECS::Entity e, bool isRoot, const AssetManager& assetManager,
                                     std::unordered_map<Common::UUID, Common::UUID>& liveToRecord )
    {
        if ( !e )
            return;

        EntityData                        record = Core::Serialize::EntitySerializer::SerializeEntity( e, assetManager );
        const std::optional<Common::UUID> liveId = record.id;

        // A NESTED PREFAB IS A LINK, NOT A COPY OF ITS BODY.
        //
        // This walk used to descend into nested instances as well, writing the nested prefab's every
        // entity into THIS file beside the link to it. Instantiating the result then produced both:
        // PrefabFactory creates a PrefabPath record in place (CreatedInPlace) AND hangs the nested
        // file's body underneath it, so every nested entity appeared TWICE — and an edit to the
        // nested prefab reached only one of the two copies. Stopping here is what makes "nested
        // prefab" mean a reference at all; what the instance holds of its own travels as overrides
        // on this record, exactly as it does for a scene.
        const bool stopsHere = !isRoot && e.HasComponent<ECS::PrefabComponent>() && record.PrefabPath.has_value();

        // A RECORD ID IS A LINK KEY, AND IT MUST SURVIVE "APPLY INSTANCE CHANGES TO PREFAB".
        //
        // EntitySerializer writes the entity's own uuid, and PrefabFactory mints those fresh on every
        // instantiation — so rewriting a prefab from one of its instances used to renumber every
        // record in the file. Nothing depended on that before; now every override in every scene is
        // addressed by these ids, and renumbering them would orphan the lot in one save, silently.
        // The entity already knows which record it came from, so the file keeps that number.
        //
        // WHICH ELEMENT OF THE PATH. The last one for an entity this walk descends through — that is
        // its own record in the prefab it belongs to. The one BEFORE last for an entity the walk
        // stops at, because the record being written there is not the nested root's: it is the
        // NESTING record of the file being written, and its id is what the nested body will be
        // addressed under. Both are unique among the records this walk emits, because everything it
        // descends through belongs to one prefab.
        if ( e.HasComponent<ECS::PrefabInstanceComponent>() )
        {
            const auto& path = e.GetComponent<ECS::PrefabInstanceComponent>().SourcePath;
            if ( stopsHere && path.size() >= 2 )
            {
                record.id = path[path.size() - 2];
            }
            else if ( !stopsHere && !path.empty() )
            {
                record.id = path.back();
            }
        }
        if ( liveId.has_value() && record.id.has_value() )
            liveToRecord.emplace( *liveId, *record.id );

        if ( stopsHere )
        {
            // A LINK STANDS WHERE ITS RECORD SAYS (UE: a level instance nested in a level carries its own actor
            // transform there). The record keeps the nested root's local transform SerializeEntity wrote, and the
            // root's override loses the same three fields, so the placement has one home: the one the factory
            // places the nested root by, the one IndexBaseRecords bases the next capture on, and the one
            // Rules::PrefabBounds composes the nested box with (an override is invisible to it, so a link whose
            // placement lived there was measured at its parent's origin).
            auto capture = Core::Serialize::CapturePrefabInstance( e, assetManager );
            if ( capture.RootPath.has_value() )
            {
                for ( PrefabOverrideData& over : capture.Overrides )
                {
                    if ( over.Path != *capture.RootPath )
                        continue;
                    over.Translation.reset();
                    over.Rotation.reset();
                    over.Scale.reset();
                }
                std::erase_if( capture.Overrides,
                               [&]( const PrefabOverrideData& over )
                               {
                                   return over.Path == *capture.RootPath && !over.Tag.has_value() &&
                                          over.Components.begin() == over.Components.end();
                               } );
            }
            if ( !capture.Overrides.empty() )
            {
                record.PrefabOverrides = std::move( capture.Overrides );
            }

            record.Tag = std::nullopt;
            record.Components.clear();

            m_EntityData.push_back( std::move( record ) );
            return;
        }

        m_EntityData.push_back( std::move( record ) );

        // Traverse children
        if ( e.HasComponent<ECS::RelationshipComponent>() )
        {
            const auto& rel = e.GetComponent<ECS::RelationshipComponent>();
            for ( auto childHandle : rel.Children )
            {
                AppendSubtree( ECS::Entity{ childHandle, *e.GetRegistry() }, false, assetManager, liveToRecord );
            }
        }
    }

    void PrefabAsset::CreateFromEntity( ECS::Entity rootEntity, const AssetManager& assetManager )
    {
        if ( !rootEntity )
            return;

        m_EntityData.clear();

        // The live uuid each record was written from -> the record id it is written under (RemapBodyLinks).
        std::unordered_map<Common::UUID, Common::UUID> liveToRecord;
        AppendSubtree( rootEntity, true, assetManager, liveToRecord );

        // THE LINKS INSIDE THE BODY FOLLOW THE IDS. A record now carries its RECORD id, but the serializer wrote
        // its `parent` and its entity references (Rules::kEntityReferences) as the LIVE uuids the factory minted -
        // so a re-instantiated body found no parent for any record below the root and hung them all off the scene
        // root, and an attachment named an entity of the instance it was captured from. Both are rewritten here
        // through the same map; a link that leaves the body (the root's own parent) names no record and is kept.
        RemapBodyLinks( m_EntityData, liveToRecord );

        // Root entity's own PrefabComponent points to this very file — strip it from the
        // serialized data so that re-instantiating doesn't recurse into itself.
        if ( !m_EntityData.empty() )
            m_EntityData[0].PrefabPath = std::nullopt;

        // The captured root's own streaming override is the instance's word on how this prefab streams: applied,
        // it becomes the prefab's (WP21-WHOLE). The record keeps none - a body record with no PrefabPath that
        // stated one would be a setting nothing reads, and the cook refuses it.
        if ( !m_EntityData.empty() && m_EntityData[0].RuntimeBehavior.has_value() )
            m_RuntimeBehavior = *m_EntityData[0].RuntimeBehavior;
        if ( !m_EntityData.empty() )
            m_EntityData[0].RuntimeBehavior.reset();

        // A new body is a new source: every packed instance of this prefab follows the form packed from it.
        AdoptOrRepack( std::nullopt );
        m_IsLoaded = true;
        // Nothing on disk holds this yet — see IsReloadableFromFile and Unload's refusal.
        m_CapturedInMemory = true;
    }

    void PrefabAsset::ReplaceBody( std::vector<EntityData> records )
    {
        m_EntityData = std::move( records );
        m_IsLoaded   = true;
        // As CreateFromEntity: the body is the caller's until SaveTo puts it in the file.
        AdoptOrRepack( std::nullopt );
        m_CapturedInMemory = true;
    }

    Common::BoolResultStr PrefabAsset::CreateFromEntities( std::string_view        rootTag,
                                                           std::span<const Member> members,
                                                           const AssetManager&     assetManager )
    {
        std::vector<EntityData> previous = std::move( m_EntityData );
        m_EntityData.clear();
        const auto refuse = [&]( std::string reason )
        {
            m_EntityData = std::move( previous );
            return Common::MakeError<bool>( std::move( reason ) );
        };
        if ( members.empty() )
            return refuse(
                 std::format( "prefab '{}' not captured: no entity to capture", m_Metadata.Filepath.string() ) );

        // THE NEW ROOT IS DATA ONLY: no entity stands for it in the scene, so nothing is created and torn down
        // again.
        EntityData root;
        root.id                   = Common::UUID::Generate();
        root.Tag                  = std::string( rootTag );
        root.Translation          = glm::vec3( 0.0f );
        root.Rotation             = glm::vec3( 0.0f );
        root.Scale                = glm::vec3( 1.0f );
        const Common::UUID rootId = *root.id;
        m_EntityData.push_back( std::move( root ) );

        std::unordered_map<Common::UUID, Common::UUID> liveToRecord;
        for ( const Member& member : members )
        {
            if ( !member.Entity )
                return refuse( std::format( "prefab '{}' not captured: a member entity no longer exists",
                                            m_Metadata.Filepath.string() ) );
            // The member's own record is the first its walk appends, a plain entity's or a nested link's alike:
            // both state their placement on the record (AppendSubtree), so the new frame is written there for
            // both.
            const std::size_t first = m_EntityData.size();
            AppendSubtree( member.Entity, false, assetManager, liveToRecord );
            EntityData& top = m_EntityData[first];
            top.parent      = rootId;
            top.Translation = member.InRoot.Translation;
            top.Rotation    = member.InRoot.Rotation;
            top.Scale       = member.InRoot.Scale;
        }

        RemapBodyLinks( m_EntityData, liveToRecord );

        m_IsLoaded = true;
        // Nothing on disk holds this yet — see IsReloadableFromFile and Unload's refusal.
        AdoptOrRepack( std::nullopt );
        m_CapturedInMemory = true;
        return BOOLSUCCESS;
    }

    std::optional<Common::Math::AABB> PrefabAsset::BodyBounds() const
    {
        const Common::UUID   root = m_EntityData.empty() ? Common::UUID::Null()
                                                         : m_EntityData.front().id.value_or( Common::UUID::Null() );
        Common::Json::Issues issues;
        auto box = Core::Rules::PrefabBounds( m_EntityData, root, &ContentRegistry::BoundsOf, issues );
        if ( !issues.empty() )
            Common::Json::ReportIssues( issues, "prefab bounds of '" + m_Metadata.Filepath.string() + "'" );
        return box;
    }

    void PrefabAsset::AdoptOrRepack( std::optional<PackedPrefabSer> stated )
    {
        const std::string checksum = PrefabSourceHash( m_EntityData );
        if ( stated.has_value() && stated->SourceHash == checksum )
        {
            m_Packed = std::make_shared<const PackedPrefabSer>( std::move( *stated ) );
            return;
        }
        // A stated form of another body: the file was edited outside the editor, or by a build whose writer
        // spelled the body differently. Derived data, so it is rebuilt rather than refused - and said.
        if ( stated.has_value() )
            LOG_INFO( "[Prefab] '{}': the packed form states source checksum {} and the body is {}; repacked.",
                      m_Metadata.Filepath.string(), stated->SourceHash, checksum );
        m_Packed = std::make_shared<const PackedPrefabSer>( PackPrefab( m_EntityData ) );
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
    // Everything this adds over the factory is the PLACEMENT: where the instance hangs, whether it may
    // hang there, and the position the Lua binding and the editor's drag-and-drop pass in.
    Common::ResultStr<ECS::Entity> PrefabAsset::Instantiate( Core::Scene* scene, const AssetManager& assetManager,
                                                             ECS::Entity parent, const glm::vec3* position ) const
    {
        if ( scene == nullptr )
        {
            return Common::MakeFormattedError<ECS::Entity>(
                 "[Prefab] '{}' was not instantiated: no scene was given.", m_Metadata.Filepath.string() );
        }
        if ( m_EntityData.empty() )
        {
            return Common::MakeFormattedError<ECS::Entity>(
                 "[Prefab] '{}' holds no entities; nothing was created. Load it first, or re-save it from "
                 "an entity.",
                 m_Metadata.Filepath.string() );
        }

        // THE VERDICT IS TAKEN BEFORE ANYTHING EXISTS. Asking after the entities are in the scene would
        // mean deleting them again on refusal, and a half-built instance that is then torn down is the
        // state every "Instantiate appeared to do nothing" report has come from.
        entt::registry& registry = scene->GetRegistry();

        bool        targetUnderCanvas = false;
        std::string targetName        = "the scene root";
        if ( parent )
        {
            targetUnderCanvas = UI::CanvasOf( registry, parent.GetHandle() ) != entt::null;
            if ( parent.HasComponent<ECS::TagComponent>() )
            {
                targetName = "'" + parent.GetComponent<ECS::TagComponent>().Tag + "'";
            }
        }

        const PrefabPlacementVerdict verdict =
             CheckPrefabPlacement( ClassifyPrefabRoot( m_EntityData.front() ), targetUnderCanvas,
                                   m_Metadata.Filepath.string(), targetName );
        if ( !verdict.Allowed )
        {
            return Common::MakeError<ECS::Entity>( verdict.Refusal );
        }

        std::unordered_set<Common::UUID> stack;
        const ECS::Entity                rootEntity =
             Runtime::Factory::PrefabFactory::Instantiate( *this, *scene, assetManager, stack );

        if ( !rootEntity )
        {
            return Common::MakeFormattedError<ECS::Entity>(
                 "[Prefab] '{}' produced no root entity. Its records were read but none could be created — "
                 "see the [PrefabFactory] lines above for which.",
                 m_Metadata.Filepath.string() );
        }

        if ( parent )
        {
            scene->Attach( parent, rootEntity );
        }

        if ( position != nullptr )
        {
            if ( rootEntity.HasComponent<ECS::TransformComponent>() )
            {
                rootEntity.SetLocalTranslation( *position );
            }
            else
            {
                rootEntity.AddComponent<ECS::TransformComponent>( ECS::LocalTransform{ .Translation = *position } );
            }
        }

        return Common::MakeSuccess( rootEntity );
    }

} // namespace Desert::Assets