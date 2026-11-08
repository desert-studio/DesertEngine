// DesertAsset {"Kind":"Shader","Guid":"bb83d40865ae4d408ed5de2311038a40","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DefaultDecal"
{
    // THE ENGINE'S DECAL (UE DefaultDeferredDecalMaterial; DECALS-DBUFFER): what a DecalComponent with no material
    // draws in "Deferred: Decals" (System::DecalRenderer). One unit box [-1, 1]^3 per decal from 36 procedural
    // vertices (no vertex stream), its back faces only and no depth attachment, so every covered pixel shades
    // once, with the camera inside the box too. The fragment reconstructs the receiver from the full early-Z depth
    // (SceneDepthTexture), takes it into the box (DecalRow.ClipToDecal) and writes the premultiplied DBuffer
    // (Common/DBuffer.glslh); the pipeline blends rgb = src + dst x (1 - src.a), alpha = dst x (1 - src.a).
    //
    // The look is procedural so a decal with no asset shows every channel it writes: diagonal stripes of two
    // colours (DBufferA), a grid of domes as a bump normal in the decal's own frame (DBufferB), and a rougher
    // second stripe (DBufferC). A content decal is a `Domain DeferredDecal` shader of its own with this layout.
    Domain DeferredDecal
    DecalChannels BaseColor Normal Roughness

    Vertex
    {
        // One row of "Decals.Rows" (System::DecalRowGpu, 208 bytes std430), baked per view on the CPU.
        struct DecalRow
        {
            mat4 DecalToClip; // the box's corners to clip space (jittered view-projection x DecalToWorld)
            mat4 ClipToDecal; // a depth texel's clip position into the box
            vec4 AxisX;       // the box's world axes, normalised: x / y the texture plane, z the projection axis
            vec4 AxisY;
            vec4 AxisZ;
            vec4 Params; // x opacity (the CPU fade), y edge fade, z the channel mask (DecalChannel bits, float)
        };
        ReadBuffer(2) DecalRowsBuffer
        {
            DecalRow DecalRows[];
        };
        // The draw's first row; the instance index walks the run (DecalRenderer::Render: one run = one draw).
        PushConstant DecalPush
        {
            uint FirstRow;
            uint Pad0;
            uint Pad1;
            uint Pad2;
        };

        layout(location = 0) out flat mat4 v_ClipToDecal;
        layout(location = 4) out flat vec4 v_AxisX;
        layout(location = 5) out flat vec4 v_AxisY;
        layout(location = 6) out flat vec4 v_AxisZ;
        layout(location = 7) out flat vec4 v_Params;

        // The unit cube's 12 triangles over the corners (bit 0 = +x, bit 1 = +y, bit 2 = +z), every face wound
        // counter-clockwise seen from outside: the pipeline culls those (CullMode::Front) and keeps the far side.
        const uint kDecalBoxCorners[36] = uint[36]( 1u, 3u, 7u, 1u, 7u, 5u,   // +x
                                                    0u, 4u, 6u, 0u, 6u, 2u,   // -x
                                                    2u, 6u, 7u, 2u, 7u, 3u,   // +y
                                                    0u, 1u, 5u, 0u, 5u, 4u,   // -y
                                                    4u, 5u, 7u, 4u, 7u, 6u,   // +z
                                                    0u, 2u, 3u, 0u, 3u, 1u ); // -z

        vec3 DecalBoxCorner( const uint vertexIndex )
        {
            const uint corner = kDecalBoxCorners[vertexIndex % 36u];
            return vec3( uvec3( corner & 1u, ( corner >> 1u ) & 1u, ( corner >> 2u ) & 1u ) ) * 2.0 - 1.0;
        }

        void main()
        {
            const DecalRow row = DecalRows[FirstRow + uint( gl_InstanceIndex )];
            v_ClipToDecal      = row.ClipToDecal;
            v_AxisX            = row.AxisX;
            v_AxisY            = row.AxisY;
            v_AxisZ            = row.AxisZ;
            v_Params           = row.Params;
            gl_Position        = row.DecalToClip * vec4( DecalBoxCorner( uint( gl_VertexIndex ) ), 1.0 );
        }
    }

    Fragment
    {
        // The writer side of the DBuffer: the encodings and the box fade, none of the receivers' reads.
        #define DESERT_DBUFFER_WRITER
        #include <Common/DBuffer.glslh>

        layout(location = 0) in flat mat4 v_ClipToDecal;
        layout(location = 4) in flat vec4 v_AxisX;
        layout(location = 5) in flat vec4 v_AxisY;
        layout(location = 6) in flat vec4 v_AxisZ;
        layout(location = 7) in flat vec4 v_Params;

        Out(0) vec4 o_DBufferA;
        Out(1) vec4 o_DBufferB;
        Out(2) vec4 o_DBufferC;

        Uniform(1) sampler PointClampSampler;
        Uniform(0) texture2D SceneDepthTexture;
        #define SceneDepthTexture sampler2D(SceneDepthTexture, PointClampSampler)

        // The look's numbers (box units: the texture plane is [-1, 1]^2).
        const float kStripesPerBox   = 4.0;
        const float kDomesPerBox     = 4.0;
        const float kDomeRadius      = 0.38; // of a cell
        const float kDomeStrength    = 0.9;
        const vec3  kStripeColor     = vec3( 0.80, 0.16, 0.04 );
        const vec3  kGapColor        = vec3( 0.92, 0.78, 0.18 );
        const float kStripeRoughness = 0.35;
        const float kGapRoughness    = 0.75;

        void main()
        {
            // The receiver: the full early-Z's depth at this pixel (reversed-Z: 0 is the sky, nothing to project
            // onto), into the box through the same clip mapping the frame rastered with (HeightFog.shader).
            const ivec2 coord = ivec2( gl_FragCoord.xy );
            const ivec2 size  = textureSize( SceneDepthTexture, 0 );
            const float depth = texelFetch( SceneDepthTexture, coord, 0 ).r;
            if ( depth <= 0.0 )
                discard;
            const vec2 uv    = ( vec2( coord ) + vec2( 0.5 ) ) / vec2( size );
            const vec2 ndc   = vec2( uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0 );
            const vec4 h     = v_ClipToDecal * vec4( ndc, depth, 1.0 );
            const vec3 local = h.xyz / h.w;

            const float opacity = DesertDecalBoxFade( local, v_Params.y ) * v_Params.x;
            if ( opacity <= 0.0 )
                discard;

            // The side of the box the camera looks from: the pixel's ray from the near plane (depth 1) to the
            // receiver, in the box. The decal faces the camera along its projection axis either way it is placed.
            const vec4  nearH  = v_ClipToDecal * vec4( ndc, 1.0, 1.0 );
            const vec3  ray    = local - nearH.xyz / nearH.w;
            const float facing = ray.z > 0.0 ? -1.0 : 1.0;

            // Base colour: diagonal stripes over the texture plane.
            const vec2  plane  = local.xy * 0.5 + 0.5;
            const float stripe = step( 0.5, fract( ( plane.x + plane.y ) * kStripesPerBox * 0.5 ) );
            const vec3  color  = mix( kGapColor, kStripeColor, stripe );

            // Normal: a dome per cell, its height h = 1 - (r / R)^2 inside the radius; the tangent-space normal
            // is (-dh/dx, -dh/dy, 1) in the decal's orthonormal frame (AxisX, AxisY, facing x AxisZ).
            const vec2 cell  = fract( plane * kDomesPerBox ) - 0.5;
            const vec2 slope = length( cell ) < kDomeRadius ? cell * ( 2.0 / ( kDomeRadius * kDomeRadius ) )
                                                            : vec2( 0.0 );
            const vec3 tangentNormal = normalize( vec3( slope * kDomeStrength, 1.0 ) );
            const vec3 worldNormal   = normalize( tangentNormal.x * v_AxisX.xyz + tangentNormal.y * v_AxisY.xyz +
                                                  tangentNormal.z * facing * v_AxisZ.xyz );

            const float roughness = mix( kGapRoughness, kStripeRoughness, stripe );

            // A channel the component masked off writes zero with zero opacity: the blend leaves it as it was.
            const uint channels = uint( v_Params.z + 0.5 );
            o_DBufferA = ( channels & 1u ) != 0u ? DesertDBufferEncodeA( color, opacity ) : vec4( 0.0 );
            o_DBufferB = ( channels & 2u ) != 0u ? DesertDBufferEncodeB( worldNormal, opacity ) : vec4( 0.0 );
            o_DBufferC =
                 ( channels & 4u ) != 0u ? DesertDBufferEncodeC( roughness, 0.0, 1.0, opacity ) : vec4( 0.0 );
        }
    }
}
