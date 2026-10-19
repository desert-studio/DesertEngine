#include "VFXSystemDocument.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/AssetPickerRows.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Commands/VFXSystemEdit.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/PreviewViewpoints.hpp>
#include <Editor/Core/SubjectTitle.hpp>
#include <Editor/Widgets/PreviewEnvironmentUI.hpp>
#include <Editor/Widgets/PreviewInput.hpp>
#include <Editor/Widgets/PreviewViewport.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/VFXModuleAsset.hpp>
#include <Engine/Assets/VFXSystemAsset.hpp>
#include <Engine/VFX/VFXStackCompiler.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/UUID.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>
#include <ImGui/imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <map>
#include <system_error>

namespace Desert::Editor
{
    namespace S = Assets::Serialization;

    namespace
    {
        constexpr std::array<VFXSystemDocument::Stage, 3> kStages = { VFXSystemDocument::Stage::EmitterUpdate,
                                                                      VFXSystemDocument::Stage::ParticleSpawn,
                                                                      VFXSystemDocument::Stage::ParticleUpdate };

        const char* StageLabel( const VFXSystemDocument::Stage stage )
        {
            switch ( stage )
            {
                case VFXSystemDocument::Stage::EmitterUpdate:
                    return "Emitter Update";
                case VFXSystemDocument::Stage::ParticleSpawn:
                    return "Particle Spawn";
                case VFXSystemDocument::Stage::ParticleUpdate:
                    return "Particle Update";
            }
            return "?";
        }

        const char* SourceLabel( const S::VFXInputSource source )
        {
            switch ( source )
            {
                case S::VFXInputSource::Value:
                    return "Value";
                case S::VFXInputSource::Random:
                    return "Random range";
                case S::VFXInputSource::Curve:
                    return "Curve over age";
                case S::VFXInputSource::Binding:
                    return "Binding";
            }
            return "?";
        }

        const char* RendererLabel( const S::VFXRendererKind kind )
        {
            switch ( kind )
            {
                case S::VFXRendererKind::Sprite:
                    return "Sprite Renderer";
                case S::VFXRendererKind::Ribbon:
                    return "Ribbon Renderer";
                case S::VFXRendererKind::Mesh:
                    return "Mesh Renderer";
            }
            return "?";
        }

        const char* MeshFacingLabel( const S::VFXMeshFacing facing )
        {
            switch ( facing )
            {
                case S::VFXMeshFacing::Default:
                    return "Default";
                case S::VFXMeshFacing::Velocity:
                    return "Velocity";
                case S::VFXMeshFacing::CameraPosition:
                    return "Camera Position";
                case S::VFXMeshFacing::CameraPlane:
                    return "Camera Plane";
            }
            return "?";
        }

        const char* RibbonFacingLabel( const S::VFXRibbonFacing facing )
        {
            return facing == S::VFXRibbonFacing::Screen ? "Screen" : "Custom";
        }

        // A registry row as the reference a `.dfx` states (ContentRegistry::ReferenceTo); none for a file whose
        // header states no GUID - such a row cannot be referenced and the picker shows it disabled.
        std::optional<Assets::AssetGuidRef> RefOfRow( const Assets::ContentRegistry::PickerRow& row )
        {
            if ( !row.Guid )
                return std::nullopt;
            return Assets::ContentRegistry::ReferenceTo( *row.Guid );
        }

        // The rows of @p rows as a filtered, scrolling list (StaticMeshComponent's mesh picker); the reference of
        // the clicked one, if any. @p current is the slot's GUID text (highlighted).
        std::optional<Assets::AssetGuidRef> PickAssetRow( const std::vector<Assets::ContentRegistry::PickerRow>& rows,
                                                         const std::string& current, ImGuiTextFilter& filter )
        {
            filter.Draw( "##search", 200.0f );
            ImGui::Separator();
            std::optional<Assets::AssetGuidRef> picked;
            constexpr float                     kPickerRows = 14.0f;
            ImGui::BeginChild( "##rows", ImVec2( ImGui::GetFontSize() * 20.0f,
                                                 ImGui::GetTextLineHeightWithSpacing() * kPickerRows ) );
            for ( const auto& row : rows )
            {
                const std::string name = PickerDisplayName( row );
                if ( !filter.PassFilter( name.c_str() ) )
                    continue;
                const auto ref = RefOfRow( row );
                ImGui::BeginDisabled( !ref );
                if ( ImGui::Selectable( name.c_str(), ref && ref->Guid == current ) )
                    picked = ref;
                ImGui::EndDisabled();
                if ( !ref && ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
                    ImGui::SetTooltip( "%s states no GUID in its header: a .dfx cannot reference it",
                                       row.Key.c_str() );
            }
            ImGui::EndChild();
            return picked;
        }

        const char* EventLabel( const S::VFXEventKind kind )
        {
            switch ( kind )
            {
                case S::VFXEventKind::Death:
                    return "Death";
                case S::VFXEventKind::Collision:
                    return "Collision";
                case S::VFXEventKind::Location:
                    return "Location";
            }
            return "?";
        }

        const char* LoopLabel( const S::VFXLoopBehavior loop )
        {
            switch ( loop )
            {
                case S::VFXLoopBehavior::Once:
                    return "Once";
                case S::VFXLoopBehavior::Multiple:
                    return "Multiple";
                case S::VFXLoopBehavior::Infinite:
                    return "Infinite";
            }
            return "?";
        }

        // A row shows its module's display name; a module the catalog does not hold shows its file stem, marked.
        std::string ModuleLabel( std::span<const VFX::VFXModuleInfo> catalog, const Assets::AssetGuidRef& module )
        {
            if ( const VFX::VFXModuleInfo* info = VFX::FindModule( catalog, module ) )
                return info->DisplayName;
            return std::format( "{} (unresolved)", std::filesystem::path( module.Path ).stem().string() );
        }

        VFX::VFXModuleStage ModuleStageOf( const VFXSystemDocument::Stage stage )
        {
            switch ( stage )
            {
                case VFXSystemDocument::Stage::EmitterUpdate:
                    return VFX::VFXModuleStage::EmitterUpdate;
                case VFXSystemDocument::Stage::ParticleSpawn:
                    return VFX::VFXModuleStage::ParticleSpawn;
                case VFXSystemDocument::Stage::ParticleUpdate:
                    break;
            }
            return VFX::VFXModuleStage::ParticleUpdate;
        }

        // The widget's printf format: the unit after the number (UE's DisplayUnit), fewer decimals on a wide
        // range. '%' in a unit would be a conversion, so it is doubled.
        std::string NumberFormat( const std::string& units, const std::optional<VFX::VFXInputRange>& range,
                                  const bool whole )
        {
            std::string unit;
            for ( const char c : units )
                unit += c == '%' ? std::string( "%%" ) : std::string( 1, c );
            const bool  wide   = range.has_value() && range->Max - range->Min >= 100.0f;
            const char* number = whole ? "%d" : ( wide ? "%.1f" : "%.3f" );
            return unit.empty() ? std::string( number ) : std::format( "{} {}", number, unit );
        }

        float DragSpeed( const std::optional<VFX::VFXInputRange>& range )
        {
            return range.has_value() ? ( range->Max - range->Min ) / 500.0f : 0.05f;
        }

        // A value of `type` in a vec4, edited in place. Bool is a checkbox (0 / 1); Int a whole drag.
        bool ValueWidget( const char* id, glm::vec4& value, const S::VFXValueType type, const std::string& units,
                          const std::optional<VFX::VFXInputRange>& range )
        {
            const float min = range.has_value() ? range->Min : 0.0f;
            const float max = range.has_value() ? range->Max : 0.0f;
            ImGui::SetNextItemWidth( -1.0f );
            switch ( type )
            {
                case S::VFXValueType::Bool:
                {
                    bool on = value.x != 0.0f;
                    if ( !ImGui::Checkbox( id, &on ) )
                        return false;
                    value.x = on ? 1.0f : 0.0f;
                    return true;
                }
                case S::VFXValueType::Int:
                {
                    int whole = static_cast<int>( std::lround( value.x ) );
                    if ( !ImGui::DragInt( id, &whole, std::max( DragSpeed( range ), 0.1f ),
                                          static_cast<int>( min ), static_cast<int>( max ),
                                          NumberFormat( units, range, true ).c_str() ) )
                        return false;
                    value.x = static_cast<float>( whole );
                    return true;
                }
                case S::VFXValueType::Float:
                case S::VFXValueType::Vec2:
                case S::VFXValueType::Vec3:
                case S::VFXValueType::Vec4:
                    return ImGui::DragScalarN(
                         id, ImGuiDataType_Float, &value.x, static_cast<int>( S::ComponentCount( type ) ),
                         DragSpeed( range ), range.has_value() ? &min : nullptr,
                         range.has_value() ? &max : nullptr, NumberFormat( units, range, false ).c_str(),
                         range.has_value() ? ImGuiSliderFlags_AlwaysClamp : 0 );
            }
            return false;
        }

        void PropertyLabel( const char* label, const char* units = "" )
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex( 0 );
            ImGui::AlignTextToFramePadding();
            if ( units[0] != '\0' )
                ImGui::Text( "%s (%s)", label, units );
            else
                ImGui::TextUnformatted( label );
            ImGui::TableSetColumnIndex( 1 );
        }

        bool BeginProperties( const char* id )
        {
            if ( !ImGui::BeginTable( id, 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg ) )
                return false;
            ImGui::TableSetupColumn( "Property", ImGuiTableColumnFlags_WidthFixed, 190.0f );
            ImGui::TableSetupColumn( "Value", ImGuiTableColumnFlags_WidthStretch );
            return true;
        }

        std::string FreeEmitterName( const S::VFXSystemData& system, const std::string& base )
        {
            for ( std::size_t n = 0;; ++n )
            {
                const std::string name = n == 0 ? base : std::format( "{}{}", base, n );
                if ( std::ranges::none_of( system.Emitters,
                                           [&]( const S::VFXEmitterData& e ) { return e.Name == name; } ) )
                    return name;
            }
        }

        // The value an input shows when its source changes: what it holds now, in the new source's terms.
        glm::vec4 CurrentValue( const S::VFXModuleInput& in )
        {
            if ( in.Value )
                return *in.Value;
            if ( in.Random )
                return in.Random->Min;
            glm::vec4 v( 0.0f );
            if ( in.Curve )
                for ( std::size_t c = 0; c < in.Curve->size() && c < 4; ++c )
                    if ( !( *in.Curve )[c].empty() )
                        v[static_cast<int>( c )] = ( *in.Curve )[c].front().Value;
            return v;
        }

        Common::BoolResultStr SetSource( S::VFXModuleInput& in, const S::VFXInputSource source,
                                         const S::VFXSystemData& system )
        {
            const glm::vec4   value = CurrentValue( in );
            const auto        n     = S::ComponentCount( in.Type );
            S::VFXModuleInput next;
            next.Name   = in.Name;
            next.Type   = in.Type;
            next.Source = source;
            switch ( source )
            {
                case S::VFXInputSource::Value:
                    next.Value = value;
                    break;
                case S::VFXInputSource::Random:
                    next.Random = S::VFXRandomRange{ value, value };
                    break;
                case S::VFXInputSource::Curve:
                {
                    std::vector<std::vector<S::VFXCurveKey>> channels( n );
                    for ( uint32_t c = 0; c < n; ++c )
                    {
                        S::VFXCurveKey k0;
                        k0.Value          = value[static_cast<int>( c )];
                        S::VFXCurveKey k1 = k0;
                        k1.Time           = 1.0f;
                        channels[c]       = { k0, k1 };
                    }
                    next.Curve = std::move( channels );
                    break;
                }
                case S::VFXInputSource::Binding:
                {
                    const auto user = std::ranges::find_if( system.UserParams, [&]( const S::VFXUserParam& p )
                                                            { return p.Type == in.Type; } );
                    if ( user == system.UserParams.end() )
                        return Common::MakeFormattedError<bool>(
                             "'{}' can only bind to a User parameter of its type, and the system has none",
                             in.Name );
                    next.Binding = std::string( S::kVFXUserPrefix ) + user->Name;
                    break;
                }
            }
            in = std::move( next );
            return BOOLSUCCESS;
        }
    } // namespace

    std::vector<S::VFXModuleUse>& StageModules( S::VFXEmitterData& emitter, const VFXSystemDocument::Stage stage )
    {
        switch ( stage )
        {
            case VFXSystemDocument::Stage::EmitterUpdate:
                return emitter.Stack.EmitterUpdate;
            case VFXSystemDocument::Stage::ParticleSpawn:
                return emitter.Stack.ParticleSpawn;
            case VFXSystemDocument::Stage::ParticleUpdate:
                return emitter.Stack.ParticleUpdate;
        }
        return emitter.Stack.ParticleUpdate;
    }

    VFXSystemDocument::VFXSystemDocument( const Assets::AssetHandle& subject, Assets::AssetManager* assets )
         : ISubjectDocument( AssetSubjectTitle( subject, assets, "VFX System" ),
                             AssetSubject( subject, static_cast<uint32_t>( Assets::AssetTypeID::VFXSystem ) ) ),
           m_Assets( assets )
    {
        const auto asset =
             m_Assets != nullptr ? m_Assets->FindByHandle<Assets::VFXSystemAsset>( subject ) : nullptr;
        if ( !asset )
        {
            m_LoadError = "the VFX system asset is not held by the asset manager";
            return;
        }
        if ( !asset->IsReadyForUse() )
        {
            if ( auto loaded = asset->LoadFromFile(); !loaded )
            {
                m_LoadError = loaded.GetError();
                return;
            }
        }
        m_Working    = asset->GetData();
        m_Saved      = m_Working;
        m_FrameStart = m_Working;
        m_Loaded     = true;

        // The library the stack picks from and the templates "Add emitter" offers; either missing is shown, and
        // the stack is still editable without them.
        if ( auto modules = Assets::ReadVFXModuleCatalog(); modules )
            m_Modules = modules.ExtractValue();
        else
            m_LibraryError = modules.GetError();
        if ( auto templates = ReadVFXEmitterTemplates( VFXEmitterTemplateDir() ); templates )
            m_Templates = templates.ExtractValue();
        else
            m_LibraryError = templates.GetError();
        if ( auto categories = S::ReadVFXCategories( Common::Constants::Path::CurrentProjectRoot().ProjectDir /
                                                     "Config" / S::kVFXCategoriesFileName );
             categories )
            m_Categories = categories.ExtractValue();
    }

    VFXSystemDocument::~VFXSystemDocument()
    {
        // The history's entries point at m_Working, which dies with this window.
        CommandHistory::Get().DropFor( &m_Working );
        // The preview (which names it) goes first, then the transient system.
        m_Preview.reset();
        if ( static_cast<uint64_t>( m_WorkingHandle ) != 0 )
            if ( auto* systems = Runtime::ResourceRegistry::GetVFXSystemService() )
                systems->ReleaseWorkingCopy( m_WorkingHandle );
    }

    bool VFXSystemDocument::IsSubjectAlive() const
    {
        return m_Assets != nullptr &&
               m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }

    ISubjectDocument::DiskState VFXSystemDocument::GetDiskState() const
    {
        if ( !m_Loaded )
            return DiskState::Untracked;
        return m_Working != m_Saved ? DiskState::Dirty : DiskState::Clean;
    }

    bool VFXSystemDocument::SaveDocument()
    {
        if ( !m_Loaded || m_Assets == nullptr )
            return false;
        const Assets::AssetHandle handle( Subject().Owner );
        const auto*               meta = m_Assets->FindMetadataByHandle( handle );
        if ( meta == nullptr )
        {
            m_Refusal = "Save: the VFX system asset is no longer held by the asset manager";
            return false;
        }
        if ( auto valid = S::ValidateVFXSystemData( m_Working ); !valid )
        {
            m_Refusal = std::format( "Save: {}", valid.GetError() );
            return false;
        }
        if ( auto saved = Assets::VFXSystemAsset::Save( meta->Filepath, m_Working ); !saved )
        {
            m_Refusal = std::format( "Save: {}", saved.GetError() );
            LOG_ERROR( "[VFX] '{}' was not saved: {}", meta->Filepath.string(), saved.GetError() );
            return false;
        }
        m_Saved = m_Working;
        m_Refusal.clear();
        // The held asset re-reads what was written, so the next placement and the next open see the new system.
        if ( const auto asset = m_Assets->FindByHandle<Assets::VFXSystemAsset>( handle ) )
        {
            if ( auto reloaded = asset->LoadFromFile(); !reloaded )
                LOG_ERROR( "[VFX] '{}' was saved but would not be read back: {}", meta->Filepath.string(),
                           reloaded.GetError() );
        }
        return true;
    }

    void VFXSystemDocument::Report( const Common::BoolResultStr& result )
    {
        if ( result.IsSuccess() )
        {
            m_Refusal.clear();
            return;
        }
        m_Refusal = result.GetError();
        LOG_WARN( "[VFX] {}: refused - {}", GetName(), m_Refusal );
    }

    void VFXSystemDocument::EnsurePreview()
    {
        if ( m_Preview || !m_Loaded )
            return;
        m_Preview  = std::make_unique<PreviewViewport>();
        m_UIHelper = std::make_unique<UI::UIHelper>();
        m_UIHelper->Init();
        m_PreviewLive = true;
        // Particles move: the pane renders every frame (UE's viewport is realtime while an effect plays).
        m_Preview->SetRealtime( true );
        m_Previewed.reset(); // the new pane is handed the working copy below
        if ( m_PendingOrbitDegrees )
        {
            m_Preview->SetOrbit( glm::radians( m_PendingOrbitDegrees->x ),
                                 glm::radians( m_PendingOrbitDegrees->y ) );
            m_PendingOrbitDegrees.reset();
        }
    }

    void VFXSystemDocument::ReleaseView()
    {
        m_Preview.reset();
        m_UIHelper.reset();
        m_Previewed.reset();
        m_PreviewLive = false;
    }

    void VFXSystemDocument::SetPreviewViewpoint( const PreviewViewpoint& viewpoint )
    {
        if ( !m_Preview )
        {
            m_PendingOrbitDegrees = glm::vec2( viewpoint.YawDegrees, viewpoint.PitchDegrees );
            return;
        }
        m_Preview->SetOrbit( glm::radians( viewpoint.YawDegrees ), glm::radians( viewpoint.PitchDegrees ) );
    }

    void VFXSystemDocument::OnPreUpdate()
    {
        // THE SLOT IS NOT CLAIMED UNTIL THE WINDOW HAS BEEN DRAWN (StaticMeshViewerDocument::OnPreUpdate).
        if ( !m_DrewThisFrame )
            return;
        m_DrewThisFrame = false;

        EnsurePreview();
        if ( !m_Preview || m_RenderSize.x == 0u || m_RenderSize.y == 0u )
            return;

        // THE EDIT REACHES THE PREVIEW: a working copy that differs from the one playing, and validates (a drag
        // in progress may pass through values the commit would refuse), is handed over as a new system.
        if ( !m_Previewed || *m_Previewed != m_Working )
        {
            auto* systems = Runtime::ResourceRegistry::GetVFXSystemService();
            if ( systems != nullptr && S::ValidateVFXSystemData( m_Working ) )
            {
                if ( static_cast<uint64_t>( m_WorkingHandle ) == 0 )
                    m_WorkingHandle = Assets::AssetHandle( Common::UUID::Generate() );
                auto next = std::make_shared<const S::VFXSystemData>( m_Working );
                if ( auto published = systems->PublishWorkingCopy( m_WorkingHandle, next ); published )
                {
                    m_Previewed = std::move( next );
                    m_Preview->SetVFXSystem( m_WorkingHandle );
                }
                else
                    Report( published );
            }
        }

        PreviewEnvironment::ApplyTo( *m_Preview, m_Assets );
        m_Preview->Update( m_RenderSize.x, m_RenderSize.y );
    }

    std::vector<ISubjectDocument::DocumentAction> VFXSystemDocument::Actions()
    {
        std::vector<DocumentAction> actions;
        for ( std::size_t i = 0; i < m_Templates.size(); ++i )
            actions.push_back( { std::format( "Add emitter: {}", m_Templates[i].Name ),
                                 [this, i]() { AddEmitterFromTemplate( i ); } } );
        actions.push_back( { "Restart preview", [this]()
                             {
                                 if ( m_Preview )
                                     m_Preview->RestartVFX();
                             } } );
        PreviewEnvironment::AppendActions( actions );
        return actions;
    }

    void VFXSystemDocument::ExtendToolbar( AssetEditorToolbar& toolbar )
    {
        toolbar.AddButton( ICON_MDI_RESTART, "Restart", "Every emitter of the preview starts over",
                           [this]()
                           {
                               if ( m_Preview )
                                   m_Preview->RestartVFX();
                           } );
    }

    void VFXSystemDocument::AddEmitterFromTemplate( const std::size_t index )
    {
        if ( !m_Loaded || index >= m_Templates.size() )
            return;
        const VFXEmitterTemplate& t  = m_Templates[index];
        const std::size_t         at = m_Working.Emitters.size();
        m_DeferredSelection          = Selection{ Selection::Kind::Emitter, at, Stage::EmitterUpdate, 0 };
        Defer( std::format( "Add emitter from template '{}'", t.Name ),
               [t]( S::VFXSystemData& d ) -> Common::BoolResultStr
               {
                   S::VFXEmitterData emitter = t.Emitter;
                   emitter.Name              = FreeEmitterName( d, t.Emitter.Name );
                   // The emitter keeps the template's fixed bounds: each emitter culls by its own box.
                   d.Emitters.push_back( std::move( emitter ) );
                   return BOOLSUCCESS;
               } );
    }

    std::string VFXSystemDocument::PanelTitle( const char* name ) const
    {
        // "###" + the subject: two open systems must not share one "Selection" window.
        return std::format( "{}###VFX{}{}", name, name, static_cast<uint64_t>( Subject().Owner ) );
    }

    void VFXSystemDocument::BuildLayout( const unsigned int dockId ) const
    {
        // Niagara's default: the preview viewport left, the System Overview centre, the Selection right. The
        // shares are Niagara's at a 1200 px window, not measured from a screenshot.
        ImGui::DockBuilderRemoveNode( dockId );
        ImGui::DockBuilderAddNode( dockId, ImGuiDockNodeFlags_DockSpace );
        ImGui::DockBuilderSetNodeSize( dockId, ImGui::GetContentRegionAvail() );
        ImGuiID       center  = dockId;
        const ImGuiID preview = ImGui::DockBuilderSplitNode( center, ImGuiDir_Left, 0.38f, nullptr, &center );
        const ImGuiID details = ImGui::DockBuilderSplitNode( center, ImGuiDir_Right, 0.50f, nullptr, &center );
        ImGui::DockBuilderDockWindow( PanelTitle( "Preview" ).c_str(), preview );
        ImGui::DockBuilderDockWindow( PanelTitle( "System Overview" ).c_str(), center );
        ImGui::DockBuilderDockWindow( PanelTitle( "Selection" ).c_str(), details );
        ImGui::DockBuilderGetNode( preview )->LocalFlags |= ImGuiDockNodeFlags_HiddenTabBar;
        ImGui::DockBuilderFinish( dockId );
    }

    void VFXSystemDocument::DrawPreview( const ImVec2& size )
    {
        m_RenderSize = glm::uvec2( static_cast<uint32_t>( std::max( size.x, 1.0f ) ),
                                   static_cast<uint32_t>( std::max( size.y, 1.0f ) ) );
        if ( ImGui::BeginChild( "##vfxpreview", size ) )
        {
            if ( !m_Preview || !m_UIHelper )
                ImGui::TextDisabled( "Starting the preview..." );
            else
            {
                const ImVec2 origin = ImGui::GetCursorScreenPos();
                (void)m_Preview->Draw( *m_UIHelper, size, PreviewInteraction::Interactive );
                if ( m_Working.Emitters.empty() )
                    ImGui::GetWindowDrawList()->AddText( ImVec2( origin.x + 10.0f, origin.y + 8.0f ),
                                                         IM_COL32( 200, 200, 200, 255 ),
                                                         "No emitters - Add emitter picks a template" );
            }
        }
        ImGui::EndChild();
    }

    void VFXSystemDocument::Defer( std::string                                               label,
                                   std::function<Common::BoolResultStr( S::VFXSystemData& )> edit )
    {
        // One discrete gesture per frame: a second click in the same frame is not a thing a person does.
        if ( m_Deferred )
            return;
        m_DeferredLabel = std::move( label );
        m_Deferred      = std::move( edit );
    }

    void VFXSystemDocument::TrackGesture( const char* label )
    {
        if ( ImGui::IsItemActivated() && !m_GestureBefore )
            m_GestureBefore = m_FrameStart;
        if ( ImGui::IsItemDeactivated() && m_GestureBefore )
        {
            Report( CommitVFXSystemGesture( m_Working, *m_GestureBefore, CommandHistory::Get(), label ) );
            m_GestureBefore.reset();
        }
    }

    void VFXSystemDocument::ClampSelection()
    {
        Selection& s = m_Selection;
        if ( s.What == Selection::Kind::System )
            return;
        if ( s.Emitter >= m_Working.Emitters.size() )
        {
            s = Selection{};
            return;
        }
        auto&             emitter = m_Working.Emitters[s.Emitter];
        const std::size_t rows    = s.What == Selection::Kind::Module     ? StageModules( emitter, s.Group ).size()
                                    : s.What == Selection::Kind::Renderer ? emitter.Renderers.size()
                                                                          : 1;
        if ( s.What != Selection::Kind::Emitter && s.Index >= rows )
            s = Selection{ Selection::Kind::Emitter, s.Emitter, Stage::EmitterUpdate, 0 };
    }

    void VFXSystemDocument::OnUIRender()
    {
        if ( !m_Loaded )
        {
            ImGui::TextWrapped( "This VFX system could not be read: %s", m_LoadError.c_str() );
            return;
        }
        if ( !m_GestureBefore )
            m_FrameStart = m_Working;
        ClampSelection();

        // No ImGui::Begin: EditorLayer's document loop wraps this in Begin/End.
        m_DrewThisFrame = true;

        DrawToolbar();

        // UE's Niagara System editor: the Preview viewport, the System Overview and the Selection are dockable
        // windows of this document (a nested Begin is legal), laid out once per new DockSpace node.
        const ImGuiID        dockId = ImGui::GetID( "VFXSystemDock1" );
        const ImGuiDockNode* node   = ImGui::DockBuilderGetNode( dockId );
        if ( node == nullptr || node->IsLeafNode() )
            BuildLayout( dockId );
        ImGui::DockSpace( dockId, ImVec2( 0.0f, 0.0f ) );
        const auto panel = [this]( const char* name, const ImGuiWindowFlags flags, auto&& draw )
        {
            if ( ImGui::Begin( PanelTitle( name ).c_str(), nullptr, flags ) )
                draw();
            ImGui::End();
        };
        panel( "Preview", ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse,
               [this]() { DrawPreview( ImGui::GetContentRegionAvail() ); } );
        panel( "System Overview", 0, [this]() { DrawOverview(); } );
        panel( "Selection", 0, [this]() { DrawDetails(); } );

        // The frame's discrete edit lands after every widget is drawn: nothing holds a reference into m_Working.
        if ( m_Deferred )
        {
            const auto edit = std::move( m_Deferred );
            m_Deferred      = nullptr;
            const auto done = ApplyVFXSystemEdit( m_Working, CommandHistory::Get(), m_DeferredLabel, edit );
            Report( done );
            if ( done.IsSuccess() && m_DeferredSelection )
                m_Selection = *m_DeferredSelection;
            m_DeferredSelection.reset();
        }
        ClampSelection();
    }

    void VFXSystemDocument::DrawToolbar()
    {
        ImGui::BeginDisabled( m_Templates.empty() );
        if ( ImGui::Button( ICON_MDI_PLUS " Add emitter" ) )
            ImGui::OpenPopup( "##templates" );
        ImGui::EndDisabled();
        if ( ImGui::BeginPopup( "##templates" ) )
        {
            ImGui::TextDisabled( "Emitter templates" );
            ImGui::Separator();
            for ( std::size_t i = 0; i < m_Templates.size(); ++i )
                if ( ImGui::Selectable( m_Templates[i].Name.c_str() ) )
                    AddEmitterFromTemplate( i );
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::TextDisabled( "%zu emitter(s)%s", m_Working.Emitters.size(),
                             GetDiskState() == DiskState::Dirty ? "  - unsaved" : "" );

        if ( !m_LibraryError.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.75f, 0.3f, 1.0f ), "%s", m_LibraryError.c_str() );
        if ( !m_Refusal.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "%s", m_Refusal.c_str() );
    }

    void VFXSystemDocument::DrawOverview()
    {
        if ( ImGui::Selectable( ICON_MDI_FIRE " System", m_Selection.What == Selection::Kind::System ) )
            m_Selection = Selection{};
        ImGui::Separator();
        for ( std::size_t e = 0; e < m_Working.Emitters.size(); ++e )
            DrawEmitterStack( e );
        if ( m_Working.Emitters.empty() )
            ImGui::TextDisabled( "No emitters - Add emitter picks a template." );
    }

    void VFXSystemDocument::DrawEmitterStack( const std::size_t e )
    {
        ImGui::PushID( static_cast<int>( e ) );
        const S::VFXEmitterData& emitter = m_Working.Emitters[e];
        constexpr ImGuiTreeNodeFlags kFlags =
             ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowItemOverlap;
        const bool open = ImGui::TreeNodeEx( "##emitter", kFlags, "" );
        ImGui::SameLine();
        bool enabled = emitter.Enabled;
        if ( ImGui::Checkbox( "##enabled", &enabled ) )
            Defer( enabled ? "Enable emitter" : "Disable emitter",
                   [e, enabled]( S::VFXSystemData& d ) -> Common::BoolResultStr
                   {
                       d.Emitters[e].Enabled = enabled;
                       return BOOLSUCCESS;
                   } );
        ImGui::SameLine();
        const bool selected = m_Selection.What == Selection::Kind::Emitter && m_Selection.Emitter == e;
        if ( ImGui::Selectable( emitter.Name.c_str(), selected ) )
            m_Selection = Selection{ Selection::Kind::Emitter, e, Stage::EmitterUpdate, 0 };
        if ( open )
        {
            for ( const Stage stage : kStages )
                DrawStage( e, stage );
            ImGui::TextColored( ImVec4( 0.55f, 0.75f, 0.55f, 1.0f ), "Render" );
            for ( std::size_t r = 0; r < m_Working.Emitters[e].Renderers.size(); ++r )
            {
                const S::VFXRendererData& renderer = m_Working.Emitters[e].Renderers[r];
                ImGui::PushID( static_cast<int>( r ) );
                ImGui::Indent();
                const bool rendererSelected = m_Selection.What == Selection::Kind::Renderer &&
                                              m_Selection.Emitter == e && m_Selection.Index == r;
                if ( ImGui::Selectable( std::format( "{}{}", RendererLabel( renderer.Kind ),
                                                     renderer.Enabled ? "" : " (disabled)" )
                                             .c_str(),
                                        rendererSelected ) )
                    m_Selection = Selection{ Selection::Kind::Renderer, e, Stage::EmitterUpdate, r };
                ImGui::Unindent();
                ImGui::PopID();
            }
            DrawAddRenderer( e );
            ImGui::TreePop();
        }
        ImGui::Separator();
        ImGui::PopID();
    }

    void VFXSystemDocument::DrawStage( const std::size_t e, const Stage stage )
    {
        ImGui::PushID( static_cast<int>( stage ) );
        ImGui::TextColored( ImVec4( 0.55f, 0.70f, 0.95f, 1.0f ), "%s", StageLabel( stage ) );
        const auto&       modules = StageModules( m_Working.Emitters[e], stage );
        const std::size_t count   = modules.size();
        for ( std::size_t m = 0; m < count; ++m )
        {
            const S::VFXModuleUse& use = modules[m];
            ImGui::PushID( static_cast<int>( m ) );
            ImGui::Indent();
            bool on = use.Enabled;
            if ( ImGui::Checkbox( "##on", &on ) )
                Defer( on ? "Enable module" : "Disable module",
                       [e, stage, m, on]( S::VFXSystemData& d ) -> Common::BoolResultStr
                       {
                           StageModules( d.Emitters[e], stage )[m].Enabled = on;
                           return BOOLSUCCESS;
                       } );
            ImGui::SameLine();
            const bool selected = m_Selection.What == Selection::Kind::Module && m_Selection.Emitter == e &&
                                  m_Selection.Group == stage && m_Selection.Index == m;
            const float buttons = ImGui::GetFrameHeight() * 3.0f + ImGui::GetStyle().ItemSpacing.x * 3.0f;
            ImGui::BeginDisabled( !use.Enabled );
            if ( ImGui::Selectable( ModuleLabel( m_Modules, use.Module ).c_str(), selected, 0,
                                    ImVec2( ImGui::GetContentRegionAvail().x - buttons, 0.0f ) ) )
                m_Selection = Selection{ Selection::Kind::Module, e, stage, m };
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled( m == 0 );
            const bool up = ImGui::SmallButton( ICON_MDI_ARROW_UP );
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled( m + 1 >= count );
            const bool down = ImGui::SmallButton( ICON_MDI_ARROW_DOWN );
            ImGui::EndDisabled();
            ImGui::SameLine();
            const bool remove = ImGui::SmallButton( ICON_MDI_CLOSE );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Remove this module" );
            ImGui::Unindent();
            ImGui::PopID();

            if ( up || down )
            {
                const std::size_t to = up ? m - 1 : m + 1;
                if ( selected )
                    m_DeferredSelection = Selection{ Selection::Kind::Module, e, stage, to };
                Defer( "Move module",
                       [e, stage, m, to]( S::VFXSystemData& d ) -> Common::BoolResultStr
                       {
                           auto& rows = StageModules( d.Emitters[e], stage );
                           std::swap( rows[m], rows[to] );
                           return BOOLSUCCESS;
                       } );
            }
            else if ( remove )
                Defer( "Remove module",
                       [e, stage, m]( S::VFXSystemData& d ) -> Common::BoolResultStr
                       {
                           auto& rows = StageModules( d.Emitters[e], stage );
                           rows.erase( rows.begin() + static_cast<std::ptrdiff_t>( m ) );
                           return BOOLSUCCESS;
                       } );
        }

        ImGui::Indent();
        if ( ImGui::SmallButton( ICON_MDI_PLUS " Add module" ) )
            ImGui::OpenPopup( "##add" );
        ImGui::Unindent();
        if ( ImGui::BeginPopup( "##add" ) )
        {
            DrawModuleMenu( stage,
                            [this, e, stage, count]( const VFX::VFXModuleInfo& info )
                            {
                                m_DeferredSelection = Selection{ Selection::Kind::Module, e, stage, count };
                                Defer( std::format( "Add module {}", info.DisplayName ),
                                       [e, stage, use = VFX::DefaultModuleUse( info )](
                                            S::VFXSystemData& d ) -> Common::BoolResultStr
                                       {
                                           StageModules( d.Emitters[e], stage ).push_back( use );
                                           return BOOLSUCCESS;
                                       } );
                            } );
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    void VFXSystemDocument::DrawModuleMenu( const Stage                                             stage,
                                            const std::function<void( const VFX::VFXModuleInfo& )>& chosen )
    {
        // By Category, then by name (std::map orders both); an empty Category is the catalog's "Other".
        std::map<std::string, std::map<std::string, const VFX::VFXModuleInfo*>> byCategory;
        for ( const VFX::VFXModuleInfo& info : m_Modules )
            if ( info.Addable && info.AllowsStage( ModuleStageOf( stage ) ) )
                byCategory[info.Category.empty() ? std::string( "Other" ) : info.Category]
                          [ModuleLabel( m_Modules, info.Module )] = &info;
        if ( byCategory.empty() )
            ImGui::TextDisabled( "No module of the catalog runs in %s", StageLabel( stage ) );
        for ( const auto& [category, modules] : byCategory )
        {
            if ( !ImGui::BeginMenu( category.c_str() ) )
                continue;
            for ( const auto& [label, info] : modules )
            {
                if ( ImGui::MenuItem( label.c_str() ) )
                    chosen( *info );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "%s", info->Module.Path.c_str() );
            }
            ImGui::EndMenu();
        }
    }

    void VFXSystemDocument::DrawDetails()
    {
        switch ( m_Selection.What )
        {
            case Selection::Kind::System:
                DrawSystemDetails();
                return;
            case Selection::Kind::Emitter:
                DrawEmitterDetails( m_Selection.Emitter );
                return;
            case Selection::Kind::Renderer:
                DrawRendererDetails( m_Selection.Emitter, m_Selection.Index );
                return;
            case Selection::Kind::Module:
                DrawModuleDetails( m_Selection.Emitter, m_Selection.Group, m_Selection.Index );
                return;
        }
    }

    void VFXSystemDocument::DrawSystemDetails()
    {
        ImGui::TextUnformatted( "System" );
        ImGui::Separator();
        if ( !BeginProperties( "##system" ) )
            return;

        PropertyLabel( "Category" );
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::BeginCombo( "##category", m_Working.Category.c_str() ) )
        {
            for ( const S::VFXCategory& category : m_Categories.Categories )
                if ( ImGui::Selectable( category.DisplayName.c_str(), category.Id == m_Working.Category ) &&
                     category.Id != m_Working.Category )
                    Defer( "Set category",
                           [id = category.Id]( S::VFXSystemData& d ) -> Common::BoolResultStr
                           {
                               d.Category = id;
                               return BOOLSUCCESS;
                           } );
            ImGui::EndCombo();
        }

        PropertyLabel( "Duration", "s" );
        ImGui::SetNextItemWidth( -1.0f );
        ImGui::DragFloat( "##duration", &m_Working.Duration, 0.01f, 0.0f, 600.0f, "%.2f s",
                          ImGuiSliderFlags_AlwaysClamp );
        TrackGesture( "Set system duration" );

        PropertyLabel( "Loop" );
        bool loop = m_Working.Loop;
        if ( ImGui::Checkbox( "##loop", &loop ) )
            Defer( "Set system loop",
                   [loop]( S::VFXSystemData& d ) -> Common::BoolResultStr
                   {
                       d.Loop = loop;
                       return BOOLSUCCESS;
                   } );

        PropertyLabel( "Seed" );
        ImGui::SetNextItemWidth( -1.0f );
        ImGui::DragScalar( "##seed", ImGuiDataType_U32, &m_Working.Seed, 1.0f );
        TrackGesture( "Set system seed" );

        PropertyLabel( "User parameters" );
        if ( m_Working.UserParams.empty() )
            ImGui::TextDisabled( "none" );
        for ( const S::VFXUserParam& p : m_Working.UserParams )
            ImGui::Text( "User.%s", p.Name.c_str() );
        ImGui::EndTable();
    }

    void VFXSystemDocument::DrawEmitterDetails( const std::size_t e )
    {
        ImGui::Text( "Emitter  %s", m_Working.Emitters[e].Name.c_str() );
        ImGui::SameLine();
        const bool duplicate = ImGui::SmallButton( ICON_MDI_CONTENT_COPY " Duplicate" );
        ImGui::SameLine();
        const bool remove = ImGui::SmallButton( ICON_MDI_CLOSE " Remove" );
        ImGui::Separator();
        if ( duplicate )
        {
            m_DeferredSelection =
                 Selection{ Selection::Kind::Emitter, m_Working.Emitters.size(), Stage::EmitterUpdate, 0 };
            Defer( "Duplicate emitter",
                   [e]( S::VFXSystemData& d ) -> Common::BoolResultStr
                   {
                       S::VFXEmitterData copy = d.Emitters[e];
                       copy.Name              = FreeEmitterName( d, copy.Name );
                       d.Emitters.push_back( std::move( copy ) );
                       return BOOLSUCCESS;
                   } );
        }
        if ( remove )
        {
            m_DeferredSelection = Selection{};
            Defer( "Remove emitter",
                   [e]( S::VFXSystemData& d ) -> Common::BoolResultStr
                   {
                       // The handlers spawning from its events go with it (their source no longer exists).
                       const std::string gone = d.Emitters[e].Name;
                       d.Emitters.erase( d.Emitters.begin() + static_cast<std::ptrdiff_t>( e ) );
                       for ( S::VFXEmitterData& other : d.Emitters )
                           std::erase_if( other.EventHandlers, [&gone]( const S::VFXEventHandler& handler )
                                          { return handler.SourceEmitter == gone; } );
                       return BOOLSUCCESS;
                   } );
        }
        if ( !BeginProperties( "##emitter" ) )
            return;
        S::VFXEmitterData& emitter = m_Working.Emitters[e];

        // NAME: committed once, when the text box is left after an edit - one rename, one undo step.
        PropertyLabel( "Name" );
        std::array<char, 128> shown{};
        auto&                 buffer = m_NameTyping ? m_NameBuffer : shown;
        if ( !m_NameTyping )
            emitter.Name.copy( shown.data(), std::min( emitter.Name.size(), shown.size() - 1 ) );
        ImGui::SetNextItemWidth( -1.0f );
        ImGui::InputText( "##name", buffer.data(), buffer.size() );
        if ( ImGui::IsItemActivated() )
        {
            m_NameTyping = true;
            m_NameBuffer = {};
            emitter.Name.copy( m_NameBuffer.data(), std::min( emitter.Name.size(), m_NameBuffer.size() - 1 ) );
        }
        if ( ImGui::IsItemDeactivated() )
        {
            m_NameTyping = false;
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                Defer( "Rename emitter",
                       [e, typed = std::string( buffer.data() )]( S::VFXSystemData& d ) -> Common::BoolResultStr
                       {
                           // Handlers name their source by emitter name: they follow the rename (UE renames the
                           // emitter's references with it), in the same undo step.
                           const std::string old = d.Emitters[e].Name;
                           for ( S::VFXEmitterData& other : d.Emitters )
                               for ( S::VFXEventHandler& handler : other.EventHandlers )
                                   if ( handler.SourceEmitter == old )
                                       handler.SourceEmitter = typed;
                           d.Emitters[e].Name = typed;
                           return BOOLSUCCESS;
                       } );
        }

        // UE: the emitter's Fixed Bounds - what every view culls this emitter's draws by, around the placement.
        PropertyLabel( "Bounds min", "cm" );
        ImGui::SetNextItemWidth( -1.0f );
        ImGui::DragFloat3( "##bmin", &emitter.Bounds.Min.x, 1.0f, -100000.0f, 100000.0f, "%.0f cm" );
        TrackGesture( "Set emitter bounds" );
        PropertyLabel( "Bounds max", "cm" );
        ImGui::SetNextItemWidth( -1.0f );
        ImGui::DragFloat3( "##bmax", &emitter.Bounds.Max.x, 1.0f, -100000.0f, 100000.0f, "%.0f cm" );
        TrackGesture( "Set emitter bounds" );

        PropertyLabel( "Simulation space" );
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::BeginCombo( "##space", emitter.Space == S::VFXSimulationSpace::World ? "World" : "Local" ) )
        {
            for ( const auto space : { S::VFXSimulationSpace::World, S::VFXSimulationSpace::Local } )
                if ( ImGui::Selectable( space == S::VFXSimulationSpace::World ? "World" : "Local",
                                        space == emitter.Space ) &&
                     space != emitter.Space )
                    Defer( "Set simulation space",
                           [e, space]( S::VFXSystemData& d ) -> Common::BoolResultStr
                           {
                               d.Emitters[e].Space = space;
                               return BOOLSUCCESS;
                           } );
            ImGui::EndCombo();
        }

        PropertyLabel( "Capacity", "particles" );
        ImGui::SetNextItemWidth( -1.0f );
        ImGui::DragScalar( "##capacity", ImGuiDataType_U32, &emitter.Capacity, 4.0f );
        TrackGesture( "Set emitter capacity" );

        PropertyLabel( "Start delay", "s" );
        ImGui::SetNextItemWidth( -1.0f );
        ImGui::DragFloat( "##delay", &emitter.Lifecycle.Delay, 0.01f, 0.0f, 600.0f, "%.2f s",
                          ImGuiSliderFlags_AlwaysClamp );
        TrackGesture( "Set emitter delay" );

        PropertyLabel( "Loop duration", "s" );
        ImGui::SetNextItemWidth( -1.0f );
        ImGui::DragFloat( "##loopDuration", &emitter.Lifecycle.LoopDuration, 0.01f, 0.01f, 600.0f, "%.2f s",
                          ImGuiSliderFlags_AlwaysClamp );
        TrackGesture( "Set loop duration" );

        PropertyLabel( "Loop behavior" );
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::BeginCombo( "##loopBehavior", LoopLabel( emitter.Lifecycle.Loop ) ) )
        {
            for ( const auto loop :
                  { S::VFXLoopBehavior::Once, S::VFXLoopBehavior::Multiple, S::VFXLoopBehavior::Infinite } )
                if ( ImGui::Selectable( LoopLabel( loop ), loop == emitter.Lifecycle.Loop ) &&
                     loop != emitter.Lifecycle.Loop )
                    Defer( "Set loop behavior",
                           [e, loop]( S::VFXSystemData& d ) -> Common::BoolResultStr
                           {
                               d.Emitters[e].Lifecycle.Loop = loop;
                               return BOOLSUCCESS;
                           } );
            ImGui::EndCombo();
        }
        if ( emitter.Lifecycle.Loop == S::VFXLoopBehavior::Multiple )
        {
            PropertyLabel( "Loop count" );
            ImGui::SetNextItemWidth( -1.0f );
            const uint32_t one = 1;
            ImGui::DragScalar( "##loopCount", ImGuiDataType_U32, &emitter.Lifecycle.LoopCount, 0.1f, &one );
            TrackGesture( "Set loop count" );
        }
        ImGui::EndTable();
        DrawEventHandlers( e );
    }

    void VFXSystemDocument::DrawEventHandlers( const std::size_t e )
    {
        ImGui::Spacing();
        ImGui::TextUnformatted( "Event Handlers" );
        ImGui::Separator();
        S::VFXEmitterData& emitter = m_Working.Emitters[e];
        // A handler's source is ANOTHER emitter of this system (ValidateVFXSystemData): with none, nothing to add.
        std::vector<std::string> sources;
        for ( std::size_t k = 0; k < m_Working.Emitters.size(); ++k )
            if ( k != e )
                sources.push_back( m_Working.Emitters[k].Name );
        for ( std::size_t h = 0; h < emitter.EventHandlers.size(); ++h )
        {
            S::VFXEventHandler& handler = emitter.EventHandlers[h];
            ImGui::PushID( static_cast<int>( h ) );
            const bool remove = ImGui::SmallButton( ICON_MDI_CLOSE );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Remove this event handler" );
            ImGui::SameLine();
            ImGui::Text( "Handler %zu", h );
            if ( BeginProperties( "##handler" ) )
            {
                PropertyLabel( "Source emitter" );
                ImGui::SetNextItemWidth( -1.0f );
                if ( ImGui::BeginCombo( "##source", handler.SourceEmitter.c_str() ) )
                {
                    for ( const std::string& name : sources )
                        if ( ImGui::Selectable( name.c_str(), name == handler.SourceEmitter ) &&
                             name != handler.SourceEmitter )
                            Defer( "Set event source",
                                   [e, h, name]( S::VFXSystemData& d ) -> Common::BoolResultStr
                                   {
                                       d.Emitters[e].EventHandlers[h].SourceEmitter = name;
                                       return BOOLSUCCESS;
                                   } );
                    ImGui::EndCombo();
                }
                PropertyLabel( "Event" );
                ImGui::SetNextItemWidth( -1.0f );
                if ( ImGui::BeginCombo( "##event", EventLabel( handler.Event ) ) )
                {
                    for ( const auto kind :
                          { S::VFXEventKind::Death, S::VFXEventKind::Collision, S::VFXEventKind::Location } )
                        if ( ImGui::Selectable( EventLabel( kind ), kind == handler.Event ) && kind != handler.Event )
                            Defer( "Set event kind",
                                   [e, h, kind]( S::VFXSystemData& d ) -> Common::BoolResultStr
                                   {
                                       d.Emitters[e].EventHandlers[h].Event = kind;
                                       return BOOLSUCCESS;
                                   } );
                    ImGui::EndCombo();
                }
                PropertyLabel( "Spawn count", "particles" );
                ImGui::SetNextItemWidth( -1.0f );
                const uint32_t one = 1;
                ImGui::DragScalar( "##spawn", ImGuiDataType_U32, &handler.SpawnCount, 0.1f, &one, nullptr, "%u",
                                   ImGuiSliderFlags_AlwaysClamp );
                TrackGesture( "Set event spawn count" );
                PropertyLabel( "Inherit velocity", "x" );
                ImGui::SetNextItemWidth( -1.0f );
                ImGui::DragFloat( "##inherit", &handler.InheritVelocity, 0.01f, -10.0f, 10.0f, "%.2f" );
                TrackGesture( "Set event inherited velocity" );
                ImGui::EndTable();
            }
            ImGui::PopID();
            if ( remove )
                Defer( "Remove event handler",
                       [e, h]( S::VFXSystemData& d ) -> Common::BoolResultStr
                       {
                           auto& handlers = d.Emitters[e].EventHandlers;
                           handlers.erase( handlers.begin() + static_cast<std::ptrdiff_t>( h ) );
                           return BOOLSUCCESS;
                       } );
        }
        ImGui::BeginDisabled( sources.empty() );
        if ( ImGui::SmallButton( ICON_MDI_PLUS " Add event handler" ) )
            Defer( "Add event handler",
                   [e, source = sources.empty() ? std::string() : sources.front()](
                        S::VFXSystemData& d ) -> Common::BoolResultStr
                   {
                       S::VFXEventHandler handler;
                       handler.SourceEmitter = source;
                       d.Emitters[e].EventHandlers.push_back( std::move( handler ) );
                       return BOOLSUCCESS;
                   } );
        ImGui::EndDisabled();
        if ( sources.empty() && ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip( "An event handler spawns from another emitter's events: add a second emitter" );
    }

    void VFXSystemDocument::DrawRendererDetails( const std::size_t e, const std::size_t r )
    {
        ImGui::Text( "Renderer of %s", m_Working.Emitters[e].Name.c_str() );
        ImGui::Separator();
        if ( !BeginProperties( "##renderer" ) )
            return;
        const S::VFXRendererData renderer = m_Working.Emitters[e].Renderers[r];
        // The kind is the row's identity (its Mesh / Ribbon block follows it): shown, not chosen.
        PropertyLabel( "Kind" );
        ImGui::TextUnformatted( RendererLabel( renderer.Kind ) );
        ImGui::SameLine();
        if ( ImGui::SmallButton( ICON_MDI_CLOSE " Remove" ) )
        {
            m_DeferredSelection = Selection{ Selection::Kind::Emitter, e, Stage::EmitterUpdate, 0 };
            Defer( "Remove renderer",
                   [e, r]( S::VFXSystemData& d ) -> Common::BoolResultStr
                   {
                       auto& rows = d.Emitters[e].Renderers;
                       rows.erase( rows.begin() + static_cast<std::ptrdiff_t>( r ) );
                       return BOOLSUCCESS;
                   } );
        }

        // THE MATERIAL (UE: the renderer's Material): a `.demat` whose template declares the kind's Usage;
        // none = the kind's engine default template.
        PropertyLabel( "Material" );
        const bool noMaterial = !renderer.Material.has_value();
        if ( Utils::ImGuiUtilities::AssetSlot( "##material",
                                               noMaterial ? "Default (engine)" : renderer.Material->Path.c_str(),
                                               noMaterial ) )
            ImGui::OpenPopup( "##materials" );
        if ( ImGui::BeginPopup( "##materials" ) )
        {
            if ( ImGui::Selectable( "Default (engine)", noMaterial ) && !noMaterial )
                Defer( "Clear renderer material",
                       [e, r]( S::VFXSystemData& d ) -> Common::BoolResultStr
                       {
                           d.Emitters[e].Renderers[r].Material.reset();
                           return BOOLSUCCESS;
                       } );
            static ImGuiTextFilter materialFilter;
            if ( const auto picked =
                      PickAssetRow( Assets::ContentRegistry::Rows( Common::Content::ContentKind::Material ),
                                    noMaterial ? std::string() : renderer.Material->Guid, materialFilter ) )
                Defer( "Set renderer material",
                       [e, r, ref = *picked]( S::VFXSystemData& d ) -> Common::BoolResultStr
                       {
                           d.Emitters[e].Renderers[r].Material = ref;
                           return BOOLSUCCESS;
                       } );
            ImGui::EndPopup();
        }

        if ( renderer.Mesh )
        {
            // UE UNiagaraMeshRendererProperties: the static mesh every particle draws, and how it is turned.
            PropertyLabel( "Mesh" );
            if ( Utils::ImGuiUtilities::AssetSlot( "##mesh", renderer.Mesh->Mesh.Path.c_str() ) )
                ImGui::OpenPopup( "##meshes" );
            if ( ImGui::BeginPopup( "##meshes" ) )
            {
                static ImGuiTextFilter meshFilter;
                if ( const auto picked = PickAssetRow( Assets::ContentRegistry::MeshRows( false ),
                                                       renderer.Mesh->Mesh.Guid, meshFilter ) )
                    Defer( "Set renderer mesh",
                           [e, r, ref = *picked]( S::VFXSystemData& d ) -> Common::BoolResultStr
                           {
                               d.Emitters[e].Renderers[r].Mesh->Mesh = ref;
                               return BOOLSUCCESS;
                           } );
                ImGui::EndPopup();
            }
            PropertyLabel( "Facing" );
            ImGui::SetNextItemWidth( -1.0f );
            if ( ImGui::BeginCombo( "##meshFacing", MeshFacingLabel( renderer.Mesh->Facing ) ) )
            {
                for ( const auto facing : { S::VFXMeshFacing::Default, S::VFXMeshFacing::Velocity,
                                            S::VFXMeshFacing::CameraPosition, S::VFXMeshFacing::CameraPlane } )
                    if ( ImGui::Selectable( MeshFacingLabel( facing ), facing == renderer.Mesh->Facing ) &&
                         facing != renderer.Mesh->Facing )
                        Defer( "Set mesh facing",
                               [e, r, facing]( S::VFXSystemData& d ) -> Common::BoolResultStr
                               {
                                   d.Emitters[e].Renderers[r].Mesh->Facing = facing;
                                   return BOOLSUCCESS;
                               } );
                ImGui::EndCombo();
            }
        }
        if ( renderer.Ribbon )
        {
            // UE UNiagaraRibbonRendererProperties: the facing mode and the U tiling over the normalised age.
            PropertyLabel( "Facing" );
            ImGui::SetNextItemWidth( -1.0f );
            if ( ImGui::BeginCombo( "##ribbonFacing", RibbonFacingLabel( renderer.Ribbon->Facing ) ) )
            {
                for ( const auto facing : { S::VFXRibbonFacing::Screen, S::VFXRibbonFacing::Custom } )
                    if ( ImGui::Selectable( RibbonFacingLabel( facing ), facing == renderer.Ribbon->Facing ) &&
                         facing != renderer.Ribbon->Facing )
                        Defer( "Set ribbon facing",
                               [e, r, facing]( S::VFXSystemData& d ) -> Common::BoolResultStr
                               {
                                   d.Emitters[e].Renderers[r].Ribbon->Facing = facing;
                                   return BOOLSUCCESS;
                               } );
                ImGui::EndCombo();
            }
            PropertyLabel( "UV tiling" );
            ImGui::SetNextItemWidth( -1.0f );
            // > 0 (ValidateVFXSystemData): the drag clamps at a hundredth.
            ImGui::DragFloat( "##uvTiling", &m_Working.Emitters[e].Renderers[r].Ribbon->UVTiling, 0.01f, 0.01f,
                              1000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
            TrackGesture( "Set ribbon UV tiling" );
        }
        PropertyLabel( "Enabled" );
        bool on = renderer.Enabled;
        if ( ImGui::Checkbox( "##enabled", &on ) )
            Defer( on ? "Enable renderer" : "Disable renderer",
                   [e, r, on]( S::VFXSystemData& d ) -> Common::BoolResultStr
                   {
                       d.Emitters[e].Renderers[r].Enabled = on;
                       return BOOLSUCCESS;
                   } );
        ImGui::EndTable();
    }

    void VFXSystemDocument::DrawAddRenderer( const std::size_t e )
    {
        ImGui::Indent();
        if ( ImGui::SmallButton( ICON_MDI_PLUS " Add renderer" ) )
            ImGui::OpenPopup( "##addRenderer" );
        ImGui::Unindent();
        if ( !ImGui::BeginPopup( "##addRenderer" ) )
            return;
        const std::size_t at  = m_Working.Emitters[e].Renderers.size();
        const auto        add = [this, e, at]( const char* label, S::VFXRendererData row )
        {
            m_DeferredSelection = Selection{ Selection::Kind::Renderer, e, Stage::EmitterUpdate, at };
            Defer( label,
                   [e, row = std::move( row )]( S::VFXSystemData& d ) -> Common::BoolResultStr
                   {
                       d.Emitters[e].Renderers.push_back( row );
                       return BOOLSUCCESS;
                   } );
        };
        if ( ImGui::Selectable( "Sprite Renderer" ) )
            add( "Add sprite renderer", S::VFXRendererData{ .Kind = S::VFXRendererKind::Sprite } );
        if ( ImGui::Selectable( "Ribbon Renderer" ) )
            add( "Add ribbon renderer", S::VFXRendererData{ .Kind   = S::VFXRendererKind::Ribbon,
                                                            .Ribbon = S::VFXRibbonRendererData{} } );
        // A Mesh row names its static mesh (ValidateVFXSystemData): it is added with the one picked here.
        if ( ImGui::BeginMenu( "Mesh Renderer" ) )
        {
            static ImGuiTextFilter meshFilter;
            if ( const auto picked = PickAssetRow( Assets::ContentRegistry::MeshRows( false ), {}, meshFilter ) )
            {
                add( "Add mesh renderer",
                     S::VFXRendererData{ .Kind = S::VFXRendererKind::Mesh,
                                         .Mesh = S::VFXMeshRendererData{ .Mesh = *picked } } );
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    void VFXSystemDocument::DrawModuleDetails( const std::size_t e, const Stage stage, const std::size_t m )
    {
        const S::VFXModuleUse& use = StageModules( m_Working.Emitters[e], stage )[m];
        ImGui::Text( "%s", ModuleLabel( m_Modules, use.Module ).c_str() );
        ImGui::SameLine();
        ImGui::TextDisabled( "%s - %s of %s", use.Module.Path.c_str(), StageLabel( stage ),
                             m_Working.Emitters[e].Name.c_str() );
        // THE ROW'S .dfxmod (UE: a module node's script reference): another catalog module of the stage takes the
        // row's place; an input it declares under the same name and type keeps the row's value, every other
        // input starts at its declared Default.
        if ( ImGui::SmallButton( ICON_MDI_SWAP_HORIZONTAL " Module..." ) )
            ImGui::OpenPopup( "##swap" );
        if ( ImGui::BeginPopup( "##swap" ) )
        {
            DrawModuleMenu(
                 stage,
                 [this, e, stage, m]( const VFX::VFXModuleInfo& info )
                 {
                     m_DeferredSelection = Selection{ Selection::Kind::Module, e, stage, m };
                     Defer( std::format( "Set module {}", info.DisplayName ),
                            [e, stage, m, next = VFX::DefaultModuleUse( info )](
                                 S::VFXSystemData& d ) mutable -> Common::BoolResultStr
                            {
                                S::VFXModuleUse& row = StageModules( d.Emitters[e], stage )[m];
                                for ( S::VFXModuleInput& in : next.Inputs )
                                {
                                    const auto kept = std::ranges::find_if(
                                         row.Inputs, [&in]( const S::VFXModuleInput& old )
                                         { return old.Name == in.Name && old.Type == in.Type; } );
                                    if ( kept != row.Inputs.end() )
                                        in = *kept;
                                }
                                next.Enabled = row.Enabled;
                                row          = std::move( next );
                                return BOOLSUCCESS;
                            } );
                 } );
            ImGui::EndPopup();
        }
        ImGui::Separator();
        if ( use.Inputs.empty() )
        {
            ImGui::TextDisabled( "This module has no inputs." );
            return;
        }
        if ( !ImGui::BeginTable( "##inputs", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg ) )
            return;
        ImGui::TableSetupColumn( "Input", ImGuiTableColumnFlags_WidthFixed, 170.0f );
        ImGui::TableSetupColumn( "Source", ImGuiTableColumnFlags_WidthFixed, 120.0f );
        ImGui::TableSetupColumn( "Value", ImGuiTableColumnFlags_WidthStretch );
        for ( std::size_t i = 0; i < use.Inputs.size(); ++i )
            DrawInput( e, stage, m, i );
        ImGui::EndTable();
    }

    void VFXSystemDocument::DrawInput( const std::size_t e, const Stage stage, const std::size_t m,
                                       const std::size_t i )
    {
        S::VFXModuleUse&          use   = StageModules( m_Working.Emitters[e], stage )[m];
        S::VFXModuleInput&        in    = use.Inputs[i];
        const VFX::VFXModuleDecl* decl  = VFX::FindModuleInput( m_Modules, use.Module, in.Name );
        const std::string         units = decl != nullptr ? decl->Units : std::string{};
        const auto                range = decl != nullptr ? decl->Range : std::nullopt;

        ImGui::PushID( static_cast<int>( i ) );
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex( 0 );
        ImGui::AlignTextToFramePadding();
        if ( units.empty() )
            ImGui::TextUnformatted( in.Name.c_str() );
        else
            ImGui::Text( "%s (%s)", in.Name.c_str(), units.c_str() );

        ImGui::TableSetColumnIndex( 1 );
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::BeginCombo( "##source", SourceLabel( in.Source ) ) )
        {
            // A CPU (EmitterUpdate) input is a Value or a User binding only (VFXEmitterSpawn.hpp).
            for ( const auto source : { S::VFXInputSource::Value, S::VFXInputSource::Random,
                                        S::VFXInputSource::Curve, S::VFXInputSource::Binding } )
            {
                const bool cpuRefuses = stage == Stage::EmitterUpdate && ( source == S::VFXInputSource::Random ||
                                                                           source == S::VFXInputSource::Curve );
                if ( cpuRefuses )
                    continue;
                if ( ImGui::Selectable( SourceLabel( source ), source == in.Source ) && source != in.Source )
                    Defer( std::format( "Set {} source", in.Name ),
                           [e, stage, m, i, source]( S::VFXSystemData& d ) -> Common::BoolResultStr
                           {
                               auto& input = StageModules( d.Emitters[e], stage )[m].Inputs[i];
                               return SetSource( input, source, d );
                           } );
            }
            ImGui::EndCombo();
        }

        ImGui::TableSetColumnIndex( 2 );
        const std::string gesture = std::format( "Set {}", in.Name );
        switch ( in.Source )
        {
            case S::VFXInputSource::Value:
                if ( in.Value )
                {
                    ValueWidget( "##value", *in.Value, in.Type, units, range );
                    TrackGesture( gesture.c_str() );
                }
                break;
            case S::VFXInputSource::Random:
                if ( in.Random )
                {
                    ImGui::TextDisabled( "min" );
                    ImGui::SameLine();
                    ValueWidget( "##min", in.Random->Min, in.Type, units, range );
                    TrackGesture( gesture.c_str() );
                    ImGui::TextDisabled( "max" );
                    ImGui::SameLine();
                    ValueWidget( "##max", in.Random->Max, in.Type, units, range );
                    TrackGesture( gesture.c_str() );
                }
                break;
            case S::VFXInputSource::Binding:
                if ( in.Binding )
                {
                    ImGui::SetNextItemWidth( -1.0f );
                    if ( ImGui::BeginCombo( "##binding", in.Binding->c_str() ) )
                    {
                        for ( const S::VFXUserParam& p : m_Working.UserParams )
                        {
                            if ( p.Type != in.Type )
                                continue;
                            const std::string binding = std::string( S::kVFXUserPrefix ) + p.Name;
                            if ( ImGui::Selectable( binding.c_str(), binding == *in.Binding ) &&
                                 binding != *in.Binding )
                                Defer( std::format( "Bind {}", in.Name ),
                                       [e, stage, m, i, binding]( S::VFXSystemData& d ) -> Common::BoolResultStr
                                       {
                                           StageModules( d.Emitters[e], stage )[m].Inputs[i].Binding = binding;
                                           return BOOLSUCCESS;
                                       } );
                        }
                        ImGui::EndCombo();
                    }
                }
                break;
            case S::VFXInputSource::Curve:
                if ( in.Curve )
                {
                    static constexpr std::array<const char*, 4> kChannel = { "x", "y", "z", "w" };
                    for ( std::size_t c = 0; c < in.Curve->size(); ++c )
                    {
                        auto& keys = ( *in.Curve )[c];
                        ImGui::PushID( static_cast<int>( c ) );
                        if ( in.Curve->size() > 1 )
                            ImGui::TextDisabled( "%s", kChannel[std::min<std::size_t>( c, 3 )] );
                        for ( std::size_t k = 0; k < keys.size(); ++k )
                        {
                            ImGui::PushID( static_cast<int>( k ) );
                            ImGui::SetNextItemWidth( 90.0f );
                            // A key stays between its neighbours, so the channel stays sorted by time.
                            const float lo = k == 0 ? 0.0f : keys[k - 1].Time;
                            const float hi = k + 1 < keys.size() ? keys[k + 1].Time : 1.0f;
                            ImGui::DragFloat( "##t", &keys[k].Time, 0.005f, lo, hi, "age %.2f",
                                              ImGuiSliderFlags_AlwaysClamp );
                            TrackGesture( gesture.c_str() );
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth( -ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x );
                            ImGui::DragFloat( "##v", &keys[k].Value, DragSpeed( range ), 0.0f, 0.0f,
                                              NumberFormat( units, range, false ).c_str() );
                            TrackGesture( gesture.c_str() );
                            ImGui::SameLine();
                            ImGui::BeginDisabled( keys.size() <= 1 );
                            if ( ImGui::SmallButton( ICON_MDI_CLOSE ) )
                                Defer( "Remove curve key",
                                       [e, stage, m, i, c, k]( S::VFXSystemData& d ) -> Common::BoolResultStr
                                       {
                                           auto& ks =
                                                ( *StageModules( d.Emitters[e], stage )[m].Inputs[i].Curve )[c];
                                           ks.erase( ks.begin() + static_cast<std::ptrdiff_t>( k ) );
                                           return BOOLSUCCESS;
                                       } );
                            ImGui::EndDisabled();
                            ImGui::PopID();
                        }
                        if ( ImGui::SmallButton( ICON_MDI_PLUS " key" ) )
                            Defer( "Add curve key",
                                   [e, stage, m, i, c]( S::VFXSystemData& d ) -> Common::BoolResultStr
                                   {
                                       // In the middle of the widest gap, on the curve's chord there.
                                       auto& ks = ( *StageModules( d.Emitters[e], stage )[m].Inputs[i].Curve )[c];
                                       if ( ks.empty() )
                                       {
                                           ks.push_back( S::VFXCurveKey{} );
                                           return BOOLSUCCESS;
                                       }
                                       std::size_t at  = ks.size();
                                       float       gap = 1.0f - ks.back().Time;
                                       for ( std::size_t k = 0; k + 1 < ks.size(); ++k )
                                           if ( ks[k + 1].Time - ks[k].Time > gap )
                                           {
                                               gap = ks[k + 1].Time - ks[k].Time;
                                               at  = k + 1;
                                           }
                                       S::VFXCurveKey key = at == ks.size() ? ks.back() : ks[at - 1];
                                       key.Time += gap * 0.5f;
                                       if ( at < ks.size() )
                                           key.Value = 0.5f * ( ks[at - 1].Value + ks[at].Value );
                                       ks.insert( ks.begin() + static_cast<std::ptrdiff_t>( at ), key );
                                       return BOOLSUCCESS;
                                   } );
                        ImGui::PopID();
                    }
                }
                break;
        }
        ImGui::PopID();
    }

    SubjectEditorRegistry::PathOpenOutcome RequestVFXSystemDocument( Assets::AssetManager*        assets,
                                                                     const std::string&           path,
                                                                     const SubjectEditorRegistry& editors )
    {
        using Outcome = SubjectEditorRegistry::PathOpenOutcome;
        std::error_code ec;
        if ( assets == nullptr || std::filesystem::path( path ).extension() != S::kVFXSystemExtension ||
             !std::filesystem::exists( path, ec ) )
            return Outcome::NotMine;

        auto asset = assets->FindByPath<Assets::VFXSystemAsset>( path );
        if ( !asset )
            asset = assets->CreateAsset<Assets::VFXSystemAsset>( path );
        if ( !asset )
        {
            LOG_ERROR( "[VFX] '{}' could not be registered as a VFX system - no editor was opened.", path );
            return Outcome::Failed;
        }
        if ( !asset->IsReadyForUse() )
        {
            if ( const auto loaded = asset->LoadFromFile(); !loaded )
            {
                LOG_ERROR( "[VFX] '{}' would not load as a VFX system - no editor was opened: {}", path,
                           loaded.GetError() );
                return Outcome::Failed;
            }
        }

        const auto handle = asset->GetMetadata().Handle;
        if ( const auto opened = Core::RequestOpenAsset( assets->FindMetadataByHandle( handle ), handle, editors );
             !opened.IsSuccess() )
        {
            LOG_ERROR( "[VFX] '{}': {}", path, opened.GetError() );
            return Outcome::Failed;
        }
        return Outcome::Requested;
    }
} // namespace Desert::Editor
