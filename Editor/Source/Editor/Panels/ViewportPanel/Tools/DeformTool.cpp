#include "DeformTool.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/ModelingToolTarget.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/ToastManager.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/MeshDeformOperations.hpp>

#include <format>
#include <string>

namespace Desert::Editor::Tools
{
    namespace
    {
        using MS = Core::ModelingState;

        const char* ToolName( MS::Tool tool )
        {
            switch ( tool )
            {
                case MS::Tool::Smooth:
                    return "Smooth";
                case MS::Tool::Offset:
                    return "Offset";
                default:
                    return "?";
            }
        }

        ECS::StaticMeshComponent* MeshOf( ::Desert::Core::Scene& scene, const Common::UUID& id )
        {
            auto ref = scene.FindEntityByID( id );
            if ( !ref || !ref->get().HasComponent<ECS::StaticMeshComponent>() )
                return nullptr;
            return &ref->get().GetComponent<ECS::StaticMeshComponent>();
        }
    } // namespace

    bool DeformTool::IsDeformTool( MS::Tool tool )
    {
        return tool == MS::Tool::Smooth || tool == MS::Tool::Offset;
    }

    void DeformTool::Update( ::Desert::Core::Scene& scene )
    {
        MS&        ms     = MS::Get();
        const bool active = IsDeformTool( ms.ActiveTool );

        // The session ends without Accept when its tool is no longer the active one (the bar's Cancel ends the
        // tool and raises ReqCancel, which was this tool's) or the selection moved to another entity.
        if ( m_Session )
        {
            const auto selected = Core::SelectionManager::GetSelected();
            if ( ms.ActiveTool != m_Session->Tool || !selected.has_value() || *selected != m_Session->Entity ||
                 MeshOf( scene, m_Session->Entity ) == nullptr )
            {
                if ( ms.ActiveTool != m_Session->Tool && ms.ReqCancel )
                    ms.ReqCancel = false;
                Restore( scene );
                m_Session.reset();
            }
        }
        if ( !active )
            return;

        if ( !m_Session )
            Begin( scene, ms );
        if ( m_Session && SettingsChanged( ms ) )
            Recompute( scene, ms );

        if ( ms.ReqAccept )
        {
            ms.ReqAccept         = false;
            const bool endTool   = ms.ReqAcceptEndsTool;
            ms.ReqAcceptEndsTool = false;
            if ( !Accept( scene, ms ) )
                return; // refused: the tool stays open with its preview, and says why
            if ( endTool )
                ms.ActiveTool = MS::Tool::None;
            else
                Begin( scene, ms ); // "Accept and Start New": the committed mesh is the next input
        }
    }

    void DeformTool::Begin( ::Desert::Core::Scene& scene, MS& ms )
    {
        m_Session.reset();
        const auto selected = Core::SelectionManager::GetSelected();
        if ( !selected.has_value() )
        {
            ms.DeformStatus = "Select an entity with a static mesh.";
            return;
        }
        ECS::StaticMeshComponent* component = MeshOf( scene, *selected );
        if ( component == nullptr )
        {
            ms.DeformStatus = "The selected entity has no static mesh.";
            return;
        }
        auto target = GetToolTargetMesh( *component );
        if ( !target.IsSuccess() )
        {
            ms.DeformStatus = target.GetError();
            return;
        }
        m_Session =
             Session{ ms.ActiveTool, *selected, target.GetValue().Mesh, target.GetValue().Committed, nullptr };
        Recompute( scene, ms );
    }

    void DeformTool::Recompute( ::Desert::Core::Scene& scene, MS& ms )
    {
        m_Smooth                                             = ms.DeformSmooth;
        m_Offset                                             = ms.DeformOffset;
        const Geometry::DynamicMesh3&              input     = *m_Session->Input;
        Common::ResultStr<Geometry::DeformOutcome> out       = m_Session->Tool == MS::Tool::Smooth
                                                                    ? Geometry::Smooth( input, ms.DeformSmooth )
                                                                    : Geometry::Offset( input, ms.DeformOffset );
        ECS::StaticMeshComponent*                  component = MeshOf( scene, m_Session->Entity );
        if ( !out.IsSuccess() )
        {
            // A refused setting shows the input, not a stale preview.
            ms.DeformStatus = out.GetError();
            Restore( scene );
            m_Session->Preview.reset();
            return;
        }
        std::string report = out.GetValue().Report;
        auto preview = std::make_shared<const Geometry::DynamicMesh3>( std::move( out.ExtractValue().Mesh ) );
        if ( auto set = ECS::SetEditableMesh( *component, preview ); !set.IsSuccess() )
        {
            ms.DeformStatus = set.GetError();
            Restore( scene );
            m_Session->Preview.reset();
            return;
        }
        m_Session->Preview = std::move( preview );
        ms.DeformStatus    = std::move( report );
    }

    void DeformTool::Restore( ::Desert::Core::Scene& scene )
    {
        ECS::StaticMeshComponent* component = MeshOf( scene, m_Session->Entity );
        if ( component == nullptr )
            return;
        switch ( PlanMeshRestore( component->EditableMesh, m_Session->Committed ) )
        {
            case MeshRestore::Unchanged:
                break;
            case MeshRestore::Clear:
                ECS::ClearEditableMesh( *component );
                break;
            case MeshRestore::Set:
                if ( auto set = ECS::SetEditableMesh( *component, m_Session->Committed ); !set.IsSuccess() )
                    LOG_ERROR( "[Modeling] {0}: the mesh could not be put back: {1}", ToolName( m_Session->Tool ),
                               set.GetError() );
                break;
        }
    }

    bool DeformTool::Accept( ::Desert::Core::Scene& scene, MS& ms )
    {
        const auto refuse = [&]( const std::string& why )
        {
            LOG_ERROR( "[Modeling] {0} Accept refused: {1}", ToolName( ms.ActiveTool ), why );
            ToastManager::Push( std::format( "{} Accept refused: {}", ToolName( ms.ActiveTool ), why ),
                                ToastLevel::Error, 10.0f );
            return false;
        };
        if ( !m_Session )
            return refuse( ms.DeformStatus.empty() ? std::string( "no mesh is selected" ) : ms.DeformStatus );
        if ( !m_Session->Preview )
            return refuse( ms.DeformStatus );
        ECS::StaticMeshComponent* component = MeshOf( scene, m_Session->Entity );
        if ( component == nullptr || component->EditableMesh != m_Session->Preview )
            return refuse( "the entity no longer draws the preview" );
        Commands::RecordMeshChange( m_Session->Entity, std::format( "Modeling: {}", ToolName( m_Session->Tool ) ),
                                    m_Session->Committed );
        LOG_INFO( "[Modeling] {0} accepted: {1}", ToolName( m_Session->Tool ), ms.DeformStatus );
        m_Session.reset();
        return true;
    }

    bool DeformTool::SettingsChanged( const MS& ms ) const
    {
        switch ( m_Session->Tool )
        {
            case MS::Tool::Smooth:
                return !( ms.DeformSmooth == m_Smooth );
            case MS::Tool::Offset:
                return !( ms.DeformOffset == m_Offset );
            default:
                return false;
        }
    }
} // namespace Desert::Editor::Tools
