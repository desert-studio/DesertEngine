#pragma once

#include <Common/Core/AssetHandle.hpp>
#include <CoreReflection/ReflectionMacros.hpp>
#include <UI/Args/ArgKind.hpp>

#include <glm/glm.hpp>

// The filled, bordered, optionally textured rectangle most controls draw as their background.
// Framework data (Desert::UI): the ECS wraps each *Data in a UI*Component (ECS/Components.hpp); the
// reflected type name is the short one, so the scene format does not see the namespace.

namespace Desert::UI
{
    // A filled (rounded) rectangle — the background of a panel / window / button.
    struct UIPanelData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::Panel;

        PROPERTY( DisplayName( "Color" ), Category( "UI Panel" ), Color )
        glm::vec3 Color = glm::vec3( 0.15f, 0.16f, 0.2f );

        PROPERTY( DisplayName( "Opacity" ), Category( "UI Panel" ), Range( 0.0f, 1.0f ) )
        float Opacity = 0.92f;

        PROPERTY( DisplayName( "Corner Radius" ), Category( "UI Panel" ), Range( 0.0f, 64.0f ) )
        float CornerRadius = 6.0f;

        // Frosted glass: fill the panel with the BLURRED scene behind it instead of a flat colour, with
        // Color/Opacity acting as the tint over that blur (Opacity 1 = an ordinary opaque panel again).
        // 0 = off; higher = blurrier (the renderer maps it onto its backdrop blur pyramid).
        PROPERTY( DisplayName( "Backdrop Blur" ), Category( "UI Panel" ), Range( 0.0f, 1.0f ),
                  Tooltip( "Fill with the blurred scene behind the panel (frosted glass). 0 = off." ) )
        float BackdropBlur = 0.0f;

        // Asset<TextureAsset> is what tells the SERIALIZER which asset type this handle names. Without it
        // the resolver is handed an empty type string, falls through its table to the mesh lookup, finds
        // no mesh under a texture's handle and writes the field out as an EMPTY STRING — so the slot was
        // silently cleared by every save. (It is not what makes the field a handle: DesertHeaderTool maps
        // `Common::AssetHandle` to FieldType::AssetHandle by its spelling, which is why the Details panel
        // could always offer a picker for it. The setting could be authored and could not be kept.)
        PROPERTY( DisplayName( "Sprite" ), Category( "UI Panel" ), Asset<TextureAsset>, Preview )
        Common::AssetHandle Sprite; // optional background image, tinted by Color * Opacity. Unset = flat colour.

        PROPERTY( DisplayName( "Sprite Border L/T/R/B" ), Category( "UI Panel" ) )
        glm::vec4 SpriteBorder = glm::vec4( 0.0f ); // 9-slice: source-px borders kept unstretched (0 = stretch)

        // --- Material (Ю11) -------------------------------------------------------------------------------
        // A `.demat` whose shader declares `Domain UI` becomes this panel's FILL, in place of the colour,
        // the gradient, the sprite, the video and the glass. Its parameters are the shader's own, edited
        // in the Material Editor like every other material's, and they reach the pixel through THE
        // parameter transport (one row of `Materials[]`, named by a push constant) — so an author extends
        // the look with an expression instead of waiting for one more boolean to be added below.
        //
        // WHY IT REPLACES THE FILL RATHER THAN COMPOSING WITH IT. The panel's Color * Opacity still
        // travels, as the vertex colour the material may multiply by (the shipped UIMatRadialWipe does),
        // so nothing is lost; but a fill that were BOTH a sprite and a material would need the batcher to
        // key on two resources at once for one quad. Glow, Shadow and the Ring are separate quads and go
        // on composing around it exactly as they do around a sprite.
        //
        // A handle the UI path cannot execute — deleted asset, unregistered shader, a Surface material
        // dropped in here — draws the magenta hatch of `UIMatError` and is named in the log. It does NOT
        // fall back to the flat colour: a panel that quietly looks unmaterialised is the one failure mode
        // UE shipped and never fixed.
        PROPERTY( DisplayName( "Material" ), Category( "UI Material" ), Asset<MaterialAsset> )
        Common::AssetHandle Material;

        // WebM (AV1 + Opus) .webm streamed into this panel (loops, tinted by Color*Opacity). Drag a .webm from
        // the Content Browser. Overrides the sprite/gradient fill while set. Unset = no video. (Handle<->path
        // owned by the VideoService.) The clip's Opus track plays through the panel's own audio output — the
        // MediaSoundComponent of UE's Media Framework — at Video Volume; Video Muted is the explicit "picture
        // only" switch (the sound path still exists and is torn down, not skipped by a missing sink).
        PROPERTY( DisplayName( "Video" ), Category( "UI Panel" ), Asset<VideoAsset> )
        Common::AssetHandle Video;
        PROPERTY( DisplayName( "Video Volume" ), Category( "UI Panel" ), Range( 0.0f, 1.0f ) )
        float VideoVolume = 1.0f;
        PROPERTY( DisplayName( "Video Muted" ), Category( "UI Panel" ) )
        bool VideoMuted = false;

        // --- Shape (Phase C) ------------------------------------------------------------------------------
        PROPERTY( DisplayName( "Circle" ), Category( "UI Panel" ) )
        bool Circle = false; // force a perfect circle/ellipse (rounding = half the shorter side) at any size —
                             // for avatars, badges, status dots. Overrides Corner Radius.

        PROPERTY( DisplayName( "Ring Width" ), Category( "Ring" ), Range( 0.0f, 24.0f ) )
        float RingWidth = 0.0f; // >0 draws a gradient ring hugging the (circular or rounded) edge
        PROPERTY( DisplayName( "Ring Color A" ), Category( "Ring" ), Color )
        glm::vec3 RingColorA = glm::vec3( 1.0f, 0.48f, 0.15f );
        PROPERTY( DisplayName( "Ring Color B" ), Category( "Ring" ), Color )
        glm::vec3 RingColorB = glm::vec3( 0.18f, 0.89f, 1.0f ); // sweeps A -> B -> A around the ring

        // --- Animation (Phase F) --------------------------------------------------------------------------
        PROPERTY( DisplayName( "Pulse" ), Category( "Animation" ) )
        bool Pulse = false; // breathe the opacity between Pulse Min and full — a live "online" dot / CTA glow
        PROPERTY( DisplayName( "Pulse Speed" ), Category( "Animation" ), Range( 0.1f, 10.0f ), Units( "rad/s" ),
                  EditCondition( "Pulse" ) )
        float PulseSpeed = 2.5f; // radians/sec of the sine
        PROPERTY( DisplayName( "Pulse Min" ), Category( "Animation" ), Range( 0.0f, 1.0f ),
                  EditCondition( "Pulse" ) )
        float PulseMin = 0.35f; // opacity floor of the breathe

        // Effects (Phase 4). All in design px; scaled by the canvas scale at draw time.
        PROPERTY( DisplayName( "Use Gradient" ), Category( "Effects" ) )
        bool UseGradient = false; // vertical Color (top) -> Gradient Color (bottom); ignored when a sprite is set
        PROPERTY( DisplayName( "Gradient Color" ), Category( "Effects" ), Color, EditCondition( "UseGradient" ) )
        glm::vec3 GradientColor = glm::vec3( 0.10f, 0.11f, 0.14f );
        PROPERTY( DisplayName( "Border Width" ), Category( "Effects" ), Range( 0.0f, 16.0f ) )
        float BorderWidth = 0.0f; // 0 = no border
        PROPERTY( DisplayName( "Border Color" ), Category( "Effects" ), Color )
        glm::vec3 BorderColor = glm::vec3( 0.0f );
        PROPERTY( DisplayName( "Shadow" ), Category( "Effects" ) )
        bool Shadow = false;
        PROPERTY( DisplayName( "Shadow Color" ), Category( "Effects" ), Color )
        glm::vec3 ShadowColor = glm::vec3( 0.0f );
        PROPERTY( DisplayName( "Shadow Offset" ), Category( "Effects" ) )
        glm::vec2 ShadowOffset = glm::vec2( 3.0f, 4.0f );
        PROPERTY( DisplayName( "Glow" ), Category( "Effects" ) )
        bool Glow = false; // cheap ImDrawList glow: layered expanded rects behind, alpha falloff
        PROPERTY( DisplayName( "Glow Color" ), Category( "Effects" ), Color )
        glm::vec3 GlowColor = glm::vec3( 0.3f, 0.6f, 1.0f );
        PROPERTY( DisplayName( "Glow Size" ), Category( "Effects" ), Range( 0.0f, 48.0f ) )
        float GlowSize = 12.0f;
    };
} // namespace Desert::UI
