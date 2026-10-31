// DesertAsset {"Kind":"Shader","Guid":"9d13f93506c941749e97b61bcf80aa15","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TerrainRayHit"
{
    // THE LANDSCAPE'S HIT GROUP (UE: the landscape material's ray tracing hit shaders - a landscape section in the
    // ray tracing scene is shaded by its own material, and its Visibility Mask is the any-hit opacity). Every
    // landscape tile segment's hit record names it (LandscapeRayTracing.hpp kLandscapeRayHitTemplate) with the
    // tile's row (LandscapeRayHitRow, Programs/Terrain/TerrainRayHit.glslh); the frame's hit group table and the
    // ray tracing pipeline address it as "TerrainRayHit/RayTracing.Hit", like a surface template's cell.
    //
    //   closest hit  the ground at the hit into the ray's payload: Common/LandscapeSurface.glslh
    //                EvaluateLandscapeSurface, the statement the G-buffer program shades with - the material's layer
    //                inputs (albedo, normal, ARM roughness, height blend), far tiling and macro variation over the
    //                painted weights - with the row's copy of the material's values and its layer maps through the
    //                bindless table at the ray cone's level; the albedo times the material's Tint, metallic 0 as the
    //                G-buffer writes it, the interpolated vertex normal facing the ray (the raster draws the tile
    //                with Cull None) as the geometric normal. No lighting: the ray generation lights the payload;
    //   any hit      the hole test of the tile's masked segment (the cells that touch a hole): a candidate in a
    //                hole is ignored, the raster's discard (LandscapeTileIsHole). The opaque segment has no hole
    //                and never runs it.
    //
    // No Properties: the row is the tile's, written by the landscape's ray tracing, never an artist's.
    Pass "RayTracing.Hit"
    {
        ClosestHit
        {
            #include <Programs/Terrain/TerrainRayHit.glslh>

            void main()
            {
                LandscapeRayHitRowRef row      = LandscapeRayHitRowRef( RayHitRecord.MaterialRow );
                g_LandscapeRayHitRow           = row;
                const TerrainInstance tile     = row.Tile;
                const vec3            position = LandscapeRayHitPosition();
                vec4                  w0;
                vec4                  w1;
                LandscapeRayHitWeights( tile, row.Weightmap, position, w0, w1 );

                // The landscape's vertices are written in world space (identity instance transform); the
                // inverse transpose is applied all the same, as every hit group does.
                const DesertRayHitTriangle t = DesertRayHitFetch();
                vec3 Ng = normalize( ( t.Normal * gl_WorldToObjectEXT ).xyz );
                if ( dot( Ng, gl_WorldRayDirectionEXT ) > 0.0 )
                    Ng = -Ng;
                LandscapeRayHitConeLod( Ng );

                const float    viewDistance = distance( position, row.ViewOrigin.xyz );
                TerrainSurface s = EvaluateLandscapeSurface( tile, row.MaterialValues, position, Ng, viewDistance, w0, w1 );
                s.Albedo *= row.Tint.rgb;
                // The grass field's far cover as the raster lays it (GRASS-FIELD), seen along the ray: the ray's
                // origin view is the one whose blades are thinned out. The payload has no shading model, so the
                // cover's transmitted light is the raster's alone.
                vec3 transmission;
                LandscapeApplyGrassCover( LandscapeGrassCoverAt( tile, w0, w1, viewDistance, row.GrassLens ), -gl_WorldRayDirectionEXT,
                                          Ng, s.Albedo, s.N, s.Roughness, s.AO, transmission );

                payload.HitT      = gl_HitTEXT;
                payload.BaseColor = s.Albedo;
                payload.Metallic  = 0.0;
                payload.Roughness = s.Roughness;
                payload.Emissive  = vec3( 0.0 );
                payload.Normal    = s.N;
                payload.Flags     = gl_HitKindEXT == gl_HitKindFrontFacingTriangleEXT ? kDesertRayPayloadFrontFace : 0u;
                DesertRayHitIdentify();
            }
        }

        AnyHit
        {
            #include <Programs/Terrain/TerrainRayHit.glslh>

            void main()
            {
                LandscapeRayHitRowRef       row  = LandscapeRayHitRowRef( RayHitRecord.MaterialRow );
                const TerrainInstance       tile = row.Tile;
                vec4                        w0;
                vec4                        w1;
                LandscapeRayHitWeights( tile, row.Weightmap, LandscapeRayHitPosition(), w0, w1 );
                if ( LandscapeTileIsHole( tile, w0, w1 ) )
                    ignoreIntersectionEXT;
            }
        }
    }
}
