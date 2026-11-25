// DesertAsset {"Kind":"Shader","Guid":"b41f6f0ce27d49a5a8c3e9105d7764b2","Versions":{"SHDR":1},"Dependencies":[]}
Shader "WaterFoamUpdate"
{
    Fragment
    {
        // THE FOAM'S STEP (Water/WaterFoamField.glslh; System::WaterRenderer, "Water: Foam"): every texel of the
        // three windows takes the last frame's foam from upstream, lets it decay and adds what breaks on it now.
        // A texel the windows did not hold starts from the stationary state of its sources, never from nothing.
        #define DESERT_LINEAR_REPEAT_SAMPLER
        Uniform sampler LinearRepeatSampler;
        #include <Mesh/Surface/WaterMeshTiles.glslh>

        In(0) vec2 v_TexCoord;
        Out(0) vec4 o_Foam;

        Uniform(0) WaterFoamUB
        {
            vec4 FoamPrevious[3]; // xy = the history's window origins
            vec4 FoamTime;        // x = the time (s), y = since the history (s), z = 1 with a history, w = still Y
            vec4 FoamSource;      // x = jacobian threshold, y = softness, z = gain, w = the wave set (-1: none)
            vec4 FoamDecay;       // x = foam, y = bubbles, z = wetness (s), w = the beach's slope
            vec4 FoamWind;        // xy = the surface drift (cm/s)
        };
        Uniform(1) texture2D FoamHistoryTexture;

        #define WATER_FOAM_FETCH( t ) texelFetch( FoamHistoryTexture, t, 0 )
        #define WATER_FOAM_ORIGIN( l ) vec3( FoamPrevious[l].xy, WaterFoamOrigin[l].z )
        #define WATER_FOAM_SIDE WaterFoamShape.x
        #include <Water/WaterFoamField.glslh>

        // White water by speed: a river's own, from clear at 2 m/s to white at 5 m/s.
        float SpeedFoam( float speed )
        {
            return smoothstep( 200.0, 500.0, speed );
        }

        void main()
        {
            const ivec2 pixel = ivec2( gl_FragCoord.xy );
            const int   side  = int( WaterFoamShape.x + 0.5 );
            const int   level = min( pixel.x / side, 2 );
            const ivec2 slot  = ivec2( pixel.x - level * side, pixel.y );
            const ivec2 lower = ivec2( WaterFoamOrigin[level].xy );
            const ivec2 ij    = lower + ( ( slot - lower ) & ( side - 1 ) );
            const float texel = 1.0 / WaterFoamOrigin[level].z;
            const vec2  world = ( vec2( ij ) + 0.5 ) * texel;

            float river;
            const bool  onRiver = WaterRiverLevel( world, river );
            const vec3  flow    = onRiver ? WaterFlowAt( world ) : vec3( 0.0 );
            const float stillY  = onRiver ? river : FoamTime.w;
            const int   set     = onRiver ? -1 : int( floor( FoamSource.w + 0.5 ) );
            const float time    = FoamTime.x;
            const float dt      = FoamTime.y;
            const float slope   = onRiver ? 0.0 : FoamDecay.w;
            const WaterSurface s = WaterSurfaceAt( set, world, stillY, time, time, texel, slope );
            const vec4  swell    = set >= 0 ? WaterWaveSets[set].Swell : vec4( 0.0 );

            // What breaks here now. A coarse texel holds crests it does not resolve: its mean is softer, and lower.
            float cover = 0.0;
            if ( set >= 0 && s.Depth > 0.0 )
                cover = FoamSource.z * smoothstep( 0.0, FoamSource.y, FoamSource.x - s.Jacobian );
            cover = max( cover, s.Front );
            cover = max( cover, SpeedFoam( length( flow.xy ) ) );
            if ( onRiver )
                cover = max( cover, 0.7 * WaterRiverBankBand( world, 1.0 ) * smoothstep( 30.0, 120.0, length( flow.xy ) ) );
            cover = clamp( cover, 0.0, 1.0 );
            const float wetNow = set >= 0 && s.Depth + s.RunUp > 0.0 ? 1.0 : 0.0;

            // The stationary state: the surf's own lace inside the breaker line, the run-up's band of wet sand.
            const float surf = swell.y > 0.0 && slope > 0.0
                                    ? 0.45 * smoothstep( -0.3, 0.1, s.Depth / swell.y ) * ( 1.0 - smoothstep( 1.0, 2.0, s.Depth / swell.y ) )
                                    : 0.0;
            vec4 foam = vec4( max( cover, surf ), 0.6 * max( cover, surf ),
                              max( wetNow, swell.y > 0.0 && slope > 0.0 ? smoothstep( -1.0, -0.4, s.Depth / swell.y ) : 0.0 ), 0.5 );

            // The current that carries the foam: the wind's drift, the river, the breaking front's bore.
            const WaterShoreSample shore = WaterShoreAt( world );
            const vec2 bore     = s.Front * sqrt( GERSTNER_GRAVITY * max( s.Depth, 10.0 ) ) * shore.ShoreDirection;
            const vec2 velocity = ( onRiver ? flow.xy : FoamWind.xy ) + bore;
            const vec2 from     = world - velocity * dt;
            const vec2 at       = from * WaterFoamOrigin[level].z - FoamPrevious[level].xy;
            if ( FoamTime.z > 0.5 && all( greaterThanEqual( at, vec2( 0.5 ) ) ) && all( lessThanEqual( at, vec2( float( side ) - 0.5 ) ) ) )
            {
                const vec4 held = WaterFoamLevel( level, from );
                foam.r = max( held.r * exp( -dt / FoamDecay.x ), cover );
                foam.g = max( held.g * exp( -dt / FoamDecay.y ), 0.8 * cover );
                // Wet sand dries where it is; nothing carries it.
                foam.b = max( WaterFoamLevel( level, world ).b * exp( -dt / FoamDecay.z ), wetNow );
                foam.a = cover >= held.r ? 0.0 : min( held.a + dt / FoamDecay.x, 1.0 );
            }
            // Eight bits stall a slow decay: a step is kept in the mean by one dithered count.
            const float noise = fract( 52.9829189 * fract( dot( gl_FragCoord.xy + 5.588238 * fract( time * 7.0 ) * 64.0, vec2( 0.06711056, 0.00583715 ) ) ) );
            o_Foam = clamp( foam + ( noise - 0.5 ) / 255.0, vec4( 0.0 ), vec4( 1.0 ) );
        }
    }

    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
            v_TexCoord  = FullscreenTriangleUV();
            gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }
}
