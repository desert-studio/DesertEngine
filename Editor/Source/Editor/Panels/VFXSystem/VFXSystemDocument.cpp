#include "VFXSystemDocument.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Commands/VFXSystemEdit.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/SubjectTitle.hpp>
#include <Editor/Panels/AssetEditorToolbar.hpp>
#include <Editor/Widgets/PreviewEnvironmentUI.hpp>
#include <Editor/Widgets/PreviewInput.hpp>
#include <Editor/Widgets/PreviewViewport.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/VFXSystemAsset.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/VFX/VFXSystemService.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <system_error>
#include <unordered_map>

namespace Desert::Editor
{
    namespace
    {
        namespace S = Assets::Serialization;

        // Exhaustive on purpose (no default): a new enumerator is a -Wswitch warning here, not a blank label.
        const char* SourceLabel( const S::VFXInputSource source )
        {
            switch ( source )
            {
                case S::VFXInputSource::Value:
                    return "Value";
                case S::VFXInputSource::Curve:
                    return "Curve";
                case S::VFXInputSource::Binding:
                    return "Bind";
                case S::VFXInputSource::Random:
                    return "Random";
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

        std::string VecText( const glm::vec4& v )
        {
            return std::format( "{:.3g}, {:.3g}, {:.3g}, {:.3g}", v.x, v.y, v.z, v.w );
        }

        // What the input is set to, in the words of its source (the stack row's right-hand side).
        std::string InputSummary( const S::VFXModuleInput& input )
        {
            switch ( input.Source )
            {
                case S::VFXInputSource::Value:
                    return input.Value ? VecText( *input.Value ) : std::string( "(no value)" );
                case S::VFXInputSource::Curve:
                    return input.Curve ? std::format( "{} channel(s)", input.Curve->size() )
                                       : std::string( "(no curve)" );
                case S::VFXInputSource::Binding:
                    return input.Binding ? *input.Binding : std::string( "(unbound)" );
                case S::VFXInputSource::Random:
                    return input.Random ? std::format( "[{}] .. [{}]", VecText( input.Random->Min ),
                                                       VecText( input.Random->Max ) )
                                        : std::string( "(no range)" );
            }
            return {};
        }

        constexpr std::array<const char*, 3> kStageKeys = { "EmitterUpdate", "ParticleSpawn", "ParticleUpdate" };

        std::vector<S::VFXModuleUse>& StageModules( S::VFXEmitterStack& stack, const std::size_t stage )
        {
            return stage == 0 ? stack.EmitterUpdate : stage == 1 ? stack.ParticleSpawn : stack.ParticleUpdate;
        }

        const std::vector<S::VFXModuleUse>& StageModules( const S::VFXEmitterStack& stack,
                                                          const std::size_t         stage )
        {
            return stage == 0 ? stack.EmitterUpdate : stage == 1 ? stack.ParticleSpawn : stack.ParticleUpdate;
        }

        const char* TypeWord( const S::VFXValueType type )
        {
            switch ( type )
            {
                case S::VFXValueType::Float:
                    return "float";
                case S::VFXValueType::Vec2:
                    return "float2";
                case S::VFXValueType::Vec3:
                    return "float3";
                case S::VFXValueType::Vec4:
                    return "float4";
                case S::VFXValueType::Int:
                    return "int";
                case S::VFXValueType::Bool:
                    return "bool";
            }
            return "?";
        }

        // One settable number group of the stack: a Value input, or one end of a Random range.
        enum class InputPart
        {
            Value,
            Min,
            Max,
        };

        struct InputAddress
        {
            std::string Name;
            std::size_t Emitter = 0, Stage = 0, Module = 0, Input = 0;
            InputPart   Part = InputPart::Value;
        };

        // The census both EditableProperties and SetEditableProperty read: one list, so a name means one input.
        std::vector<InputAddress> AddressInputs( const S::VFXSystemData& data )
        {
            std::vector<InputAddress> out;
            for ( std::size_t e = 0; e < data.Emitters.size(); ++e )
            {
                for ( std::size_t st = 0; st < kStageKeys.size(); ++st )
                {
                    const auto&                          modules = StageModules( data.Emitters[e].Stack, st );
                    std::unordered_map<std::string, int> seen;
                    for ( std::size_t m = 0; m < modules.size(); ++m )
                    {
                        const std::string& full  = modules[m].Module;
                        const auto         colon = full.find( ':' );
                        std::string        name  = colon == std::string::npos ? full : full.substr( colon + 1 );
                        if ( const int repeat = ++seen[name]; repeat > 1 )
                            name = std::format( "{}#{}", name, repeat );
                        for ( std::size_t i = 0; i < modules[m].Inputs.size(); ++i )
                        {
                            const auto&       input = modules[m].Inputs[i];
                            const std::string base  = std::format( "{}.{}.{}.{}", data.Emitters[e].Name,
                                                                   kStageKeys[st], name, input.Name );
                            if ( input.Source == S::VFXInputSource::Random )
                            {
                                out.push_back( { base + ".Min", e, st, m, i, InputPart::Min } );
                                out.push_back( { base + ".Max", e, st, m, i, InputPart::Max } );
                            }
                            else
                                out.push_back( { base, e, st, m, i, InputPart::Value } );
                        }
                    }
                }
            }
            return out;
        }

        S::VFXModuleInput& InputAt( S::VFXSystemData& data, const InputAddress& a )
        {
            return StageModules( data.Emitters[a.Emitter].Stack, a.Stage )[a.Module].Inputs[a.Input];
        }

        const S::VFXModuleInput& InputAt( const S::VFXSystemData& data, const InputAddress& a )
        {
            return StageModules( data.Emitters[a.Emitter].Stack, a.Stage )[a.Module].Inputs[a.Input];
        }

        // The vec4 a part of @p input stores, or null when the input holds none (a Curve or Binding input).
        glm::vec4* PartValue( S::VFXModuleInput& input, const InputPart part )
        {
            switch ( part )
            {
                case InputPart::Value:
                    return input.Source == S::VFXInputSource::Value && input.Value ? &*input.Value : nullptr;
                case InputPart::Min:
                    return input.Random ? &input.Random->Min : nullptr;
                case InputPart::Max:
                    return input.Random ? &input.Random->Max : nullptr;
            }
            return nullptr;
        }
    } // namespace

    VFXSystemDocument::VFXSystemDocument( const Assets::AssetHandle& subject, Assets::AssetManager* assets )
         : ISubjectDocument( AssetSubjectTitle( subject, assets, "VFX System" ),
                             AssetSubject( subject, static_cast<uint32_t>( Assets::AssetTypeID::VFXSystem ) ) ),
           m_Assets( assets )
    {
        const auto asset = m_Assets != nullptr ? m_Assets->FindByHandle<Assets::VFXSystemAsset>( subject ) : nullptr;
        if ( !asset )
        {
            m_LoadError = "the system asset is not held by the asset manager";
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
        m_Working = asset->GetData();
        m_Saved   = m_Working;
        m_Loaded  = true;
    }

    VFXSystemDocument::~VFXSystemDocument()
    {
        CommandHistory::Get().DropFor( &m_Working );
    }

    bool VFXSystemDocument::IsSubjectAlive() const
    {
        return m_Assets != nullptr &&
               m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }

    void VFXSystemDocument::EnsurePreview()
    {
        if ( m_Preview || !m_Loaded )
            return;
        m_Preview  = std::make_unique<PreviewViewport>();
        m_UIHelper = std::make_unique<UI::UIHelper>();
        m_UIHelper->Init();
        m_PreviewLive = true;
        // The preview scene's own VFXComponent plays the system the VFXSystemService holds for this handle.
        m_Preview->SetVFXSystem( Assets::AssetHandle( Subject().Owner ), m_Working.Bounds );
    }

    void VFXSystemDocument::ReleaseView()
    {
        m_Preview.reset();
        m_UIHelper.reset();
        m_Played.reset();
        m_PreviewLive = false;
    }

    void VFXSystemDocument::OnPreUpdate()
    {
        // The slot is claimed only once the window has been drawn (StaticMeshViewerDocument::OnPreUpdate).
        if ( !m_DrewThisFrame )
            return;
        m_DrewThisFrame = false;

        EnsurePreview();
        if ( !m_Preview || m_ViewSize.x == 0u || m_ViewSize.y == 0u )
            return;
        // An edit, an undo or a redo changed the working copy: the preview plays a fresh copy of it (a new pointer
        // restarts every emitter). Compared by value, so whatever route changed m_Working is seen.
        if ( !m_Played || *m_Played != m_Working )
        {
            m_Played = std::make_shared<const Assets::Serialization::VFXSystemData>( m_Working );
            m_Preview->SetVFXWorkingCopy( m_Played );
        }
        PreviewEnvironment::ApplyTo( *m_Preview, m_Assets );
        m_Preview->Update( m_ViewSize.x, m_ViewSize.y );
    }

    void VFXSystemDocument::ExtendToolbar( AssetEditorToolbar& toolbar )
    {
        toolbar.AddButton(
             ICON_MDI_REFRESH, "Restart", "Start every emitter of the preview over (UE Reset Simulation)",
             [this]()
             {
                 if ( m_Preview )
                     m_Preview->RestartVFX();
             },
             []() { return false; } );
    }

    void VFXSystemDocument::DrawSystemColumn()
    {
        ImGui::TextUnformatted( "SYSTEM" );
        ImGui::Separator();
        ImGui::Text( "Category  %s", m_Working.Category.empty() ? "-" : m_Working.Category.c_str() );
        ImGui::Text( "Duration  %.2f s", m_Working.Duration );
        ImGui::Text( "Loop      %s", m_Working.Loop ? "yes" : "no" );
        ImGui::Text( "Seed      %u", m_Working.Seed );

        ImGui::Spacing();
        ImGui::TextUnformatted( "USER PARAMETERS" );
        ImGui::Separator();
        if ( m_Working.UserParams.empty() )
            ImGui::TextDisabled( "(none)" );
        for ( const auto& param : m_Working.UserParams )
            ImGui::BulletText( "User.%s", param.Name.c_str() );

        ImGui::Spacing();
        ImGui::TextUnformatted( "EMITTERS" );
        ImGui::Separator();
        for ( std::size_t e = 0; e < m_Working.Emitters.size(); ++e )
        {
            const auto&       emitter = m_Working.Emitters[e];
            const std::string label   = std::format( "{}{}  ({} max)##emitter{}", emitter.Name,
                                                     emitter.Enabled ? "" : " (disabled)", emitter.Capacity, e );
            if ( ImGui::Selectable( label.c_str(), m_SelectedEmitter == e ) )
                m_SelectedEmitter = e;
        }
    }

    void VFXSystemDocument::DrawStackColumn()
    {
        if ( m_Working.Emitters.empty() )
        {
            ImGui::TextDisabled( "The system has no emitters." );
            return;
        }
        m_SelectedEmitter   = std::min( m_SelectedEmitter, m_Working.Emitters.size() - 1 );
        const auto& emitter = m_Working.Emitters[m_SelectedEmitter];
        ImGui::Text( "STACK  %s", emitter.Name.c_str() );
        if ( !m_Refusal.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "%s", m_Refusal.c_str() );
        ImGui::Separator();
        // UE's stack order: Emitter Update runs once per tick, Particle Spawn once per new particle, Particle
        // Update per live particle per tick, then the Render rows.
        DrawStage( "Emitter Update", m_SelectedEmitter, 0 );
        DrawStage( "Particle Spawn", m_SelectedEmitter, 1 );
        DrawStage( "Particle Update", m_SelectedEmitter, 2 );
        if ( ImGui::CollapsingHeader( "Render", ImGuiTreeNodeFlags_DefaultOpen ) )
        {
            // Indexed again: an edit refused above put back a whole earlier system, so `emitter` may be stale.
            const auto& renderers = m_Working.Emitters[m_SelectedEmitter].Renderers;
            if ( renderers.empty() )
                ImGui::TextDisabled( "  (no renderers)" );
            for ( const auto& row : renderers )
                ImGui::BulletText( "%s%s%s%s", RendererLabel( row.Kind ), row.Enabled ? "" : " (disabled)",
                                   row.Material ? "  material " : "",
                                   row.Material ? row.Material->Guid.c_str() : "" );
        }
    }

    void VFXSystemDocument::OnUIRender()
    {
        // No ImGui::Begin: EditorLayer's document loop wraps this in Begin/End.
        m_DrewThisFrame = true;
        if ( !m_Loaded )
        {
            ImGui::TextWrapped( "This system could not be read: %s", m_LoadError.c_str() );
            return;
        }

        constexpr float kSystemWidth = 260.0f;
        constexpr float kStackWidth  = 380.0f;
        const float     spacing      = ImGui::GetStyle().ItemSpacing.x;
        const ImVec2    avail        = ImGui::GetContentRegionAvail();
        const ImVec2    view( std::max( avail.x - kSystemWidth - kStackWidth - 2.0f * spacing, 1.0f ),
                              std::max( avail.y, 1.0f ) );
        m_ViewSize = glm::uvec2( static_cast<uint32_t>( view.x ), static_cast<uint32_t>( view.y ) );

        if ( ImGui::BeginChild( "##vfxsystem", ImVec2( kSystemWidth, view.y ), true ) )
            DrawSystemColumn();
        ImGui::EndChild();
        ImGui::SameLine();
        if ( ImGui::BeginChild( "##vfxpreview", view ) )
        {
            if ( !m_Preview || !m_UIHelper )
                ImGui::TextDisabled( "Starting the preview..." );
            else
                (void)m_Preview->Draw( *m_UIHelper, view, PreviewInteraction::Interactive );
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if ( ImGui::BeginChild( "##vfxstack", ImVec2( kStackWidth, view.y ), true ) )
            DrawStackColumn();
        ImGui::EndChild();
    }

    void VFXSystemDocument::DrawStage( const char* title, const std::size_t emitter, const std::size_t stage )
    {
        ImGui::PushID( title );
        if ( ImGui::CollapsingHeader( title, ImGuiTreeNodeFlags_DefaultOpen ) )
        {
            // Sizes and names re-read per row, never held: a refused edit puts back a whole earlier system.
            const std::size_t count = StageModules( m_Working.Emitters[emitter].Stack, stage ).size();
            if ( count == 0 )
                ImGui::TextDisabled( "  (no modules)" );
            for ( std::size_t m = 0; m < count; ++m )
            {
                const auto& use = StageModules( m_Working.Emitters[emitter].Stack, stage )[m];
                ImGui::PushID( static_cast<int>( m ) );
                const std::string label = std::format( "{}{}", use.Module, use.Enabled ? "" : "  (disabled)" );
                if ( ImGui::TreeNodeEx( "##module", ImGuiTreeNodeFlags_DefaultOpen, "%s", label.c_str() ) )
                {
                    const std::size_t inputs = use.Inputs.size();
                    for ( std::size_t i = 0; i < inputs; ++i )
                        DrawInputEditor( emitter, stage, m, i );
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        }
        ImGui::PopID();
    }

    void VFXSystemDocument::DrawInputEditor( const std::size_t emitter, const std::size_t stage,
                                             const std::size_t module, const std::size_t index )
    {
        const InputAddress address{ {}, emitter, stage, module, index, InputPart::Value };
        auto&              input = InputAt( m_Working, address );
        ImGui::PushID( static_cast<int>( index ) );
        ImGui::BulletText( "%s  [%s]", input.Name.c_str(), SourceLabel( input.Source ) );

        const std::string     label = std::format( "{}: {}", m_Working.Emitters[emitter].Name, input.Name );
        const S::VFXValueType type  = input.Type;
        const int             count = static_cast<int>( std::clamp( S::ComponentCount( type ), 1u, 4u ) );

        // One widget = one gesture = one undo step (UE: a stack input drag is one transaction). The value before
        // the widget ran is what the gesture started from, even when the first frame already moved it.
        const auto edit = [&]( const char* id, const InputPart part )
        {
            // Fetched afresh: a refused edit of the widget before this one put back a whole earlier system.
            glm::vec4* value = PartValue( InputAt( m_Working, address ), part );
            if ( value == nullptr )
                return;
            const glm::vec4 previous = *value;
            ImGui::SetNextItemWidth( -1.0f );
            switch ( type )
            {
                case S::VFXValueType::Int:
                {
                    int whole = static_cast<int>( value->x );
                    if ( ImGui::DragInt( id, &whole, 0.25f ) )
                        value->x = static_cast<float>( whole );
                    break;
                }
                case S::VFXValueType::Bool:
                {
                    bool on = value->x != 0.0f;
                    if ( ImGui::Checkbox( id, &on ) )
                        value->x = on ? 1.0f : 0.0f;
                    break;
                }
                case S::VFXValueType::Float:
                case S::VFXValueType::Vec2:
                case S::VFXValueType::Vec3:
                case S::VFXValueType::Vec4:
                {
                    const float speed = std::max( std::abs( previous.x ) * 0.01f, 0.01f );
                    (void)ImGui::DragScalarN( id, ImGuiDataType_Float, &value->x, count, speed, nullptr, nullptr,
                                              "%.3f" );
                    break;
                }
            }
            if ( ImGui::IsItemActivated() )
            {
                m_GestureBefore = m_Working;
                if ( glm::vec4* start = PartValue( InputAt( *m_GestureBefore, address ), part ) )
                    *start = previous;
            }
            if ( ImGui::IsItemDeactivated() && m_GestureBefore )
            {
                auto before = std::move( *m_GestureBefore );
                m_GestureBefore.reset();
                (void)Commit( std::move( before ), std::format( "Edit {}", label ) );
            }
        };

        ImGui::Indent();
        switch ( input.Source )
        {
            case S::VFXInputSource::Value:
                edit( "##value", InputPart::Value );
                break;
            case S::VFXInputSource::Random:
                ImGui::TextDisabled( "min" );
                ImGui::SameLine();
                edit( "##min", InputPart::Min );
                ImGui::TextDisabled( "max" );
                ImGui::SameLine();
                edit( "##max", InputPart::Max );
                break;
            case S::VFXInputSource::Curve:
            case S::VFXInputSource::Binding:
            {
                const std::string summary = InputSummary( input );
                ImGui::TextDisabled( "%s", summary.c_str() );
                break;
            }
        }
        ImGui::Unindent();
        ImGui::PopID();
    }

    Common::BoolResultStr VFXSystemDocument::Commit( Assets::Serialization::VFXSystemData before,
                                                     const std::string&                   label )
    {
        if ( before == m_Working )
            return BOOLSUCCESS;
        if ( auto valid = S::ValidateVFXSystemData( m_Working ); !valid )
        {
            m_Working = std::move( before );
            m_Refusal = std::format( "{}: {}", label, valid.GetError() );
            return Common::MakeError<bool>( m_Refusal );
        }
        m_Refusal.clear();
        CommandHistory::Get().PushCommand(
             std::make_unique<VFXSystemEditCommand>( &m_Working, std::move( before ), m_Working, label ) );
        return BOOLSUCCESS;
    }

    bool VFXSystemDocument::SaveDocument()
    {
        if ( !m_Loaded || m_Assets == nullptr )
            return false;
        const Assets::AssetHandle handle( Subject().Owner );
        const auto*               meta = m_Assets->FindMetadataByHandle( handle );
        if ( meta == nullptr )
        {
            m_Refusal = "Save: the system asset is no longer held by the asset manager";
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
        // The held asset re-reads what was written and the service drops its copy, so every level effect playing
        // this system restarts with the saved data, and the next open reads it.
        if ( const auto asset = m_Assets->FindByHandle<Assets::VFXSystemAsset>( handle ) )
        {
            if ( auto reloaded = asset->LoadFromFile(); !reloaded )
                LOG_ERROR( "[VFX] '{}' was saved but would not be read back: {}", meta->Filepath.string(),
                           reloaded.GetError() );
        }
        if ( auto* systems = Runtime::ResourceRegistry::GetVFXSystemService() )
            systems->Refresh( handle );
        return true;
    }

    std::vector<EditableProperty> VFXSystemDocument::EditableProperties() const
    {
        std::vector<EditableProperty> out;
        if ( !m_Loaded )
            return out;
        for ( const auto& address : AddressInputs( m_Working ) )
        {
            const auto&      input = InputAt( m_Working, address );
            EditableProperty row;
            row.Name  = address.Name;
            row.Label = input.Name;
            row.Group =
                 std::format( "{}.{}", m_Working.Emitters[address.Emitter].Name, kStageKeys[address.Stage] );
            row.Type       = TypeWord( input.Type );
            row.Components = static_cast<int>( std::clamp( S::ComponentCount( input.Type ), 1u, 4u ) );
            auto copy      = input;
            if ( const glm::vec4* value = PartValue( copy, address.Part ) )
            {
                for ( int c = 0; c < 4; ++c )
                    row.Value[static_cast<std::size_t>( c )] = ( *value )[c];
            }
            else
            {
                row.Settable          = false;
                row.NotSettableReason = std::format( "a {} input is not a number (its source is {})",
                                                     TypeWord( input.Type ), SourceLabel( input.Source ) );
            }
            out.push_back( std::move( row ) );
        }
        return out;
    }

    Common::BoolResultStr VFXSystemDocument::SetEditableProperty( const std::string&        name,
                                                                  const std::vector<float>& value )
    {
        if ( !m_Loaded )
            return Common::MakeFormattedError<bool>( "the system could not be read, so '{}' cannot be set", name );
        const auto addresses = AddressInputs( m_Working );
        const auto found     = std::find_if( addresses.begin(), addresses.end(),
                                             [&name]( const InputAddress& a ) { return a.Name == name; } );
        if ( found == addresses.end() )
            return Common::MakeFormattedError<bool>(
                 "the system has no stack input '{}'; ask 'properties' for the ones it offers", name );
        auto&     input = InputAt( m_Working, *found );
        const int count = static_cast<int>( std::clamp( S::ComponentCount( input.Type ), 1u, 4u ) );
        if ( static_cast<int>( value.size() ) != count )
            return Common::MakeFormattedError<bool>( "'{}' is a {} and takes {} number(s), not {}", name,
                                                     TypeWord( input.Type ), count, value.size() );
        auto       before = m_Working;
        glm::vec4* target = PartValue( input, found->Part );
        if ( target == nullptr )
            return Common::MakeFormattedError<bool>( "'{}' takes its value from a {}, not a number", name,
                                                     SourceLabel( input.Source ) );
        for ( int c = 0; c < count; ++c )
            ( *target )[c] = value[static_cast<std::size_t>( c )];
        // The same end of a gesture the widget reaches: one undo step, or refused and put back.
        return Commit( std::move( before ), std::format( "Set {}", name ) );
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
