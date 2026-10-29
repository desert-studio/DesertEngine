#include "SceneCommands.hpp"

#include <Editor/Core/Selection/ModelingToolTarget.hpp> // PlanMeshRestore

#include "InstanceFold.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Editor/Core/StaticMeshOutput.hpp>

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/ComponentRegistry.hpp>
#include <Engine/Core/Serialize/EntitySerializer.hpp>
#include <Engine/Core/Serialize/SceneStitchRules.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/ECS/EntityLock.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <spdlog/fmt/fmt.h>

#include <glm/gtc/epsilon.hpp>

#include <format>
#include <functional>
#include <optional>
#include <unordered_map>

namespace Desert::Editor::Commands
{
    namespace
    {
        // NOTE: fully qualified — inside Desert::Editor, an unqualified `Core::` means Editor::Core.
        ::Desert::Core::Scene*                s_Scene        = nullptr;
        ::Desert::Assets::AssetManager*       s_AssetManager = nullptr;

        // A captured subtree: one record per entity, pre-order, root first — and, per record, the prefab
        // record path the entity was instantiated from (ECS::PrefabInstanceComponent).
        //
        // THE PATH IS CARRIED BESIDE THE RECORD because it is runtime-only and the serializer rightly does not
        // write it. Restoring without it rebuilt a prefab instance whose every entity had lost its link to the
        // prefab: the scene saver then counted them as "added inside the instance" and DROPPED them from the
        // file — an undone delete, a redone Replace with Prefab or a pasted instance lost its lights on save.
        struct SubtreeSnapshot
        {
            std::vector<Assets::EntityData> Records;
            std::vector<std::vector<Common::UUID>>
                 SourcePaths; // index-matched to Records; empty = not from a prefab

            // The root alone (its children stay where they are).
            void KeepRootOnly()
            {
                Records.resize( 1 );
                SourcePaths.resize( 1 );
            }
        };

        // Editor clipboard: serialized subtree snapshots (NOT entity references), so pasting works even
        // after the originals were deleted or the scene changed.
        std::vector<SubtreeSnapshot> s_Clipboard;

        bool Ready()
        {
            return s_Scene && s_AssetManager;
        }

        // Entity is a lightweight value handle — copying out of the const ref gives a mutable handle.
        std::optional<ECS::Entity> FindEntity( const Common::UUID& uuid )
        {
            if ( !s_Scene || uuid.IsNull() )
                return std::nullopt;
            if ( auto ref = s_Scene->FindEntityByID( uuid ) )
                return ref->get();
            return std::nullopt;
        }

        Common::UUID ParentUUIDOf( ECS::Entity entity )
        {
            if ( entity.HasComponent<ECS::RelationshipComponent>() )
            {
                const auto parentHandle = entity.GetComponent<ECS::RelationshipComponent>().Parent;
                if ( parentHandle != entt::null )
                {
                    ECS::Entity parent( parentHandle, *entity.GetRegistry() );
                    if ( parent.HasComponent<ECS::UUIDComponent>() )
                        return parent.GetComponent<ECS::UUIDComponent>().UUID;
                }
            }
            return Common::UUID::Null();
        }

        // Pre-order subtree snapshot through the prefab serialization path: full component fidelity,
        // root first. The root's own `parent` field records where the subtree hung in the scene.
        SubtreeSnapshot CaptureSubtree( ECS::Entity root )
        {
            SubtreeSnapshot data;

            std::function<void( ECS::Entity )> traverse = [&]( ECS::Entity e )
            {
                if ( !e )
                    return;
                data.Records.push_back(
                     ::Desert::Core::Serialize::EntitySerializer::SerializeEntity( e, *s_AssetManager ) );
                data.SourcePaths.push_back( e.HasComponent<ECS::PrefabInstanceComponent>()
                                                 ? e.GetComponent<ECS::PrefabInstanceComponent>().SourcePath
                                                 : std::vector<Common::UUID>{} );
                if ( e.HasComponent<ECS::RelationshipComponent>() )
                {
                    for ( auto childHandle : e.GetComponent<ECS::RelationshipComponent>().Children )
                        traverse( ECS::Entity( childHandle, *e.GetRegistry() ) );
                }
            };
            traverse( root );

            return data;
        }

        // Rebuilds a captured subtree. preserveIds=true (undo of a delete) recreates the exact same
        // UUIDs; false (duplicate) mints fresh ones. In-snapshot parent links are remapped; the root's
        // outer parent is re-attached by UUID if it still exists in the scene. Returns the new root.
        //
        // WHICH RECORD BECOMES WHICH ENTITY is Rules::PlanSceneStitch — the same function the scene loader
        // and the prefab factory plan with. This was the fourth hand-written copy of that stitch and the
        // one that had drifted furthest: it keyed the map with `ed.id.value_or( Null() )`, so every record
        // WITHOUT an id landed on the key 0. Two anonymous entities in one snapshot therefore collapsed
        // into one — the first received the second's components on top of its own, the second was created
        // and left bare, and any child of the second attached to the first. The plan mints a distinct id
        // per idless record, so two of them can no longer share a key.
        //
        // CreatedInPlace: a snapshot of a prefab instance contains that prefab's children as records of
        // their own, because CaptureSubtree walks the LIVE subtree. So a record carrying a PrefabPath has
        // to be rebuilt from the snapshot like any other — instantiating its file again would duplicate
        // every child the snapshot already holds.
        ECS::Entity RestoreSnapshot( const SubtreeSnapshot& snapshot, bool preserveIds )
        {
            const std::vector<Assets::EntityData>& data = snapshot.Records;
            using ::Desert::Core::Rules::kNoSlot;
            using ::Desert::Core::Rules::PlanSceneStitch;
            using ::Desert::Core::Rules::PrefabRecordPolicy;

            const ::Desert::Core::Rules::StitchPlan plan =
                 PlanSceneStitch( data, &Common::UUID::Generate, PrefabRecordPolicy::CreatedInPlace );

            // DC §1.4: a snapshot that names one id twice restores as a subtree quietly missing a piece.
            // (UnresolvedParents is not reported: the root record keeps the `parent` it had in the scene,
            // which is outside the snapshot by definition — that is the normal case, handled below.)
            if ( plan.Shadowed > 0 )
            {
                LOG_WARN( "[SceneCommands] snapshot restore: {0} of {1} record(s) claim an id another "
                          "record already claimed; their components are applied to the first claimant and "
                          "their own entities are left bare.",
                          plan.Shadowed, data.size() );
            }

            // The entity each slot became. A slot the double-restore guard finds already alive is filled
            // with the LIVE entity — children still need something to attach to — but is not written to
            // again, which is what `restored` marks.
            std::vector<ECS::Entity> created( plan.Created.size() );
            std::vector<char>        restored( plan.Created.size(), 0 );

            for ( size_t slot = 0; slot < plan.Created.size(); ++slot )
            {
                const Common::UUID original = plan.Created[slot].Id;
                if ( preserveIds )
                {
                    if ( auto alive = FindEntity( original ) )
                    {
                        created[slot] = *alive; // already alive (double-restore guard)
                        continue;
                    }
                }

                const Common::UUID newId = preserveIds ? original : Common::UUID::Generate();
                created[slot]            = s_Scene->CreateEntityWithUUID(
                     newId, data[plan.Created[slot].Record].Tag.value_or( "Entity" ) );
                restored[slot] = 1;
            }

            ECS::Entity root;
            for ( const auto& load : plan.Loads )
            {
                if ( !restored[load.Target] )
                    continue;

                const Assets::EntityData& ed = data[load.Record];
                ECS::Entity               e  = created[load.Target];
                ::Desert::Core::Serialize::EntitySerializer::DeserializeEntity( ed, e, *s_AssetManager );
                if ( load.Record < snapshot.SourcePaths.size() && !snapshot.SourcePaths[load.Record].empty() )
                {
                    if ( e.HasComponent<ECS::PrefabInstanceComponent>() )
                        e.GetComponent<ECS::PrefabInstanceComponent>().SourcePath =
                             snapshot.SourcePaths[load.Record];
                    else
                        e.AddComponent<ECS::PrefabInstanceComponent>().SourcePath =
                             snapshot.SourcePaths[load.Record];
                }

                if ( load.Parent != kNoSlot )
                {
                    s_Scene->Attach( created[load.Parent], e ); // in-snapshot child
                }
                else if ( ed.parent.has_value() && !ed.parent->IsNull() )
                {
                    if ( auto outer = FindEntity( *ed.parent ) )
                        s_Scene->Attach( *outer, e ); // the root, back onto its original scene parent
                }

                if ( !root )
                    root = e;
            }
            return root;
        }

        Common::UUID UUIDOf( ECS::Entity e )
        {
            return e.HasComponent<ECS::UUIDComponent>() ? e.GetComponent<ECS::UUIDComponent>().UUID
                                                        : Common::UUID::Null();
        }

        void DestroyByUUID( const Common::UUID& uuid )
        {
            if ( auto e = FindEntity( uuid ) )
            {
                s_Scene->DestroyEntity( *e );
                if ( const auto& sel = Core::SelectionManager::GetSelected();
                     sel.has_value() && *sel == uuid )
                    Core::SelectionManager::ClearSelection();
            }
        }

        // Entity creation/destruction relocates entt component pools — any raw property-byte entries in
        // the history would then point at freed memory, so drop them around every structural mutation.
        void OnStructuralChange()
        {
            CommandHistory::Get().DropVolatile();
        }

        // ---------------------------------------------------------------- commands

        class TransformCommand final : public ICommand
        {
        public:
            TransformCommand( const Common::UUID& entity, const glm::vec3& oldT, const glm::vec3& oldR,
                              const glm::vec3& oldS, const glm::vec3& newT, const glm::vec3& newR,
                              const glm::vec3& newS )
                 : m_Entity( entity ), m_Old{ oldT, oldR, oldS }, m_New{ newT, newR, newS }
            {
            }

            std::string GetLabel() const override
            {
                return "Move / Transform";
            }

            bool Undo() override
            {
                return Apply( m_Old );
            }
            bool Redo() override
            {
                return Apply( m_New );
            }

        private:
            struct TRS
            {
                glm::vec3 T, R, S;
            };

            bool Apply( const TRS& trs )
            {
                auto e = FindEntity( m_Entity );
                if ( !e || !e->HasComponent<ECS::TransformComponent>() )
                    return false;
                e->SetLocalTransform( ECS::LocalTransform{ trs.T, trs.R, trs.S } );
                return true;
            }

            Common::UUID m_Entity;
            TRS          m_Old, m_New;
        };

        class RenameCommand final : public ICommand
        {
        public:
            RenameCommand( const Common::UUID& entity, std::string oldName, std::string newName )
                 : m_Entity( entity ), m_OldName( std::move( oldName ) ), m_NewName( std::move( newName ) )
            {
            }

            std::string GetLabel() const override
            {
                return "Rename";
            }

            bool Undo() override
            {
                return Apply( m_OldName );
            }
            bool Redo() override
            {
                return Apply( m_NewName );
            }

        private:
            bool Apply( const std::string& name )
            {
                auto e = FindEntity( m_Entity );
                if ( !e || !e->HasComponent<ECS::TagComponent>() )
                    return false;
                e->GetComponent<ECS::TagComponent>().Tag = name;
                return true;
            }

            Common::UUID m_Entity;
            std::string  m_OldName, m_NewName;
        };

        // A lock toggle (EntityLock.hpp) over one or more subtrees: every entity's lock state BEFORE, so undo puts
        // a mixed subtree (a child locked on its own) back exactly, and the state every entity is set to on redo.
        class LockCommand final : public ICommand
        {
        public:
            LockCommand( std::vector<std::pair<Common::UUID, bool>> before, bool locked )
                 : m_Before( std::move( before ) ), m_Locked( locked )
            {
            }

            std::string GetLabel() const override
            {
                return m_Locked ? "Lock" : "Unlock";
            }

            bool Undo() override
            {
                bool any = false;
                for ( const auto& [id, wasLocked] : m_Before )
                    any |= Apply( id, wasLocked );
                return any;
            }
            bool Redo() override
            {
                bool any = false;
                for ( const auto& [id, wasLocked] : m_Before )
                    any |= Apply( id, m_Locked );
                return any;
            }

        private:
            static bool Apply( const Common::UUID& id, bool locked )
            {
                auto e = FindEntity( id );
                if ( !e )
                    return false;
                auto& registry = *e->GetRegistry();
                if ( locked && !registry.has<ECS::LockComponent>( e->GetHandle() ) )
                    registry.emplace<ECS::LockComponent>( e->GetHandle() );
                else if ( !locked && registry.has<ECS::LockComponent>( e->GetHandle() ) )
                    registry.remove<ECS::LockComponent>( e->GetHandle() );
                return true;
            }

            std::vector<std::pair<Common::UUID, bool>> m_Before;
            bool                                       m_Locked;
        };

        // An edit of an entity's editable mesh (EditableMesh.hpp), UE's FMeshReplacementChange: the two meshes BY
        // REFERENCE. They are immutable once set on a component - an edit builds a new one - so holding the old
        // pointer IS the snapshot, with no copy and no serialization, and nothing can change it under the history.
        class MeshReplacementCommand final : public ICommand
        {
        public:
            MeshReplacementCommand( const Common::UUID& entity, std::string label,
                                    std::shared_ptr<const Geometry::DynamicMesh3> before,
                                    std::shared_ptr<const Geometry::DynamicMesh3> after,
                                    std::unique_ptr<ICommand>                     alongside )
                 : m_Entity( entity ), m_Label( std::move( label ) ), m_Before( std::move( before ) ),
                   m_After( std::move( after ) ), m_Alongside( std::move( alongside ) )
            {
            }

            [[nodiscard]] std::string GetLabel() const override
            {
                return m_Label;
            }

            // The mesh first, then its companion: a selection restored after its mesh is pruned against the
            // mesh it was made on.
            bool Undo() override
            {
                const bool applied = Apply( m_Before );
                if ( applied && m_Alongside )
                    m_Alongside->Undo();
                return applied;
            }
            bool Redo() override
            {
                const bool applied = Apply( m_After );
                if ( applied && m_Alongside )
                    m_Alongside->Redo();
                return applied;
            }

        private:
            bool Apply( const std::shared_ptr<const Geometry::DynamicMesh3>& mesh )
            {
                auto e = FindEntity( m_Entity );
                if ( !e || !e->HasComponent<ECS::StaticMeshComponent>() )
                    return false;
                auto& smc = e->GetComponent<ECS::StaticMeshComponent>();
                // Null is the asset-only entity (a Modeling tool lifted its .stmesh): PlanMeshRestore drops
                // the EditableMesh.
                const MeshRestore plan = PlanMeshRestore( smc.GetEditableMesh(), mesh );
                if ( plan != MeshRestore::Set )
                {
                    if ( plan == MeshRestore::Clear )
                        ECS::ClearEditableMesh( *e->GetRegistry(), e->GetHandle() );
                    return true;
                }
                // Both states were on the component once, so a refusal here means the device refused the
                // upload; the entry is dropped (false) rather than claiming a state the screen does not show.
                if ( auto set = ECS::SetEditableMesh( *e->GetRegistry(), e->GetHandle(), mesh ); !set.IsSuccess() )
                {
                    LOG_ERROR( "[Undo] '{0}' could not restore the mesh: {1}", m_Label, set.GetError() );
                    return false;
                }
                return true;
            }

            Common::UUID                              m_Entity;
            std::string                               m_Label;
            std::shared_ptr<const Geometry::DynamicMesh3> m_Before, m_After;
            std::unique_ptr<ICommand>                 m_Alongside; // part of the same step (may be null)
        };

        class ReparentCommand final : public ICommand
        {
        public:
            ReparentCommand( const Common::UUID& child, const Common::UUID& oldParent,
                             const Common::UUID& newParent )
                 : m_Child( child ), m_OldParent( oldParent ), m_NewParent( newParent )
            {
            }

            std::string GetLabel() const override
            {
                return "Reparent";
            }

            bool Undo() override
            {
                return SetParent( m_OldParent );
            }
            bool Redo() override
            {
                return SetParent( m_NewParent );
            }

        private:
            bool SetParent( const Common::UUID& parent )
            {
                auto child = FindEntity( m_Child );
                if ( !child )
                    return false;
                if ( parent.IsNull() )
                {
                    s_Scene->Detach( *child );
                    return true;
                }
                auto parentEntity = FindEntity( parent );
                if ( !parentEntity )
                    return false;
                s_Scene->Attach( *parentEntity, *child );
                return true;
            }

            Common::UUID m_Child, m_OldParent, m_NewParent;
        };

        class DeleteCommand final : public ICommand
        {
        public:
            explicit DeleteCommand( SubtreeSnapshot&& snapshot ) : m_Snapshot( std::move( snapshot ) )
            {
            }

            std::string GetLabel() const override
            {
                return "Delete";
            }

            bool Undo() override
            {
                if ( m_Snapshot.Records.empty() )
                    return false;
                ECS::Entity root = RestoreSnapshot( m_Snapshot, /*preserveIds=*/true );
                OnStructuralChange();
                if ( !root )
                    return false;
                Core::SelectionManager::SetSelected( UUIDOf( root ) );
                return true;
            }

            bool Redo() override
            {
                const Common::UUID root = m_Snapshot.Records.empty()
                                               ? Common::UUID::Null()
                                               : m_Snapshot.Records.front().id.value_or( Common::UUID::Null() );
                if ( !FindEntity( root ) )
                    return false;
                DestroyByUUID( root );
                OnStructuralChange();
                return true;
            }

        private:
            SubtreeSnapshot m_Snapshot;
        };

        // Groups sub-commands into ONE history entry (multi-selection operations). Undo runs the
        // sub-commands in reverse; a stale sub-command is skipped, not fatal.
        // The second half of a split (Plane Cut, Keep Both Halves): a new entity carrying `m_Mesh` BY REFERENCE,
        // riding as the companion of the source's MeshReplacementCommand so the two are one step. The snapshot
        // taken at undo restores the entity; its mesh is put back from the reference, not re-read from the
        // snapshot.
        class SplitCopyCommand final : public ICommand
        {
        public:
            SplitCopyCommand( const Common::UUID& copy, std::shared_ptr<const Geometry::DynamicMesh3> mesh,
                              std::unique_ptr<ICommand> alongside )
                 : m_Copy( copy ), m_Mesh( std::move( mesh ) ), m_Alongside( std::move( alongside ) )
            {
            }

            [[nodiscard]] std::string GetLabel() const override
            {
                return "Split";
            }

            bool Undo() override
            {
                if ( m_Alongside )
                    m_Alongside->Undo();
                auto e = FindEntity( m_Copy );
                if ( !e )
                    return false;
                m_Snapshot = CaptureSubtree( *e );
                DestroyByUUID( m_Copy );
                OnStructuralChange();
                return true;
            }

            bool Redo() override
            {
                const ECS::Entity copy = RestoreSnapshot( m_Snapshot, /*preserveIds=*/true );
                if ( !copy || !copy.HasComponent<ECS::StaticMeshComponent>() )
                    return false;
                if ( auto set = ECS::SetEditableMesh( *copy.GetRegistry(), copy.GetHandle(), m_Mesh );
                     !set.IsSuccess() )
                {
                    LOG_ERROR(
                         "[SceneCommands] redo of a split: the half could not be put back on its entity: {0}",
                         set.GetError() );
                    return false;
                }
                OnStructuralChange();
                if ( m_Alongside )
                    m_Alongside->Redo();
                return true;
            }

        private:
            Common::UUID                              m_Copy;
            std::shared_ptr<const Geometry::DynamicMesh3> m_Mesh;
            std::unique_ptr<ICommand>                 m_Alongside;
            SubtreeSnapshot                               m_Snapshot;
        };

        // XForm: the state an entity is put in, and the command that walks a whole XForm edit back and forth.
        [[nodiscard]] Common::BoolResultStr ApplyXformState( const XformEntityState& state,
                                                             const Common::UUID&     id )
        {
            auto e = FindEntity( id );
            if ( !e || !e->HasComponent<ECS::StaticMeshComponent>() ||
                 !e->HasComponent<ECS::TransformComponent>() )
                return Common::MakeFormattedError<bool>( "entity {} has no mesh or no transform",
                                                         static_cast<uint64_t>( id ) );
            auto& smc = e->GetComponent<ECS::StaticMeshComponent>();
            // A null mesh is a state too: the entity drawn from its asset alone (a Modeling tool lifted the
            // .stmesh), so undoing the edit drops the EditableMesh instead of keeping the edited one.
            const MeshRestore plan = PlanMeshRestore( smc.GetEditableMesh(), state.Mesh );
            ECS::StaticMeshDesc desc = smc.GetDesc();
            if ( plan == MeshRestore::Clear )
                ECS::ClearEditableMesh( desc );
            else if ( plan == MeshRestore::Set )
                if ( auto set = ECS::SetEditableMesh( desc, state.Mesh ); !set.IsSuccess() )
                    return Common::MakeFormattedError<bool>( "entity {}: {}", static_cast<uint64_t>( id ),
                                                             set.GetError() );
            // The writer drops the runtime instances on a slot change, so the mesh system rebuilds them.
            if ( state.MaterialSlots )
                desc.MaterialSlots = *state.MaterialSlots;
            ECS::SetStaticMesh( *e->GetRegistry(), e->GetHandle(), desc );
            e->SetLocalTransform( ECS::LocalTransform{ state.Translation, state.Rotation, state.Scale } );
            if ( state.Visible )
                e->SetVisible( *state.Visible );
            return Common::MakeSuccess( true );
        }

        std::optional<XformEntityState> CaptureXformState( const Common::UUID& id, bool withSlots,
                                                           bool withVisibility )
        {
            auto e = FindEntity( id );
            if ( !e || !e->HasComponent<ECS::StaticMeshComponent>() ||
                 !e->HasComponent<ECS::TransformComponent>() )
                return std::nullopt;
            const auto&      smc = e->GetComponent<ECS::StaticMeshComponent>();
            const auto&      tc  = e->GetComponent<ECS::TransformComponent>();
            XformEntityState out{ id,       smc.GetEditableMesh(), tc.GetTranslation(), tc.GetRotation(),
                                  tc.GetScale(), std::nullopt,     std::nullopt };
            if ( withSlots )
                out.MaterialSlots = smc.GetMaterialSlots();
            // No component reads as shown (ECS::IsHidden), so undoing a hide leaves it shown either way.
            if ( withVisibility )
                out.Visible = !e->HasComponent<ECS::VisibilityComponent>() ||
                              e->GetComponent<ECS::VisibilityComponent>().IsVisible();
            return out;
        }

        class XformCommand final : public ICommand
        {
        public:
            struct Changed
            {
                XformEntityState Before, After;
            };
            struct Made
            {
                Common::UUID                    Id;
                XformEntityState                State;
                SubtreeSnapshot                 Snapshot;
            };
            struct Gone
            {
                Common::UUID                                   Id;
                std::shared_ptr<const Geometry::DynamicMesh3>  Mesh;
                SubtreeSnapshot                                Snapshot;
            };

            XformCommand( std::string label, std::vector<Changed> changed, std::vector<Made> made,
                          std::vector<Gone> gone )
                 : m_Label( std::move( label ) ), m_Changed( std::move( changed ) ), m_Made( std::move( made ) ),
                   m_Gone( std::move( gone ) )
            {
            }

            [[nodiscard]] std::string GetLabel() const override
            {
                return m_Label;
            }

            bool Undo() override
            {
                bool ok = true;
                for ( Made& made : m_Made )
                {
                    auto e = FindEntity( made.Id );
                    if ( !e )
                    {
                        ok = false;
                        continue;
                    }
                    made.Snapshot = CaptureSubtree( *e );
                    DestroyByUUID( made.Id );
                }
                for ( auto it = m_Changed.rbegin(); it != m_Changed.rend(); ++it )
                    ok &= Report( ApplyXformState( it->Before, it->Before.Entity ) );
                for ( Gone& gone : m_Gone )
                {
                    ECS::Entity root = RestoreSnapshot( gone.Snapshot, /*preserveIds=*/true );
                    if ( !root || !root.HasComponent<ECS::StaticMeshComponent>() )
                    {
                        ok = false;
                        continue;
                    }
                    // An asset-only part (null mesh) comes back from its snapshot's MeshHandle alone.
                    const auto& goneMesh = root.GetComponent<ECS::StaticMeshComponent>();
                    if ( PlanMeshRestore( goneMesh.GetEditableMesh(), gone.Mesh ) == MeshRestore::Set )
                        ok &= Report( ECS::SetEditableMesh( *root.GetRegistry(), root.GetHandle(), gone.Mesh ) );
                }
                OnStructuralChange();
                return ok;
            }

            bool Redo() override
            {
                bool ok = true;
                for ( Gone& gone : m_Gone )
                {
                    auto e = FindEntity( gone.Id );
                    if ( !e )
                    {
                        ok = false;
                        continue;
                    }
                    gone.Snapshot = CaptureSubtree( *e );
                    DestroyByUUID( gone.Id );
                }
                for ( const Changed& changed : m_Changed )
                    ok &= Report( ApplyXformState( changed.After, changed.After.Entity ) );
                for ( const Made& made : m_Made )
                {
                    ECS::Entity root = RestoreSnapshot( made.Snapshot, /*preserveIds=*/true );
                    ok &= root && Report( ApplyXformState( made.State, made.Id ) );
                }
                OnStructuralChange();
                return ok;
            }

        private:
            bool Report( const Common::BoolResultStr& result ) const
            {
                if ( !result.IsSuccess() )
                    LOG_ERROR( "[Undo] '{0}': {1}", m_Label, result.GetError() );
                return result.IsSuccess();
            }

            std::string          m_Label;
            std::vector<Changed> m_Changed;
            std::vector<Made>    m_Made;
            std::vector<Gone>    m_Gone;
        };

        class CompositeCommand final : public ICommand
        {
        public:
            CompositeCommand() = default;
            // A step that is ONE gesture to the person ("Replace with Prefab") names itself, rather than
            // showing the count of the parts it was assembled from.
            explicit CompositeCommand( std::string label ) : m_Label( std::move( label ) )
            {
            }

            void Add( std::unique_ptr<ICommand> command )
            {
                m_Commands.push_back( std::move( command ) );
            }

            bool Empty() const
            {
                return m_Commands.empty();
            }

            size_t Size() const
            {
                return m_Commands.size();
            }

            std::unique_ptr<ICommand> TakeSingle()
            {
                auto cmd = std::move( m_Commands.front() );
                m_Commands.clear();
                return cmd;
            }

            std::string GetLabel() const override
            {
                if ( !m_Label.empty() )
                    return m_Label;
                if ( m_Commands.size() == 1 )
                    return m_Commands.front()->GetLabel();
                return std::format( "Grouped edit ({})", m_Commands.size() );
            }

            bool Undo() override
            {
                bool any = false;
                for ( auto it = m_Commands.rbegin(); it != m_Commands.rend(); ++it )
                    any |= ( *it )->Undo();
                return any;
            }

            bool Redo() override
            {
                bool any = false;
                for ( auto& cmd : m_Commands )
                    any |= cmd->Redo();
                return any;
            }

        private:
            std::vector<std::unique_ptr<ICommand>> m_Commands;
            std::string                            m_Label;
        };

        // Keeps only the "top-level" entries: entities whose ancestor is ALSO in the list are dropped
        // (group delete/duplicate/reparent must not process a subtree twice).
        std::vector<Common::UUID> FilterTopLevel( const std::vector<Common::UUID>& uuids )
        {
            auto contains = [&]( const Common::UUID& id )
            {
                for ( const auto& u : uuids )
                    if ( u == id )
                        return true;
                return false;
            };

            std::vector<Common::UUID> result;
            for ( const auto& id : uuids )
            {
                auto e = FindEntity( id );
                if ( !e )
                    continue;

                bool covered = false;
                for ( Common::UUID p = ParentUUIDOf( *e ); !p.IsNull(); )
                {
                    if ( contains( p ) )
                    {
                        covered = true;
                        break;
                    }
                    auto pe = FindEntity( p );
                    p = pe ? ParentUUIDOf( *pe ) : Common::UUID::Null();
                }
                if ( !covered )
                    result.push_back( id );
            }
            return result;
        }

        // Performs the reparent and returns the undo command (nullptr when it was a no-op / refused).
        std::unique_ptr<ICommand> DoReparent( const Common::UUID& child, const Common::UUID& newParent )
        {
            auto childEntity = FindEntity( child );
            if ( !childEntity )
                return nullptr;

            const Common::UUID oldParent = ParentUUIDOf( *childEntity );
            if ( oldParent == newParent )
                return nullptr;

            if ( newParent.IsNull() )
            {
                s_Scene->Detach( *childEntity );
            }
            else
            {
                auto parentEntity = FindEntity( newParent );
                if ( !parentEntity )
                    return nullptr;
                s_Scene->Attach( *parentEntity, *childEntity );
                // Attach refuses cycles (dropping a parent onto its own child) — record nothing then.
                if ( ParentUUIDOf( *childEntity ) != newParent )
                    return nullptr;
            }
            return std::make_unique<ReparentCommand>( child, oldParent, newParent );
        }

        // Two serialized states of one entity subtree; undo/redo swap between them by delete + recreate
        // with preserved UUIDs (component add/remove has no cheaper reversible form — the registry has no
        // per-component "remove by key" path, and delete+recreate reuses the proven snapshot machinery).
        class EntityStateCommand final : public ICommand
        {
        public:
            EntityStateCommand( SubtreeSnapshot&& before, SubtreeSnapshot&& after )
                 : m_Before( std::move( before ) ), m_After( std::move( after ) )
            {
            }

            std::string GetLabel() const override
            {
                return "Component / entity edit";
            }

            bool Undo() override
            {
                return Apply( m_Before );
            }
            bool Redo() override
            {
                return Apply( m_After );
            }

        private:
            bool Apply( const SubtreeSnapshot& state )
            {
                if ( state.Records.empty() )
                    return false;
                const Common::UUID root = state.Records.front().id.value_or( Common::UUID::Null() );
                if ( root.IsNull() )
                    return false;
                DestroyByUUID( root );
                const bool ok = static_cast<bool>( RestoreSnapshot( state, /*preserveIds=*/true ) );
                OnStructuralChange();
                return ok;
            }

            SubtreeSnapshot m_Before, m_After;
        };

        // Records ALREADY-created entities. The snapshot is taken lazily on the first Undo — so it also
        // captures any edits made between creation and the undo (redo brings the entity back exactly as
        // it was when it disappeared, which is what the user expects).
        class CreateCommand final : public ICommand
        {
        public:
            explicit CreateCommand( std::vector<Common::UUID> roots ) : m_Roots( std::move( roots ) )
            {
            }

            std::string GetLabel() const override
            {
                return m_Roots.size() > 1 ? std::format( "Create ({})", m_Roots.size() ) : "Create";
            }

            bool Undo() override
            {
                bool any = false;
                m_Snapshots.clear();
                for ( const auto& root : m_Roots )
                {
                    if ( auto e = FindEntity( root ) )
                    {
                        m_Snapshots.push_back( CaptureSubtree( *e ) );
                        DestroyByUUID( root );
                        any = true;
                    }
                }
                if ( any )
                    OnStructuralChange();
                return any;
            }

            bool Redo() override
            {
                bool any = false;
                for ( const auto& snapshot : m_Snapshots )
                {
                    if ( ECS::Entity root = RestoreSnapshot( snapshot, /*preserveIds=*/true ) )
                    {
                        Core::SelectionManager::SetSelected( UUIDOf( root ) );
                        any = true;
                    }
                }
                if ( any )
                    OnStructuralChange();
                return any;
            }

        private:
            std::vector<Common::UUID>                    m_Roots;
            std::vector<SubtreeSnapshot>                 m_Snapshots;
        };

        // The file written and loaded, and the handle the component will hold; the component itself is not
        // touched, so a caller can decide whether the swap is its own undo step.
        struct WrittenStaticMesh
        {
            std::filesystem::path Path;
            Assets::AssetHandle   Handle;
        };

        Common::ResultStr<WrittenStaticMesh> WriteEntityAsStaticMesh( ECS::Entity& e, std::string_view folder,
                                                                      std::string_view name )
        {
            const std::string& tag = e.GetComponent<ECS::TagComponent>().Tag;
            if ( !e.HasComponent<ECS::StaticMeshComponent>() )
                return Common::MakeFormattedError<WrittenStaticMesh>( "'{}' has no mesh component", tag );
            const auto& smc = e.GetComponent<ECS::StaticMeshComponent>();
            if ( !smc.GetEditableMesh() )
                return Common::MakeFormattedError<WrittenStaticMesh>(
                     "'{}' has no editable mesh to convert; it already draws {}", tag,
                     smc.GetMeshHandle().IsNull() ? std::string( "a primitive or nothing" )
                                             : std::string( "a mesh asset" ) );

            auto target = Editor::StaticMeshOutputFolder( folder );
            if ( !target.IsSuccess() )
                return Common::MakeError<WrittenStaticMesh>( target.GetError() );

            // The file names each slot's material by its header GUID (MeshBinary v3); the slot holds
            // HandleForGuid of it, which cannot be inverted, so the cooked registry row answers.
            std::vector<Common::Content::AssetGuid> slots;
            slots.reserve( smc.GetMaterialSlots().size() );
            for ( std::size_t k = 0; k < smc.GetMaterialSlots().size(); ++k )
            {
                const uint64_t material = static_cast<uint64_t>( smc.GetMaterialSlots()[k] );
                if ( material == 0 )
                {
                    slots.push_back( {} );
                    continue;
                }
                const auto guid = Assets::ContentRegistry::GuidForHandle( material );
                if ( !guid )
                    return Common::MakeFormattedError<WrittenStaticMesh>(
                         "'{}' slot {} holds material handle {}, which no cooked registry row states a GUID for; "
                         "the mesh file cannot name it",
                         tag, k, material );
                slots.push_back( *guid );
            }
            auto written = Editor::WriteStaticMeshAsset( *smc.GetEditableMesh(), slots, target.GetValue(),
                                                         name.empty() ? std::string_view( tag ) : name );
            if ( !written.IsSuccess() )
                return Common::MakeError<WrittenStaticMesh>( written.GetError() );
            const std::filesystem::path path = written.ExtractValue();

            // Registered and BUILT now (MeshDnD's reason): an entity must never be handed a handle that
            // draws air, and the refusal names the file.
            auto created = s_AssetManager->CreateAsset<Assets::StaticMeshAsset>( path.generic_string() );
            if ( !created )
                return Common::MakeFormattedError<WrittenStaticMesh>(
                     "'{}' was written and could not be loaded back; the parse error is logged above",
                     path.generic_string() );
            if ( const auto readiness = Runtime::EnsureMeshDrawable( created, *s_AssetManager );
                 readiness != Runtime::MeshReadiness::Drawable )
                return Common::MakeError<WrittenStaticMesh>(
                     Runtime::ExplainMeshReadiness( readiness, path.generic_string() ) );
            return Common::MakeSuccess( WrittenStaticMesh{ path, created->GetMetadata().Handle } );
        }

        void PointAtStaticMesh( ECS::Entity& e, const Assets::AssetHandle& handle )
        {
            ECS::StaticMeshDesc desc = e.GetComponent<ECS::StaticMeshComponent>().GetDesc();
            desc.MeshHandle          = handle;
            desc.Primitive.reset();
            ECS::ClearEditableMesh( desc );
            ECS::SetStaticMesh( *e.GetRegistry(), e.GetHandle(), desc );
        }
    } // namespace

    // ---------------------------------------------------------------- public helpers

    void SetContext( ::Desert::Core::Scene* scene, ::Desert::Assets::AssetManager* assetManager )
    {
        s_Scene        = scene;
        s_AssetManager = assetManager;
        // The scene's entity ledger (WP17): the history stamps the entities each record names.
        CommandHistory::Get().BindPackages(
             scene != nullptr ? std::weak_ptr<::Desert::Core::EntityPackages>( scene->Packages() )
                              : std::weak_ptr<::Desert::Core::EntityPackages>() );
    }

    void NotifyCreated( const std::vector<Common::UUID>& roots )
    {
        if ( !Ready() )
            return;

        std::vector<Common::UUID> alive;
        for ( const auto& uuid : roots )
            if ( FindEntity( uuid ) )
                alive.push_back( uuid );
        if ( alive.empty() )
            return;

        CommandHistory::Get().PushCommand( std::make_unique<CreateCommand>( std::move( alive ) ) );
        OnStructuralChange();
    }

    void DeleteEntity( const Common::UUID& uuid )
    {
        if ( !Ready() )
            return;
        auto e = FindEntity( uuid );
        if ( !e )
            return;

        auto snapshot = CaptureSubtree( *e );
        DestroyByUUID( uuid );
        CommandHistory::Get().PushCommand( std::make_unique<DeleteCommand>( std::move( snapshot ) ) );
        OnStructuralChange();
    }

    void DeleteEntities( const std::vector<Common::UUID>& uuids )
    {
        if ( !Ready() )
            return;

        auto composite = std::make_unique<CompositeCommand>();
        for ( const auto& uuid : FilterTopLevel( uuids ) )
        {
            auto e = FindEntity( uuid );
            if ( !e )
                continue;
            auto snapshot = CaptureSubtree( *e );
            DestroyByUUID( uuid );
            composite->Add( std::make_unique<DeleteCommand>( std::move( snapshot ) ) );
        }
        if ( composite->Empty() )
            return;

        if ( composite->Size() == 1 )
            CommandHistory::Get().PushCommand( composite->TakeSingle() );
        else
            CommandHistory::Get().PushCommand( std::move( composite ) );
        OnStructuralChange();
    }

    Common::UUID DuplicateEntity( const Common::UUID& uuid )
    {
        if ( !Ready() )
            return Common::UUID::Null();
        auto source = FindEntity( uuid );
        if ( !source )
            return Common::UUID::Null();

        ECS::Entity duplicate = RestoreSnapshot( CaptureSubtree( *source ), /*preserveIds=*/false );
        if ( !duplicate )
            return Common::UUID::Null();

        if ( duplicate.HasComponent<ECS::TagComponent>() )
        {
            auto& tag = duplicate.GetComponent<ECS::TagComponent>().Tag;
            if ( tag.find( " Copy" ) == std::string::npos )
                tag = std::format( "{} Copy", tag );
        }

        const Common::UUID duplicateId = UUIDOf( duplicate );
        CommandHistory::Get().PushCommand( std::make_unique<CreateCommand>(
             std::vector<Common::UUID>{ duplicateId } ) );
        OnStructuralChange();
        return duplicateId;
    }

    std::vector<Common::UUID> DuplicateEntities( const std::vector<Common::UUID>& uuids )
    {
        std::vector<Common::UUID> duplicates;
        if ( !Ready() )
            return duplicates;

        for ( const auto& uuid : FilterTopLevel( uuids ) )
        {
            auto source = FindEntity( uuid );
            if ( !source )
                continue;
            ECS::Entity duplicate = RestoreSnapshot( CaptureSubtree( *source ), /*preserveIds=*/false );
            if ( !duplicate )
                continue;
            if ( duplicate.HasComponent<ECS::TagComponent>() )
            {
                auto& tag = duplicate.GetComponent<ECS::TagComponent>().Tag;
                if ( tag.find( " Copy" ) == std::string::npos )
                    tag = std::format( "{} Copy", tag );
            }
            duplicates.push_back( UUIDOf( duplicate ) );
        }
        if ( duplicates.empty() )
            return duplicates;

        CommandHistory::Get().PushCommand( std::make_unique<CreateCommand>( duplicates ) );
        OnStructuralChange();
        return duplicates;
    }

    void Reparent( const Common::UUID& child, const Common::UUID& newParent )
    {
        if ( !Ready() )
            return;
        if ( auto cmd = DoReparent( child, newParent ) )
            CommandHistory::Get().PushCommand( std::move( cmd ) );
    }

    void ReparentMany( const std::vector<Common::UUID>& children, const Common::UUID& newParent )
    {
        if ( !Ready() )
            return;

        auto composite = std::make_unique<CompositeCommand>();
        for ( const auto& child : FilterTopLevel( children ) )
        {
            if ( child == newParent )
                continue;
            if ( auto cmd = DoReparent( child, newParent ) )
                composite->Add( std::move( cmd ) );
        }
        if ( composite->Empty() )
            return;
        if ( composite->Size() == 1 )
            CommandHistory::Get().PushCommand( composite->TakeSingle() );
        else
            CommandHistory::Get().PushCommand( std::move( composite ) );
    }

    void Rename( const Common::UUID& uuid, const std::string& newName )
    {
        if ( !Ready() || newName.empty() )
            return;
        auto e = FindEntity( uuid );
        if ( !e || !e->HasComponent<ECS::TagComponent>() )
            return;

        auto& tag = e->GetComponent<ECS::TagComponent>().Tag;
        if ( tag == newName )
            return;

        CommandHistory::Get().PushEntityEdit( std::make_unique<RenameCommand>( uuid, tag, newName ), { uuid } );
        tag = newName;
    }

    void SetLocked( const std::vector<Common::UUID>& roots, bool locked )
    {
        if ( !Ready() )
            return;

        // Every entity of every subtree, with its lock state before: the record names them all, so a
        // partitioned save rewrites exactly their files (the lock is a field of each one's record).
        std::vector<std::pair<Common::UUID, bool>> before;
        std::vector<Common::UUID>                  touched;
        for ( const auto& root : FilterTopLevel( roots ) )
        {
            auto e = FindEntity( root );
            if ( !e )
                continue;
            auto&                     registry = *e->GetRegistry();
            std::vector<entt::entity> pending{ e->GetHandle() };
            while ( !pending.empty() )
            {
                const entt::entity entity = pending.back();
                pending.pop_back();
                if ( const auto* uuid = registry.try_get<ECS::UUIDComponent>( entity ) )
                {
                    before.emplace_back( uuid->UUID, ECS::IsLocked( registry, entity ) );
                    touched.push_back( uuid->UUID );
                }
                if ( const auto* rel = registry.try_get<ECS::RelationshipComponent>( entity ) )
                    for ( const auto child : rel->Children )
                        if ( registry.valid( child ) )
                            pending.push_back( child );
            }
        }
        if ( before.empty() )
            return;

        auto command = std::make_unique<LockCommand>( std::move( before ), locked );
        command->Redo();
        CommandHistory::Get().PushEntityEdit( std::move( command ), touched );
    }

    void RecordMeshChange( const Common::UUID& uuid, const std::string& label,
                           std::shared_ptr<const Geometry::DynamicMesh3> before,
                           std::unique_ptr<ICommand>                     alongside )
    {
        if ( !Ready() )
            return;
        auto e = FindEntity( uuid );
        if ( !e || !e->HasComponent<ECS::StaticMeshComponent>() )
            return;
        std::shared_ptr<const Geometry::DynamicMesh3> after =
             e->GetComponent<ECS::StaticMeshComponent>().GetEditableMesh();
        if ( after == before )
            return;
        CommandHistory::Get().PushEntityEdit(
             std::make_unique<MeshReplacementCommand>( uuid, label, std::move( before ), std::move( after ),
                                                       std::move( alongside ) ),
             { uuid } );
    }

    Common::ResultStr<Common::UUID> RecordMeshSplit( const Common::UUID& uuid, const std::string& label,
                                                     std::shared_ptr<const Geometry::DynamicMesh3> before,
                                                     std::shared_ptr<const Geometry::DynamicMesh3> otherHalf,
                                                     std::unique_ptr<ICommand>                     alongside )
    {
        if ( !Ready() )
            return Common::MakeFormattedError<Common::UUID>( "{}: the scene commands are not bound to a scene",
                                                             label );
        auto source = FindEntity( uuid );
        if ( !source || !source->HasComponent<ECS::StaticMeshComponent>() )
            return Common::MakeFormattedError<Common::UUID>( "{}: entity {} has no mesh to split", label,
                                                             static_cast<uint64_t>( uuid ) );
        // The entity alone: its children stay with the original (a child's record names its parent, so a
        // root-only snapshot re-parents nothing).
        SubtreeSnapshot record = CaptureSubtree( *source );
        record.KeepRootOnly();
        const ECS::Entity copy = RestoreSnapshot( record, /*preserveIds=*/false );
        if ( !copy || !copy.HasComponent<ECS::StaticMeshComponent>() )
            return Common::MakeFormattedError<Common::UUID>( "{}: the copy of entity {} could not be made", label,
                                                             static_cast<uint64_t>( uuid ) );
        const Common::UUID copyId = UUIDOf( copy );
        if ( copy.HasComponent<ECS::TagComponent>() )
        {
            std::string& tag = copy.GetComponent<ECS::TagComponent>().Tag;
            tag              = std::format( "{} Half", tag );
        }
        if ( auto set = ECS::SetEditableMesh( *copy.GetRegistry(), copy.GetHandle(), otherHalf );
             !set.IsSuccess() )
        {
            DestroyByUUID( copyId );
            OnStructuralChange();
            return Common::MakeFormattedError<Common::UUID>( "{}: the other half could not be put on the copy: {}",
                                                             label, set.GetError() );
        }
        RecordMeshChange(
             uuid, label, std::move( before ),
             std::make_unique<SplitCopyCommand>( copyId, std::move( otherHalf ), std::move( alongside ) ) );
        OnStructuralChange();
        return Common::MakeSuccess( copyId );
    }

    Common::ResultStr<std::vector<Common::UUID>> ApplyXformEdit( const std::string&                   label,
                                                                 const std::vector<XformEntityState>& changes,
                                                                 const std::vector<XformNewEntity>&   creates,
                                                                 const std::vector<Common::UUID>&     deletes )
    {
        using Out = std::vector<Common::UUID>;
        if ( !Ready() )
            return Common::MakeFormattedError<Out>( "{}: the scene commands are not bound to a scene", label );
        std::vector<XformCommand::Changed> changed;
        std::vector<XformCommand::Made>    made;
        // Put back what was already done, newest first, so a refusal leaves the scene as it was.
        auto rollback = [&]( const std::string& why )
        {
            for ( const auto& m : made )
                DestroyByUUID( m.Id );
            for ( auto it = changed.rbegin(); it != changed.rend(); ++it )
                if ( auto r = ApplyXformState( it->Before, it->Before.Entity ); !r.IsSuccess() )
                    LOG_ERROR( "[SceneCommands] {0}: rolling back: {1}", label, r.GetError() );
            OnStructuralChange();
            return Common::MakeFormattedError<Out>( "{}: {}", label, why );
        };
        // The copies are made BEFORE the changes: a copy is of the entity as it stands, not as this edit leaves
        // it - a Boolean's "Hide inputs" hides the operand it copies, and a copy made after would inherit that.
        for ( const XformNewEntity& create : creates )
        {
            auto source = FindEntity( create.CopyOf );
            if ( !source )
                return rollback(
                     fmt::format( "entity {} to copy is gone", static_cast<uint64_t>( create.CopyOf ) ) );
            SubtreeSnapshot record = CaptureSubtree( *source );
            record.KeepRootOnly();
            ECS::Entity copy = RestoreSnapshot( record, /*preserveIds=*/false );
            if ( !copy )
                return rollback( fmt::format( "the copy of entity {} could not be made",
                                              static_cast<uint64_t>( create.CopyOf ) ) );
            const Common::UUID id = UUIDOf( copy );
            if ( copy.HasComponent<ECS::TagComponent>() )
                copy.GetComponent<ECS::TagComponent>().Tag = create.Name;
            made.push_back( { id, create.State, {} } );
            made.back().State.Entity = id;
            if ( auto r = ApplyXformState( made.back().State, id ); !r.IsSuccess() )
                return rollback( r.GetError() );
        }
        for ( const XformEntityState& after : changes )
        {
            auto before =
                 CaptureXformState( after.Entity, after.MaterialSlots.has_value(), after.Visible.has_value() );
            if ( !before )
                return rollback( fmt::format( "entity {} has no mesh or no transform",
                                              static_cast<uint64_t>( after.Entity ) ) );
            if ( auto r = ApplyXformState( after, after.Entity ); !r.IsSuccess() )
                return rollback( r.GetError() );
            changed.push_back( { std::move( *before ), after } );
        }
        std::vector<XformCommand::Gone> gone;
        for ( const Common::UUID& id : deletes )
        {
            auto e = FindEntity( id );
            if ( !e || !e->HasComponent<ECS::StaticMeshComponent>() )
                return rollback( fmt::format( "entity {} to delete has no mesh", static_cast<uint64_t>( id ) ) );
            gone.push_back( { id, e->GetComponent<ECS::StaticMeshComponent>().GetEditableMesh(), {} } );
        }
        // Destroyed last: nothing can refuse after this point.
        for ( XformCommand::Gone& g : gone )
        {
            g.Snapshot = CaptureSubtree( *FindEntity( g.Id ) );
            DestroyByUUID( g.Id );
        }
        Out created;
        for ( const auto& m : made )
            created.push_back( m.Id );
        CommandHistory::Get().PushCommand(
             std::make_unique<XformCommand>( label, std::move( changed ), std::move( made ), std::move( gone ) ) );
        OnStructuralChange();
        return Common::MakeSuccess( std::move( created ) );
    }

    void RecordTransformEdit( const Common::UUID& uuid, const glm::vec3& oldTranslation,
                              const glm::vec3& oldRotation, const glm::vec3& oldScale )
    {
        if ( !Ready() )
            return;
        auto e = FindEntity( uuid );
        if ( !e || !e->HasComponent<ECS::TransformComponent>() )
            return;

        const auto& tc      = e->GetComponent<ECS::TransformComponent>();
        const float epsilon = 1e-6f;
        if ( glm::all( glm::epsilonEqual( tc.GetTranslation(), oldTranslation, epsilon ) ) &&
             glm::all( glm::epsilonEqual( tc.GetRotation(), oldRotation, epsilon ) ) &&
             glm::all( glm::epsilonEqual( tc.GetScale(), oldScale, epsilon ) ) )
            return; // click without an actual drag

        CommandHistory::Get().PushEntityEdit(
             std::make_unique<TransformCommand>( uuid, oldTranslation, oldRotation, oldScale, tc.GetTranslation(),
                                                 tc.GetRotation(), tc.GetScale() ),
             { uuid } );
    }

    bool RecordTransformEdits( const std::vector<TransformSnapshot>& before )
    {
        if ( !Ready() )
            return false;

        const float epsilon   = 1e-6f;
        auto        composite = std::make_unique<CompositeCommand>();
        for ( const auto& snap : before )
        {
            auto e = FindEntity( snap.Entity );
            if ( !e || !e->HasComponent<ECS::TransformComponent>() )
                continue;
            const auto& tc = e->GetComponent<ECS::TransformComponent>();
            if ( glm::all( glm::epsilonEqual( tc.GetTranslation(), snap.Translation, epsilon ) ) &&
                 glm::all( glm::epsilonEqual( tc.GetRotation(), snap.Rotation, epsilon ) ) &&
                 glm::all( glm::epsilonEqual( tc.GetScale(), snap.Scale, epsilon ) ) )
                continue;
            composite->Add( std::make_unique<TransformCommand>( snap.Entity, snap.Translation,
                                                                snap.Rotation, snap.Scale, tc.GetTranslation(),
                                                                tc.GetRotation(), tc.GetScale() ) );
        }
        if ( composite->Empty() )
            return false;
        if ( composite->Size() == 1 )
            CommandHistory::Get().PushCommand( composite->TakeSingle() );
        else
            CommandHistory::Get().PushCommand( std::move( composite ) );
        return true;
    }

    void CopySelectionToClipboard( const std::vector<Common::UUID>& uuids )
    {
        if ( !Ready() )
            return;

        std::vector<SubtreeSnapshot> snapshots;
        for ( const auto& uuid : FilterTopLevel( uuids ) )
            if ( auto e = FindEntity( uuid ) )
                snapshots.push_back( CaptureSubtree( *e ) );

        if ( !snapshots.empty() )
            s_Clipboard = std::move( snapshots );
    }

    bool ClipboardHasContent()
    {
        return !s_Clipboard.empty();
    }

    std::vector<Common::UUID> PasteClipboard()
    {
        std::vector<Common::UUID> pasted;
        if ( !Ready() || s_Clipboard.empty() )
            return pasted;

        for ( const auto& snapshot : s_Clipboard )
            if ( ECS::Entity root = RestoreSnapshot( snapshot, /*preserveIds=*/false ) )
                pasted.push_back( UUIDOf( root ) );

        if ( pasted.empty() )
            return pasted;

        CommandHistory::Get().PushCommand( std::make_unique<CreateCommand>( pasted ) );
        OnStructuralChange();
        return pasted;
    }

    bool ApplyPrefabInstance( const Common::UUID& uuid )
    {
        if ( !Ready() )
            return false;
        auto root = FindEntity( uuid );
        if ( !root || !root->HasComponent<ECS::PrefabComponent>() )
            return false;

        auto asset = s_AssetManager->FindByHandle<Assets::PrefabAsset>(
             root->GetComponent<ECS::PrefabComponent>().Prefab );
        if ( !asset )
        {
            LOG_ERROR( "[Prefab] Apply failed: source asset not found for '{}'",
                       root->GetComponent<ECS::TagComponent>().Tag );
            return false;
        }

        asset->CreateFromEntity( *root, *s_AssetManager );
        // ofstream silently writes NOTHING when the directory is missing — make sure it exists.
        std::error_code ec;
        std::filesystem::create_directories( asset->GetMetadata().Filepath.parent_path(), ec );

        // The `bool` this function returns used to answer only the LOOKUPS above; the write itself was
        // unreportable, so "Applied" was logged and `true` returned for a .deprefab that had not been
        // touched. Every other instance of that prefab would then be re-instantiated from the OLD file
        // while the editor said the changes had been applied.
        if ( const auto written = asset->SaveTo( asset->GetMetadata().Filepath ); !written )
        {
            LOG_ERROR( "[Prefab] Apply failed: {} was not written: {}", asset->GetMetadata().Filepath.string(),
                       written.GetError() );
            return false;
        }

        LOG_INFO( "[Prefab] Applied instance changes -> {}", asset->GetMetadata().Filepath.string() );
        return true;
    }

    Common::UUID RevertPrefabInstance( const Common::UUID& uuid )
    {
        if ( !Ready() )
            return Common::UUID::Null();
        auto root = FindEntity( uuid );
        if ( !root || !root->HasComponent<ECS::PrefabComponent>() )
            return Common::UUID::Null();

        auto asset = s_AssetManager->FindByHandle<Assets::PrefabAsset>(
             root->GetComponent<ECS::PrefabComponent>().Prefab );
        if ( !asset )
        {
            LOG_ERROR( "[Prefab] Revert failed: source asset not found for '{}'",
                       root->GetComponent<ECS::TagComponent>().Tag );
            return Common::UUID::Null();
        }
        if ( !asset->IsReadyForUse() && !asset->Load() )
        {
            LOG_ERROR( "[Prefab] Revert failed: could not load {}", asset->GetMetadata().Filepath.string() );
            return Common::UUID::Null();
        }

        // Keep the instance's place in the world: same translation, same parent.
        glm::vec3 translation( 0.0f );
        if ( root->HasComponent<ECS::TransformComponent>() )
            translation = root->GetComponent<ECS::TransformComponent>().GetTranslation();
        const Common::UUID parentId = ParentUUIDOf( *root );

        auto composite = std::make_unique<CompositeCommand>();

        // 1) Delete the modified instance (undo restores it).
        auto snapshot = CaptureSubtree( *root );
        DestroyByUUID( uuid );
        composite->Add( std::make_unique<DeleteCommand>( std::move( snapshot ) ) );

        // 2) Fresh instantiation from the source file.
        // The parent goes IN, and is no longer attached afterwards: the placement rule has to see where
        // the instance will hang, or a UI element reverted under its canvas would be judged as if it were
        // going to the scene root and refused.
        ECS::Entity parentEntity;
        if ( !parentId.IsNull() )
        {
            if ( auto parent = FindEntity( parentId ) )
            {
                parentEntity = *parent;
            }
        }

        const auto placed = asset->Instantiate( s_Scene, *s_AssetManager, parentEntity, &translation );
        if ( !placed )
        {
            // Roll the delete back and report failure — better a live (modified) instance than nothing.
            composite->Undo();
            LOG_ERROR( "[Prefab] Revert failed: {}", placed.GetError() );
            return Common::UUID::Null();
        }
        const ECS::Entity fresh = placed.GetValue();

        const Common::UUID freshId = UUIDOf( fresh );
        composite->Add( std::make_unique<CreateCommand>( std::vector<Common::UUID>{ freshId } ) );

        CommandHistory::Get().PushCommand( std::move( composite ) );
        OnStructuralChange();
        Core::SelectionManager::SetSelected( freshId );
        return freshId;
    }

    Common::ResultStr<std::vector<Common::UUID>> ReplaceWithPrefab( const std::vector<Common::UUID>& uuids,
                                                                    const std::filesystem::path&     prefabFile,
                                                                    ReplaceMeshSource                mesh )
    {
        using Result = std::vector<Common::UUID>;
        if ( !Ready() )
            return Common::MakeError<Result>( "Replace with Prefab: there is no open scene" );
        const std::vector<Common::UUID> roots = FilterTopLevel( uuids );
        if ( roots.empty() )
            return Common::MakeError<Result>( "Replace with Prefab: nothing is selected" );
        if ( prefabFile.extension() != Common::Constants::Extensions::PREFAB_EXTENSION )
            return Common::MakeFormattedError<Result>( "Replace with Prefab: '{}' is not a {} file",
                                                       prefabFile.generic_string(),
                                                       Common::Constants::Extensions::PREFAB_EXTENSION );

        auto prefab = s_AssetManager->FindByPath<Assets::PrefabAsset>( prefabFile );
        if ( !prefab )
            prefab = s_AssetManager->CreateAsset<Assets::PrefabAsset>( prefabFile.generic_string() );
        if ( !prefab )
            return Common::MakeFormattedError<Result>( "Replace with Prefab: '{}' could not be loaded",
                                                       prefabFile.generic_string() );
        if ( !prefab->IsReadyForUse() && !prefab->Load() )
            return Common::MakeFormattedError<Result>( "Replace with Prefab: '{}' could not be parsed",
                                                       prefabFile.generic_string() );

        // EVERY REFUSAL IS ASKED BEFORE THE FIRST ENTITY GOES, so a refused replace leaves the scene as it was
        // rather than half-replaced.
        if ( mesh == ReplaceMeshSource::KeepEntityMesh )
        {
            const auto& records = prefab->GetEntities();
            if ( records.empty() || !records.front().Components.get( "StaticMesh" ).has_value() )
                return Common::MakeFormattedError<Result>(
                     "Replace with Prefab: '{}' has no static mesh on its root, so there is nothing to keep each "
                     "entity's mesh on; replace without keeping meshes, or give the prefab's root a mesh",
                     prefabFile.generic_string() );
        }
        for ( const Common::UUID& uuid : roots )
            if ( !FindEntity( uuid ) )
                return Common::MakeFormattedError<Result>( "Replace with Prefab: entity {} is not in the scene",
                                                           static_cast<uint64_t>( uuid ) );

        auto   composite = std::make_unique<CompositeCommand>( "Replace with Prefab" );
        Result created;
        created.reserve( roots.size() );
        for ( const Common::UUID& uuid : roots )
        {
            ECS::Entity old = *FindEntity( uuid );

            // What the instance takes over: place, name and (on request) geometry. Copied OUT before the
            // destroy — the component storage goes with the entity.
            const Common::UUID parentId = ParentUUIDOf( old );
            glm::vec3          translation( 0.0f ), rotation( 0.0f ), scale( 1.0f );
            if ( old.HasComponent<ECS::TransformComponent>() )
            {
                const auto& tc = old.GetComponent<ECS::TransformComponent>();
                translation    = tc.GetTranslation();
                rotation       = tc.GetRotation();
                scale          = tc.GetScale();
            }
            const std::string tag =
                 old.HasComponent<ECS::TagComponent>() ? old.GetComponent<ECS::TagComponent>().Tag : std::string();
            std::optional<ECS::StaticMeshDesc> keptMesh;
            if ( mesh == ReplaceMeshSource::KeepEntityMesh && old.HasComponent<ECS::StaticMeshComponent>() )
                keptMesh = old.GetComponent<ECS::StaticMeshComponent>().GetDesc();

            auto snapshot = CaptureSubtree( old );
            DestroyByUUID( uuid );
            composite->Add( std::make_unique<DeleteCommand>( std::move( snapshot ) ) );

            ECS::Entity parentEntity;
            if ( !parentId.IsNull() )
                if ( auto parent = FindEntity( parentId ) )
                    parentEntity = *parent;

            const auto placed = prefab->Instantiate( s_Scene, *s_AssetManager, parentEntity, &translation );
            if ( !placed )
            {
                // Everything done so far is put back: a replace is all of the selection or none of it.
                composite->Undo();
                OnStructuralChange();
                return Common::MakeFormattedError<Result>( "Replace with Prefab: {}", placed.GetError() );
            }
            ECS::Entity fresh = placed.GetValue();
            if ( fresh.HasComponent<ECS::TransformComponent>() )
            {
                fresh.SetLocalTransform( ECS::LocalTransform{ translation, rotation, scale } );
            }
            if ( !tag.empty() && fresh.HasComponent<ECS::TagComponent>() )
                fresh.GetComponent<ECS::TagComponent>().Tag = tag;
            if ( keptMesh && fresh.HasComponent<ECS::StaticMeshComponent>() )
            {
                ECS::StaticMeshDesc desc = fresh.GetComponent<ECS::StaticMeshComponent>().GetDesc();
                desc.MeshHandle          = keptMesh->MeshHandle;
                desc.Primitive           = keptMesh->Primitive;
                if ( keptMesh->EditableMesh )
                {
                    if ( auto set = ECS::SetEditableMesh( desc, keptMesh->EditableMesh ); !set.IsSuccess() )
                    {
                        composite->Add(
                             std::make_unique<CreateCommand>( std::vector<Common::UUID>{ UUIDOf( fresh ) } ) );
                        composite->Undo();
                        OnStructuralChange();
                        return Common::MakeFormattedError<Result>( "Replace with Prefab: '{}': {}", tag,
                                                                   set.GetError() );
                    }
                }
                else
                {
                    ECS::ClearEditableMesh( desc );
                }
                // The writer drops the runtime instances on a slot change, so the mesh system rebuilds them.
                desc.MaterialSlots = keptMesh->MaterialSlots;
                ECS::SetStaticMesh( *fresh.GetRegistry(), fresh.GetHandle(), desc );
            }

            const Common::UUID freshId = UUIDOf( fresh );
            composite->Add( std::make_unique<CreateCommand>( std::vector<Common::UUID>{ freshId } ) );
            created.push_back( freshId );
        }

        CommandHistory::Get().PushCommand( std::move( composite ) );
        OnStructuralChange();
        Core::SelectionManager::SetSelection( created );
        LOG_INFO( "[Prefab] replaced {} entit{} with '{}'", created.size(), created.size() == 1 ? "y" : "ies",
                  prefabFile.generic_string() );
        return Common::MakeSuccess( std::move( created ) );
    }

    namespace
    {
        // The fold's identity, read off a live entity. One reader, two callers — the selector below and
        // the fold itself — so "what matches" and "what folds" are one answer.
        std::optional<FoldMeshIdentity> StaticMeshIdentityOf( ECS::Entity entity )
        {
            if ( !entity.HasComponent<ECS::StaticMeshComponent>() )
                return std::nullopt;
            const auto&      mesh = entity.GetComponent<ECS::StaticMeshComponent>();
            FoldMeshIdentity identity;
            identity.Mesh        = mesh.GetMeshHandle();
            identity.Primitive   = mesh.GetPrimitive();
            identity.Materials   = mesh.GetMaterialSlots();
            identity.CastShadows = mesh.GetCastShadows();
            identity.CastContactShadow = mesh.GetCastContactShadow();
            return identity;
        }
    } // namespace

    size_t SelectMatchingStaticMeshes( const Common::UUID& like )
    {
        if ( !Ready() )
            return 0;
        auto seed = FindEntity( like );
        if ( !seed )
            return 0;
        const auto wanted = StaticMeshIdentityOf( *seed );
        if ( !wanted )
            return 0;

        std::vector<Common::UUID> matching;
        for ( const auto& entity : s_Scene->GetAllEntities() )
        {
            const ECS::Entity candidate = entity;
            const auto        identity  = StaticMeshIdentityOf( candidate );
            if ( !identity || !identity->SameAs( *wanted ) )
                continue;
            const Common::UUID uuid = UUIDOf( candidate );
            if ( !uuid.IsNull() )
                matching.push_back( uuid );
        }
        if ( matching.empty() )
            return 0;

        Core::SelectionManager::SetSelection( matching );
        return matching.size();
    }

    Common::ResultStr<Common::UUID> CollapseIntoInstancedMesh( const std::vector<Common::UUID>& uuids )
    {
        if ( !Ready() )
            return Common::MakeError<Common::UUID>( "No scene is open." );

        // WHAT THE PLANNER IS NOT ALLOWED TO GUESS. Everything below gathers FACTS about the selection
        // and hands them over; the decision (and every refusal message) lives in the std-only planner,
        // which is what lets a suite ask it without a device.
        std::vector<FoldCandidate> candidates;
        size_t                     withoutAMesh = 0;
        for ( const Common::UUID& uuid : uuids )
        {
            const auto found = FindEntity( uuid );
            if ( !found )
                continue;

            // Unwrapped ONCE, right after the check. Threading the optional through the thirty lines
            // below is what makes `bugprone-unchecked-optional-access` fire four times on a function
            // that does test it — and the check is right to be nervous: "it was checked further up" is
            // exactly the argument that stops being true when somebody inserts a line.
            const ECS::Entity entity   = *found;
            const auto        identity = StaticMeshIdentityOf( entity );
            if ( !identity )
            {
                ++withoutAMesh;
                continue;
            }

            const auto& mesh = entity.GetComponent<ECS::StaticMeshComponent>();

            FoldCandidate candidate;
            candidate.Entity   = uuid;
            candidate.Name     = entity.HasComponent<ECS::TagComponent>()
                                      ? entity.GetComponent<ECS::TagComponent>().Tag
                                      : std::string( "Entity" );
            candidate.World    = entity.GetWorldTransform();
            candidate.Identity = *identity;

            // The static component's own fields that an ISM has no room for. Each one is a knob somebody
            // deliberately moved, so losing it silently is losing an authoring decision.
            if ( mesh.GetOutlineDraw() )
                candidate.Blockers.emplace_back( "a persistent outline" );
            if ( mesh.GetForcedLOD() >= 0 )
                candidate.Blockers.emplace_back( "a forced LOD" );
            if ( mesh.GetLODBias() != 0 )
                candidate.Blockers.emplace_back( "a LOD bias" );
            if ( !mesh.GetReceiveShadows() )
                candidate.Blockers.emplace_back( "receive-shadows off" );
            if ( mesh.GetTranslucencySortPriority() != 0 )
                candidate.Blockers.emplace_back( "a translucency sort priority" );
            if ( mesh.GetHiddenSubmeshes() != 0 )
                candidate.Blockers.emplace_back( "hidden submeshes" );
            if ( mesh.GetMaxDrawDistance() > 0.0f )
                candidate.Blockers.emplace_back( "a max draw distance" );
            if ( mesh.GetEditableMesh() )
                candidate.Blockers.emplace_back( "a mesh built in the editor (an instanced mesh names an asset or "
                                                 "a primitive)" );

            if ( entity.HasComponent<ECS::RelationshipComponent>() &&
                 !entity.GetComponent<ECS::RelationshipComponent>().Children.empty() )
                candidate.Blockers.emplace_back( "children" );

            // AND EVERY OTHER COMPONENT, ASKED OF THE SERIALIZATION REGISTRY RATHER THAN LISTED HERE.
            // A hand-written list of "things a prop must not carry" is a list that is wrong the day
            // somebody adds a component — and wrong in the direction that destroys data. The registry is
            // the census of everything an entity can carry across a save, so it is the honest source.
            // `Visibility` is the one exception, and only while it says VISIBLE: dropping a component
            // that changes nothing is not a loss, while dropping a HIDDEN flag would put a prop back on
            // screen.
            for ( const auto& serializer : ::Desert::Core::Serialize::ComponentRegistry::Get().All() )
            {
                if ( serializer.Key == "StaticMesh" || !serializer.Has || !serializer.Has( entity ) )
                    continue;
                if ( serializer.Key == "Visibility" &&
                     ( !entity.HasComponent<ECS::VisibilityComponent>() ||
                       entity.GetComponent<ECS::VisibilityComponent>().IsVisible() ) )
                    continue;
                candidate.Blockers.push_back( "a " + serializer.Key + " component" );
            }

            candidates.push_back( std::move( candidate ) );
        }

        auto planned = PlanInstanceFold( candidates );
        if ( !planned.IsSuccess() )
        {
            // The count of selected entities that carry no mesh at all is knowledge the planner does not
            // have and the person needs: "two of the seven you picked are lights" is a different problem
            // from "these two cubes are different cubes".
            std::string message = planned.GetError();
            if ( withoutAMesh > 0 )
                message = std::format( "{} ({} of the {} selected carry no static mesh)", message, withoutAMesh,
                                       uuids.size() );
            return Common::MakeError<Common::UUID>( message );
        }
        const FoldPlan& plan = planned.GetValue();

        // The name comes from the first source, because that is the prop a person recognises in the
        // Outliner; the count is appended so the row says what happened without being opened.
        std::string name = "Instanced Mesh";
        if ( auto first = FindEntity( plan.Sources.front() ); first && first->HasComponent<ECS::TagComponent>() )
            name = first->GetComponent<ECS::TagComponent>().Tag;
        name = std::format( "{} x{}", name, plan.InstanceTransforms.size() );

        auto composite = std::make_unique<CompositeCommand>();
        for ( const Common::UUID& source : plan.Sources )
        {
            auto entity = FindEntity( source );
            if ( !entity )
                continue;
            auto snapshot = CaptureSubtree( *entity );
            DestroyByUUID( source );
            composite->Add( std::make_unique<DeleteCommand>( std::move( snapshot ) ) );
        }

        const Common::UUID folded  = Common::UUID::Generate();
        const ECS::Entity& created = s_Scene->CreateEntityWithUUID( folded, name );
        ECS::InstancedStaticMeshDesc ismDesc;
        ismDesc.MeshHandle         = plan.Identity.Mesh;
        ismDesc.Primitive          = plan.Identity.Primitive;
        ismDesc.MaterialSlots      = plan.Identity.Materials;
        ismDesc.CastShadows        = plan.Identity.CastShadows;
        ismDesc.CastContactShadow  = plan.Identity.CastContactShadow;
        ismDesc.InstanceTransforms = plan.InstanceTransforms;
        ECS::SetInstancedStaticMesh( *created.GetRegistry(), created.GetHandle(), ismDesc );
        const auto& ism = created.GetComponent<ECS::InstancedStaticMeshComponent>();
        composite->Add( std::make_unique<CreateCommand>( std::vector<Common::UUID>{ folded } ) );

        CommandHistory::Get().PushCommand( std::move( composite ) );
        OnStructuralChange();
        Core::SelectionManager::SetSelected( folded );

        LOG_INFO( "[Collapse] {0} entities -> 1 instanced mesh with {1} instances.", plan.Sources.size(),
                  ism.GetInstanceTransforms().size() );
        return Common::MakeSuccess( Common::UUID( folded ) );
    }

    Common::ResultStr<Common::UUID> ReplaceWithNewEntity( const Common::UUID& source, const std::string& name,
                                                          const std::function<void( ECS::Entity& )>& build )
    {
        if ( !Ready() )
            return Common::MakeError<Common::UUID>( "No scene is open." );
        auto found = FindEntity( source );
        if ( !found )
            return Common::MakeFormattedError<Common::UUID>( "entity {} is not in the scene",
                                                             static_cast<uint64_t>( source ) );

        auto composite = std::make_unique<CompositeCommand>();
        auto snapshot  = CaptureSubtree( *found );
        DestroyByUUID( source );
        composite->Add( std::make_unique<DeleteCommand>( std::move( snapshot ) ) );

        const Common::UUID created = Common::UUID::Generate();
        ECS::Entity        entity  = s_Scene->CreateEntityWithUUID( created, name );
        build( entity );
        composite->Add( std::make_unique<CreateCommand>( std::vector<Common::UUID>{ created } ) );

        CommandHistory::Get().PushCommand( std::move( composite ) );
        OnStructuralChange();
        Core::SelectionManager::SetSelected( created );
        return Common::MakeSuccess( Common::UUID( created ) );
    }

    struct EntityStateSnapshot
    {
        Common::UUID                    Root;
        SubtreeSnapshot                 State;
    };

    std::shared_ptr<const EntityStateSnapshot> CaptureEntityState( const Common::UUID& uuid )
    {
        if ( !Ready() )
            return nullptr;
        auto e = FindEntity( uuid );
        if ( !e )
            return nullptr;
        return std::make_shared<const EntityStateSnapshot>( EntityStateSnapshot{ uuid, CaptureSubtree( *e ) } );
    }

    void CommitEntityState( const std::shared_ptr<const EntityStateSnapshot>& before )
    {
        if ( !Ready() || !before )
            return;
        auto e = FindEntity( before->Root );
        if ( !e )
            return;
        auto state = before->State;
        CommandHistory::Get().PushCommand(
             std::make_unique<EntityStateCommand>( std::move( state ), CaptureSubtree( *e ) ) );
    }

    void RevertEntityState( const std::shared_ptr<const EntityStateSnapshot>& before )
    {
        if ( !Ready() || !before || before->State.Records.empty() )
            return;
        DestroyByUUID( before->Root );
        (void)RestoreSnapshot( before->State, /*preserveIds=*/true );
        OnStructuralChange();
    }

    void MutateEntityUndoable( const Common::UUID& uuid, const std::function<void()>& mutate )
    {
        if ( !Ready() || !mutate )
            return;
        auto e = FindEntity( uuid );
        if ( !e )
        {
            if ( mutate )
                mutate(); // still perform the action; just not undoable without a live entity
            return;
        }

        auto before = CaptureSubtree( *e );
        mutate();
        auto after = CaptureSubtree( *e );

        CommandHistory::Get().PushCommand(
             std::make_unique<EntityStateCommand>( std::move( before ), std::move( after ) ) );
        OnStructuralChange(); // the mutation itself may have relocated component pools
    }

    Common::ResultStr<std::filesystem::path> OutputStaticMesh( const Common::UUID& uuid, std::string_view folder,
                                                               std::string_view name )
    {
        if ( !Ready() )
            return Common::MakeError<std::filesystem::path>( "no scene or asset manager is bound to the editor" );
        auto e = FindEntity( uuid );
        if ( !e )
            return Common::MakeFormattedError<std::filesystem::path>( "no entity {}", uuid.ToString() );
        auto written = WriteEntityAsStaticMesh( *e, folder, name );
        if ( !written.IsSuccess() )
            return Common::MakeError<std::filesystem::path>( written.GetError() );
        PointAtStaticMesh( *e, written.GetValue().Handle );
        return Common::MakeSuccess( written.GetValue().Path );
    }

    Common::ResultStr<std::filesystem::path> ConvertToStaticMesh( const Common::UUID& uuid,
                                                                  std::string_view folder, std::string_view name )
    {
        if ( !Ready() )
            return Common::MakeError<std::filesystem::path>( "no scene or asset manager is bound to the editor" );
        auto e = FindEntity( uuid );
        if ( !e )
            return Common::MakeFormattedError<std::filesystem::path>( "no entity {}", uuid.ToString() );
        // The file first, outside the undo step: a refusal leaves no history entry and no half-converted entity.
        auto written = WriteEntityAsStaticMesh( *e, folder, name );
        if ( !written.IsSuccess() )
            return Common::MakeError<std::filesystem::path>( written.GetError() );
        const Assets::AssetHandle handle = written.GetValue().Handle;
        MutateEntityUndoable( uuid,
                              [&]()
                              {
                                  if ( auto live = FindEntity( uuid ) )
                                      PointAtStaticMesh( *live, handle );
                              } );
        return Common::MakeSuccess( written.GetValue().Path );
    }
} // namespace Desert::Editor::Commands
