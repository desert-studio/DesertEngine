// DesertAsset {"Kind":"Shader","Guid":"3929ff33c4ed4add85b611e12c6a5812","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DefaultLightFunction"
{
    // THE ENGINE'S LIGHT FUNCTION (UE MD_LightFunction; LIGHT-FUNCTIONS): what System::LightFunctionAtlas draws into
    // a light's tile of the light function atlas ("Lights: Light Function Atlas"), and the form every `Domain
    // LightFunction` shader copies: the rows buffer, the push block and the vertex stage below are the contract,
    // the fragment is the look. One quad per light from 6 procedural vertices over its tile (LightFunctionRow.TileUV);
    // the fragment turns its tile coordinate into what UE hands a light function material
    // (Common/LightFunction.glslh DesertLightFunctionInputs) and writes the light's filter colour (1 = unchanged).
    //
    // The look: stripes across the light's projection, LightVector.yz / LightVector.x (a spot's cone plane), so a
    // spot with this function throws bars on a wall; their count, width and drift are the material's.
    Domain LightFunction

    Properties Binding(1)
    {
        Color Tint        ("Tint")                                  = (1.0, 1.0, 1.0, 1.0)
        float StripeCount ("Stripes across the cone", Range(1,64))  = 8.0
        float StripeDuty  ("Lit share of a stripe", Range(0,1))     = 0.5
        float Softness    ("Edge softness", Range(0,0.5))           = 0.05
        float DarkLevel   ("Dark stripe level", Range(0,1))         = 0.0
        float Drift       ("Drift (stripes/s)", Range(-4,4))        = 0.0
    }

    State
    {
        Cull None
    }

    Vertex
    {
        #include <Common/LightFunction.glslh>
        #include <Common/ScreenUV.glslh>

        // "LightFunctions.Rows" (System::LightFunctionRowGpu), baked per frame on the CPU.
        ReadBuffer(0) LightFunctionRowsBuffer
        {
            LightFunctionRow LightFunctionRows[];
        };
        // The fragment's engine push block (MaterialTransport.glslh, injected for its `Material` row): one pipeline
        // has one block, so the vertex stage declares the same one. PrimitiveIndex carries the draw's first row;
        // the instance index walks the run (LightFunctionAtlas::Render: one run = one draw).
        #include <Common/MaterialTransport.glslh>

        layout(location = 0) out vec2 v_Local;
        layout(location = 1) out flat vec4 v_Forward;
        layout(location = 2) out flat vec4 v_Tangent;
        layout(location = 3) out flat vec4 v_Bitangent;
        layout(location = 4) out flat vec4 v_Position;
        layout(location = 5) out flat vec4 v_InvScale;

        // Two triangles over the unit square (x right, y down the tile's texel rows).
        const vec2 kLightFunctionQuad[6] = vec2[6]( vec2( 0.0, 0.0 ), vec2( 0.0, 1.0 ), vec2( 1.0, 0.0 ),
                                                    vec2( 1.0, 0.0 ), vec2( 0.0, 1.0 ), vec2( 1.0, 1.0 ) );

        void main()
        {
            const LightFunctionRow row    = LightFunctionRows[m_PushConstants.PrimitiveIndex + uint( gl_InstanceIndex )];
            const vec2             corner = kLightFunctionQuad[uint( gl_VertexIndex ) % 6u];
            v_Local                       = corner * 2.0 - 1.0;
            v_Forward                     = row.Forward;
            v_Tangent                     = row.Tangent;
            v_Bitangent                   = row.Bitangent;
            v_Position                    = row.Position;
            v_InvScale                    = row.InvScale;
            // The tile in atlas texture coordinates, through the one screen mapping every pass shares.
            gl_Position = vec4( ScreenUVToNdc( mix( row.TileUV.xy, row.TileUV.zw, corner ) ), 0.0, 1.0 );
        }
    }

    Fragment
    {
        #include <Common/LightFunction.glslh>

        layout(location = 0) in vec2 v_Local;
        layout(location = 1) in flat vec4 v_Forward;
        layout(location = 2) in flat vec4 v_Tangent;
        layout(location = 3) in flat vec4 v_Bitangent;
        layout(location = 4) in flat vec4 v_Position;
        layout(location = 5) in flat vec4 v_InvScale;

        Out(0) vec4 o_LightFunction;

        void main()
        {
            LightFunctionRow row;
            row.Forward   = v_Forward;
            row.Tangent   = v_Tangent;
            row.Bitangent = v_Bitangent;
            row.Position  = v_Position;
            row.InvScale  = v_InvScale;
            row.TileUV    = vec4( 0.0 );
            const LightFunctionInputs inputs = DesertLightFunctionInputs( row, v_Local );

            // The projection plane one unit along the light (behind it the plane mirrors; a spot never shades
            // there, the cone factor is 0).
            const vec2  plane  = inputs.LightVector.yz / max( abs( inputs.LightVector.x ), 1.0e-4 );
            const float phase  = fract( plane.x * Material.StripeCount * 0.5 + inputs.Time * Material.Drift );
            const float edge   = max( Material.Softness, 1.0e-4 );
            const float lit    = smoothstep( 0.0, edge, phase ) *
                              ( 1.0 - smoothstep( Material.StripeDuty, Material.StripeDuty + edge, phase ) );
            const float level  = mix( Material.DarkLevel, 1.0, lit );
            o_LightFunction    = vec4( Material.Tint.rgb * level, 1.0 );
        }
    }
}
