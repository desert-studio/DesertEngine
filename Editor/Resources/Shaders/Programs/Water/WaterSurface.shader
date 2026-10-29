// DesertAsset {"Kind":"Shader","Guid":"bcefeba392c545f6b95dcb1c2554b7a8","Versions":{"SHDR":1},"Dependencies":[]}
Shader "WaterSurface"
{
    // THE SINGLE LAYER WATER PROGRAM (UE: a material of ShadingModel SingleLayerWater drawn through the water mesh
    // vertex factory). Every tile the water quadtree selected (Engine/Water/WaterQuadTree.hpp) is drawn by
    // Graphic::System::WaterRenderer after the scene-colour copy and before the glass, into the scene target with
    // depth. One vertexless instanced draw per grid density: the vertex stage decodes its quad from gl_VertexIndex,
    // its tile row from MaterialIndex + gl_InstanceIndex, and morphs toward the next LOD by distance (UE's water
    // vertex factory, ported) so a LOD boundary never opens a crack.
    //
    // The program is the PHYSICS of light in water; the LOOK is the body's `.demat` of this domain: scattering,
    // absorption, phase, the tint behind the water, roughness and the ripple normal maps are the Properties below,
    // and nothing in the stages states a colour. The fragment is UE's single layer water: the scene behind the
    // surface (the opaque copy, without water) refracted by the surface normal, attenuated by Beer-Lambert over the
    // view-ray length through the water (the opaque depth, SceneViewInputs::SceneDepth, is the bottom) plus the
    // single-scattered in-scatter of the sun (Henyey-Greenstein) and the sky (the irradiance cube), mixed by Fresnel
    // into the view's reflection environment and the sun's GGX highlight.
    Domain SingleLayerWater

    Properties
    {
        Color       ScatteringCoefficients ("Scattering Coefficients (1/m)", Category("Water")) = (0.02, 0.06, 0.06, 1)
        Color       AbsorptionCoefficients ("Absorption Coefficients (1/m)", Category("Water")) = (0.45, 0.09, 0.06, 1)
        Float       PhaseG ("Phase G", Range(-0.9,0.9), Category("Water")) = 0.3
        Color       ColorScaleBehindWater ("Color Scale Behind Water", Category("Water")) = (1, 1, 1, 1)
        Float       Roughness ("Roughness", Range(0.02,1), Category("Surface")) = 0.05
        Float       Specular ("Specular", Range(0,1), Category("Surface")) = 0.255
        Float       RefractionStrength ("Refraction Strength (cm)", Range(0,200), Category("Surface")) = 40
        Texture2D   RippleNormalA ("Ripple Normal A", Category("Ripples"), Intent(NormalMap)) = "normal"
        Texture2D   RippleNormalB ("Ripple Normal B", Category("Ripples"), Intent(NormalMap)) = "normal"
        Float       RippleTileSizeA ("Ripple Tile Size A (cm)", Range(10,10000), Category("Ripples")) = 400
        Float       RippleTileSizeB ("Ripple Tile Size B (cm)", Range(10,10000), Category("Ripples")) = 170
        Float       RippleAngleA ("Ripple Direction A (deg)", Range(-180,180), Category("Ripples")) = 20
        Float       RippleAngleB ("Ripple Direction B (deg)", Range(-180,180), Category("Ripples")) = -70
        Float       RippleSpeed ("Ripple Speed (cm/s)", Range(0,500), Category("Ripples")) = 20
        Float       RippleStrength ("Ripple Strength", Range(0,4), Category("Ripples")) = 1
    }

    State
    {
        Topology Triangles
        Cull None
        ZTest Less
        ZWrite On
    }

    Vertex
    {
        #include <Common/MaterialTransport.glslh>
        #include <Programs/Water/WaterSurfaceData.glslh>

        Out(0) vec3 v_WorldPos;

        // UE MorphTranslatedWorldPosition (Plugins/Experimental/Water/Shaders/Private/WaterMeshVertexFactory.ush:
        // 252-275), in the tile's own frame: @p local is the vertex relative to the tile's lower corner, the frame UE
        // measures the morph in (InMorphOriginTWS = the node's corner). Every other vertex slides toward its
        // neighbour by the LOD factor, so at factor 1 the grid IS the next LOD's (two quads per quad).
        vec2 MorphLocal( vec2 local, WaterTileRow t, float lodLevel, float quadSize, float heightLodFactor,
                         out float lodFactor )
        {
            vec2  world = t.Zone.xy + t.Tile.xy + local;
            float dist  = distance( world, u.CameraPos.xz );
            lodFactor   = clamp( dist / ( t.Zone.z * exp2( lodLevel ) ) - 1.0, 0.0, 1.0 );
            lodFactor   = clamp( heightLodFactor + lodFactor, 0.0, 1.0 );

            float grid4  = 4.0 * quadSize;
            vec2  offset = fract( local / grid4 ) - vec2( 0.5 );
            const float kMinRadius = 0.26;
            if ( abs( offset.x ) < kMinRadius )
                local.x += offset.x * lodFactor * grid4;
            if ( abs( offset.y ) < kMinRadius )
                local.y += offset.y * lodFactor * grid4;
            return local;
        }

        void main()
        {
            WaterTileRow t = WaterTileRows[int( m_PushConstants.MaterialIndex ) + gl_InstanceIndex];

            // Two triangles per quad, quads row by row.
            const ivec2 kCorner[6] = ivec2[]( ivec2( 0, 0 ), ivec2( 1, 0 ), ivec2( 0, 1 ),
                                              ivec2( 1, 0 ), ivec2( 1, 1 ), ivec2( 0, 1 ) );
            int   quads    = int( t.Zone.w + 0.5 );
            int   quad     = gl_VertexIndex / 6;
            ivec2 q        = ivec2( quad % quads, quad / quads ) + kCorner[gl_VertexIndex % 6];
            float quadSize = t.Tile.w / float( quads );
            vec2  local    = vec2( q ) * quadSize;

            if ( t.Lod.y > 0.5 )
            {
                // The lowest LOD also carries the camera height's global morph (t.Lod.w).
                float lodFactor = 0.0;
                local = MorphLocal( local, t, t.Lod.x, quadSize, t.Lod.w, lodFactor );
                // Fully morphed: morph again as a vertex of the next LOD — a tile reaches into the next ring because
                // the observer moves, so it must be able to span three LODs (no height factor on the second).
                if ( lodFactor >= 1.0 && t.Lod.z > 0.5 )
                    local = MorphLocal( local, t, t.Lod.x + 1.0, quadSize * 2.0, 0.0, lodFactor );
            }

            // Zone-relative first (exact multiples of the quad size), then the zone's origin: two tiles sharing an
            // edge add the same two numbers, so the edge is at the same bits in both.
            vec2 zoneLocal = t.Tile.xy + local;
            v_WorldPos     = vec3( t.Zone.x + zoneLocal.x, t.Tile.z, t.Zone.y + zoneLocal.y );
            gl_Position    = m_PushConstants.Transform * vec4( v_WorldPos, 1.0 );
        }
    }

    Fragment
    {
        In(0) vec3 v_WorldPos;
        Out(0) vec4 o_Color;
        Out(1) vec4 oVelocity; // the view's velocity, slot 1 of the scene target
        // The view's thin-geometry coverage, slot 3 of the scene target: this pass writes the view's depth. Water is
        // a lit surface that is not foliage (UE: SingleLayerWater counts as a regular surface).
        Out(3) float oThinGeometryCoverage;

        #include <Programs/Water/WaterSurfaceData.glslh>
        #include <Common/Velocity.glslh>
        #include <Common/ThinGeometryCoverage.glslh>

        // The material's ripple maps (the `.demat`'s textures; the flat "normal" default when it names none).
        Uniform(3) texture2D RippleNormalA;
        Uniform(4) sampler RippleNormalASampler;
        #define RippleNormalA sampler2D(RippleNormalA, RippleNormalASampler)
        Uniform(5) texture2D RippleNormalB;
        Uniform(6) sampler RippleNormalBSampler;
        #define RippleNormalB sampler2D(RippleNormalB, RippleNormalBSampler)

        // The scene/view inputs (SceneViewInputs, bound by name: BindSceneViewInputs) and the opaque scene's copy.
        Uniform sampler LinearClampSampler;
        Uniform sampler PointClampSampler;
        Uniform(7) texture2D SceneColorTexture; // the composited opaque scene, pre-exposed, without the water
        #define SceneColorTexture sampler2D(SceneColorTexture, LinearClampSampler)
        Uniform(8) texture2D SceneDepthTexture; // the opaque scene's device depth (reversed-Z), the water's bottom
        #define SceneDepthTexture sampler2D(SceneDepthTexture, PointClampSampler)
        Uniform(9) textureCube EnvironmentSpecularTexture; // the view's prefiltered sky
        #define EnvironmentSpecularTexture samplerCube(EnvironmentSpecularTexture, LinearClampSampler)
        Uniform(10) textureCube EnvironmentIrradianceTexture; // the view's diffuse sky
        #define EnvironmentIrradianceTexture samplerCube(EnvironmentIrradianceTexture, LinearClampSampler)

        const float kPi = 3.14159265;

        // Tangent-space map texel (x, y in the plane, z up) -> world, the plane being X/Z with Y up.
        vec3 RippleSample( sampler2D map, vec2 uv )
        {
            vec3 t = texture( map, uv ).xyz * 2.0 - 1.0;
            return vec3( t.x, t.z, t.y );
        }

        vec3 WorldAt( vec2 uv, float deviceDepth )
        {
            vec4 w = u.InvViewProjection * vec4( uv * 2.0 - 1.0, deviceDepth, 1.0 );
            return w.xyz / w.w;
        }

        float HenyeyGreenstein( float cosTheta, float g )
        {
            float g2 = g * g;
            return ( 1.0 - g2 ) / ( 4.0 * kPi * pow( max( 1.0 + g2 - 2.0 * g * cosTheta, 1e-4 ), 1.5 ) );
        }

        float GgxD( float NdH, float a )
        {
            float a2 = a * a;
            float d  = NdH * NdH * ( a2 - 1.0 ) + 1.0;
            return a2 / ( kPi * d * d );
        }

        void main()
        {
            const float exposure = u.Exposure.x;
            vec3        V        = normalize( u.CameraPos.xyz - v_WorldPos );
            vec3        L        = normalize( -u.SunDir.xyz );
            vec3        sun      = u.SunColor.rgb * u.SunColor.a; // illuminance, not yet pre-exposed

            // --- The surface normal: two ripple maps scrolling across each other (UE's water material's detail
            // normals), whiteout-blended, tilted by the material's strength.
            float t     = u.Exposure.y;
            vec2  dirA  = wm.RippleDirection.xy;
            vec2  dirB  = wm.RippleDirection.zw;
            vec2  uvA   = ( v_WorldPos.xz + dirA * wm.Ripple.z * t ) / wm.Ripple.x;
            vec2  uvB   = ( v_WorldPos.xz + dirB * wm.Ripple.z * t ) / wm.Ripple.y;
            vec3  nA    = RippleSample( RippleNormalA, uvA );
            vec3  nB    = RippleSample( RippleNormalB, uvB );
            vec2  slope = ( nA.xz + nB.xz ) * wm.Ripple.w;
            vec3  N     = normalize( vec3( slope.x, nA.y * nB.y, slope.y ) );
            if ( dot( N, V ) < 0.0 )
                N = normalize( N + V * ( 0.01 - dot( N, V ) ) ); // never face away from the eye

            // --- The scene behind: refracted by the normal's tilt, the offset growing with the water in front of it
            // (UE SingleLayerWater's distortion), refused where the bent sample lands in front of the surface.
            vec2  screen   = vec2( textureSize( SceneColorTexture, 0 ) );
            vec2  uv       = gl_FragCoord.xy / screen;
            float baseDev  = texture( SceneDepthTexture, uv ).r;
            vec3  baseBack = WorldAt( uv, baseDev );
            float baseLen  = baseDev <= 0.0 ? 1e7 : distance( v_WorldPos, baseBack );
            vec4  clipOff  = u.ViewProjection * vec4( v_WorldPos + vec3( N.x, 0.0, N.z ) *
                                                                        wm.Shading.z * clamp( baseLen / 100.0, 0.0, 1.0 ),
                                                                    1.0 );
            vec2  refrUv   = clamp( ( clipOff.xy / clipOff.w ) * 0.5 + 0.5, vec2( 0.0 ), vec2( 1.0 ) );
            float refrDev  = texture( SceneDepthTexture, refrUv ).r;
            vec3  refrBack = WorldAt( refrUv, refrDev );
            bool  inFront  = refrDev > 0.0 && dot( refrBack - u.CameraPos.xyz, -V ) < dot( v_WorldPos - u.CameraPos.xyz, -V );
            if ( inFront )
            {
                refrUv   = uv;
                refrDev  = baseDev;
                refrBack = baseBack;
            }
            float pathCm = refrDev <= 0.0 ? 1e7 : distance( v_WorldPos, refrBack );
            vec3  behind = texture( SceneColorTexture, refrUv ).rgb; // pre-exposed

            // --- The water volume: Beer-Lambert transmittance over the view ray through it and the single-scattered
            // light it adds (UE SingleLayerWater: the sun through the phase function, the sky's ambient).
            vec3  sigmaS   = wm.Scattering.rgb;
            vec3  sigmaT   = max( wm.Absorption.rgb + sigmaS, vec3( 1e-5 ) );
            float pathM    = pathCm * 0.01;
            vec3  trans    = exp( -sigmaT * pathM );
            vec3  albedo   = sigmaS / sigmaT;
            vec3  skyIrr   = texture( EnvironmentIrradianceTexture, vec3( 0.0, 1.0, 0.0 ) ).rgb;
            float phase    = HenyeyGreenstein( dot( -V, L ), wm.Shading.x );
            vec3  sunIn    = sun * max( L.y, 0.0 ) * phase;
            vec3  inscat   = ( sunIn + skyIrr / ( 4.0 * kPi ) ) * albedo * ( 1.0 - trans ) * exposure;
            vec3  volume   = behind * wm.ColorScaleBehindWater.rgb * trans + inscat;

            // --- The interface: Schlick Fresnel at water's F0 (Specular 0.255 -> 0.02), the reflection environment
            // at the material's roughness, the sun's GGX highlight.
            float NdV     = max( dot( N, V ), 1e-4 );
            float f0      = 0.08 * wm.Shading.w;
            float fresnel = f0 + ( 1.0 - f0 ) * pow( 1.0 - NdV, 5.0 );
            vec3  R       = reflect( -V, N );
            R.y           = abs( R.y ); // the reflection of a surface seen from above is of the upper hemisphere
            float rough   = wm.Shading.y;
            float mips    = float( textureQueryLevels( EnvironmentSpecularTexture ) - 1 );
            vec3  refl    = textureLod( EnvironmentSpecularTexture, R, rough * mips ).rgb * exposure;
            vec3  H       = normalize( V + L );
            float a       = max( rough * rough, 0.002 );
            float NdL     = max( dot( N, L ), 0.0 );
            float k       = a * 0.5;
            float vis     = 0.25 / ( ( NdL * ( 1.0 - k ) + k ) * ( NdV * ( 1.0 - k ) + k ) );
            vec3  glint   = sun * GgxD( max( dot( N, H ), 0.0 ), a ) * vis * NdL * fresnel * exposure;

            o_Color    = vec4( mix( volume, refl, fresnel ) + glint, 1.0 );
            vec4 world = vec4( v_WorldPos, 1.0 );
            oVelocity  = DesertVelocity( u.ViewProjection * world, u.PrevViewProjection * world, 0.0 ); // the surface does not move (Common/Velocity.glslh)
            oThinGeometryCoverage = kThinGeometryCoverageSurface;
        }
    }
}
