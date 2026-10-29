#include "EditMaterialsTool.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/MeshElementSelection.hpp>
#include <Editor/Core/Selection/ModelingToolTarget.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/ToastManager.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/DynamicMeshSelection.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>
#include <filesystem>
#include <format>

namespace Desert::Editor::Tools
{
    namespace
    {
        using MS = Core::ModelingState;

        ECS::StaticMeshComponent* MeshOf( ::Desert::Core::Scene& scene, const Common::UUID& id )
        {
            auto ref = scene.FindEntityByID( id );
            if ( !ref || !ref->get().HasComponent<ECS::StaticMeshComponent>() )
                return nullptr;
            return &ref->get().GetComponent<ECS::StaticMeshComponent>();
        }

        // The runtime instances follow the slots; dropping them makes the mesh system rebuild them.
        void PutSlots( ECS::StaticMeshComponent& component, const std::vector<Common::AssetHandle>& slots )
        {
            if ( component.GetMaterialSlots() == slots )
                return;
            component.MaterialSlots = slots;
            component.RuntimeMaterialInstances().clear();
            component.RuntimeSlots().reset();
            component.SeenMaterialsVersion() = 0;
        }

        std::string SlotLabel( const Common::AssetHandle& handle )
        {
            return handle == Common::UUID::Null() ? std::string( "None (default material)" )
                                                  : std::format( "asset {}", static_cast<uint64_t>( handle ) );
        }

        bool Refuse( MS& ms, const std::string& what, const std::string& why )
        {
            ms.EditMaterialsStatus = std::format( "{} refused: {}", what, why );
            LOG_ERROR( "[Modeling] Edit Materials: {0}", ms.EditMaterialsStatus );
            ToastManager::Push( "Edit Materials: " + ms.EditMaterialsStatus, ToastLevel::Error, 10.0f );
            return false;
        }
    } // namespace

    void EditMaterialsTool::Update( ::Desert::Core::Scene& scene, const ::Desert::Assets::AssetManager* assets )
    {
        MS&        ms     = MS::Get();
        const bool active = ms.ActiveTool == MS::Tool::EditMaterials;

        // As MeshProcessingTool: another tool (the bar's Cancel included) or another entity ends the session.
        if ( m_Session )
        {
            const auto selected = Core::SelectionManager::GetSelected();
            if ( !active || !selected.has_value() || *selected != m_Session->Entity ||
                 MeshOf( scene, m_Session->Entity ) == nullptr )
            {
                if ( !active && ms.ReqCancel )
                    ms.ReqCancel = false;
                Restore( scene );
                m_Session.reset();
            }
        }
        if ( !active )
            return;
        if ( !m_Session )
            Begin( scene, ms );
        if ( !m_Session )
        {
            ms.EditMaterialsReqAssign = ms.EditMaterialsReqAddSlot = ms.EditMaterialsReqRemoveSlot = false;
            ms.EditMaterialsReqSlotMaterial.clear();
            ms.EditMaterialsSlotNames.clear();
            return;
        }

        if ( ms.EditMaterialsReqAddSlot )
        {
            ms.EditMaterialsReqAddSlot = false;
            m_Session->Slots.push_back( Common::UUID::Null() );
            m_Session->SlotLabels.push_back( SlotLabel( Common::UUID::Null() ) );
            ms.EditMaterialsActiveSlot = static_cast<int>( m_Session->Slots.size() ) - 1;
            ms.EditMaterialsStatus     = std::format( "Slot {} added.", ms.EditMaterialsActiveSlot );
            Show( scene );
        }
        if ( ms.EditMaterialsReqRemoveSlot )
        {
            ms.EditMaterialsReqRemoveSlot = false;
            RemoveLastSlot( scene, ms );
        }
        if ( !ms.EditMaterialsReqSlotMaterial.empty() )
        {
            const std::string path = std::move( ms.EditMaterialsReqSlotMaterial );
            ms.EditMaterialsReqSlotMaterial.clear();
            SetSlotMaterial( scene, assets, path, ms );
        }
        if ( ms.EditMaterialsReqAssign )
        {
            ms.EditMaterialsReqAssign = false;
            AssignSelection( scene, ms );
        }
        ms.EditMaterialsSlotNames = m_Session->SlotLabels;

        if ( ms.ReqAccept )
        {
            ms.ReqAccept         = false;
            const bool endTool   = ms.ReqAcceptEndsTool;
            ms.ReqAcceptEndsTool = false;
            if ( !Accept( scene, ms ) )
                return; // refused: the preview stays and the status says why
            if ( endTool )
                ms.ActiveTool = MS::Tool::None;
            else
                Begin( scene, ms );
        }
    }

    void EditMaterialsTool::Begin( ::Desert::Core::Scene& scene, MS& ms )
    {
        m_Session.reset();
        const auto selected = Core::SelectionManager::GetSelected();
        if ( !selected.has_value() )
        {
            ms.EditMaterialsStatus = "Select an entity with a static mesh.";
            return;
        }
        ECS::StaticMeshComponent* component = MeshOf( scene, *selected );
        if ( component == nullptr )
        {
            ms.EditMaterialsStatus = "The selected entity has no static mesh.";
            return;
        }
        auto target = GetToolTargetMesh( *component );
        if ( !target.IsSuccess() )
        {
            ms.EditMaterialsStatus = target.GetError();
            return;
        }
        Session session{ *selected, target.GetValue().Committed, component->GetMaterialSlots(), target.GetValue().Mesh,
                         component->GetMaterialSlots(), {} };
        // Every material id the mesh uses names a slot (ECS::CoverMaterialIds' rule), so Assign can address it.
        int maxId = 0;
        if ( session.Preview->HasAttributes() && session.Preview->Attributes()->HasMaterialID() )
        {
            const auto* ids = session.Preview->Attributes()->GetMaterialID();
            for ( int tid = 0; tid < session.Preview->MaxTriangleID(); ++tid )
                if ( session.Preview->IsTriangle( tid ) )
                    maxId = std::max( maxId, static_cast<int>( ids->GetValue( tid ) ) );
        }
        if ( session.Slots.size() < static_cast<std::size_t>( maxId ) + 1 )
            session.Slots.resize( static_cast<std::size_t>( maxId ) + 1 );
        for ( const auto& handle : session.Slots )
            session.SlotLabels.push_back( SlotLabel( handle ) );
        m_Session                  = std::move( session );
        ms.EditMaterialsActiveSlot = std::clamp( ms.EditMaterialsActiveSlot, 0,
                                                 static_cast<int>( m_Session->Slots.size() ) - 1 );
        ms.EditMaterialsStatus     = std::format( "{} slots. Select triangles, then Assign Active Material.",
                                                  m_Session->Slots.size() );
        Show( scene );
    }

    void EditMaterialsTool::Show( ::Desert::Core::Scene& scene )
    {
        ECS::StaticMeshComponent* component = MeshOf( scene, m_Session->Entity );
        if ( component == nullptr )
            return;
        if ( component->GetEditableMesh() != m_Session->Preview )
            if ( auto set = ECS::SetEditableMesh( *component, m_Session->Preview ); !set.IsSuccess() )
                LOG_ERROR( "[Modeling] Edit Materials: the preview could not be shown: {0}", set.GetError() );
        PutSlots( *component, m_Session->Slots );
    }

    void EditMaterialsTool::Restore( ::Desert::Core::Scene& scene )
    {
        ECS::StaticMeshComponent* component = MeshOf( scene, m_Session->Entity );
        if ( component == nullptr )
            return;
        switch ( PlanMeshRestore( component->GetEditableMesh(), m_Session->Committed ) )
        {
            case MeshRestore::Unchanged:
                break;
            case MeshRestore::Clear:
                ECS::ClearEditableMesh( *component );
                break;
            case MeshRestore::Set:
                if ( auto set = ECS::SetEditableMesh( *component, m_Session->Committed ); !set.IsSuccess() )
                    LOG_ERROR( "[Modeling] Edit Materials: the mesh could not be put back: {0}", set.GetError() );
                break;
        }
        PutSlots( *component, m_Session->CommittedSlots );
    }

    // UE UEditMeshMaterialsTool::ApplyMaterialAction / AssignMaterial: the active slot into every selected
    // triangle's MaterialID (a polygroup / edge / vertex selection reaches its triangles via ConvertSelection).
    void EditMaterialsTool::AssignSelection( ::Desert::Core::Scene& scene, MS& ms )
    {
        const auto& selection = Core::MeshElementSelection::Get();
        if ( selection.Entity() != m_Session->Entity || !selection.HasMesh() || selection.Selection().Empty() )
        {
            Refuse( ms, "Assign", "no triangles of this mesh are selected" );
            return;
        }
        const int slot = ms.EditMaterialsActiveSlot;
        if ( slot < 0 || slot >= static_cast<int>( m_Session->Slots.size() ) )
        {
            Refuse( ms, "Assign", std::format( "slot {} does not exist ({} slots)", slot, m_Session->Slots.size() ) );
            return;
        }
        const Geometry::ElementSelection tris =
             Geometry::ConvertSelection( *selection.Mesh(), *selection.Topology(), selection.Selection(),
                                         Geometry::ElementMode::Triangle );
        auto next = std::make_shared<Geometry::DynamicMesh3>( *m_Session->Preview );
        if ( !next->HasAttributes() )
            next->EnableAttributes();
        if ( !next->Attributes()->HasMaterialID() )
            next->Attributes()->EnableMaterialID();
        auto* ids     = next->Attributes()->GetMaterialID();
        int   changed = 0;
        for ( const int tid : tris.Ids() )
            if ( next->IsTriangle( tid ) && ids->GetValue( tid ) != slot )
            {
                ids->SetValue( tid, slot );
                ++changed;
            }
        ms.EditMaterialsStatus = std::format( "Slot {} assigned to {} of {} selected triangles.", slot, changed,
                                              tris.Size() );
        LOG_INFO( "[Modeling] Edit Materials: {0}", ms.EditMaterialsStatus );
        if ( changed == 0 )
            return;
        m_Session->Preview = std::move( next );
        Show( scene );
    }

    void EditMaterialsTool::RemoveLastSlot( ::Desert::Core::Scene& scene, MS& ms )
    {
        if ( m_Session->Slots.size() <= 1 )
        {
            Refuse( ms, "Remove slot", "a mesh keeps at least one slot" );
            return;
        }
        const int last = static_cast<int>( m_Session->Slots.size() ) - 1;
        const auto& mesh = *m_Session->Preview;
        if ( mesh.HasAttributes() && mesh.Attributes()->HasMaterialID() )
            for ( int tid = 0; tid < mesh.MaxTriangleID(); ++tid )
                if ( mesh.IsTriangle( tid ) && mesh.Attributes()->GetMaterialID()->GetValue( tid ) == last )
                {
                    Refuse( ms, "Remove slot", std::format( "triangle {} still uses slot {}", tid, last ) );
                    return;
                }
        m_Session->Slots.pop_back();
        m_Session->SlotLabels.pop_back();
        ms.EditMaterialsActiveSlot = std::min( ms.EditMaterialsActiveSlot, last - 1 );
        ms.EditMaterialsStatus     = std::format( "Slot {} removed.", last );
        Show( scene );
    }

    // As the Details panel's slot (MaterialComponentWidget::AssignMaterialFromPath), into the working slots.
    void EditMaterialsTool::SetSlotMaterial( ::Desert::Core::Scene& scene, const ::Desert::Assets::AssetManager* assets,
                                             const std::string& path, MS& ms )
    {
        const int slot = ms.EditMaterialsActiveSlot;
        if ( assets == nullptr )
        {
            Refuse( ms, "Slot material", "the viewport has no asset manager" );
            return;
        }
        if ( slot < 0 || slot >= static_cast<int>( m_Session->Slots.size() ) )
        {
            Refuse( ms, "Slot material", std::format( "slot {} does not exist", slot ) );
            return;
        }
        auto& manager = const_cast<::Desert::Assets::AssetManager&>( *assets );
        auto  asset   = manager.FindByPath<::Desert::Assets::SurfaceMaterialAsset>( path );
        if ( !asset )
            asset = manager.CreateAsset<::Desert::Assets::SurfaceMaterialAsset>( path );
        if ( !asset )
        {
            Refuse( ms, "Slot material", std::format( "'{}' is not a material asset", path ) );
            return;
        }
        const auto handle = asset->GetMetadata().Handle;
        if ( !::Desert::Runtime::ResourceRegistry::GetMaterialService()->Get( handle ) )
            ::Desert::Runtime::ResourceRegistry::GetMaterialService()->Register( asset );
        m_Session->Slots[slot]      = handle;
        m_Session->SlotLabels[slot] = std::filesystem::path( path ).stem().string();
        ms.EditMaterialsStatus      = std::format( "Slot {} = {}.", slot, m_Session->SlotLabels[slot] );
        Show( scene );
    }

    bool EditMaterialsTool::Accept( ::Desert::Core::Scene& scene, MS& ms )
    {
        if ( !m_Session )
            return Refuse( ms, "Accept", ms.EditMaterialsStatus );
        auto ref = scene.FindEntityByID( m_Session->Entity );
        if ( !ref || !ref->get().HasComponent<ECS::TransformComponent>() )
            return Refuse( ms, "Accept", "the entity is gone" );
        // The component goes back to the "before" first, so the one recorded step holds before and after.
        Restore( scene );
        const auto&                     tc = ref->get().GetComponent<ECS::TransformComponent>();
        const Commands::XformEntityState after{ m_Session->Entity, m_Session->Preview, tc.GetTranslation(),
                                                tc.GetRotation(),       tc.GetScale(),           m_Session->Slots,
                                                std::nullopt };
        if ( auto done = Commands::ApplyXformEdit( "Modeling: Edit Materials", { after }, {}, {} ); !done.IsSuccess() )
        {
            Show( scene );
            return Refuse( ms, "Accept", done.GetError() );
        }
        LOG_INFO( "[Modeling] Edit Materials accepted: {0} triangles, {1} slots", m_Session->Preview->TriangleCount(),
                  m_Session->Slots.size() );
        m_Session.reset();
        return true;
    }
} // namespace Desert::Editor::Tools
