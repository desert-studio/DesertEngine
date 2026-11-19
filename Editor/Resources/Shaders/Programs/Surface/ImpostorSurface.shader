// DesertAsset {"Kind":"Shader","Guid":"2f529b86e17c474b9d8a426559632481","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ImpostorSurface"
{
    // THE OCTAHEDRAL IMPOSTOR (IMPOSTORS-HLOD; UE ImpostorBaker's Impostor material + ImposterCommon.ush): the far
    // copy of a mesh as ONE camera-facing card per instance, shaded from an atlas of FramesPerSide^2 orthographic
    // photos the HLOD impostor builder baked (Core::BakeImpostorAtlas; editor HLODImpostorBake). The card is placed by
    // the vertex factory (`VertexFactory Impostor`: Mesh/Surface/Vertex_Instanced.glslh, Common/ImpostorOctahedral.glslh);
    // this surface reprojects the pixel's view ray into the three frames around the view direction, blends them, cuts
    // the pixel the blended coverage leaves empty and moves its depth onto the blended surface point (pixel depth
    // offset, in the view, the depth prepass, the cascades and the virtual shadow map alike).
    //
    // The atlas maps are the bake's (Core::HLODImpostorAtlas): BaseColor (sRGB, A = coverage), NormalDepth (LINEAR:
    // impostor-space normal * 0.5 + 0.5, A = depth behind the frame's centre plane), ORM (AO, Roughness, Metallic).
    // ImpostorSphere is the sphere the frames were photographed around (impostor = mesh-local space, centre xyz,
    // radius w); FramesPerSide / FrameResolution / Hemisphere the bake's HLODImpostorSettings. One material per baked
    // mesh (M_I<key>.demat, the builder writes every value).

    Domain Surface
    VertexFactory Impostor
    // The card turns to every viewer, so either winding may face it: no cull (the surface reads the flip back).
    TwoSided

    Properties Binding(2)
    {
        Texture2D   BaseColorAtlas ("Base Color Atlas", Category("Impostor"), Intent(Colour))
        Texture2D   NormalDepthAtlas ("Normal Depth Atlas", Category("Impostor"), Intent(Data))
        Texture2D   OrmAtlas ("ORM Atlas", Category("Impostor"), Intent(PackedMasks))
        Vec4        ImpostorSphere ("Sphere (centre, radius)", Category("Impostor")) = (0, 0, 0, 100)
        Float       FramesPerSide ("Frames Per Side", Range(8,16), Category("Impostor")) = 12
        Float       FrameResolution ("Frame Resolution", Range(32,1024), Category("Impostor")) = 256
        Float       Hemisphere ("Hemisphere", Range(0,1), Category("Impostor")) = 1
        // The blended coverage below it is cut (UE Opacity Mask Clip Value of the impostor material).
        Float       OpacityMaskClipValue ("Opacity Mask Clip Value", Range(0,1), Category("Impostor")) = 0.5
        Float       ReceiveSunShadows ("Receive Sun Shadows", Range(0,1), Category("Shadows")) = 1
        Float       ReceiveDecals ("Receive Decals", Range(0,1), Category("Decals")) = 0
    }

    // The frame math (no bindings), in every stage ahead of the surface: the vertex factory includes it as well.
    Include
    {
        #include <Common/ImpostorOctahedral.glslh>
    }

    BlendMode Masked
    // The coverage is the three frames' blended BaseColor A at view-dependent UVs: computed, not one texel.
    OpacityMask Function(ImpostorCoverage)

    Surface
    {
        // The pixel's ray and the three frames it reads, in impostor space (the vertex factory's varyings: UV0 / UV1.x
        // = the card point there, UV1.y = the instance's scale, TBN = impostor-to-world rotation, Normal = the card's
        // facing - DesertSurfaceInput negates both on a back face, which the dot with the view ray undoes).
        struct ImpostorPixel
        {
            vec3          RayOrigin;
            vec3          RayDirection;
            mat3          ToWorld;
            float         Flip; // DesertSurfaceInput's two-sided sign, read back
            float         Scale;
            ImpostorBlend Blend;
            vec2          UV[3];
            vec3          PlaneHit[3];
            vec2          GradX[3];
            vec2          GradY[3];
        };

        ivec2 ImpostorFrameOf( ImpostorBlend b, int k )
        {
            return k == 0 ? b.Frame0 : k == 1 ? b.Frame1 : b.Frame2;
        }

        ImpostorPixel ImpostorSetup( SurfaceInput i )
        {
            const vec3  rayWorld   = DesertSurfaceViewRay( i.WorldPosition );
            const float flip       = dot( i.Normal, rayWorld ) <= 0.0 ? 1.0 : -1.0;
            const int   frames     = int( Material.FramesPerSide + 0.5 );
            const int   resolution = int( Material.FrameResolution + 0.5 );
            const bool  hemi       = Material.Hemisphere > 0.5;
            const vec3  centre     = Material.ImpostorSphere.xyz;
            const float radius     = max( Material.ImpostorSphere.w, 1.0e-3 );

            ImpostorPixel p;
            p.Flip         = flip;
            p.ToWorld      = i.TBN * flip;
            p.RayOrigin    = vec3( i.UV0, i.UV1.x );
            p.RayDirection = normalize( transpose( p.ToWorld ) * rayWorld );
            p.Scale        = max( i.UV1.y, 1.0e-4 );
            p.Blend        = ImpostorBlendFrames( -p.RayDirection, frames, hemi );
            // The atlas gradients from the card point's own screen derivatives, projected into each frame: the frame
            // index may change between the pixels of a quad, the card point never jumps.
            const vec3 dx = dFdx( p.RayOrigin );
            const vec3 dy = dFdy( p.RayOrigin );
            for ( int k = 0; k < 3; ++k )
            {
                const ivec2 frame = ImpostorFrameOf( p.Blend, k );
                p.UV[k] = ImpostorFrameAtlasUV( frame, p.RayOrigin, p.RayDirection, centre, radius, frames, resolution,
                                                hemi, p.PlaneHit[k] );
                vec3 right, up;
                ImpostorFrameBasis( ImpostorFrameDirection( frame, frames, hemi ), right, up );
                const float toAtlas = 0.5 / ( radius * float( frames ) );
                p.GradX[k] = vec2( dot( dx, right ), -dot( dx, up ) ) * toAtlas;
                p.GradY[k] = vec2( dot( dy, right ), -dot( dy, up ) ) * toAtlas;
            }
            return p;
        }

        float ImpostorWeight( ImpostorBlend b, int k )
        {
            return k == 0 ? b.Weights.x : k == 1 ? b.Weights.y : b.Weights.z;
        }

        float ImpostorCoverage( SurfaceInput i )
        {
            const ImpostorPixel p = ImpostorSetup( i );
            float coverage = 0.0;
            for ( int k = 0; k < 3; ++k )
                coverage += ImpostorWeight( p.Blend, k ) *
                            SampleBindlessGrad( Material.BaseColorAtlas, p.UV[k], p.GradX[k], p.GradY[k] ).a;
            return coverage;
        }

        SurfaceOutput EvaluateSurface( SurfaceInput i )
        {
            SurfaceOutput       s      = DefaultSurfaceOutput();
            const ImpostorPixel p      = ImpostorSetup( i );
            const int           frames = int( Material.FramesPerSide + 0.5 );
            const bool          hemi   = Material.Hemisphere > 0.5;
            const float         radius = max( Material.ImpostorSphere.w, 1.0e-3 );

            vec3  baseColor = vec3( 0.0 );
            vec3  normal    = vec3( 0.0 );
            vec3  orm       = vec3( 0.0 );
            vec3  surface   = vec3( 0.0 );
            for ( int k = 0; k < 3; ++k )
            {
                const float w  = ImpostorWeight( p.Blend, k );
                const vec4  bc = SampleBindlessGrad( Material.BaseColorAtlas, p.UV[k], p.GradX[k], p.GradY[k] );
                const vec4  nd = SampleBindlessGrad( Material.NormalDepthAtlas, p.UV[k], p.GradX[k], p.GradY[k] );
                baseColor += w * bc.rgb;
                normal += w * ( nd.xyz * 2.0 - 1.0 );
                orm += w * SampleBindlessGrad( Material.OrmAtlas, p.UV[k], p.GradX[k], p.GradY[k] ).rgb;
                surface += w * ImpostorFrameSurfacePoint( ImpostorFrameOf( p.Blend, k ), p.PlaneHit[k], nd.a, radius,
                                                          frames, hemi );
            }
            s.BaseColor        = baseColor;
            // Impostor-space normal: the cells take i.TBN * s.Normal, and i.TBN is the impostor-to-world rotation times
            // the two-sided sign, which the flip cancels.
            s.Normal           = DesertSafeNormalize( normal * p.Flip, vec3( 0.0, 1.0, 0.0 ) );
            s.AmbientOcclusion = orm.x;
            s.Roughness        = orm.y;
            s.Metallic         = orm.z;
            // The blended surface point behind the card, along the ray, in world centimetres (>= 0: the card is in
            // front of the whole sphere, ImpostorCardCornerFacing).
            s.PixelDepthOffset    = max( dot( surface - p.RayOrigin, p.RayDirection ), 0.0 ) * p.Scale;
            s.ReceiveSunShadows   = Material.ReceiveSunShadows;
            s.ReceiveDecals       = Material.ReceiveDecals;
            s.SampledTextureCount = 9.0;
            return s;
        }
    }
}
