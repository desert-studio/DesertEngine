#include "UVTool.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/ModelingToolTarget.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/ToastManager.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/DynamicMeshUVOperations.hpp>

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
                case MS::Tool::AutoUV:
                    return "AutoUV";
                case MS::Tool::UVProject:
                    return "UV Project";
                case MS::Tool::UVUnwrap:
                    return "UV Unwrap";
                case MS::Tool::UVLayout:
                    return "UV Layout";
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

    bool UVTool::IsUVTool( MS::Tool tool )
    {
        return tool == MS::Tool::AutoUV || tool == MS::Tool::UVProject || tool == MS::Tool::UVUnwrap ||
               tool == MS::Tool::UVLayout;
    }

    void UVTool::Update( ::Desert::Core::Scene& scene )
    {
        MS&        ms       = MS::Get();
        const bool uvActive = IsUVTool( ms.ActiveTool );

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
        if ( !uvActive )
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

    void UVTool::Begin( ::Desert::Core::Scene& scene, MS& ms )
    {
        m_Session.reset();
        const auto selected = Core::SelectionManager::GetSelected();
        if ( !selected.has_value() )
        {
            ms.UVStatus = "Select an entity with a static mesh.";
            return;
        }
        ECS::StaticMeshComponent* component = MeshOf( scene, *selected );
        if ( component == nullptr )
        {
            ms.UVStatus = "The selected entity has no static mesh.";
            return;
        }
        auto target = GetToolTargetMesh( *component );
        if ( !target.IsSuccess() )
        {
            ms.UVStatus = target.GetError();
            return;
        }
        m_Session =
             Session{ ms.ActiveTool, *selected, target.GetValue().Mesh, target.GetValue().Committed, nullptr };
        Recompute( scene, ms );
    }

    void UVTool::Recompute( ::Desert::Core::Scene& scene, MS& ms )
    {
        RememberSettings( ms );
        const Geometry::DynamicMesh3&          input = *m_Session->Input;
        Common::ResultStr<Geometry::UVOutcome> out   = Common::MakeError<Geometry::UVOutcome>( "no UV tool" );
        switch ( m_Session->Tool )
        {
            case MS::Tool::AutoUV:
                out = Geometry::AutoUV( input, ms.UVAuto );
                break;
            case MS::Tool::UVProject:
                out = Geometry::ProjectUVs( input, ms.UVProject );
                break;
            case MS::Tool::UVUnwrap:
                out = Geometry::UnwrapUVs( input, ms.UVUnwrap );
                break;
            case MS::Tool::UVLayout:
                out = Geometry::LayoutUVs( input, ms.UVLayout );
                break;
            default:
                break;
        }
        ECS::StaticMeshComponent* component = MeshOf( scene, m_Session->Entity );
        if ( !out.IsSuccess() )
        {
            // A refused setting shows the input, not a stale preview.
            ms.UVStatus = out.GetError();
            Restore( scene );
            m_Session->Preview.reset();
            return;
        }
        auto preview = std::make_shared<const Geometry::DynamicMesh3>( std::move( out.ExtractValue().Mesh ) );
        if ( auto set = ECS::SetEditableMesh( *component, preview ); !set.IsSuccess() )
        {
            ms.UVStatus = set.GetError();
            Restore( scene );
            m_Session->Preview.reset();
            return;
        }
        m_Session->Preview = std::move( preview );
        ms.UVStatus        = out.GetValue().Report;
    }

    void UVTool::Restore( ::Desert::Core::Scene& scene )
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

    bool UVTool::Accept( ::Desert::Core::Scene& scene, MS& ms )
    {
        const auto refuse = [&]( const std::string& why )
        {
            LOG_ERROR( "[Modeling] {0} Accept refused: {1}", ToolName( ms.ActiveTool ), why );
            ToastManager::Push( std::format( "{} Accept refused: {}", ToolName( ms.ActiveTool ), why ),
                                ToastLevel::Error, 10.0f );
            return false;
        };
        if ( !m_Session )
            return refuse( ms.UVStatus.empty() ? std::string( "no mesh is selected" ) : ms.UVStatus );
        if ( !m_Session->Preview )
            return refuse( ms.UVStatus );
        ECS::StaticMeshComponent* component = MeshOf( scene, m_Session->Entity );
        if ( component == nullptr || component->EditableMesh != m_Session->Preview )
            return refuse( "the entity no longer draws the preview" );
        Commands::RecordMeshChange( m_Session->Entity, std::format( "Modeling: {}", ToolName( m_Session->Tool ) ),
                                    m_Session->Committed );
        LOG_INFO( "[Modeling] {0} accepted: {1}", ToolName( m_Session->Tool ), ms.UVStatus );
        m_Session.reset();
        return true;
    }

    bool UVTool::SettingsChanged( const MS& ms ) const
    {
        switch ( m_Session->Tool )
        {
            case MS::Tool::AutoUV:
                return !( ms.UVAuto == m_AutoUV );
            case MS::Tool::UVProject:
                return !( ms.UVProject == m_Project );
            case MS::Tool::UVUnwrap:
                return !( ms.UVUnwrap == m_Unwrap );
            case MS::Tool::UVLayout:
                return !( ms.UVLayout == m_Layout );
            default:
                return false;
        }
    }

    void UVTool::RememberSettings( const MS& ms )
    {
        m_AutoUV  = ms.UVAuto;
        m_Project = ms.UVProject;
        m_Unwrap  = ms.UVUnwrap;
        m_Layout  = ms.UVLayout;
    }
} // namespace Desert::Editor::Tools
