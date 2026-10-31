#include "VFXSystemDocument.hpp"
#include <Engine/Assets/VFXModuleAsset.hpp>
#include <charconv>
#include "VFXCurveInputEditor.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Commands/VFXSystemEdit.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/SubjectTitle.hpp>
#include <Editor/Core/AssetEditorToolbar.hpp>
#include <Editor/Widgets/PreviewEnvironmentUI.hpp>
#include <Editor/Widgets/PreviewInput.hpp>
#include <Editor/Widgets/PreviewViewport.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/VFXSystemAsset.hpp>
#include <Engine/VFX/VFXStackCompiler.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <utility>
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
                case S::VFXRendererKind::Light:
                    return "Light Renderer";
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

        // Indexed by VFX::VFXStage.
        constexpr std::array<const char*, 4> kStageKeys = { "EmitterSpawn", "EmitterUpdate", "ParticleSpawn",
                                                            "ParticleUpdate" };

        std::vector<S::VFXModuleUse>& StageModules( S::VFXEmitterStack& stack, const std::size_t stage )
        {
            return stage == 0   ? stack.EmitterSpawn
                   : stage == 1 ? stack.EmitterUpdate
                   : stage == 2 ? stack.ParticleSpawn
                                : stack.ParticleUpdate;
        }

        const std::vector<S::VFXModuleUse>& StageModules( const S::VFXEmitterStack& stack,
                                                          const std::size_t         stage )
        {
            return stage == 0   ? stack.EmitterSpawn
                   : stage == 1 ? stack.EmitterUpdate
                   : stage == 2 ? stack.ParticleSpawn
                                : stack.ParticleUpdate;
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

        // One settable number group of the stack: a Value input, one end of a Random range, or a Curve input's
        // whole key list (VFX-13: `<input>.Keys` = per stop its time then its values, one undo step).
        enum class InputPart
        {
            Value,
            Min,
            Max,
            Keys,
        };

        struct InputAddress
        {
            std::string Name;
            std::size_t Emitter = 0, Stage = 0, Module = 0, Input = 0;
            InputPart   Part = InputPart::Value;
        };

        // The census both EditableProperties and SetEditableProperty read: one list, so a name means one input.
        /// The name a stack row shows and addresses its inputs by: its `.dfxmod`'s file stem (the engine library
        /// names each file after its module), so `engine:Engine/VFX/Modules/Gravity.dfxmod` reads `Gravity`.
        std::string ModuleName( const Assets::AssetGuidRef& module )
        {
            return std::filesystem::path( module.Path ).stem().string();
        }

        /// The module of the registry whose row key is @p path.
        std::optional<Assets::AssetGuidRef> FindModule( const VFX::VFXModuleResolver& modules, const std::string& path )
        {
            for ( Assets::AssetGuidRef& ref : modules.List() )
                if ( ref.Path == path )
                    return ref;
            return std::nullopt;
        }

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
                        std::string name = ModuleName( modules[m].Module );
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
                            else if ( input.Source == S::VFXInputSource::Curve )
                                out.push_back( { base + ".Keys", e, st, m, i, InputPart::Keys } );
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

        // A stack row's address word: the module's name without its prefix, `<Name>#2`.. for a repeat in one
        // stage - the same words AddressInputs builds input names from.
        std::string ModuleKey( const std::vector<S::VFXModuleUse>& modules, const std::size_t module )
        {
            const std::string own    = ModuleName( modules[module].Module );
            int               repeat = 0;
            for ( std::size_t m = 0; m <= module; ++m )
                repeat += ModuleName( modules[m].Module ) == own ? 1 : 0;
            return repeat > 1 ? std::format( "{}#{}", own, repeat ) : own;
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
                case InputPart::Keys:
                    return nullptr;
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
        m_Asset   = asset;
        m_Working = asset->GetData();
        m_Synced  = asset->Snapshot();
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
        // The preview scene's own VFXComponent names the asset, as a level placement does: it plays the asset's
        // data, edits included (VFXSystemService hands out the asset's current snapshot).
        // Framed by the enabled emitters' boxes (VFXSystemExtent); a system with none frames the default box.
        m_Preview->SetVFXSystem( Assets::AssetHandle( Subject().Owner ),
                                 S::VFXSystemExtent( m_Working ).value_or( S::VFXBounds{} ) );
    }

    void VFXSystemDocument::ReleaseView()
    {
        m_Preview.reset();
        m_UIHelper.reset();
        m_Replayed.reset();
        m_PreviewLive = false;
    }

    void VFXSystemDocument::SyncWithAsset()
    {
        if ( !m_Loaded || !m_Asset )
            return;
        if ( m_Asset->Snapshot() != m_Synced )
        {
            // Replaced from outside the window (Revert, the file read again): that is the system now. The
            // history's steps were edits of what was replaced, so they go with it - unless the data is the same
            // (the re-read of what this window just saved).
            m_Synced = m_Asset->Snapshot();
            if ( *m_Synced != m_Working )
            {
                CommandHistory::Get().DropFor( &m_Working );
                m_Working = *m_Synced;
                m_GestureBefore.reset();
            }
            return;
        }
        if ( *m_Synced == m_Working || !S::ValidateVFXSystemData( m_Working ) )
            return;
        m_Asset->Edit( m_Working );
        m_Synced = m_Asset->Snapshot();
    }

    ISubjectDocument::DiskState VFXSystemDocument::GetDiskState() const
    {
        if ( !m_Loaded || !m_Asset )
            return DiskState::Untracked;
        return m_Asset->HasUnsavedEdits() || m_Working != *m_Synced ? DiskState::Dirty : DiskState::Clean;
    }

    bool VFXSystemDocument::DiscardEdits()
    {
        if ( GetDiskState() != DiskState::Dirty )
            return false;
        if ( auto read = m_Asset->LoadFromFile(); !read )
        {
            m_Refusal = std::format( "Revert: {}", read.GetError() );
            LOG_ERROR( "[VFX] '{}' was not reverted: {}", m_Asset->GetMetadata().Filepath.string(),
                       read.GetError() );
            return false;
        }
        m_Refusal.clear();
        SyncWithAsset();
        return true;
    }

    void VFXSystemDocument::OnPreUpdate()
    {
        // Every frame, drawn or not: a control-socket `set` or an undo reaches the asset (and every scene) here.
        SyncWithAsset();

        // The slot is claimed only once the window has been drawn (StaticMeshViewerDocument::OnPreUpdate).
        if ( !m_DrewThisFrame )
            return;
        m_DrewThisFrame = false;

        EnsurePreview();
        if ( !m_Preview || m_ViewSize.x == 0u || m_ViewSize.y == 0u )
            return;
        // The asset took an edit (or was reverted): the preview's transport replays from zero to its time, so
        // the picture at t stays a function of t (12e) - a paused frame shows the edit at the same t. Level
        // placements are not restarted: a value edit reaches their live particles in place.
        if ( m_Replayed != m_Synced )
        {
            m_Replayed = m_Synced;
            m_Preview->RestartVFX();
        }

        // The transport's clock: wall time x speed while playing (UE's sequencer plays the timeline in real time);
        // at the end of the range a looping transport jumps back to its start, another one stops there.
        if ( m_Playing )
        {
            m_Time += static_cast<double>( ImGui::GetIO().DeltaTime ) * static_cast<double>( m_Speed );
            const double end = PlaybackEnd();
            if ( m_Time >= end )
            {
                if ( m_Loop && end > 0.0 )
                    m_Time = std::fmod( m_Time, end );
                else
                {
                    m_Time    = end;
                    m_Playing = false;
                }
            }
        }
        m_Preview->SetVFXDesiredAge( m_Time );
        PreviewEnvironment::ApplyTo( *m_Preview, m_Assets );
        m_Preview->Update( m_ViewSize.x, m_ViewSize.y );
    }

    void VFXSystemDocument::ExtendToolbar( AssetEditorToolbar& toolbar )
    {
        toolbar.AddButton(
             ICON_MDI_REFRESH, "Restart",
             "Start every emitter of the preview over from t = 0 (UE Reset Simulation)",
             [this]()
             {
                 m_Time = 0.0;
                 if ( m_Preview )
                     m_Preview->RestartVFX();
             },
             []() { return false; } );
        toolbar.AddButton(
             ICON_MDI_UNDO_VARIANT, "Revert",
             "Discard the unsaved edits: the system is read from its file again, in this window and in every scene",
             [this]() { DiscardEdits(); }, []() { return false; },
             [this]() { return GetDiskState() == DiskState::Dirty; } );
    }

    double VFXSystemDocument::PlaybackEnd() const
    {
        return m_Working.Duration > 0.0f ? static_cast<double>( m_Working.Duration )
                                         : static_cast<double>( m_PlaybackEnd );
    }

    void VFXSystemDocument::ScrubTo( const double seconds )
    {
        m_Playing = false;
        m_Time    = std::clamp( seconds, 0.0, PlaybackEnd() );
    }

    void VFXSystemDocument::StepTransport( const int steps )
    {
        // One fixed step of the effects clock: the smallest change of the picture there is.
        const double step = m_Preview ? m_Preview->GetVFXStepSeconds() : 1.0 / 60.0;
        ScrubTo( ( std::round( m_Time / step ) + static_cast<double>( steps ) ) * step );
    }

    void VFXSystemDocument::DrawTransport()
    {
        // UE Niagara's timeline transport, one row under the preview: to start, step back, play/pause, step
        // forward, loop | the time scrubber over the range | speed.
        if ( ImGui::Button( ICON_MDI_SKIP_PREVIOUS "##vfxtostart" ) )
            ScrubTo( 0.0 );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "To start (t = 0)" );
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_STEP_BACKWARD "##vfxstepback" ) )
            StepTransport( -1 );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Step back one simulation step" );
        ImGui::SameLine();
        if ( ImGui::Button( m_Playing ? ICON_MDI_PAUSE "##vfxplay" : ICON_MDI_PLAY "##vfxplay" ) )
        {
            if ( !m_Playing && !m_Loop && m_Time >= PlaybackEnd() )
                m_Time = 0.0; // UE: Play at the end of a non-looping range starts it again
            m_Playing = !m_Playing;
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( m_Playing ? "Pause" : "Play" );
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_STEP_FORWARD "##vfxstepfwd" ) )
            StepTransport( 1 );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Step forward one simulation step" );
        ImGui::SameLine();
        const bool loop = m_Loop;
        if ( loop )
            ImGui::PushStyleColor( ImGuiCol_Button, ImGui::GetStyleColorVec4( ImGuiCol_ButtonActive ) );
        if ( ImGui::Button( ICON_MDI_REPEAT "##vfxloop" ) )
            m_Loop = !m_Loop;
        if ( loop )
            ImGui::PopStyleColor();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( m_Loop ? "Loop playback: on" : "Loop playback: off" );

        constexpr float kSpeedWidth = 90.0f;
        constexpr float kEndWidth   = 70.0f;
        const bool      ownEnd      = m_Working.Duration <= 0.0f;
        const float     spacing     = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SameLine();
        const float scrubWidth =
             ImGui::GetContentRegionAvail().x - kSpeedWidth - spacing - ( ownEnd ? kEndWidth + spacing : 0.0f );
        ImGui::SetNextItemWidth( std::max( scrubWidth, 60.0f ) );
        float       time = static_cast<float>( m_Time );
        const float end  = static_cast<float>( PlaybackEnd() );
        const auto  text = std::format( "{:.3f} / {:.2f} s  (sim {:.3f})", m_Time, end,
                                       m_Preview ? m_Preview->GetVFXAge() : 0.0 );
        if ( ImGui::SliderFloat( "##vfxtime", &time, 0.0f, end, text.c_str(), ImGuiSliderFlags_NoRoundToFormat ) )
            ScrubTo( static_cast<double>( time ) );
        if ( ownEnd )
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth( kEndWidth );
            if ( ImGui::DragFloat( "##vfxend", &m_PlaybackEnd, 0.05f, 0.1f, 600.0f, "end %.2f s" ) )
                m_Time = std::min( m_Time, static_cast<double>( m_PlaybackEnd ) );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "The system has no Duration: the end of the preview's range" );
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth( kSpeedWidth );
        static constexpr std::array<float, 5> kSpeeds{ 0.1f, 0.25f, 0.5f, 1.0f, 2.0f };
        const auto                            speedText = std::format( "{:g}x", m_Speed );
        if ( ImGui::BeginCombo( "##vfxspeed", speedText.c_str() ) )
        {
            for ( const float speed : kSpeeds )
                if ( ImGui::Selectable( std::format( "{:g}x", speed ).c_str(), speed == m_Speed ) )
                    m_Speed = speed;
            ImGui::EndCombo();
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Playback speed" );
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
        // UE emitter properties' Fixed Bounds: the box (cm, the placement frame; VFXBounds) every view and shadow
        // view culls the emitter by. Each corner is one undo step, committed on Enter; a box the format refuses
        // (Min above Max) is put back with the reason above.
        if ( ImGui::CollapsingHeader( "Fixed Bounds", ImGuiTreeNodeFlags_DefaultOpen ) )
        {
            S::VFXBounds bounds = emitter.Bounds;
            ImGui::PushID( "FixedBounds" );
            const bool minSet = ImGui::InputFloat3( "Min (cm)", &bounds.Min.x, "%.1f",
                                                    ImGuiInputTextFlags_EnterReturnsTrue );
            const bool maxSet = ImGui::InputFloat3( "Max (cm)", &bounds.Max.x, "%.1f",
                                                    ImGuiInputTextFlags_EnterReturnsTrue );
            ImGui::PopID();
            if ( minSet || maxSet )
                (void)SetEmitterBounds( m_SelectedEmitter, bounds );
        }
        // UE's stack order: Emitter Update runs once per tick, Particle Spawn once per new particle, Particle
        // Update per live particle per tick, then the Render rows.
        DrawStage( "Emitter Spawn", m_SelectedEmitter, 0 );
        DrawStage( "Emitter Update", m_SelectedEmitter, 1 );
        DrawStage( "Particle Spawn", m_SelectedEmitter, 2 );
        DrawStage( "Particle Update", m_SelectedEmitter, 3 );
        if ( ImGui::CollapsingHeader( "Render", ImGuiTreeNodeFlags_DefaultOpen ) )
        {
            // Indexed again: an edit refused above put back a whole earlier system, so `emitter` may be stale.
            const auto& renderers = m_Working.Emitters[m_SelectedEmitter].Renderers;
            if ( renderers.empty() )
                ImGui::TextDisabled( "  (no renderers)" );
            // UE's renderer rows: the enable box, the renderer, and a Light row's settings (each committed on
            // Enter / toggle, one undo step). Applied after the loop: an edit rewrites m_Working.
            std::function<void()> edit;
            const std::size_t     e = m_SelectedEmitter;
            for ( std::size_t r = 0; r < renderers.size(); ++r )
            {
                const auto& row = renderers[r];
                ImGui::PushID( static_cast<int>( r ) );
                bool enabled = row.Enabled;
                if ( ImGui::Checkbox( "##enabled", &enabled ) )
                    edit = [this, e, r, row, enabled]
                    {
                        S::VFXRendererData v = row;
                        v.Enabled            = enabled;
                        (void)SetRenderer( e, r, v,
                                           std::format( "{} renderer {}", enabled ? "Enable" : "Disable", r ) );
                    };
                ImGui::SameLine();
                ImGui::Text( "%s%s%s%s", RendererLabel( row.Kind ), row.Enabled ? "" : " (disabled)",
                             row.Material ? "  material " : "", row.Material ? row.Material->Guid.c_str() : "" );
                if ( row.Light )
                {
                    ImGui::Indent();
                    S::VFXLightRendererData l       = *row.Light;
                    bool                    changed = false;
                    const auto              enter   = ImGuiInputTextFlags_EnterReturnsTrue;
                    changed |= ImGui::InputFloat( "Radius Scale", &l.RadiusScale, 0.0f, 0.0f, "%.3f", enter );
                    changed |= ImGui::InputFloat( l.UseInverseSquaredFalloff ? "Intensity (cd)" : "Intensity",
                                                  &l.Intensity, 0.0f, 0.0f, "%.3f", enter );
                    changed |= ImGui::InputFloat3( "Color Add", &l.ColorAdd.x, "%.3f", enter );
                    int maxLights = static_cast<int>( l.MaxLights );
                    if ( ImGui::InputInt( "Max Lights", &maxLights, 0, 0, enter ) )
                    {
                        l.MaxLights = static_cast<uint32_t>( std::max( maxLights, 0 ) );
                        changed     = true;
                    }
                    changed |= ImGui::Checkbox( "Inverse Squared Falloff", &l.UseInverseSquaredFalloff );
                    changed |= ImGui::Checkbox( "Alpha Scales Brightness", &l.AlphaScalesBrightness );
                    if ( changed )
                        edit = [this, e, r, row, l]
                        {
                            S::VFXRendererData v = row;
                            v.Light              = l;
                            (void)SetRenderer( e, r, v, std::format( "Edit light renderer {}", r ) );
                        };
                    ImGui::Unindent();
                }
                ImGui::PopID();
            }
            if ( edit )
                edit();
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
        // The picture leaves one row at the bottom of the column for the transport.
        const ImVec2 picture( view.x, std::max( view.y - ImGui::GetFrameHeightWithSpacing(), 1.0f ) );
        m_ViewSize = glm::uvec2( static_cast<uint32_t>( picture.x ), static_cast<uint32_t>( picture.y ) );

        if ( ImGui::BeginChild( "##vfxsystem", ImVec2( kSystemWidth, view.y ), true ) )
            DrawSystemColumn();
        ImGui::EndChild();
        ImGui::SameLine();
        if ( ImGui::BeginChild( "##vfxpreview", view ) )
        {
            if ( !m_Preview || !m_UIHelper )
                ImGui::TextDisabled( "Starting the preview..." );
            else
                (void)m_Preview->Draw( *m_UIHelper, picture, PreviewInteraction::Interactive );
            DrawTransport();
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
            // Sizes and names re-read per row, never held: a refused edit puts back a whole earlier system. A
            // structural edit is applied after the rows are drawn, so no row reads a stage that moved under it.
            const std::size_t count = StageModules( m_Working.Emitters[emitter].Stack, stage ).size();
            if ( count == 0 )
                ImGui::TextDisabled( "  (no modules)" );
            std::function<void()> structural;
            for ( std::size_t m = 0; m < count; ++m )
            {
                const auto& use = StageModules( m_Working.Emitters[emitter].Stack, stage )[m];
                ImGui::PushID( static_cast<int>( m ) );
                // UE's stack row: the enable checkbox, the module, then its row menu (reorder, delete).
                bool enabled = use.Enabled;
                if ( ImGui::Checkbox( "##enabled", &enabled ) )
                    structural = [this, emitter, stage, m, enabled]
                    { (void)SetModuleEnabled( emitter, stage, m, enabled ); };
                ImGui::SameLine();
                const std::string label = std::format( "{}{}", ModuleName( use.Module ), use.Enabled ? "" : "  (disabled)" );
                const bool        open  = ImGui::TreeNodeEx(
                     "##module", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowItemOverlap, "%s",
                     label.c_str() );
                ImGui::SameLine(
                     std::max( ImGui::GetContentRegionMax().x - 3.0f * ImGui::GetFrameHeight(), 0.0f ) );
                ImGui::BeginDisabled( m == 0 );
                if ( ImGui::SmallButton( ICON_MDI_ARROW_UP ) )
                    structural = [this, emitter, stage, m] { (void)MoveModule( emitter, stage, m, -1 ); };
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled( m + 1 == count );
                if ( ImGui::SmallButton( ICON_MDI_ARROW_DOWN ) )
                    structural = [this, emitter, stage, m] { (void)MoveModule( emitter, stage, m, +1 ); };
                ImGui::EndDisabled();
                ImGui::SameLine();
                if ( ImGui::SmallButton( ICON_MDI_DELETE ) )
                    structural = [this, emitter, stage, m] { (void)RemoveModule( emitter, stage, m ); };
                if ( open )
                {
                    const std::size_t inputs = use.Inputs.size();
                    for ( std::size_t i = 0; i < inputs; ++i )
                        DrawInputEditor( emitter, stage, m, i );
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            // The stage's module picker (UE's "+" on a stack group), filtered by each module's Usage.
            if ( ImGui::SmallButton( ICON_MDI_PLUS " Module" ) )
                ImGui::OpenPopup( "##addmodule" );
            if ( ImGui::BeginPopup( "##addmodule" ) )
            {
                for ( const ModuleChoice& choice : ModuleLibrary( stage ) )
                {
                    if ( !choice.Error.empty() )
                    {
                        ImGui::BeginDisabled();
                        ImGui::Selectable( choice.Ref.c_str() );
                        ImGui::EndDisabled();
                        if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
                            ImGui::SetTooltip( "%s", choice.Error.c_str() );
                        continue;
                    }
                    if ( choice.NeedsChannel )
                    {
                        // UE's Spawn From Data Channel row starts by naming its channel: the name, then Add.
                        if ( ImGui::BeginMenu( choice.Ref.c_str() ) )
                        {
                            ImGui::SetNextItemWidth( 160.0f );
                            ImGui::InputTextWithHint( "##channel", "Data channel", m_ChannelPick.data(),
                                                      m_ChannelPick.size() );
                            const std::string channel( m_ChannelPick.data() );
                            ImGui::BeginDisabled( channel.empty() );
                            if ( ImGui::Button( "Add" ) )
                            {
                                structural = [this, emitter, stage, module = choice.Ref, channel]
                                { (void)AddModule( emitter, stage, module, channel ); };
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndDisabled();
                            ImGui::EndMenu();
                        }
                        continue;
                    }
                    if ( ImGui::Selectable( choice.Ref.c_str() ) )
                        structural = [this, emitter, stage, module = choice.Ref]
                        { (void)AddModule( emitter, stage, module ); };
                }
                ImGui::EndPopup();
            }
            if ( structural )
                structural();
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
        const std::string label     = std::format( "{}: {}", m_Working.Emitters[emitter].Name, input.Name );
        const std::string inputName = input.Name;
        // UE's stack input dropdown: a number input can become a curve over the normalised age and back. One
        // undo step each; the curve starts flat at the value, the value is the curve at age 0.
        if ( ImGui::BeginPopupContextItem( "##source" ) )
        {
            bool       converted = false;
            const bool number    = input.Type != S::VFXValueType::Int && input.Type != S::VFXValueType::Bool;
            if ( input.Source == S::VFXInputSource::Value && input.Value && number &&
                 ImGui::MenuItem( "Use a curve over life" ) )
            {
                auto               before = m_Working;
                auto&              in     = InputAt( m_Working, address );
                std::vector<float> packed;
                for ( const float t : { 0.0f, 1.0f } )
                {
                    packed.push_back( t );
                    for ( uint32_t c = 0; c < S::ComponentCount( in.Type ); ++c )
                        packed.push_back( ( *in.Value )[static_cast<glm::length_t>( c )] );
                }
                if ( auto curve = VFXCurveEdit::FromPacked( packed, S::ComponentCount( in.Type ) ) )
                {
                    in.Source = S::VFXInputSource::Curve;
                    in.Curve  = std::move( curve.GetValue() );
                    in.Value.reset();
                    converted = true;
                    (void)Commit( std::move( before ), std::format( "{}: use a curve", label ) );
                }
            }
            if ( input.Source == S::VFXInputSource::Curve && input.Curve && ImGui::MenuItem( "Use a value" ) )
            {
                auto  before = m_Working;
                auto& in     = InputAt( m_Working, address );
                in.Value     = VFXCurveEdit::ValueAt( *in.Curve, 0.0f );
                in.Source    = S::VFXInputSource::Value;
                in.Curve.reset();
                converted = true;
                (void)Commit( std::move( before ), std::format( "{}: use a value", label ) );
            }
            ImGui::EndPopup();
            if ( converted ) // a refused conversion put back a whole earlier system: `input` is not read again
            {
                ImGui::PopID();
                return;
            }
        }

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
            {
                // The one curve editor every module's curve input shares (VFX-13): a gradient for a colour, a plot
                // otherwise, sampled by the particle's normalised age. Edited on a copy taken before the widgets
                // ran, so the gesture's undo step starts from what the curve was when the drag began.
                if ( !input.Curve )
                    break;
                const VFXCurveEdit::Channels previous = *input.Curve;
                const bool                   colour   = type == S::VFXValueType::Vec4 && input.Curve->size() == 4;
                const auto                   gesture  = VFXCurveEdit::Draw( "##curve", *input.Curve, colour );
                if ( gesture.Activated && !m_GestureBefore )
                {
                    m_GestureBefore                            = m_Working;
                    InputAt( *m_GestureBefore, address ).Curve = previous;
                }
                if ( gesture.Deactivated && m_GestureBefore )
                {
                    auto before = std::move( *m_GestureBefore );
                    m_GestureBefore.reset();
                    (void)Commit( std::move( before ), std::format( "Edit {} curve", label ) );
                }
                break;
            }
            case S::VFXInputSource::Binding:
            {
                const std::string summary = InputSummary( input );
                ImGui::TextDisabled( "%s", summary.c_str() );
                break;
            }
        }
        ImGui::Unindent();
        // A `set` from the control channel brings its row into view, as UE's stack scrolls to what it selects.
        if ( !m_Reveal.empty() &&
             m_Reveal.starts_with(
                  std::format( "{}.{}.", m_Working.Emitters[emitter].Name, kStageKeys[stage] ) ) &&
             m_Reveal.find( std::format( ".{}", inputName ) ) != std::string::npos )
        {
            const auto& modules = StageModules( m_Working.Emitters[emitter].Stack, stage );
            if ( m_Reveal.starts_with( std::format( "{}.{}.{}.{}", m_Working.Emitters[emitter].Name,
                                                    kStageKeys[stage], ModuleKey( modules, module ),
                                                    inputName ) ) )
            {
                ImGui::SetScrollHereY( 0.5f );
                m_Reveal.clear();
            }
        }
        ImGui::PopID();
    }

    Common::BoolResultStr VFXSystemDocument::SetEmitterBounds( const std::size_t emitter, const S::VFXBounds bounds )
    {
        if ( emitter >= m_Working.Emitters.size() )
            return Common::MakeFormattedError<bool>( "the system has no emitter {}", emitter );
        auto before                          = m_Working;
        m_Working.Emitters[emitter].Bounds = bounds;
        // ValidateVFXSystemData refuses a box whose Min is above its Max: put back, said in the stack.
        return Commit( std::move( before ), std::format( "Set {} Fixed Bounds", m_Working.Emitters[emitter].Name ) );
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
        if ( !m_Loaded || !m_Asset )
            return false;
        SyncWithAsset();
        if ( m_Working != *m_Synced )
        {
            // Only a state the format refuses stays in the window alone (SyncWithAsset): it is not written.
            m_Refusal = "Save: the system as edited is not valid; finish or undo the edit first";
            return false;
        }
        if ( auto saved = m_Asset->SaveEdits(); !saved )
        {
            m_Refusal = std::format( "Save: {}", saved.GetError() );
            LOG_ERROR( "[VFX] '{}' was not saved: {}", m_Asset->GetMetadata().Filepath.string(), saved.GetError() );
            return false;
        }
        m_Refusal.clear();
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
            if ( address.Part == InputPart::Keys && input.Curve )
            {
                // A curve row: its Value is the curve at age 0; the key list is what `set` takes.
                row.Type              = std::format( "curve:{}", TypeWord( input.Type ) );
                const glm::vec4 start = VFXCurveEdit::ValueAt( *input.Curve, 0.0f );
                for ( int c = 0; c < 4; ++c )
                    row.Value[static_cast<std::size_t>( c )] = start[c];
            }
            else if ( const glm::vec4* value = PartValue( copy, address.Part ) )
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
        // Each stack row's enable checkbox: `<Emitter>.<Stage>.<Module>` (three words, an input has four).
        for ( std::size_t e = 0; e < m_Working.Emitters.size(); ++e )
        {
            for ( std::size_t st = 0; st < kStageKeys.size(); ++st )
            {
                const auto& modules = StageModules( m_Working.Emitters[e].Stack, st );
                for ( std::size_t m = 0; m < modules.size(); ++m )
                {
                    EditableProperty row;
                    row.Name       = std::format( "{}.{}.{}", m_Working.Emitters[e].Name, kStageKeys[st],
                                                  ModuleKey( modules, m ) );
                    row.Label      = ModuleName( modules[m].Module );
                    row.Group      = std::format( "{}.{}", m_Working.Emitters[e].Name, kStageKeys[st] );
                    row.Type       = "bool";
                    row.Components = 1;
                    row.Value[0]   = modules[m].Enabled ? 1.0f : 0.0f;
                    out.push_back( std::move( row ) );
                }
            }
            // The emitter's Fixed Bounds corners: `<Emitter>.Bounds.Min|Max`, three numbers (cm, placement frame).
            for ( const bool max : { false, true } )
            {
                const glm::vec3& corner = max ? m_Working.Emitters[e].Bounds.Max : m_Working.Emitters[e].Bounds.Min;
                EditableProperty row;
                row.Name       = std::format( "{}.Bounds.{}", m_Working.Emitters[e].Name, max ? "Max" : "Min" );
                row.Label      = max ? "Fixed Bounds Max (cm)" : "Fixed Bounds Min (cm)";
                row.Group      = std::format( "{}.Bounds", m_Working.Emitters[e].Name );
                row.Type       = "float3";
                row.Components = 3;
                for ( int c = 0; c < 3; ++c )
                    row.Value[static_cast<std::size_t>( c )] = corner[c];
                out.push_back( std::move( row ) );
            }
        }
        // Each renderer row: `<Emitter>.Render.<row>.Enabled`, and a Light row's settings beside it (VFX-LIGHT).
        for ( const auto& emitter : m_Working.Emitters )
        {
            for ( std::size_t r = 0; r < emitter.Renderers.size(); ++r )
            {
                const auto& renderer = emitter.Renderers[r];
                const auto  add =
                     [&]( const char* field, const char* type, const int components, const glm::vec3 v )
                {
                    EditableProperty row;
                    row.Name       = std::format( "{}.Render.{}.{}", emitter.Name, r, field );
                    row.Label      = std::format( "{} {}", RendererLabel( renderer.Kind ), field );
                    row.Group      = std::format( "{}.Render", emitter.Name );
                    row.Type       = type;
                    row.Components = components;
                    for ( int c = 0; c < components; ++c )
                        row.Value[static_cast<std::size_t>( c )] = v[c];
                    out.push_back( std::move( row ) );
                };
                add( "Enabled", "bool", 1, glm::vec3( renderer.Enabled ? 1.0f : 0.0f ) );
                if ( !renderer.Light )
                    continue;
                const S::VFXLightRendererData& l = *renderer.Light;
                add( "RadiusScale", "float", 1, glm::vec3( l.RadiusScale ) );
                add( "Intensity", "float", 1, glm::vec3( l.Intensity ) );
                add( "ColorAdd", "float3", 3, l.ColorAdd );
                add( "MaxLights", "int", 1, glm::vec3( static_cast<float>( l.MaxLights ) ) );
                add( "UseInverseSquaredFalloff", "bool", 1,
                     glm::vec3( l.UseInverseSquaredFalloff ? 1.0f : 0.0f ) );
                add( "AlphaScalesBrightness", "bool", 1, glm::vec3( l.AlphaScalesBrightness ? 1.0f : 0.0f ) );
            }
        }
        // The transport: the window's, not the asset's - setting it is never an undo step.
        const auto transport = [&out]( const char* name, const char* label, const char* type, const float value )
        {
            EditableProperty row;
            row.Name       = std::format( "Transport.{}", name );
            row.Label      = label;
            row.Group      = "Transport";
            row.Type       = type;
            row.Components = 1;
            row.Value[0]   = value;
            out.push_back( std::move( row ) );
        };
        transport( "Time", "Time (s; setting it pauses, as a scrub)", "float", static_cast<float>( m_Time ) );
        transport( "Playing", "Playing", "bool", m_Playing ? 1.0f : 0.0f );
        transport( "Speed", "Speed (0.1 .. 2)", "float", m_Speed );
        transport( "Loop", "Loop", "bool", m_Loop ? 1.0f : 0.0f );
        transport( "Step", "Step (n simulation steps from now; pauses)", "int", 0.0f );
        return out;
    }

    Common::BoolResultStr VFXSystemDocument::SetEditableProperty( const std::string&        name,
                                                                  const std::vector<float>& value )
    {
        if ( !m_Loaded )
            return Common::MakeFormattedError<bool>( "the system could not be read, so '{}' cannot be set", name );
        if ( name.starts_with( "Transport." ) )
        {
            if ( value.size() != 1 )
                return Common::MakeFormattedError<bool>( "'{}' takes one number, not {}", name, value.size() );
            const float v = value[0];
            if ( name == "Transport.Time" )
            {
                if ( v < 0.0f || static_cast<double>( v ) > PlaybackEnd() )
                    return Common::MakeFormattedError<bool>( "'{}' is {} s, outside the range 0 .. {} s", name, v,
                                                             PlaybackEnd() );
                ScrubTo( static_cast<double>( v ) );
            }
            else if ( name == "Transport.Playing" )
                m_Playing = v != 0.0f;
            else if ( name == "Transport.Speed" )
            {
                if ( v < 0.1f || v > 2.0f )
                    return Common::MakeFormattedError<bool>( "'{}' is {}, outside 0.1 .. 2", name, v );
                m_Speed = v;
            }
            else if ( name == "Transport.Loop" )
                m_Loop = v != 0.0f;
            else if ( name == "Transport.Step" )
                StepTransport( static_cast<int>( std::lround( v ) ) );
            else
                return Common::MakeFormattedError<bool>(
                     "the transport has no '{}'; it offers Time, Playing, Speed, Loop and Step", name );
            return BOOLSUCCESS;
        }
        if ( auto renderer = SetRendererProperty( name, value ) )
            return *renderer;
        for ( std::size_t e = 0; e < m_Working.Emitters.size(); ++e )
            for ( const bool max : { false, true } )
            {
                if ( name != std::format( "{}.Bounds.{}", m_Working.Emitters[e].Name, max ? "Max" : "Min" ) )
                    continue;
                if ( value.size() != 3 )
                    return Common::MakeFormattedError<bool>( "'{}' is a float3 (cm) and takes 3 numbers, not {}",
                                                             name, value.size() );
                S::VFXBounds bounds = m_Working.Emitters[e].Bounds;
                ( max ? bounds.Max : bounds.Min ) = glm::vec3( value[0], value[1], value[2] );
                return SetEmitterBounds( e, bounds );
            }
        for ( std::size_t e = 0; e < m_Working.Emitters.size(); ++e )
        {
            for ( std::size_t st = 0; st < kStageKeys.size(); ++st )
            {
                const auto& modules = StageModules( m_Working.Emitters[e].Stack, st );
                for ( std::size_t m = 0; m < modules.size(); ++m )
                {
                    if ( name != std::format( "{}.{}.{}", m_Working.Emitters[e].Name, kStageKeys[st],
                                              ModuleKey( modules, m ) ) )
                        continue;
                    if ( value.size() != 1 )
                        return Common::MakeFormattedError<bool>( "'{}' is a bool and takes one number, not {}",
                                                                 name, value.size() );
                    return SetModuleEnabled( e, st, m, value[0] != 0.0f );
                }
            }
        }
        const auto addresses = AddressInputs( m_Working );
        const auto found     = std::find_if( addresses.begin(), addresses.end(),
                                             [&name]( const InputAddress& a ) { return a.Name == name; } );
        if ( found == addresses.end() )
            return Common::MakeFormattedError<bool>(
                 "the system has no stack input '{}'; ask 'properties' for the ones it offers", name );
        auto&     input = InputAt( m_Working, *found );
        const int count = static_cast<int>( std::clamp( S::ComponentCount( input.Type ), 1u, 4u ) );
        m_Reveal        = name;
        if ( found->Part == InputPart::Keys )
        {
            auto curve = VFXCurveEdit::FromPacked( value, static_cast<std::size_t>( count ) );
            if ( !curve.IsSuccess() )
                return Common::MakeFormattedError<bool>( "'{}': {}", name, curve.GetError() );
            auto before = m_Working;
            input.Curve = std::move( curve.GetValue() );
            return Commit( std::move( before ), std::format( "Set {}", name ) );
        }
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

    std::vector<VFXSystemDocument::ModuleChoice> VFXSystemDocument::ModuleLibrary( const std::size_t stage ) const
    {
        std::vector<ModuleChoice> out;
        const uint8_t             bit = VFX::UsageBit( static_cast<VFX::VFXStage>( stage ) );
        const Assets::RegistryVFXModuleResolver modules;
        for ( const Assets::AssetGuidRef& ref : modules.List() )
        {
            const auto described = VFX::DescribeModule( ref, modules );
            if ( !described.IsSuccess() )
                out.push_back( { ref.Path, described.GetError() } );
            else if ( ( described.GetValue().Usage & bit ) != 0 )
            {
                bool needsChannel = false;
                for ( const auto& decl : described.GetValue().Inputs )
                    needsChannel = needsChannel || decl.BindingRequired;
                out.push_back( { ref.Path, {}, needsChannel } );
            }
        }
        return out;
    }

    Common::BoolResultStr VFXSystemDocument::AddModule( const std::size_t emitter, const std::size_t stage,
                                                        const std::string& module, const std::string& channel )
    {
        if ( emitter >= m_Working.Emitters.size() || stage >= kStageKeys.size() )
            return Common::MakeFormattedError<bool>( "'{}' can be added only to a stage of an emitter", module );
        const Assets::RegistryVFXModuleResolver modules;
        const auto                              ref = FindModule( modules, module );
        if ( !ref )
        {
            m_Refusal = std::format( "Add {}: no VFX module of the content registry has that path", module );
            return Common::MakeError<bool>( m_Refusal );
        }
        auto described = VFX::DescribeModule( *ref, modules );
        if ( !described.IsSuccess() )
        {
            m_Refusal = std::format( "Add {}: {}", module, described.GetError() );
            return Common::MakeError<bool>( m_Refusal );
        }
        if ( ( described.GetValue().Usage & VFX::UsageBit( static_cast<VFX::VFXStage>( stage ) ) ) == 0 )
        {
            m_Refusal = std::format( "Add {}: its Usage does not include {}", module, kStageKeys[stage] );
            return Common::MakeError<bool>( m_Refusal );
        }
        S::VFXModuleUse use;
        use.Module = *ref;
        for ( const auto& decl : described.GetValue().Inputs )
        {
            S::VFXModuleInput input;
            input.Name = decl.Name;
            input.Type = decl.Type;
            if ( decl.BindingRequired )
            {
                if ( channel.empty() || channel.find( '.' ) != std::string::npos )
                {
                    m_Refusal = std::format( "Add {}: input '{}' needs a data channel name", module, decl.Name );
                    return Common::MakeError<bool>( m_Refusal );
                }
                input.Source  = S::VFXInputSource::Binding;
                input.Binding = std::format( "{}{}", S::kVFXDataChannelPrefix, channel );
            }
            else
            {
                input.Source = S::VFXInputSource::Value;
                input.Value  = decl.Default; // the module's own default (UE: the module script's input default)
            }
            use.Inputs.push_back( std::move( input ) );
        }
        auto before = m_Working;
        StageModules( m_Working.Emitters[emitter].Stack, stage ).push_back( std::move( use ) );
        return Commit( std::move( before ), std::format( "Add {} to {}.{}", module,
                                                         m_Working.Emitters[emitter].Name, kStageKeys[stage] ) );
    }

    Common::BoolResultStr VFXSystemDocument::RemoveModule( const std::size_t emitter, const std::size_t stage,
                                                           const std::size_t module )
    {
        if ( emitter >= m_Working.Emitters.size() || stage >= kStageKeys.size() ||
             module >= StageModules( m_Working.Emitters[emitter].Stack, stage ).size() )
            return Common::MakeError<bool>( "there is no such stack row to remove" );
        auto        before  = m_Working;
        auto&       modules = StageModules( m_Working.Emitters[emitter].Stack, stage );
        std::string label   = std::format( "Remove {}.{}.{}", m_Working.Emitters[emitter].Name, kStageKeys[stage],
                                           ModuleKey( modules, module ) );
        modules.erase( modules.begin() + static_cast<std::ptrdiff_t>( module ) );
        return Commit( std::move( before ), label );
    }

    Common::BoolResultStr VFXSystemDocument::MoveModule( const std::size_t emitter, const std::size_t stage,
                                                         const std::size_t module, const int delta )
    {
        if ( emitter >= m_Working.Emitters.size() || stage >= kStageKeys.size() )
            return Common::MakeError<bool>( "there is no such stack row to move" );
        auto&                modules = StageModules( m_Working.Emitters[emitter].Stack, stage );
        const std::ptrdiff_t target  = static_cast<std::ptrdiff_t>( module ) + delta;
        if ( module >= modules.size() || target < 0 || target >= static_cast<std::ptrdiff_t>( modules.size() ) )
            return Common::MakeError<bool>( "the stack row cannot move past the end of its stage" );
        auto              before = m_Working;
        const std::string label  = std::format( "Move {}.{}.{}", m_Working.Emitters[emitter].Name,
                                                kStageKeys[stage], ModuleKey( modules, module ) );
        std::swap( modules[module], modules[static_cast<std::size_t>( target )] );
        return Commit( std::move( before ), label );
    }

    Common::BoolResultStr VFXSystemDocument::SetModuleEnabled( const std::size_t emitter, const std::size_t stage,
                                                               const std::size_t module, const bool enabled )
    {
        if ( emitter >= m_Working.Emitters.size() || stage >= kStageKeys.size() ||
             module >= StageModules( m_Working.Emitters[emitter].Stack, stage ).size() )
            return Common::MakeError<bool>( "there is no such stack row to enable" );
        auto        before  = m_Working;
        auto&       modules = StageModules( m_Working.Emitters[emitter].Stack, stage );
        std::string label =
             std::format( "{} {}.{}.{}", enabled ? "Enable" : "Disable", m_Working.Emitters[emitter].Name,
                          kStageKeys[stage], ModuleKey( modules, module ) );
        modules[module].Enabled = enabled;
        return Commit( std::move( before ), label );
    }

    Common::BoolResultStr VFXSystemDocument::SetRenderer( const std::size_t emitter, const std::size_t row,
                                                          S::VFXRendererData value, const std::string& label )
    {
        if ( emitter >= m_Working.Emitters.size() || row >= m_Working.Emitters[emitter].Renderers.size() )
            return Common::MakeError<bool>( "there is no such renderer row" );
        auto before                                = m_Working;
        m_Working.Emitters[emitter].Renderers[row] = std::move( value );
        return Commit( std::move( before ), std::format( "{} ({})", label, m_Working.Emitters[emitter].Name ) );
    }

    std::optional<Common::BoolResultStr> VFXSystemDocument::SetRendererProperty( const std::string&        name,
                                                                                 const std::vector<float>& value )
    {
        for ( std::size_t e = 0; e < m_Working.Emitters.size(); ++e )
        {
            const std::string prefix = std::format( "{}.Render.", m_Working.Emitters[e].Name );
            if ( !name.starts_with( prefix ) )
                continue;
            const std::string rest = name.substr( prefix.size() );
            const auto        dot  = rest.find( '.' );
            if ( dot == std::string::npos )
                continue;
            std::size_t r = 0;
            if ( std::from_chars( rest.data(), rest.data() + dot, r ).ec != std::errc{} ||
                 r >= m_Working.Emitters[e].Renderers.size() )
                return Common::MakeFormattedError<bool>( "'{}': emitter '{}' has no renderer row '{}'", name,
                                                         m_Working.Emitters[e].Name, rest.substr( 0, dot ) );
            const std::string  field = rest.substr( dot + 1 );
            S::VFXRendererData v     = m_Working.Emitters[e].Renderers[r];
            const std::size_t  want  = field == "ColorAdd" ? 3u : 1u;
            if ( value.size() != want )
                return Common::MakeFormattedError<bool>( "'{}' takes {} number(s), not {}", name, want,
                                                         value.size() );
            if ( field == "Enabled" )
                v.Enabled = value[0] != 0.0f;
            else if ( !v.Light )
                return Common::MakeFormattedError<bool>( "'{}': a {} row offers only Enabled", name,
                                                         RendererLabel( v.Kind ) );
            else if ( field == "RadiusScale" )
                v.Light->RadiusScale = value[0];
            else if ( field == "Intensity" )
                v.Light->Intensity = value[0];
            else if ( field == "ColorAdd" )
                v.Light->ColorAdd = glm::vec3( value[0], value[1], value[2] );
            else if ( field == "MaxLights" )
                v.Light->MaxLights = static_cast<uint32_t>( std::max( std::lround( value[0] ), 0L ) );
            else if ( field == "UseInverseSquaredFalloff" )
                v.Light->UseInverseSquaredFalloff = value[0] != 0.0f;
            else if ( field == "AlphaScalesBrightness" )
                v.Light->AlphaScalesBrightness = value[0] != 0.0f;
            else
                return Common::MakeFormattedError<bool>(
                     "'{}': a Light row offers Enabled, RadiusScale, Intensity, ColorAdd, MaxLights, "
                     "UseInverseSquaredFalloff and AlphaScalesBrightness",
                     name );
            return SetRenderer( e, r, std::move( v ), std::format( "Set {}", name ) );
        }
        return std::nullopt;
    }

    std::vector<ISubjectDocument::DocumentAction> VFXSystemDocument::Actions()
    {
        std::vector<DocumentAction> out;
        if ( !m_Loaded || m_Working.Emitters.empty() )
            return out;
        const std::size_t e    = std::min( m_SelectedEmitter, m_Working.Emitters.size() - 1 );
        const std::string name = m_Working.Emitters[e].Name;
        // Rows are addressed by words, not captured indices: the list is built once and run later, and a stack
        // that moved in between must refuse rather than act on whatever row now sits at the index.
        const auto locate = [this,
                             name]( const std::size_t  st,
                                    const std::string& key ) -> std::optional<std::pair<std::size_t, std::size_t>>
        {
            for ( std::size_t em = 0; em < m_Working.Emitters.size(); ++em )
            {
                if ( m_Working.Emitters[em].Name != name )
                    continue;
                const auto& modules = StageModules( m_Working.Emitters[em].Stack, st );
                for ( std::size_t m = 0; m < modules.size(); ++m )
                    if ( ModuleKey( modules, m ) == key )
                        return std::pair{ em, m };
            }
            return std::nullopt;
        };
        for ( std::size_t st = 0; st < kStageKeys.size(); ++st )
        {
            const auto& modules = StageModules( m_Working.Emitters[e].Stack, st );
            for ( std::size_t m = 0; m < modules.size(); ++m )
            {
                const std::string key   = ModuleKey( modules, m );
                const std::string where = std::format( "{}.{}.{}", name, kStageKeys[st], key );
                out.push_back( { std::format( "Remove {}", where ), [this, locate, st, key]
                                 {
                                     if ( const auto at = locate( st, key ) )
                                         (void)RemoveModule( at->first, st, at->second );
                                 } } );
                out.push_back( { std::format( "Move Up {}", where ), [this, locate, st, key]
                                 {
                                     if ( const auto at = locate( st, key ) )
                                         (void)MoveModule( at->first, st, at->second, -1 );
                                 } } );
                out.push_back( { std::format( "Move Down {}", where ), [this, locate, st, key]
                                 {
                                     if ( const auto at = locate( st, key ) )
                                         (void)MoveModule( at->first, st, at->second, +1 );
                                 } } );
            }
            for ( const ModuleChoice& choice : ModuleLibrary( st ) )
                if ( choice.Error.empty() && !choice.NeedsChannel ) // the channel is named in the picker
                    out.push_back( { std::format( "Add {} to {}.{}", choice.Ref, name, kStageKeys[st] ),
                                     [this, name, st, module = choice.Ref]
                                     {
                                         for ( std::size_t em = 0; em < m_Working.Emitters.size(); ++em )
                                             if ( m_Working.Emitters[em].Name == name )
                                             {
                                                 (void)AddModule( em, st, module );
                                                 return;
                                             }
                                     } } );
        }
        // Once per emitter, not per stage: the spawn location is one module of the spawn group (VFX-15).
        out.push_back( { std::format( "Spawn {} from point set PaintedPoints", name ), [this, name]
                         {
                             for ( std::size_t em = 0; em < m_Working.Emitters.size(); ++em )
                                 if ( m_Working.Emitters[em].Name == name )
                                 {
                                     (void)SpawnFromPointSet( em, "PaintedPoints" );
                                     return;
                                 }
                         } } );
        return out;
    }

    Common::BoolResultStr VFXSystemDocument::SpawnFromPointSet( const std::size_t emitter, const std::string& set )
    {
        std::size_t spawn = kStageKeys.size();
        for ( std::size_t st = 0; st < kStageKeys.size(); ++st )
            if ( std::string_view( kStageKeys[st] ) == "ParticleSpawn" )
                spawn = st;
        if ( emitter >= m_Working.Emitters.size() || spawn == kStageKeys.size() || set.empty() ||
             set.find( '.' ) != std::string::npos )
        {
            m_Refusal = std::format( "Spawn from point set '{}': no such emitter or not a set name", set );
            return Common::MakeError<bool>( m_Refusal );
        }
        auto before = m_Working;
        auto& modules = StageModules( m_Working.Emitters[emitter].Stack, spawn );
        for ( S::VFXModuleUse& use : modules )
            if ( ModuleName( use.Module ).starts_with( "Shape" ) )
                use.Enabled = false;
        const Assets::RegistryVFXModuleResolver library;
        const auto pointSet = FindModule( library, "engine:Engine/VFX/Modules/ShapePointSet.dfxmod" );
        if ( !pointSet )
        {
            m_Refusal = "Spawn from point set: the engine module library has no ShapePointSet.dfxmod";
            return Common::MakeError<bool>( m_Refusal );
        }
        S::VFXModuleUse use;
        use.Module = *pointSet;
        S::VFXModuleInput location;
        location.Name    = "Location";
        location.Type    = S::VFXValueType::Vec3;
        location.Source  = S::VFXInputSource::Binding;
        location.Binding = std::format( "{}{}", S::kVFXPointSetPrefix, set );
        use.Inputs.push_back( std::move( location ) );
        modules.push_back( std::move( use ) );
        return Commit( std::move( before ), std::format( "Spawn {} from PointSet.{}",
                                                         m_Working.Emitters[emitter].Name, set ) );
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
