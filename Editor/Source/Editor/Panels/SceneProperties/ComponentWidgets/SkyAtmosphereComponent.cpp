#include <Editor/Core/ImGuiUtilities.hpp>

#include <ImGui/imgui.h>

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>
#include <Editor/Panels/PropertyEditor/PropertyEditorBuilder.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/System/SystemRules.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Graphic/SkyPresets.hpp>

#include <cmath>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    namespace
    {
        ImU32 ToU32( const glm::vec3& c )
        {
            return ImGui::ColorConvertFloat4ToU32( ImVec4( c.r, c.g, c.b, 1.0f ) );
        }

        // A ramp of the sky's OWN authored colours (zenith -> horizon -> ground) with the sun marked at
        // its current elevation. Deliberately NOT a render of the sky shader: it is a legend for the
        // colour fields below, readable while you drag them, and it costs nothing.
        void DrawSkyRamp( const ECS::SkyAtmosphereData& sky, float sunElevationDeg, bool haveSun )
        {
            const float  width  = ImGui::GetContentRegionAvail().x;
            const float  height = 96.0f;
            const ImVec2 p0     = ImGui::GetCursorScreenPos();
            const ImVec2 p1( p0.x + width, p0.y + height );
            ImDrawList*  dl = ImGui::GetWindowDrawList();

            // Horizon sits 2/3 down, like the sky itself: most of the frame is sky.
            const float horizonY = p0.y + height * 0.66f;

            // Low sun washes the horizon band with the sunset tint — the same idea the sky shader uses,
            // so the strip tracks the setting you are actually editing.
            const float     sunset  = haveSun ? glm::clamp( 1.0f - sunElevationDeg / 20.0f, 0.0f, 1.0f ) : 0.0f;
            const glm::vec3 horizon = glm::mix( sky.HorizonColor, sky.SunsetColor, sunset * 0.8f );

            dl->AddRectFilledMultiColor( p0, ImVec2( p1.x, horizonY ), ToU32( sky.ZenithColor ),
                                         ToU32( sky.ZenithColor ), ToU32( horizon ), ToU32( horizon ) );
            dl->AddRectFilled( ImVec2( p0.x, horizonY ), p1, ToU32( sky.GroundColor ) );
            dl->AddLine( ImVec2( p0.x, horizonY ), ImVec2( p1.x, horizonY ), IM_COL32( 0, 0, 0, 90 ) );
            dl->AddRect( p0, p1, ImGui::GetColorU32( ImGuiCol_Border ) );

            if ( haveSun )
            {
                // Elevation 90 = top of the strip, 0 = the horizon line, below that = under the ground.
                const float  t = glm::clamp( sunElevationDeg / 90.0f, -1.0f, 1.0f );
                const float  y = t >= 0.0f ? glm::mix( horizonY, p0.y, t ) : glm::mix( horizonY, p1.y, -t );
                const ImVec2 c( p0.x + width * 0.5f, y );
                if ( sunElevationDeg >= 0.0f )
                {
                    dl->AddCircleFilled( c, 9.0f, ToU32( sky.SunColor ), 20 );
                    dl->AddCircle( c, 9.0f, IM_COL32( 0, 0, 0, 60 ), 20 );
                }
                else
                {
                    dl->AddCircle( c, 9.0f, IM_COL32( 150, 160, 190, 200 ), 20, 2.0f );
                }
            }

            ImGui::Dummy( ImVec2( width, height ) );
            if ( !haveSun )
                ImGui::TextDisabled( "No directional light in the scene — sun position unknown" );
        }

        // The preset picker. ActivePreset is a ReadOnly reflected field — the grid shows what the palette
        // came from, it does not let you choose from there — so choosing happens here, in the one control
        // that also has to write the thirteen palette fields behind it.
        //
        // The apply is ONE undo step over the whole entity, not the per-field byte copy the reflected grid
        // records: undoing a preset has to put back thirteen values, and undoing a one-byte enum would
        // leave the new palette wearing the old name.
        void DrawPresetRow( ECS::Entity& entity, ECS::SkyAtmosphereData& sky )
        {
            ImGui::TextUnformatted( "Preset" );
            ImGui::SameLine();
            ImGui::PushItemWidth( -1 );
            if ( ImGui::BeginCombo( "##skypreset", Graphic::SkyPresetName( sky.ActivePreset ) ) )
            {
                for ( const Graphic::SkyPresetEntry& entry : Graphic::kSkyPresets )
                {
                    if ( ImGui::Selectable( entry.Name, entry.Id == sky.ActivePreset ) )
                    {
                        const ECS::SkyPreset picked = entry.Id;
                        Commands::MutateEntityUndoable( entity.GetComponent<ECS::UUIDComponent>().UUID,
                                                        [picked, &sky]
                                                        {
                                                            Graphic::ApplySkyPreset( picked, sky );
                                                            sky.ActivePreset = picked;
                                                        } );
                    }
                    if ( entry.Id == sky.ActivePreset )
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PopItemWidth();
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Replaces the sky colours and the sun's look. Time of day, the "
                                   "environment-bake settings and the planet radius are left alone." );
        }
    } // namespace

    // All 23 fields of the atmosphere are AUTO-GENERATED from its REFLECT()/PROPERTY() metadata
    // (PropertyEditorBuilder). This entry is custom only for the two things reflection cannot express:
    // the colour ramp — which needs the scene's sun elevation, and that lives in the sun light's transform,
    // not in a field — and the IBL bake action.
    //
    // Written out rather than using DESERT_REGISTER_CUSTOM_COMPONENT because that macro cannot fill
    // ReflectedTypeName / DataPtr, and without them the collapsed-header summary (the Summary fields) and
    // the Details search filter would silently skip this component.
    ComponentEditorEntry MakeSkyAtmosphereEntry()
    {
        using C = ::Desert::ECS::SkyAtmosphereComponent;
        ComponentEditorEntry e;
        e.Name              = "Sky Atmosphere";
        e.CanRemove         = true;
        e.ReflectedTypeName = "SkyAtmosphereData";
        e.Has               = []( ECS::Entity& en ) { return en.HasComponent<C>(); };
        e.Add               = []( ECS::Entity& en ) { en.AddComponent<C>(); };
        e.Remove            = []( ECS::Entity& en ) { en.RemoveComponent<C>(); };
        e.DataPtr           = []( ECS::Entity& en ) -> void* { return &en.GetComponent<C>().Data; };
        e.Draw              = []( ECS::Entity& en, ::Desert::Core::Scene* scene, const ComponentEditContext& ctx )
        {
            auto& atmosphere = en.GetComponent<C>();

            // The sun's elevation comes from the scene's directional light (its Translation is the
            // direction light TRAVELS, so the sun is the other way).
            float sunElevation = 0.0f;
            bool  haveSun      = false;
            if ( scene )
            {
                auto view = scene->GetRegistry().view<ECS::DirectionLightComponent, ECS::TransformComponent>();
                for ( const auto lightEntity : view )
                {
                    const glm::vec3 travel = view.template get<ECS::TransformComponent>( lightEntity ).Translation;
                    // Through the rules, not a second copy of them: the epsilon was spelled 1e-4f here
                    // and the negation was open-coded, while SystemRules.hpp calls itself "the engine's
                    // ONE negation" and says every one of them goes through it. It did not.
                    if ( ECS::Rules::IsSunDirectionValid( travel ) )
                    {
                        const glm::vec3 toSun = ECS::Rules::AtmosphereSunDirection( travel );
                        sunElevation          = glm::degrees( std::asin( glm::clamp( toSun.y, -1.0f, 1.0f ) ) );
                        haveSun               = true;
                        break;
                    }
                }
            }

            if ( !ctx.FieldFilter )
            {
                DrawSkyRamp( atmosphere.Data, sunElevation, haveSun );
                DrawPresetRow( en, atmosphere.Data );
            }

            // Snapshot the palette AFTER the preset row, so applying a preset is not then mistaken for a
            // hand edit of the values it just wrote.
            const Graphic::SkyPresetValues paletteBefore = Graphic::ExtractSkyPresetValues( atmosphere.Data );

            const bool changed = PropertyEditorBuilder::Draw( &atmosphere.Data, "SkyAtmosphereData",
                                                              ctx.AssetMgr(), ctx.UIHelper, ctx.FieldFilter );

            // The preset name follows the VALUES, and only the palette values. Gating on the palette
            // rather than on "some widget reported a change" is what keeps dragging the time of day or
            // the rebake threshold from clearing a name that is still true; deriving the name with
            // MatchSkyPreset rather than forcing Custom means dialling a colour back also restores it.
            if ( changed && !( Graphic::ExtractSkyPresetValues( atmosphere.Data ) == paletteBefore ) )
                atmosphere.Data.ActivePreset = Graphic::MatchSkyPreset( atmosphere.Data );

            // ── Environment lighting (IBL) bake — the sky's contribution to scene lighting/reflections is
            //    baked into cubemaps; this is a heavy device-idle op, so it's an explicit action. ──
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextUnformatted( "Environment Lighting (IBL)" );
            ImGui::TextDisabled( "Bake the sky into the irradiance / reflection maps used by lit surfaces." );
            if ( ImGui::Button( "Bake Sky IBL", ImVec2( ImGui::GetContentRegionAvail().x, 0 ) ) )
                atmosphere.RequestBake = true;
        };
        return e;
    }
} // namespace Desert::Editor

namespace
{
    const int _desert_sky_atmosphere_component_reg =
         ::Desert::Editor::ComponentWidgetRegistry::Get().Register( ::Desert::Editor::MakeSkyAtmosphereEntry() );
} // namespace
