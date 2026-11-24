// DesertAsset {"Kind":"Shader","Guid":"d633c40a6746443087643b0b7d0d87cc","Versions":{"SHDR":1},"Dependencies":[]}
Shader "WaterCausticsSplat"
{
    Compute
    {
        // THE CAUSTICS' FIRST HALF (WATER-CAUSTICS, Common/WaterCaustics.glslh; Wallace / GPU Gems 1 ch. 2): one
        // thread per map texel shoots a 2x2 grid of sun rays at the water surface the water mesh draws (the
        // body's Gerstner waves of this frame, damped over the WaterInfo ground as the vertex path damps them),
        // refracts each into the water and follows it to the floor below; the light it carries - the flux the
        // displaced surface patch intercepts - is splatted bilinearly where it lands, by integer atomics into
        // WaterCausticsEnergy (cleared by the graph before this pass). Where the waves focus the beams, many
        // land on one texel: the bright lines. WaterCausticsResolve turns the counts into the map.
        LocalSize(8, 8, 1);

        // Binding 24 WaterTiles (only its waves block: the frame's WaterRenderer upload head) and binding 25
        // WaterInfoTexture: the wave evaluation the water mesh itself draws with.
        #include <Mesh/Surface/WaterMeshTiles.glslh>

        // The frame row (Common/WaterCaustics.glslh): its body table gives each texel's still level.
        layout(std430, binding = 1) readonly buffer WaterCausticsFrameUB
        {
        	vec4 WaterCausticsWindow;
        	vec4 WaterCausticsShape;
        	vec4 WaterCausticsBodies[];
        };
        #define WATER_CAUSTICS_FRAME
        #include <Common/WaterCaustics.glslh>

        Buffer(0) WaterCausticsEnergy
        {
            uint Energy[WATER_CAUSTICS_SIZE * WATER_CAUSTICS_SIZE];
        };

        PushConstant PushConstants
        {
            vec4 Window; // xy = world (X, Z) of the map's lower corner (cm), z = the texel's side (cm),
                         // w = the floor's depth where no WaterInfo ground is baked (cm)
            vec4 Sun;    // xyz = the direction the sunlight travels (normalised, y < 0), w = time (s, cameraUB.Time)
            vec4 Camera; // xyz = the view's world position (cm, cameraUB.CameraPos): the FFT cascades' distance fade
        };

        // THE ONE SURFACE QUERY: the water surface's point and normal above the still-plane point @p worldXZ, and
        // the horizontal Jacobian of its displacement (the share of the plane cell's area the displaced patch
        // keeps: < 1 where the crests bunch the water together). The very calls Vertex_WaterMesh draws with: the
        // body's wave set @p waveSet (Gerstner or the FFT ocean, WaterWaveSetOf; -1 = flat), damped by the same
        // WaterWaveAttenuation, the cascades faded by the same still-particle distance to the eye.
        void WaterCausticsSurface( int waveSet, vec2 worldXZ, float stillY, float attenuation, float viewDist,
                                   float time, out vec3 position, out vec3 normal, out float jacobian )
        {
            const vec2 plane = WaterWavePlane( waveSet, worldXZ );
            position = vec3( worldXZ.x, stillY, worldXZ.y ) +
                       WaterWavesDisplacement( waveSet, plane, time, viewDist ) * attenuation;
            normal   = normalize(
                 mix( vec3( 0.0, 1.0, 0.0 ), WaterWavesNormal( waveSet, plane, time, viewDist ), attenuation ) );
            jacobian = mix( 1.0, WaterWavesJacobian( waveSet, plane, time, viewDist ), attenuation );
        }

        void SplatEnergy( vec2 land, float energy )
        {
            // Texel-centre space: texel (x, y) is centred on (x, y).
            const vec2  p  = ( land - Window.xy ) / Window.z - 0.5;
            const ivec2 i0 = ivec2( floor( p ) );
            const vec2  f  = p - vec2( i0 );
            const float w[4] = float[4]( ( 1.0 - f.x ) * ( 1.0 - f.y ), f.x * ( 1.0 - f.y ), ( 1.0 - f.x ) * f.y, f.x * f.y );
            for ( int k = 0; k < 4; ++k )
            {
                const ivec2 t = i0 + ivec2( k & 1, k >> 1 );
                if ( any( lessThan( t, ivec2( 0 ) ) ) || any( greaterThanEqual( t, ivec2( WATER_CAUSTICS_SIZE ) ) ) )
                    continue;
                const uint counts = uint( energy * w[k] * WATER_CAUSTICS_ENERGY_SCALE + 0.5 );
                if ( counts > 0u )
                    atomicAdd( Energy[t.y * WATER_CAUSTICS_SIZE + t.x], counts );
            }
        }

        void main()
        {
            const ivec2 cell = ivec2( gl_GlobalInvocationID.xy );
            if ( any( greaterThanEqual( cell, ivec2( WATER_CAUSTICS_SIZE ) ) ) )
                return;
            const vec3  sun  = Sun.xyz;
            const float time = Sun.w;
            const float eta  = 1.0 / WATER_CAUSTICS_IOR;
            // Still water's refracted ray: the shift from where a ray enters to where it lands, per cm of depth.
            // The rays are shot from the texel moved back by it, so still water lands each one in its own texel.
            const vec3 still      = refract( sun, vec3( 0.0, 1.0, 0.0 ), eta );
            const vec2 stillShift = still.xz / max( -still.y, 1e-3 );
            for ( int s = 0; s < 4; ++s )
            {
                const vec2  sub         = ( vec2( s & 1, s >> 1 ) + 0.5 ) * 0.5;
                const vec2  target      = Window.xy + ( vec2( cell ) + sub ) * Window.z;
                // The body over the target (the frame row's table): of the footprints that hold it, the one whose
                // baked ground lies the least deep under its level (the water the floor there is under, where two
                // footprints overlap), else the first that holds it (no ground baked). A texel under no body's
                // footprint lights none.
                int   body        = -1;
                float targetDepth = 0.0;
                for ( int i = 0; i < int( WaterCausticsShape.x ); ++i )
                {
                    const vec4 footprint = WaterCausticsBodies[2 * i];
                    if ( any( lessThan( target, footprint.xy ) ) || any( greaterThan( target, footprint.zw ) ) )
                        continue;
                    const float d = WaterGroundDepth( target, WaterCausticsBodyLevel( i ) );
                    if ( body < 0 || ( d > 0.0 && ( targetDepth <= 0.0 || d < targetDepth ) ) )
                    {
                        body        = i;
                        targetDepth = d;
                    }
                }
                if ( body < 0 )
                    continue;
                const float stillY      = WaterCausticsBodyLevel( body );
                // -1 for a flat body: the still surface lands every ray in its own texel, the map reads 1 there.
                const int   waveSet     = WaterWaveSetOf( WaterCausticsBodyWaveLane( body ) );
                const vec2  source      = target - stillShift * ( targetDepth > 0.0 ? targetDepth : Window.w );
                // The floor under the ray's entry: the WaterInfo ground (the waves damp over it as the drawn
                // surface's do, WaterWaveAttenuation), or the push's depth where none is baked.
                const float ground      = WaterGroundDepth( source, stillY );
                const float depth       = ground > 0.0 ? ground : Window.w;
                const float attenuation = WaterWaveAttenuation( source, stillY, WaterCausticsBodyWaveMaskDepth( body ) );
                const float viewDist    = WaterViewDistance( source, stillY, Camera.xyz );
                vec3  position;
                vec3  normal;
                float jacobian;
                WaterCausticsSurface( waveSet, source, stillY, attenuation, viewDist, time, position, normal,
                                      jacobian );
                const vec3 ray = refract( sun, normal, eta );
                if ( ray.y >= -1e-4 )
                    continue; // grazing: it never reaches the floor
                const float drop = position.y - ( stillY - depth );
                const vec2  land = position.xz + ray.xz * ( drop / -ray.y );
                // A quarter texel of still-water irradiance per ray, times the share of it the patch intercepts.
                SplatEnergy( land, 0.25 * max( jacobian, 0.0 ) );
            }
        }
    }
}
