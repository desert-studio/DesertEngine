Shader "Terrain"
{
    // Data-driven material metadata (consumed by the pipeline cache + generic material in later phases).
    // Splat layers — drag textures onto these in Details; unassigned = white fallback (shows the base tint).

    Domain Terrain

    // Binding(1) is what makes these parameters a ROW of the shared `Materials[]` storage buffer, read
    // through `u_Material` in the fragment stage. The block used to be written out by hand below and was
    // a uniform block per material — one set of values for every terrain in the scene, which is the same
    // defect the graph materials had (Engine/Core/Formats/MaterialParamRow.hpp).
    Properties Binding(1)
    {
        Color       Tint ("Tint") = (1, 1, 1, 1)
        Float       DetailTiling ("Texture Tiling (m)", Range(0.25,64)) = 4
        Texture2D   u_GrassTex ("Grass Texture")
        Texture2D   u_RockTex ("Rock Texture")
        Texture2D   u_SnowTex ("Snow Texture")
    }

    State
    {
        Topology Patches 4
        Cull None
        ZTest Less
        ZWrite On
    }

    // ── The split every stage below repeats ─────────────────────────────────────────────────────────
    //
    // TerrainUB (binding 0) holds ONLY what every terrain of a frame shares: View, Projection and the
    // sun. Everything per-terrain — Model, sizes, seed, layer modes — is a row of TerrainInstances[]
    // (binding 8), named per draw by the SAME push-constant index that names the material param row.
    //
    // WHY: the terrain pass records every draw of a frame before the GPU executes any of them, and a
    // material's uniform buffer has one copy per frame — so per-terrain fields kept in TerrainUB were
    // read as the LAST terrain's values by every terrain (two terrains drew as one). A push constant is
    // snapshotted at record time, so a row named by it cannot be clobbered by the next draw's setup —
    // the exact argument that already moved the material params to Materials[] (MaterialParamRow.hpp).
    // The C++ mirror of TerrainInstance lives in Engine/Graphic/Systems/Scene/Terrain/TerrainBatch.hpp,
    // and the ShaderCacheKey suite asserts both statements of the layout against the compiled SPIR-V.

    Vertex
    {
        // GPU terrain — vertexless patch grid. A draw of (gridDim*gridDim*4) vertices synthesizes a grid of
        // quad patches purely from gl_VertexIndex (no vertex buffer). This stage emits each patch corner as a
        // world-space control point on the y=0 plane; the TES projects + (later) displaces it.

        #include <Common/MaterialTransport.glslh>

        struct TerrainInstance
        {
            mat4 Model;
            vec4 Params;     // x = world size, y = gridDim (patches/side), z = heightScale, w = tessLevel
            vec4 Params2;    // x = noiseFrequency, y = seed, z/w = spare
            vec4 LayerModes; // x = grass, y = rock, z = snow (0=Auto,1=Manual,2=Off), w = std430 padding
        };
        ReadBuffer(8) TerrainInstances
        {
            TerrainInstance u_Terrains[];
        };
        #define u_T u_Terrains[m_PushConstants.MaterialIndex]

        Out(0) vec2 v_WorldXZ;

        void main()
        {
            int gridDim = int( u_T.Params.y );
            int patchId = gl_VertexIndex / 4;
            int corner  = gl_VertexIndex % 4;

            int gx = patchId % gridDim;
            int gz = patchId / gridDim;

            // Unit-quad corner offsets in CCW order: (0,0) (1,0) (1,1) (0,1).
            vec2 off = vec2( ( corner == 1 || corner == 2 ) ? 1.0 : 0.0, ( corner == 2 || corner == 3 ) ? 1.0 : 0.0 );

            float size = u_T.Params.x;
            float cell = size / float( gridDim );
            float x    = ( float( gx ) + off.x ) * cell - size * 0.5;
            float z    = ( float( gz ) + off.y ) * cell - size * 0.5;

            v_WorldXZ   = vec2( x, z );
            gl_Position = vec4( x, 0.0, z, 1.0 ); // world-space control point (projection happens in the TES)
        }
    }

    TessControl
    {
        // Tessellation control — one 4-vertex quad patch in, distance-based LOD out (Stage 4). Each edge's
        // tessellation level is derived from its midpoint distance to the camera in VIEW space (the camera sits
        // at the origin in view space, so no camera-position uniform is needed). Adjacent patches share an edge's
        // two corner positions, so they compute the same midpoint -> the same edge tess level -> CRACK-FREE.
        // Near patches get full detail (Params.w), far patches drop toward minTess.

        layout( vertices = 4 ) out;

        // Shared frame data only — the per-terrain half arrives per draw, see the note above Vertex.
        Uniform(0) TerrainUB
        {
            mat4 View;
            mat4 Projection;
            vec4 SunDir;
            vec4 SunColor;
        }
        u;

        #include <Common/MaterialTransport.glslh>

        struct TerrainInstance
        {
            mat4 Model;
            vec4 Params;     // x = size, y = gridDim, z = heightScale, w = tessLevel (used here as MAX/near tess)
            vec4 Params2;    // x = noiseFrequency, y = seed, z/w = spare
            vec4 LayerModes; // x = grass, y = rock, z = snow (0=Auto,1=Manual,2=Off), w = std430 padding
        };
        ReadBuffer(8) TerrainInstances
        {
            TerrainInstance u_Terrains[];
        };
        #define u_T u_Terrains[m_PushConstants.MaterialIndex]

        In(0) vec2 v_WorldXZ[];
        Out(0) vec2 tc_WorldXZ[];

        float TessForDistance( float d )
        {
            float maxTess = max( u_T.Params.w, 1.0 );
            float minTess = 2.0;
            // Distance band scales with terrain size so LOD adapts to small and large terrains alike.
            float nearD = max( u_T.Params.x * 0.05, 2.0 );
            float farD  = max( u_T.Params.x * 1.5, nearD + 1.0 );
            float t     = clamp( ( farD - d ) / ( farD - nearD ), 0.0, 1.0 );
            return mix( minTess, maxTess, t );
        }

        // Tessellation level for the edge between two control points, from its midpoint's view-space distance.
        float EdgeTess( vec3 viewA, vec3 viewB )
        {
            return TessForDistance( length( ( viewA + viewB ) * 0.5 ) );
        }

        void main()
        {
            if ( gl_InvocationID == 0 )
            {
                // Control-point corners in view space (camera at origin). gl_in are the flat y=0 control points;
                // displacement is small vs. the LOD distances, so using the flat positions is fine and stable.
                mat4 mv = u.View * u_T.Model;
                vec3 c0 = ( mv * gl_in[0].gl_Position ).xyz; // (u,v)=(0,0)
                vec3 c1 = ( mv * gl_in[1].gl_Position ).xyz; // (1,0)
                vec3 c2 = ( mv * gl_in[2].gl_Position ).xyz; // (1,1)
                vec3 c3 = ( mv * gl_in[3].gl_Position ).xyz; // (0,1)

                // Quad edge -> outer-tess mapping: [0]=u0 (c0-c3), [1]=v0 (c0-c1), [2]=u1 (c1-c2), [3]=v1 (c3-c2).
                float e0 = EdgeTess( c0, c3 );
                float e1 = EdgeTess( c0, c1 );
                float e2 = EdgeTess( c1, c2 );
                float e3 = EdgeTess( c3, c2 );

                gl_TessLevelOuter[0] = e0;
                gl_TessLevelOuter[1] = e1;
                gl_TessLevelOuter[2] = e2;
                gl_TessLevelOuter[3] = e3;

                // Inner levels: horizontal (u) from the v=0/v=1 edges, vertical (v) from the u=0/u=1 edges.
                gl_TessLevelInner[0] = max( e1, e3 );
                gl_TessLevelInner[1] = max( e0, e2 );
            }

            gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
            tc_WorldXZ[gl_InvocationID]         = v_WorldXZ[gl_InvocationID];
        }
    }

    TessEval
    {
        // Tessellation evaluation — bilinearly interpolates the tessellated grid vertex across the patch, then
        // displaces it along Y by a procedural fBm height field (Stage 2). Normals are derived analytically from
        // the same height function via central differences, so the lit relief needs no normal map.
        //
        // The height source is isolated in TerrainHeight(): Stage 5 (sculpting) swaps the fBm for a sampled,
        // editable heightmap texture without touching the displacement/normal math below.

        layout( quads, equal_spacing, cw ) in;

        // Shared frame data only — the per-terrain half arrives per draw, see the note above Vertex.
        Uniform(0) TerrainUB
        {
            mat4 View;
            mat4 Projection;
            vec4 SunDir;
            vec4 SunColor;
        }
        u;

        #include <Common/MaterialTransport.glslh>

        struct TerrainInstance
        {
            mat4 Model;
            vec4 Params;     // x = size, y = gridDim, z = heightScale, w = tessLevel
            vec4 Params2;    // x = noiseFrequency, y = seed, z/w = spare
            vec4 LayerModes; // x = grass, y = rock, z = snow (0=Auto,1=Manual,2=Off), w = std430 padding
        };
        ReadBuffer(8) TerrainInstances
        {
            TerrainInstance u_Terrains[];
        };
        #define u_T u_Terrains[m_PushConstants.MaterialIndex]

        In(0) vec2 tc_WorldXZ[];

        Out(0) vec3 v_WorldPos;
        Out(1) vec3 v_Normal;
        Out(2) float v_Height01; // normalized height [0..1] for slope/height tinting

        // --- Value-noise fBm -------------------------------------------------------------------------------

        float Hash( vec2 p )
        {
            p = fract( p * vec2( 123.34, 456.21 ) );
            p += dot( p, p + 45.32 );
            return fract( p.x * p.y );
        }

        float ValueNoise( vec2 p )
        {
            vec2 i = floor( p );
            vec2 f = fract( p );
            vec2 u = f * f * ( 3.0 - 2.0 * f ); // smoothstep

            float a = Hash( i + vec2( 0.0, 0.0 ) );
            float b = Hash( i + vec2( 1.0, 0.0 ) );
            float c = Hash( i + vec2( 0.0, 1.0 ) );
            float d = Hash( i + vec2( 1.0, 1.0 ) );

            return mix( mix( a, b, u.x ), mix( c, d, u.x ), u.y );
        }

        float FBm( vec2 p )
        {
            float sum = 0.0;
            float amp = 0.5;
            float freq = 1.0;
            for ( int i = 0; i < 5; ++i )
            {
                sum += amp * ValueNoise( p * freq );
                freq *= 2.0;
                amp *= 0.5;
            }
            return sum; // ~[0..1]
        }

        // Returns terrain height (world units) at a world-space XZ position.
        float TerrainHeight( vec2 worldXZ )
        {
            float freq = max( u_T.Params2.x, 0.0001 );
            vec2  seed = vec2( u_T.Params2.y * 0.137, u_T.Params2.y * 0.911 );
            float h    = FBm( worldXZ * freq + seed );
            return ( h - 0.5 ) * 2.0 * u_T.Params.z; // center around 0, scale by heightScale
        }

        void main()
        {
            // Corners stored CCW: p0=(0,0) p1=(1,0) p2=(1,1) p3=(0,1). Bilerp over gl_TessCoord.
            vec4 p0 = gl_in[0].gl_Position;
            vec4 p1 = gl_in[1].gl_Position;
            vec4 p2 = gl_in[2].gl_Position;
            vec4 p3 = gl_in[3].gl_Position;

            vec4 a   = mix( p0, p1, gl_TessCoord.x );
            vec4 b   = mix( p3, p2, gl_TessCoord.x );
            vec4 pos = mix( a, b, gl_TessCoord.y );

            // Displace along Y by the height field (Stage 2).
            pos.y = TerrainHeight( pos.xz );

            // Analytic normal via central differences on the height field. Epsilon scales with the grid cell.
            float eps = max( u_T.Params.x / max( u_T.Params.y, 1.0 ), 0.01 ) * 0.5;
            float hL  = TerrainHeight( pos.xz - vec2( eps, 0.0 ) );
            float hR  = TerrainHeight( pos.xz + vec2( eps, 0.0 ) );
            float hD  = TerrainHeight( pos.xz - vec2( 0.0, eps ) );
            float hU  = TerrainHeight( pos.xz + vec2( 0.0, eps ) );
            vec3  n   = normalize( vec3( hL - hR, 2.0 * eps, hD - hU ) );

            vec4 worldPos = u_T.Model * vec4( pos.xyz, 1.0 ); // apply the terrain entity's transform
            v_WorldPos    = worldPos.xyz;
            v_Normal      = normalize( mat3( u_T.Model ) * n );
            v_Height01    = clamp( pos.y / max( u_T.Params.z, 0.0001 ) * 0.5 + 0.5, 0.0, 1.0 );

            gl_Position = u.Projection * u.View * worldPos;
        }
    }

    Fragment
    {
        // Stage 3 + 3a: textured terrain splatting with per-layer modes. Three layers (grass / rock / snow) are
        // triplanar-mapped in world space. Each layer's weight comes from its mode: Auto = height/slope rules,
        // Manual = the painted splat map channel (brush, Stage 3b), Off = 0. Each layer is modulated by a base
        // tint so that with UNASSIGNED textures (white backend fallback) the result matches the Stage 2 relief.
        // Lit with a directional key light using the analytic normal from the TES.

        In(0) vec3 v_WorldPos;
        In(1) vec3 v_Normal;
        In(2) float v_Height01;

        Out(0) vec4 o_Color;

        // Engine-filled terrain UB (binding 0) — shared frame data only; the per-terrain half (Model,
        // Params, layer modes) is this draw's TerrainInstances row, see the note above Vertex.
        Uniform(0) TerrainUB
        {
            mat4 View;
            mat4 Projection;
            vec4 SunDir;     // xyz = normalized light direction (scene directional light)
            vec4 SunColor;   // rgb = color, a = intensity
        }
        u;

        struct TerrainInstance
        {
            mat4 Model;
            vec4 Params;     // x = size, y = gridDim, z = heightScale, w = tessLevel
            vec4 Params2;    // x = noiseFrequency, y = seed, z/w = spare
            vec4 LayerModes; // x = grass, y = rock, z = snow (0=Auto,1=Manual,2=Off), w = std430 padding
        };
        ReadBuffer(8) TerrainInstances
        {
            TerrainInstance u_Terrains[];
        };
        // m_PushConstants is already declared here: the generated Materials[] transport injects
        // MaterialTransport.glslh at the top of the fragment stage, and its include guard makes this
        // stage's declaration one with the other stages' explicit includes.
        #define u_T u_Terrains[m_PushConstants.MaterialIndex]

        // Data-driven material params (binding 1) are generated from the Properties block above and read
        // through `u_Material`; nothing is declared here.

        // Splat layers (texture2D #pragma params; unassigned => white fallback). Bindings follow MaterialUB.
        Uniform(2) sampler2D u_GrassTex;
        Uniform(3) sampler2D u_RockTex;
        Uniform(4) sampler2D u_SnowTex;
        // Per-terrain splat map (R=grass, G=rock, B=snow weights), painted by the brush (Stage 3b). Engine-bound;
        // unassigned => white fallback (Manual layers show everywhere until painted).
        Uniform(5) sampler2D u_SplatMap;

        // THE CLOUD LAYER'S SHADOW ON THE WORLD. The terrain is the surface this feature is FOR — a deck
        // of cumulus drifting over open ground is the whole picture — and it is the surface that never
        // received it: the wrapper lived inside the deferred composite, and a terrain is drawn by neither
        // render path's mesh shaders. Slots 6/7 are the first free ones in this shader's own small layout;
        // they need not (and do not) match the mesh shaders' 20/21, because only the NAMES are shared and
        // every material binds by name (Graphic::CloudShadowBind).
        Uniform(6) sampler2D u_CloudShadowMap;
        Uniform(7) CloudShadowUB
        {
        	mat4 u_CloudShadowWorldToMap;
        	// x = the kilometres the map's clip z spans, y = 1 when the map is real and must be read,
        	// z = the UV width of the border fade, w = the artist's shadow strength.
        	vec4 u_CloudShadowParams;
        };

        // THE receiver, shared verbatim with the deferred composite and the mesh shaders.
        #include <Common/CloudShadowReceiver.glslh>

        // Triplanar sample: project onto the three world planes and blend by the (squared) normal so steep faces
        // don't stretch. scale = tiles per world unit.
        vec3 Triplanar( sampler2D tex, vec3 wpos, vec3 n, float scale )
        {
            vec3 bw = abs( n );
            bw      = pow( bw, vec3( 4.0 ) );
            bw     /= max( bw.x + bw.y + bw.z, 0.0001 );

            vec3 xa = texture( tex, wpos.yz * scale ).rgb;
            vec3 ya = texture( tex, wpos.xz * scale ).rgb;
            vec3 za = texture( tex, wpos.xy * scale ).rgb;
            return xa * bw.x + ya * bw.y + za * bw.z;
        }

        // Resolve a layer's weight from its mode: 0=Auto (rule), 1=Manual (splat channel), 2=Off (0).
        float LayerWeight( float mode, float autoWeight, float splatChannel )
        {
            if ( mode > 1.5 ) return 0.0;          // Off
            if ( mode > 0.5 ) return splatChannel; // Manual
            return autoWeight;                      // Auto
        }

        void main()
        {
            vec3 N = normalize( v_Normal );

            // Slope = how far the normal tilts from straight up (0 = flat, 1 = vertical cliff).
            float slope = clamp( 1.0 - N.y, 0.0, 1.0 );

            // Base tints (placeholder until Stage 6 PBR textures). The grass layer is a DARK olive ground
            // tone; rock is the default ground and snow caps it.
            //
            // IT USED TO BE THE FLOOR UNDER GENERATED BLADES and it is not any more (Г25). The tone was
            // chosen dark so the gaps between thin procedural blades read as a continuous lawn rather than
            // glowing bright-green through them, and it was multiplied by Params2.z — the blade Brightness
            // slider — so ground and blades tracked each other as one material. Both the blades and that
            // slider are gone; the tone is kept as authored because it is the ground this repository's
            // terrain scenes were dressed against, and grass now arrives as a MESH ASSET scattered by the
            // Foliage tool, which stands on this ground rather than being tinted with it.
            //
            // 0.031/0.027 are frequencies per world unit — a ~2 m patch, which is what the metre era's
            // 3.1/2.7 meant. Left alone they were a 2 cm chequer: shimmer, not variation.
            float groundVar  = sin( v_WorldPos.x * 0.031 ) * sin( v_WorldPos.z * 0.027 ) * 0.5 + 0.5;
            vec3  grassCol   = vec3( 0.052, 0.10, 0.034 ) * ( 0.75 + 0.5 * groundVar );
            vec3  rockCol   = vec3( 0.40, 0.37, 0.33 ); // bare rock (default base)
            vec3  snowCol   = vec3( 0.86, 0.88, 0.92 );

            float scale  = 1.0 / max( u_Material.DetailTiling, 0.001 );
            vec3  grassT = Triplanar( u_GrassTex, v_WorldPos, N, scale ) * grassCol;
            vec3  rockT  = Triplanar( u_RockTex, v_WorldPos, N, scale ) * rockCol;
            vec3  snowT  = Triplanar( u_SnowTex, v_WorldPos, N, scale ) * snowCol;

            // Splat-map weights. UV is terrain-local (subtract the Model translation) so painting matches
            // regardless of where the terrain entity sits in the world.
            vec2  splatUV  = ( v_WorldPos.xz - u_T.Model[3].xz ) / max( u_T.Params.x, 0.001 ) + 0.5;
            vec4  splat    = texture( u_SplatMap, splatUV );
            // EACH LAYER'S OWN AUTO RULE, AND grassAuto IS 1.0 ON PURPOSE (Г26). It used to be
            // `1.0 - rockAuto`, which is the same picture written the other way round: rock was the BED
            // and grass was mixed over it by the complement of the slope rule, so the rock layer had no
            // weight of its own and `LayerModes.y` had nothing to gate. Writing it as "grass covers the
            // ground, rock takes it back on slopes, snow caps it" gives rock a weight the mode can act on
            // and is ALGEBRAICALLY THE SAME MIX while every layer is on Auto:
            //   mix(mix(rock, grass, 1), rock, rockAuto) == rock*rockAuto + grass*(1 - rockAuto),
            // which is exactly what the old two-line form produced. That is why every terrain scene in the
            // repository renders byte-identically across this change, and it is the negative control the
            // change was measured with.
            float rockAuto  = smoothstep( 0.25, 0.55, slope );
            float grassAuto = 1.0;                                 // the ground is grass until something takes it
            float snowAuto  = smoothstep( 0.75, 0.95, v_Height01 ) * ( 1.0 - smoothstep( 0.4, 0.7, slope ) );

            // THE ROCK LAYER WAS THE THIRD DEAD KNOB OF THE SAME FAMILY, AND IT SURVIVED THE CENSUS THAT
            // WAS SUPPOSED TO CATCH IT. `Rock Layer` is a reflected, serialized, Details-visible enum;
            // TerrainECSSystem packs it into LayerModes.y; this file never read LayerModes.y and never
            // sampled splat.g, so the paint tool's own instruction — "set the layer to 'Manual' in Details
            // to see painted weights" — was false for the `Rock (G)` brush, which is one of the three it
            // offers. Г25 fixed exactly this for grass (LayerModes.x) and the same shape was left standing
            // one channel over. `SettingConsumers` stayed green throughout, because a WIRED row asks for a
            // read of the FIELD and TerrainECSSystem reads it: the census sees the first link of the chain
            // and cannot see that the last one drops it.
            //
            // ROCK IS STILL THE BED under everything, because a ground shader must draw something where no
            // layer claims a pixel and inventing a fourth colour for that case would be a worse answer
            // than the ground this repository's terrain scenes are dressed against. With grass on Auto the
            // bed is fully covered, so `Rock Layer = Off` is visible where it means something — the
            // slopes — rather than nowhere.
            float wGrass = LayerWeight( u_T.LayerModes.x, grassAuto, splat.r );
            float wRock  = LayerWeight( u_T.LayerModes.y, rockAuto, splat.g );
            float wSnow  = LayerWeight( u_T.LayerModes.z, snowAuto, splat.b );

            vec3 albedo = rockT;
            albedo      = mix( albedo, grassT, clamp( wGrass, 0.0, 1.0 ) );
            albedo      = mix( albedo, rockT, clamp( wRock, 0.0, 1.0 ) );
            albedo      = mix( albedo, snowT, clamp( wSnow, 0.0, 1.0 ) );

            // PBR-ish lighting using the SCENE directional light (dir/color/intensity). Camera position recovered
            // from the inverse view matrix for the specular view vector.
            vec3 camPos = inverse( u.View )[3].xyz;
            vec3 V      = normalize( camPos - v_WorldPos );
            vec3 L      = normalize( -u.SunDir.xyz );          // to-light (engine stores travel direction)
            vec3 sun    = u.SunColor.rgb * max( u.SunColor.a, 0.0001 );
            vec3 H      = normalize( L + V );

            float ndl = max( dot( N, L ), 0.0 );

            // Soft sky/ground ambient (hemisphere): brighter from above, cooler/darker from below.
            vec3  skyCol  = vec3( 0.45, 0.52, 0.62 );
            vec3  gndCol  = vec3( 0.20, 0.18, 0.14 );
            vec3  ambient = mix( gndCol, skyCol, N.y * 0.5 + 0.5 ) * 0.5;

            // Per-layer roughness: rock/grass matte, snow has a subtle sheen.
            float gloss = mix( 0.0, 0.5, clamp( wSnow, 0.0, 1.0 ) );
            float spec  = pow( max( dot( N, H ), 0.0 ), mix( 16.0, 90.0, gloss ) ) * gloss;

            // The cloud layer attenuates the SUN and nothing else: the hemisphere ambient above is the
            // whole sky dome, which a deck occludes with a different geometry than a direction. Same
            // split the deferred composite and the mesh shaders make.
            float cloudShadow = CloudShadowFactor( v_WorldPos );

            vec3 lit = albedo * ( ambient + sun * ndl * cloudShadow ) + sun * spec * ndl * cloudShadow;

            o_Color = vec4( lit * u_Material.Tint.rgb, 1.0 );
        }
    }
}
