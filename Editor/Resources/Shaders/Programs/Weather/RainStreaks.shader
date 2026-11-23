// DesertAsset {"Kind":"Shader","Guid":"0743e446b55241488457dbb3a1f73547","Versions":{"SHDR":1},"Dependencies":[]}
// THE FALLING RAIN (NG-PUDDLES pass 3; UE reference: the Niagara GPU rain emitter of the weather samples - drops in a
// camera-relative box, velocity-aligned streaks, killed by the depth/occlusion of what stands overhead). Procedural:
// no simulation buffer. Drop i is a hashed point of a WORLD-ANCHORED box tiled around the camera (it falls and drifts
// with the scene's air, and wraps into the box on the other side), drawn as a camera-facing quad stretched along its
// velocity over the streak's exposure. Under a roof (the view's rain occlusion map) the drop is collapsed, so no
// streak is ever drawn inside the barn. Every value is the WORLD's (WorldParameters: RainIntensity, RainDrops,
// RainDropShape, keys in Common/WeatherRain.glslh); the air is the scene's (SceneWind: the view's directional
// velocity and gust fronts, the same air ParticleSimulate's VFX_SceneWind reads). RainStreakRenderer draws
// 6 vertices x RainStreakRenderer::DropCount instances in the translucency, after the particles.
//
// Blend: premultiplied over (One, OneMinusSrcAlpha), depth tested against the opaque scene, no depth write.
Shader "RainStreaks"
{
    Vertex
    {
        #include <Common/WorldParameters.glslh>
        #include <Common/RainOcclusion.glslh>
        #include <Common/WindGust.glslh>

        // The keys (FNV-1a of the names), pinned by static_assert in Graphic/WorldParameterTable.hpp; the rows are
        // documented with the rest of the rain's in Common/WeatherRain.glslh.
        const uint kDesertRainDropsKey     = 0xe5bb4d7bu; // "RainDrops"
        const uint kDesertRainDropShapeKey = 0x71b62487u; // "RainDropShape"

        Uniform(0) RainStreaksUB
        {
            mat4 RainViewProjection; // the view's jittered view-projection
            vec4 RainCamera;         // xyz camera (cm), w = scene time (s)
            vec4 RainWind;           // xz = SceneWind directional velocity (cm/s), y = 0, w = PreExposure
            vec4 RainGust;           // SceneWind gust fronts (CameraUB::WindGust, Common/WindGust.glslh)
            vec4 RainLight;          // x = the drop's radiance (RainDropShape.z), yzw = 0
        };

        Out(0) vec2  v_Quad;    // x across the streak (-1..1), y along it (0 tail .. 1 head)
        Out(1) float v_Opacity; // the drop's coverage (distance fade x intensity)

        uint RainHash( uint x )
        {
            x ^= x >> 16u;
            x *= 0x7feb352du;
            x ^= x >> 15u;
            x *= 0x846ca68bu;
            x ^= x >> 16u;
            return x;
        }

        vec3 RainHash3( uint i )
        {
            const uint a = RainHash( i * 3u + 0u );
            const uint b = RainHash( i * 3u + 1u );
            const uint c = RainHash( i * 3u + 2u );
            return vec3( a, b, c ) * ( 1.0 / 4294967296.0 );
        }

        void main()
        {
            const vec4 drops = DesertWorldParameter( kDesertRainDropsKey );     // density, half extent, fall, width
            const vec4 shape = DesertWorldParameter( kDesertRainDropShapeKey ); // exposure (s), opacity
            const float halfBox = max( drops.y, 1.0 );
            const float box     = 2.0 * halfBox;
            const uint  drop    = uint( gl_InstanceIndex );
            const vec3  seed    = RainHash3( drop );
            const float t       = RainCamera.w;

            // The drop's own speed jitter (+-15 %) keeps the sheet from falling as one plane.
            const float fall = drops.z * ( 0.85 + 0.3 * fract( seed.x * 7.31 + seed.z * 3.17 ) );
            // The scene's air at the drop's cell (directional x gust fronts; the box is small next to a front).
            const vec3  anchor = seed * box;
            const float gust   = WindGustScale( RainCamera.x + anchor.x, RainCamera.z + anchor.z, RainGust );
            const vec3  v      = vec3( RainWind.x * gust, -fall, RainWind.z * gust );

            // World-anchored: the hashed point moves with v, then wraps into the box around the camera.
            const vec3 moved  = anchor + v * t;
            const vec3 centre = RainCamera.xyz + mod( moved - RainCamera.xyz + halfBox, vec3( box ) ) - halfBox;

            const float speed  = max( length( v ), 1e-3 );
            const vec3  along  = v / speed;
            const float streak = speed * shape.x;
            const vec3  head   = centre + along * ( 0.5 * streak );
            const vec3  tail   = centre - along * ( 0.5 * streak );
            const vec3  toEye  = RainCamera.xyz - centre;
            vec3        across = cross( along, toEye );
            const float acrossLen = length( across );
            across = acrossLen > 1e-3 ? across / acrossLen : vec3( 1.0, 0.0, 0.0 );

            // Two triangles: (0,1,2) (2,1,3) over corners (x = -1/+1, y = 0 tail / 1 head).
            const int  corners[6] = int[6]( 0, 1, 2, 2, 1, 3 );
            const int  c          = corners[gl_VertexIndex % 6];
            const vec2 q          = vec2( ( c & 1 ) == 0 ? -1.0 : 1.0, ( c & 2 ) == 0 ? 0.0 : 1.0 );
            const vec3 p          = mix( tail, head, q.y ) + across * ( q.x * 0.5 * drops.w );

            // Under a roof (either end) or in a box with no rain: collapsed, no fragment.
            const float covered = max( DesertRainOcclusionAt( head ), DesertRainOcclusionAt( tail ) );
            // Fade in from the near plane over a box tenth, out toward the box's edge (no popping at the wrap).
            const float dist    = length( toEye );
            const float fade    = smoothstep( 0.02 * box, 0.1 * box, dist ) * ( 1.0 - smoothstep( 0.7 * halfBox, halfBox, dist ) );
            const float opacity = shape.y * fade;
            if ( covered > 0.5 || !( opacity > 0.0 ) || !( drops.w > 0.0 ) )
            {
                gl_Position = vec4( 0.0, 0.0, 0.0, 0.0 );
                v_Quad      = vec2( 0.0 );
                v_Opacity   = 0.0;
                return;
            }
            gl_Position = RainViewProjection * vec4( p, 1.0 );
            v_Quad      = q;
            v_Opacity   = opacity;
        }
    }

    Fragment
    {
        Uniform(0) RainStreaksUB
        {
            mat4 RainViewProjection;
            vec4 RainCamera;
            vec4 RainWind;
            vec4 RainGust;
            vec4 RainLight;
        };

        In(0) vec2  v_Quad;
        In(1) float v_Opacity;

        Out(0) vec4 o_Color;

        void main()
        {
            // A thin water cylinder seen side-on: bright rim fading across, tapered at both ends (UE rain sprite).
            const float across = 1.0 - v_Quad.x * v_Quad.x;
            const float taper  = smoothstep( 0.0, 0.2, v_Quad.y ) * smoothstep( 1.0, 0.6, v_Quad.y );
            const float alpha  = clamp( v_Opacity * across * taper, 0.0, 1.0 );
            // Premultiplied; the radiance is RainDropShape.z (RainStreakRenderer copies it into RainLight) stored at
            // the view's PreExposure, like every HDR write into the scene colour.
            o_Color = vec4( vec3( RainLight.x * alpha * RainWind.w ), alpha );
        }
    }
}
