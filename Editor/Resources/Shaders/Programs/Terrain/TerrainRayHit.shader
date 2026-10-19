// DesertAsset {"Kind":"Shader","Guid":"9d13f93506c941749e97b61bcf80aa15","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TerrainRayHit"
{
    // THE LANDSCAPE'S HIT GROUP (UE: the landscape material's ray tracing hit shaders - a landscape section in the
    // ray tracing scene is shaded by its own material, and its Visibility Mask is the any-hit opacity). Every
    // landscape tile segment's hit record names it (LandscapeRayTracing.hpp kLandscapeRayHitTemplate) with the
    // tile's row (LandscapeRayHitRow, Programs/Terrain/TerrainRayHit.glslh); the frame's hit group table and the
    // ray tracing pipeline address it as "TerrainRayHit/RayTracing.Hit", like a surface template's cell.
    //
    //   closest hit  the ground at the hit into the ray's payload: the painted layers over the root's ground
    //                (Common/LandscapeSurface.glslh, the statement the G-buffer program shades with) times the
    //                material's Tint, the matte roughness, the interpolated vertex normal facing the ray (the
    //                raster draws the tile with Cull None). No lighting: the ray generation lights the payload;
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
                LandscapeRayHitRowRef       row  = LandscapeRayHitRowRef( u_RayHitRecord.MaterialRow );
                const TerrainInstance       tile = row.Tile;
                vec4                        w0;
                vec4                        w1;
                LandscapeRayHitWeights( tile, row.Weightmap, LandscapeRayHitPosition(), w0, w1 );

                // The landscape's vertices are written in world space (identity instance transform); the
                // inverse transpose is applied all the same, as every hit group does.
                const DesertRayHitTriangle t = DesertRayHitFetch();
                vec3 N = normalize( ( t.Normal * gl_WorldToObjectEXT ).xyz );
                if ( dot( N, gl_WorldRayDirectionEXT ) > 0.0 )
                    N = -N;

                payload.HitT      = gl_HitTEXT;
                payload.BaseColor = LandscapeGroundAlbedo( tile, w0, w1 ) * row.Tint.rgb;
                payload.Metallic  = 0.0;
                payload.Roughness = kLandscapeRoughness;
                payload.Emissive  = vec3( 0.0 );
                payload.Normal    = N;
                payload.Flags     = gl_HitKindEXT == gl_HitKindFrontFacingTriangleEXT ? kDesertRayPayloadFrontFace : 0u;
                DesertRayHitIdentify();
            }
        }

        AnyHit
        {
            #include <Programs/Terrain/TerrainRayHit.glslh>

            void main()
            {
                LandscapeRayHitRowRef       row  = LandscapeRayHitRowRef( u_RayHitRecord.MaterialRow );
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
