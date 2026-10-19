// DesertAsset {"Kind":"Shader","Guid":"bcefeba392c545f6b95dcb1c2554b7a8","Versions":{"SHDR":1},"Dependencies":[]}
Shader "WaterSurface"
{
    // THE WATER SURFACE (WATER-W6): every tile the water quadtree selected (Engine/Water/WaterQuadTree.hpp), drawn
    // by Graphic::System::WaterRenderer after the scene-colour copy and before the glass, into the scene target with
    // depth. One vertexless instanced draw per grid density: the vertex stage decodes its quad from gl_VertexIndex,
    // its tile row from MaterialIndex + gl_InstanceIndex, and morphs toward the next LOD by distance (UE's water
    // vertex factory, ported) so a LOD boundary never opens a crack. The surface is the bodies' still plane; the
    // waves and the single-layer-water shading are WATER-W7.

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
            WaterTileRow t = u_Tiles[int( m_PushConstants.MaterialIndex ) + gl_InstanceIndex];

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
        Out(1) vec2 oVelocity; // the view's velocity, slot 1 of the scene target
        // The view's thin-geometry coverage, slot 3 of the scene target: this pass writes the view's depth. Water is
        // a lit surface that is not foliage (UE: SingleLayerWater counts as a regular surface).
        Out(3) float oThinGeometryCoverage;

        #include <Programs/Water/WaterSurfaceData.glslh>
        #include <Common/ObjectMotion.glslh>
        #include <Common/ThinGeometryCoverage.glslh>

        void main()
        {
            // The still plane, lit by the scene sun: Schlick Fresnel between the water body's colour and the sky it
            // mirrors, plus the sun's glint. WATER-W7 moves this to UE's single layer water (scene-copy refraction,
            // volume absorption and scattering, SSR).
            const vec3 N    = vec3( 0.0, 1.0, 0.0 );
            vec3       V    = normalize( u.CameraPos.xyz - v_WorldPos );
            vec3       L    = normalize( -u.SunDir.xyz );
            vec3       sun  = u.SunColor.rgb * max( u.SunColor.a, 0.0001 );
            float      NdV  = abs( dot( N, V ) );
            float      fres = 0.02 + 0.98 * pow( 1.0 - NdV, 5.0 );

            vec3 skyCol   = vec3( 0.45, 0.52, 0.62 ) * 0.5;
            vec3 waterCol = vec3( 0.02, 0.09, 0.13 );
            vec3 body     = waterCol * ( skyCol + sun * max( L.y, 0.0 ) * 0.25 );
            vec3 H        = normalize( V + L );
            vec3 glint    = sun * pow( max( dot( N, H ), 0.0 ), 400.0 ) * fres * step( 0.0, L.y );

            o_Color   = vec4( mix( body, skyCol, fres ) + glint, 1.0 );
            vec4 world = vec4( v_WorldPos, 1.0 );
            oVelocity  = DesertVelocity( u.ViewProjection * world, u.PrevViewProjection * world );
            oThinGeometryCoverage = kThinGeometryCoverageSurface;
        }
    }
}
