#include "ParticleEditorPanel.hpp"
#include <Editor/Panels/PanelContext.hpp>

#include <Editor/Core/IconsMaterialDesignIcons.hpp>

#include <Common/Core/Math/Rounding.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    namespace
    {
        using Data = ECS::ParticleEmitterData;

        // The glowing presets (fire, sparks, magic, an explosion) composite additively: they name the engine's
        // additive sprite material; Smoke keeps the default translucent sprite (no material).
        Assets::AssetHandle AdditiveMaterial()
        {
            return Assets::AssetHandle::FromKey( ECS::kParticleAdditiveMaterialKey );
        }

        struct Preset
        {
            const char* Name;
            void ( *Apply )( Data& );
        };

        void Fire( Data& d )
        {
            d.Material       = AdditiveMaterial();
            d.SpawnRate      = 300.0f;
            d.Lifetime       = 1.2f;
            d.StartSpeed     = 150.0f;
            d.SpeedVariance  = 0.4f;
            d.Direction      = glm::vec3( 0.0f, 1.0f, 0.0f );
            d.ConeAngle      = 22.0f;
            d.Gravity        = glm::vec3( 0.0f, 120.0f, 0.0f ); // buoyant rise
            d.StartSize      = 30.0f;
            d.EndSize        = 5.0f;
            d.SizeCurvePower = 0.6f;
            d.StartColor     = glm::vec3( 1.0f, 0.6f, 0.15f );
            d.EndColor       = glm::vec3( 0.7f, 0.1f, 0.0f );
            d.StartAlpha     = 0.9f;
            d.EndAlpha       = 0.0f;
        }
        void Smoke( Data& d )
        {
            d.SpawnRate      = 60.0f;
            d.Lifetime       = 3.0f;
            d.StartSpeed     = 60.0f;
            d.SpeedVariance  = 0.3f;
            d.Direction      = glm::vec3( 0.0f, 1.0f, 0.0f );
            d.ConeAngle      = 25.0f;
            d.Gravity        = glm::vec3( 0.0f, 40.0f, 0.0f );
            d.StartSize      = 40.0f;
            d.EndSize        = 130.0f;
            d.SizeCurvePower = 1.4f;
            d.StartColor     = glm::vec3( 0.32f, 0.32f, 0.34f );
            d.EndColor       = glm::vec3( 0.1f, 0.1f, 0.1f );
            d.StartAlpha     = 0.45f;
            d.EndAlpha       = 0.0f;
        }
        void Sparks( Data& d )
        {
            d.Material       = AdditiveMaterial();
            d.SpawnRate      = 220.0f;
            d.Lifetime       = 0.8f;
            d.StartSpeed     = 600.0f;
            d.SpeedVariance  = 0.5f;
            d.Direction      = glm::vec3( 0.0f, 1.0f, 0.0f );
            d.ConeAngle      = 65.0f;
            d.Gravity        = glm::vec3( 0.0f, -980.0f, 0.0f ); // Niagara's Gravity Force default
            d.StartSize      = 6.0f;
            d.EndSize        = 2.0f;
            d.SizeCurvePower = 1.0f;
            d.StartColor     = glm::vec3( 1.0f, 0.9f, 0.5f );
            d.EndColor       = glm::vec3( 1.0f, 0.4f, 0.1f );
            d.StartAlpha     = 1.0f;
            d.EndAlpha       = 0.0f;
        }
        void Magic( Data& d )
        {
            d.Material       = AdditiveMaterial();
            d.SpawnRate      = 150.0f;
            d.Lifetime       = 2.0f;
            d.StartSpeed     = 100.0f;
            d.SpeedVariance  = 0.6f;
            d.Direction      = glm::vec3( 0.0f, 1.0f, 0.0f );
            d.ConeAngle      = 180.0f; // omni
            d.Gravity        = glm::vec3( 0.0f, 20.0f, 0.0f );
            d.StartSize      = 15.0f;
            d.EndSize        = 0.0f;
            d.SizeCurvePower = 0.8f;
            d.StartColor     = glm::vec3( 0.5f, 0.4f, 1.0f );
            d.EndColor       = glm::vec3( 0.2f, 0.6f, 1.0f );
            d.StartAlpha     = 1.0f;
            d.EndAlpha       = 0.0f;
        }
        void Explosion( Data& d )
        {
            d.Material       = AdditiveMaterial();
            d.SpawnRate      = 500.0f;
            d.Lifetime       = 0.6f;
            d.StartSpeed     = 800.0f;
            d.SpeedVariance  = 0.6f;
            d.Direction      = glm::vec3( 0.0f, 1.0f, 0.0f );
            d.ConeAngle      = 180.0f;
            d.Gravity        = glm::vec3( 0.0f, -200.0f, 0.0f );
            d.StartSize      = 50.0f;
            d.EndSize        = 140.0f;
            d.SizeCurvePower = 2.0f;
            d.StartColor     = glm::vec3( 1.0f, 0.7f, 0.2f );
            d.EndColor       = glm::vec3( 0.3f, 0.0f, 0.0f );
            d.StartAlpha     = 1.0f;
            d.EndAlpha       = 0.0f;
        }

        // THE SLIDERS TAKE THEIR BOUNDS AND UNIT FROM THE REFLECTED PROPERTY (Components.hpp), the one place
        // they are declared — the Details panel reads the same Range/Units. A field without a declared Range
        // is refused out loud rather than given a guessed one.
        bool ReflectedSlider( const Reflection::TypeInfo& type, const char* field, const char* label, float* v,
                              int components = 1 )
        {
            const Reflection::FieldInfo* info = nullptr;
            for ( const Reflection::FieldInfo& f : type.Fields )
                if ( f.Name == field )
                    info = &f;
            if ( info == nullptr || !info->Meta.HasRange )
            {
                ImGui::TextDisabled( "%s: ParticleEmitterData.%s declares no Range", label, field );
                return false;
            }
            const Reflection::PropertyMetadata& meta  = info->Meta;
            const char*                         unit  = meta.IsLength ? "cm" : meta.Units.c_str();
            const bool                          whole = meta.RangeMax - meta.RangeMin >= 100.0f;
            const std::string                   format =
                 std::format( "{}{}{}", whole ? "%.0f" : "%.2f", *unit != '\0' ? " " : "", unit );
            if ( components == 3 )
                return ImGui::SliderFloat3( label, v, meta.RangeMin, meta.RangeMax, format.c_str() );
            return ImGui::SliderFloat( label, v, meta.RangeMin, meta.RangeMax, format.c_str() );
        }

        constexpr Preset kPresets[] = {
             { "Fire", Fire },   { "Smoke", Smoke },         { "Sparks", Sparks },
             { "Magic", Magic }, { "Explosion", Explosion },
        };

        // A horizontal colour-over-life bar (start -> end, alpha shown as a checkerboard-over blend).
        void GradientBar( const glm::vec3& startC, float startA, const glm::vec3& endC, float endA )
        {
            ImDrawList*  dl = ImGui::GetWindowDrawList();
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const float  w  = ImGui::GetContentRegionAvail().x;
            const float  h  = 22.0f;
            const ImVec2 p1 = ImVec2( p0.x + w, p0.y + h );
            const ImU32  cL = ImGui::ColorConvertFloat4ToU32( ImVec4( startC.r, startC.g, startC.b, startA ) );
            const ImU32  cR = ImGui::ColorConvertFloat4ToU32( ImVec4( endC.r, endC.g, endC.b, endA ) );
            dl->AddRectFilledMultiColor( p0, p1, cL, cR, cR, cL );
            dl->AddRect( p0, p1, IM_COL32( 120, 120, 120, 160 ) );
            ImGui::Dummy( ImVec2( w, h ) );
        }

        // Plots size(t) = mix(startSize, endSize, pow(t, power)) over the particle's life (t in 0..1).
        void SizeCurvePlot( float startSize, float endSize, float power )
        {
            ImDrawList*  dl = ImGui::GetWindowDrawList();
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const float  w  = ImGui::GetContentRegionAvail().x;
            const float  h  = 70.0f;
            const ImVec2 p1 = ImVec2( p0.x + w, p0.y + h );

            dl->AddRectFilled( p0, p1, IM_COL32( 18, 20, 24, 255 ) );
            dl->AddRect( p0, p1, IM_COL32( 90, 90, 96, 160 ) );

            const float maxS = std::max( { startSize, endSize, 0.0001f } );
            ImVec2      prev;
            for ( int i = 0; i <= 48; ++i )
            {
                const float  t  = static_cast<float>( i ) / 48.0f;
                const float  st = std::pow( t, power > 0.0f ? power : 1.0f );
                const float  s  = startSize + ( endSize - startSize ) * st;
                const float  ny = 1.0f - std::clamp( s / maxS, 0.0f, 1.0f );
                const ImVec2 pt( p0.x + t * w, p0.y + 4.0f + ny * ( h - 8.0f ) );
                if ( i > 0 )
                    dl->AddLine( prev, pt, IM_COL32( 90, 200, 255, 255 ), 2.0f );
                prev = pt;
            }
            dl->AddText( ImVec2( p0.x + 4.0f, p0.y + 2.0f ), IM_COL32( 150, 150, 155, 200 ), "size over life" );
            ImGui::Dummy( ImVec2( w, h ) );
        }
    } // namespace

    ParticleEditorPanel::ParticleEditorPanel( const SubjectId& subject, const std::string& displayName,
                                              const std::shared_ptr<::Desert::Core::Scene>& scene )
         : ISubjectDocument( displayName, subject ), m_Scene( scene )
    {
    }

    ECS::ParticleEmitterComponent* ParticleEditorPanel::ResolveComponent() const
    {
        const auto scene = m_Scene.lock();
        if ( !scene )
            return nullptr; // the scene this document was opened over has been closed

        const auto entOpt = scene->FindEntityByID( Subject().Owner );
        if ( !entOpt )
            return nullptr; // the entity was deleted

        auto& entity = entOpt->get();
        if ( !entity.HasComponent<ECS::ParticleEmitterComponent>() )
            return nullptr; // the component was removed from under the window

        return &entity.GetComponent<ECS::ParticleEmitterComponent>();
    }

    void ParticleEditorPanel::OnUIRender()
    {
        // THE FOUR EMPTY STATES ARE GONE. They existed because the window was about "whatever is selected",
        // so every way that could be nothing needed its own sentence. This window is about one emitter for
        // its whole life; a null here means the subject has just died, and the editor closes the document
        // for it on the same frame (EditorLayer::CloseDocumentsWhoseSubjectIsGone).
        ECS::ParticleEmitterComponent* emitter = ResolveComponent();
        if ( !emitter )
        {
            ImGui::TextDisabled( "This entity, its Particle Emitter or its scene is gone — closing." );
            return;
        }

        Data& d = emitter->Data;

        const Reflection::TypeInfo* type = Reflection::ReflectionRegistry::Get().Find( "ParticleEmitterData" );
        if ( type == nullptr )
        {
            ImGui::TextDisabled(
                 "ParticleEmitterData is not reflected — the generated registrations did not link." );
            return;
        }

        // Presets.
        ImGui::TextColored( ImVec4( 0.70f, 0.78f, 0.90f, 1.0f ), ICON_MDI_STAR " Presets" );
        for ( int i = 0; i < static_cast<int>( std::size( kPresets ) ); ++i )
        {
            if ( i > 0 )
                ImGui::SameLine();
            if ( ImGui::Button( kPresets[i].Name ) )
                kPresets[i].Apply( d );
        }
        ImGui::Separator();

        // Colour over life.
        ImGui::TextColored( ImVec4( 0.70f, 0.78f, 0.90f, 1.0f ), ICON_MDI_CREATION " Colour over life" );
        GradientBar( d.StartColor, d.StartAlpha, d.EndColor, d.EndAlpha );
        glm::vec4 start( d.StartColor, d.StartAlpha );
        glm::vec4 end( d.EndColor, d.EndAlpha );
        if ( ImGui::ColorEdit4( "Start", &start.x, ImGuiColorEditFlags_AlphaBar ) )
        {
            d.StartColor = glm::vec3( start );
            d.StartAlpha = start.a;
        }
        if ( ImGui::ColorEdit4( "End", &end.x, ImGuiColorEditFlags_AlphaBar ) )
        {
            d.EndColor = glm::vec3( end );
            d.EndAlpha = end.a;
        }
        ImGui::Separator();

        // Size over life.
        ImGui::TextColored( ImVec4( 0.70f, 0.78f, 0.90f, 1.0f ), ICON_MDI_CHART_BELL_CURVE " Size over life" );
        SizeCurvePlot( d.StartSize, d.EndSize, d.SizeCurvePower );
        ReflectedSlider( *type, "StartSize", "Start Size", &d.StartSize );
        ReflectedSlider( *type, "EndSize", "End Size", &d.EndSize );
        ReflectedSlider( *type, "SizeCurvePower", "Curve Power", &d.SizeCurvePower );
        ImGui::Separator();

        // Emission.
        ImGui::TextColored( ImVec4( 0.70f, 0.78f, 0.90f, 1.0f ), ICON_MDI_FOUNTAIN " Emission" );
        ImGui::Checkbox( "Enabled", &d.Enabled );
        ImGui::SameLine();
        ImGui::Checkbox( "Looping", &d.Looping );
        ImGui::SliderInt( "Max Particles", &d.MaxParticles, 1, 100000 );
        ReflectedSlider( *type, "SpawnRate", "Spawn Rate", &d.SpawnRate );
        ReflectedSlider( *type, "Lifetime", "Lifetime", &d.Lifetime );
        ReflectedSlider( *type, "LifetimeVariance", "Lifetime Var", &d.LifetimeVariance );
        ImGui::Separator();

        // Motion.
        ImGui::TextColored( ImVec4( 0.70f, 0.78f, 0.90f, 1.0f ), ICON_MDI_ARROW_TOP_RIGHT " Motion" );
        ReflectedSlider( *type, "StartSpeed", "Start Speed", &d.StartSpeed );
        ReflectedSlider( *type, "SpeedVariance", "Speed Var", &d.SpeedVariance );
        ImGui::SliderFloat3( "Direction", &d.Direction.x, -1.0f, 1.0f );
        ReflectedSlider( *type, "ConeAngle", "Cone Angle", &d.ConeAngle );
        ReflectedSlider( *type, "Gravity", "Gravity", &d.Gravity.x, 3 );
        ImGui::Separator();

        // Stats.
        const int maxAlive = std::min(
             d.MaxParticles, static_cast<int>( Common::Math::RoundToNearest( d.SpawnRate * d.Lifetime ) ) );
        ImGui::TextDisabled( "~%d particles alive (rate x lifetime, capped at Max).", std::max( 0, maxAlive ) );
    }

} // namespace Desert::Editor
