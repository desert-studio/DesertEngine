// DesertAsset {"Kind":"Shader","Guid":"d0265186322d672567a7ed58bbaf7625","Versions":{"SHDR":1},"Dependencies":[]}
// A UI-domain material: the radial wipe (cooldown sweep / masked reveal / radial gauge).
//
// It ships because a domain with no material in it is a domain nobody can check. This is the shape the
// fixed effect list on UIPanelData could never express — the look is an EXPRESSION over the element's
// own UV, not one more boolean beside Glow and Shadow — and it is the roadmap's "masked reveal" and
// "procedural fill" lines in one program.
//
// Every parameter below reaches a pixel, and each reaches a DIFFERENT one, which is what makes them
// separable by a positive control rather than by reading this file:
//   Progress  moves the swept edge round the circle          (angular)
//   Feather   softens that edge and nothing else             (only within Feather of the edge)
//   Tint      multiplies the whole fill                      (everywhere the fill is)
//   Mask      multiplies alpha from a texture                (wherever the texture is not white)
Shader "UIMatRadialWipe"
{
    Domain UI

    Properties Binding(1) TextureBinding(2)
    {
        Color     Tint     ("Tint")                          = (0.20, 0.75, 1.0, 1.0)
        float     Progress ("Progress", Range(0,1))          = 1.0
        float     Feather  ("Edge Feather", Range(0,0.5))    = 0.02
        float     StartAngle ("Start Angle (turns)", Range(0,1)) = 0.0
        Texture2D Mask     ("Mask")
    }

    State
    {
        // Depth, culling and the load render pass belong to the UI PHASE and are set by Render2D;
        // what a UI material owns is how its fill composites. Only what is declared here is applied
        // (Graphic::ApplyShaderRenderState), so there is no state below that the UI path ignores.
        Cull None
        Blend SrcAlpha OneMinusSrcAlpha
    }

    Vertex
    {
        #include <Common/UIVertex.glslh>
    }

    Fragment
    {
        In(0) vec2 v_TexCoord;
        In(1) vec4 v_Color;

        Out(0) vec4 o_Color;

        void main()
        {
            // Sweep clockwise from StartAngle, measured in TURNS so the parameter reads 0..1 like
            // Progress does and the two can be keyed against each other without a conversion.
            vec2  p     = v_TexCoord - vec2( 0.5 );
            float turn  = fract( atan( p.x, -p.y ) / 6.28318530718 - u_Material.StartAngle );

            // The swept region is turn < Progress. Feather is applied in turns, so the soft edge is a
            // constant ANGLE and does not fatten as the element grows.
            float edge  = max( u_Material.Feather, 1e-4 );
            float sweep = 1.0 - smoothstep( u_Material.Progress - edge, u_Material.Progress, turn );

            // Progress 1 must be a FULLY closed disc: without this the smoothstep above still eats a
            // feather-wide wedge at turn ~= 1, so "full" would render with a seam.
            sweep = max( sweep, step( 1.0, u_Material.Progress ) );

            vec4 mask = texture( Mask, v_TexCoord );

            o_Color = vec4( v_Color.rgb * u_Material.Tint.rgb * mask.rgb,
                            v_Color.a * u_Material.Tint.a * mask.a * sweep );
        }
    }
}
