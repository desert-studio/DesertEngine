// DesertAsset {"Kind":"Shader","Guid":"d7926d5472ac436a812d51b09fc07d9d","Versions":{"SHDR":1},"Dependencies":[]}
// The engine's LIT translucent particle sprite (UE's lit particle material: ShadingModel DefaultLit, Translucency
// Lighting Mode Volumetric NonDirectional): smoke, dust, steam - a medium that is SEEN by the light it scatters,
// not one that glows. `Usage ParticleSprites` gives it the ParticleSprite.Forward cell; its lit model makes the
// sprite pass header (Mesh/Surface/Pass_ParticleSprite.glslh) shade it as an isotropic scatterer of albedo
// BaseColor through the view's translucency lighting volume (sun through its cascades and the cloud shadow, the
// light grid's local lights through their shadows, the sky through its occlusion), so a puff in the barn's shade
// is darker than one in the sun. BlendMode Translucent = laid OVER the scene (SrcAlpha, OneMinusSrcAlpha).
// The shape is the default sprite's radial soft dot; Emissive adds a glow of its own on top of the light.
Shader "ParticleSpriteLit"
{
    Domain Surface
    Usage ParticleSprites
    BlendMode Translucent
    // UE bResponsiveAA: a sprite writes no velocity of its own; its coverage tells TAA to trust the current frame.
    ResponsiveAA

    Properties Binding(2)
    {
        // The scatterer's albedo, times the particle's simulated colour.
        Color Tint ("Tint", Category("Surface")) = (1, 1, 1, 1)
        // Self-emission added to the scattered light (0 = lit only, UE's lit smoke).
        Float EmissiveScale ("Emissive", Range(0,64), Category("Surface")) = 0
    }

    State
    {
        Cull None
        ZTest LEqual
        ZWrite Off
    }

    ShadingModel DefaultLit

    Surface
    {
        SurfaceOutput EvaluateSurface( SurfaceInput i )
        {
            // 1 at the centre of the quad, 0 at its inscribed circle, smooth in between.
            const float r       = length( i.UV0 * 2.0 - 1.0 );
            const float disc    = 1.0 - smoothstep( 0.0, 1.0, r );
            const vec4  colour  = i.Particle.Color * Material.Tint;
            SurfaceOutput s     = DefaultSurfaceOutput();
            s.BaseColor         = clamp( colour.rgb, vec3( 0.0 ), vec3( 1.0 ) );
            s.Emissive          = colour.rgb * Material.EmissiveScale;
            s.Opacity           = colour.a * disc;
            return s;
        }
    }
}
