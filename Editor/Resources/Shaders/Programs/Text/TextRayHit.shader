// DesertAsset {"Kind":"Shader","Guid":"c4fa69873c074f8987686f202dd83035","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TextRayHit"
{
    // THE 3D TEXT'S HIT GROUP (UE: a text render component's font material in the ray tracing scene - the glyph
    // quads are its instance's geometry, the material's opacity mask its any-hit). Every glyph quad segment of a
    // label in the TLAS names it (Graphic/Systems/RayTracing/TextRayTracing.hpp kTextRayHitTemplate) with the
    // label's row (TextRayHit, Programs/Text/TextRayHit.glslh); the frame's hit group table and the ray tracing
    // pipeline address it as "TextRayHit/RayTracing.Hit".
    //
    //   closest hit  the label at the hit into the ray's payload, as Programs/Text/TextSDF.shader draws it: an
    //                emitter of TextColor x EmissiveIntensity with no reflectance (black base colour), the
    //                interpolated normal facing the ray (the raster draws the quads with Cull None). No lighting:
    //                the ray generation adds the emission;
    //   any hit      the glyph's coverage (TextRayHitCovered): a candidate outside the outline is ignored, as the
    //                raster discards it.
    //
    // No Properties: the row is the label's, written by the text system from the values its raster draw carries.
    Pass "RayTracing.Hit"
    {
        ClosestHit
        {
            #include <Programs/Text/TextRayHit.glslh>

            void main()
            {
                TextRayHitRowRef           row = TextRayHitRowRef( u_RayHitRecord.MaterialRow );
                const DesertRayHitTriangle t   = DesertRayHitFetch();
                vec3 N = normalize( ( t.Normal * gl_WorldToObjectEXT ).xyz );
                if ( dot( N, gl_WorldRayDirectionEXT ) > 0.0 )
                    N = -N;

                payload.HitT      = gl_HitTEXT;
                payload.BaseColor = vec3( 0.0 );
                payload.Metallic  = 0.0;
                payload.Roughness = 1.0;
                payload.Emissive  = row.TextColor.rgb * row.EmissiveIntensity.x;
                payload.Normal    = N;
                payload.Flags     = gl_HitKindEXT == gl_HitKindFrontFacingTriangleEXT ? kDesertRayPayloadFrontFace : 0u;
                DesertRayHitIdentify();
            }
        }

        AnyHit
        {
            #include <Programs/Text/TextRayHit.glslh>

            void main()
            {
                TextRayHitRowRef row = TextRayHitRowRef( u_RayHitRecord.MaterialRow );
                if ( !TextRayHitCovered( row, DesertRayHitFetch().UV ) )
                    ignoreIntersectionEXT;
            }
        }
    }
}
