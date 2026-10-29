// DesertAsset {"Kind":"Shader","Guid":"5c7221306c9643589755dfc28bebcaa7","Versions":{"SHDR":1},"Dependencies":[]}
// THE SURFACE CACHE'S INDIRECT LIGHTING (GI-SHADE-DARK; UE LumenRadiosity.usf LumenRadiosityIntegrateCS + the
// combine of LumenSceneLighting.usf): every page relit this frame (the same SceneFrame::LightingPages the direct
// lighting and the radiosity trace relit) integrates its texels' irradiance from the card's OWN radiosity probes
// (Common/SurfaceCacheRadiosity.glslh: the four nearest probes of the page, bilinear, each probe's traced hemisphere
// cosine-weighted about the texel's normal - SurfaceCacheRadiosityTrace.shader traced them this frame) and leaves
// IndirectLighting = albedo x that irradiance - plus, on a subsurface texel (the Subsurface layer), subsurface x the
// world radiance cache's irradiance at -N: the sky and bounce let through a leaf from its far side
// (FOLIAGE-LOOK-5; a probe's hemisphere holds only the front) - and recombines FinalLighting = direct + indirect +
// emissive. A traced hit then reads its bounce from the card in one fetch.
//
// As UE: the card's radiosity depends on no screen or world probe cache (GI-SHADE-DARK p1: the world cache was
// nearly empty at the surfaces, so shaded walls had no bounce at all and every ray reading them came back black).
// Runs after the radiosity trace of the frame; the trace's hits read the previous FinalLighting (one frame of lag,
// as UE's radiosity has).
Shader "SurfaceCacheIndirectLighting"
{
    Vertex
    {
        #include <Common/SurfaceCache.glslh>
        #include <Common/ScreenUV.glslh>
        ReadBuffer(0) SurfaceCardPageBuffer
        {
            SurfaceCardPage surfaceCardPages[];
        };
        ReadBuffer(1) SurfaceLightingPages
        {
            uint lightingPages[];
        };
        layout(location = 0) out flat uint v_CardIndex;
        layout(location = 1) out flat vec4 v_CardUVRect;
        layout(location = 2) out flat vec4 v_AtlasUVRect;

        void main()
        {
            const SurfaceCardPage page    = surfaceCardPages[lightingPages[gl_InstanceIndex]];
            const vec2            corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                                       vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
            const vec2 uv = page.physicalAtlasUVRect.xy + corners[gl_VertexIndex] * page.physicalAtlasUVRect.zw;
            v_CardIndex   = page.card.x;
            v_CardUVRect  = page.cardUVRect;
            v_AtlasUVRect = page.physicalAtlasUVRect;
            gl_Position   = vec4(ScreenUVToNdc(uv), 0.0, 1.0);
        }
    }
    Fragment
    {
        #include <Common/SurfaceCache.glslh>
        #include <Common/RadianceCache.glslh>
        ReadBuffer(2) SurfaceCardBuffer
        {
            SurfaceCard surfaceCards[];
        };
        Uniform sampler LinearClampSampler;
        Uniform sampler PointClampSampler;
        Uniform(3) texture2D AtlasDepthTexture;
        #define AtlasDepthTexture sampler2D(AtlasDepthTexture, PointClampSampler)
        Uniform(4) texture2D AtlasAlbedoTexture;
        #define AtlasAlbedoTexture sampler2D(AtlasAlbedoTexture, PointClampSampler)
        Uniform(5) texture2D AtlasOpacityTexture;
        #define AtlasOpacityTexture sampler2D(AtlasOpacityTexture, PointClampSampler)
        Uniform(6) texture2D AtlasNormalTexture;
        #define AtlasNormalTexture sampler2D(AtlasNormalTexture, PointClampSampler)
        Uniform(7) texture2D AtlasEmissiveTexture;
        #define AtlasEmissiveTexture sampler2D(AtlasEmissiveTexture, PointClampSampler)
        Uniform(8) texture2D AtlasDirectLightingTexture;
        #define AtlasDirectLightingTexture sampler2D(AtlasDirectLightingTexture, PointClampSampler)
        ReadBuffer(9) RadianceCacheParamsUB
        {
            RadianceCacheParams radianceCache;
        };
        ReadBuffer(10) RadianceCacheIndirectionUB
        {
            uint radianceCacheIndirection[];
        };
        ReadBuffer(11) RadianceCacheProbesUB
        {
            RadianceCacheProbe radianceCacheProbes[];
        };
        Uniform(12) texture2D RadianceCacheIrradianceTexture;
        #define RadianceCacheIrradianceTexture sampler2D(RadianceCacheIrradianceTexture, LinearClampSampler)
        Uniform(13) texture2D RadianceCacheDepthTexture;
        #define RadianceCacheDepthTexture sampler2D(RadianceCacheDepthTexture, LinearClampSampler)
        // The light the texel lets through (rgb); 0 = opaque (SurfaceCacheCardCopy).
        Uniform(14) texture2D AtlasSubsurfaceTexture;
        #define AtlasSubsurfaceTexture sampler2D(AtlasSubsurfaceTexture, PointClampSampler)
        // The radiosity probes' traced radiance, one stratum per texel (SurfaceCacheRadiosityTrace.shader).
        Uniform(15) texture2D RadiosityTraceTexture;
        #define RadiosityTraceTexture sampler2D(RadiosityTraceTexture, PointClampSampler)
        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #include <Common/RadianceCache.glslh>
        #define RADIOSITY_OPACITY AtlasOpacityTexture
        #include <Common/SurfaceCacheRadiosity.glslh>

        layout(location = 0) in flat uint v_CardIndex;
        layout(location = 1) in flat vec4 v_CardUVRect;
        layout(location = 2) in flat vec4 v_AtlasUVRect;
        Out(0) vec4 oIndirect;
        Out(1) vec4 oFinal;

        // The irradiance over pi at the texel of normal @p N from the probes of its page (UE IntegrateProbes):
        // the four nearest tiles' probes, bilinear by the texel's place among the tile centres, a probe facing
        // away from N (dot <= 0) left out; each probe's strata cosine-weighted about N.
        vec3 RadiosityIrradiance( ivec4 page, ivec2 atlasTexel, vec3 N )
        {
            const ivec2 inPage = atlasTexel - page.xy;
            const ivec2 tiles  = ( page.zw + RADIOSITY_PROBE_SPACING - 1 ) / RADIOSITY_PROBE_SPACING;
            const vec2  t      = ( vec2( inPage ) + 0.5 ) / float( RADIOSITY_PROBE_SPACING ) - 0.5;
            const ivec2 t0     = ivec2( floor( t ) );
            const vec2  f      = t - vec2( t0 );
            vec3  sum    = vec3( 0.0 );
            float weight = 0.0;
            for ( int corner = 0; corner < 4; ++corner )
            {
                const ivec2 offset = ivec2( corner & 1, corner >> 1 );
                const ivec2 tileId = clamp( t0 + offset, ivec2( 0 ), tiles - 1 );
                const vec2  b      = mix( 1.0 - f, f, vec2( offset ) );
                const float w      = b.x * b.y;
                if ( w <= 0.0 )
                    continue;
                const ivec4 tile = RadiosityTileOf( tileId, page.zw );
                ivec2       probeTexel;
                if ( !RadiosityProbeTexel( page.xy + tile.xy, tile.zw, probeTexel ) )
                    continue;
                const vec3 probeN =
                     RadiosityOctahedronToUnitVector( texelFetch( AtlasNormalTexture, probeTexel, 0 ).rg * 2.0 - 1.0 );
                if ( dot( probeN, N ) <= 0.0 )
                    continue;
                vec3 e = vec3( 0.0 );
                for ( int y = 0; y < tile.w; ++y )
                    for ( int x = 0; x < tile.z; ++x )
                    {
                        const vec3 direction =
                             RadiosityHemisphereDirection( ( vec2( x, y ) + 0.5 ) / vec2( tile.zw ), probeN );
                        const vec3 radiance =
                             texelFetch( RadiosityTraceTexture, page.xy + tile.xy + ivec2( x, y ), 0 ).rgb;
                        e += radiance * max( dot( N, direction ), 0.0 );
                    }
                sum += w * e * ( 2.0 / float( tile.z * tile.w ) );
                weight += w;
            }
            return weight > 0.0 ? sum / weight : vec3( 0.0 );
        }

        void main()
        {
            const ivec2 atlasTexel = ivec2(gl_FragCoord.xy);
            if (texelFetch(AtlasOpacityTexture, atlasTexel, 0).r < 0.5)
            {
                oIndirect = vec4(0.0);
                oFinal    = vec4(0.0);
                return;
            }
            // The texel's world position and normal, exactly as the direct lighting places them.
            const SurfaceCard card = surfaceCards[v_CardIndex];
            const ivec4 page = RadiosityPageTexels( v_AtlasUVRect, vec2( textureSize( AtlasOpacityTexture, 0 ) ) );
            const vec3  worldPos = RadiosityTexelWorldPosition( card, v_CardUVRect, page, atlasTexel,
                                                                texelFetch( AtlasDepthTexture, atlasTexel, 0 ).r );
            const vec3 N =
                 RadiosityOctahedronToUnitVector( texelFetch( AtlasNormalTexture, atlasTexel, 0 ).rg * 2.0 - 1.0 );

            const vec3 albedo   = texelFetch( AtlasAlbedoTexture, atlasTexel, 0 ).rgb;
            vec3       indirect = albedo * RadiosityIrradiance( page, atlasTexel, N );
            // The far side of a leaf, from the world radiance cache at -N.
            const vec3 subsurface = texelFetch(AtlasSubsurfaceTexture, atlasTexel, 0).rgb;
            if (max(subsurface.r, max(subsurface.g, subsurface.b)) > 0.0)
            {
                const vec4 back = RadianceCacheIrradianceAt(worldPos, -N, -N, 0.5);
                if (back.a > 0.0)
                    indirect += subsurface * max(back.rgb, vec3(0.0));
            }

            const vec3 emissive = texelFetch(AtlasEmissiveTexture, atlasTexel, 0).rgb;
            const vec3 direct   = texelFetch(AtlasDirectLightingTexture, atlasTexel, 0).rgb;
            oIndirect = vec4(indirect, 0.0);
            oFinal    = vec4(direct + indirect + emissive, 0.0);
        }
    }
}
