// DesertAsset {"Kind":"Shader","Guid":"cd0bd94d209f27d8576346de2fd1d52b","Versions":{"SHDR":1},"Dependencies":[]}
// THE UI MATERIAL ERROR FILL — what an element draws when the material slot names something the UI
// path cannot execute: a handle that resolves to no `.demat`, a `.demat` whose shader is not
// registered, or a shader whose Domain is not UI.
//
// It exists because the alternative is the failure mode UE shipped and never fixed: a wrong-domain UMG
// material has no compiled Slate permutation, TryGetShaders fails, the batch is dropped, and the widget
// renders SILENTLY NOTHING with no error and no marker (SlateRHIRenderingPolicy.cpp:1150-1175, and
// Image.cpp:270 still carries `//TODO UMG Check if the material can be used with the UI`). An invisible
// element and a correctly-invisible element are indistinguishable, so the author debugs the wrong thing.
// Here the element is loud, the log names the handle and the reason, and the two states are told apart
// by looking.
//
// Magenta on a diagonal hatch rather than flat magenta: flat magenta is a colour somebody may have
// authored, the hatch is not. The stripes are in element-local UV so they scale with the element and
// stay visible at any size.
Shader "UIMatError"
{
    Domain UI

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
            // 12 stripes across the diagonal of the element, whatever its pixel size.
            const float stripes = 12.0;
            float       band    = fract( ( v_TexCoord.x + v_TexCoord.y ) * stripes * 0.5 );
            vec3        color   = band < 0.5 ? vec3( 1.0, 0.0, 1.0 ) : vec3( 0.15, 0.0, 0.15 );
            o_Color             = vec4( color, 1.0 );
        }
    }
}
