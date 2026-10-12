// DesertAsset {"Kind":"Shader","Guid":"9a7faa02cb9a4624ba264b8527e6dde8","Versions":{"SHDR":1},"Dependencies":[]}
// A UI-domain material: a radial glow (a sun's halo, a light pool, a soft spot behind an icon).
//
// A glow built from stacked discs is a staircase: N discs give N visible bands, however their alphas are
// chosen. The falloff is a function of the distance from the element's centre, so it is evaluated per
// pixel here and is continuous at any size.
//
// The element's rect is the glow's extent: the ellipse inscribed in it is where the glow reaches zero.
//   Inner     the fraction of the radius that stays at full strength      (the flat core)
//   Exponent  the shape of the ramp from Inner to the edge: 1 = linear,   (the spread of the light)
//             >1 = concentrated toward the core, <1 = wide and flat
//   Tint      multiplies the whole fill                                    (colour and peak strength)
Shader "UIMatRadialGlow"
{
    Domain UI

    Properties Binding(1) TextureBinding(2)
    {
        Color Tint     ("Tint")                         = (1.0, 1.0, 1.0, 1.0)
        float Inner    ("Inner Radius", Range(0,0.99))  = 0.0
        float Exponent ("Falloff Exponent", Range(0.25,8)) = 1.0
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
            // 0 at the centre, 1 on the inscribed ellipse.
            float r    = length( v_TexCoord - vec2( 0.5 ) ) * 2.0;
            float ramp = clamp( ( 1.0 - r ) / max( 1.0 - u_Material.Inner, 1e-4 ), 0.0, 1.0 );
            float glow = pow( ramp, max( u_Material.Exponent, 1e-3 ) );

            o_Color = vec4( v_Color.rgb * u_Material.Tint.rgb, v_Color.a * u_Material.Tint.a * glow );
        }
    }
}
