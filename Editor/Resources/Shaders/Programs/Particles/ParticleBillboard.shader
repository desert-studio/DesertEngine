// DesertAsset {"Kind":"Shader","Guid":"367fecbc7338d8efc9d3dbd1f92fa6f2","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ParticleBillboard"
{
    Vertex
    {
        // Vertexless billboard draw: 6 vertices per particle, corners from gl_VertexIndex. Each particle is
        // read from the simulation's storage buffer by index and expanded into a camera-facing quad in view
        // space. Size + colour were baked by the compute pass, so the only bindings are the shared camera UB
        // and the particle buffer. Drawn INDIRECT (Renderer::DrawProceduralIndirect, the emitter's last compact slot).

        #include <Common/CameraUB.glslh>

        // The element layout, shared with ParticleSimulate.shader rather than restated here — the two
        // used to declare it independently, and Д26 gave Age.yzw a meaning in only one of them.
        #include <Common/ParticleState.glslh>

        ReadBuffer(1) Particles
        {
            Particle u_Particles[];
        };

        // VFX-07: the emitter's alive list in the world pool. The draw is indirect (ParticleCompact's slot):
        // VertexCount = 6 x alive, FirstVertex = 6 x the emitter's pool base, so gl_VertexIndex / 6 is the alive
        // entry Base + k and every drawn particle is alive.
        ReadBuffer(2) AliveList
        {
            uint u_Alive[];
        };

        Out(0) vec2 v_UV;
        Out(1) vec4 v_Color;

        void main()
        {
            uint corner = uint( gl_VertexIndex ) % 6u;

            Particle p = u_Particles[u_Alive[uint( gl_VertexIndex ) / 6u]];

            const vec2 corners[6] = vec2[6]( vec2( -1.0, -1.0 ), vec2( 1.0, -1.0 ), vec2( -1.0, 1.0 ),
                                             vec2( 1.0, -1.0 ), vec2( 1.0, 1.0 ), vec2( -1.0, 1.0 ) );
            vec2 c  = corners[corner];
            v_UV    = c * 0.5 + 0.5;
            v_Color = p.Color;

            // Camera-facing: offset the particle centre in VIEW space so the quad always faces the camera.
            vec3 viewPos = ( cameraUB.View * vec4( p.PosSize.xyz, 1.0 ) ).xyz;
            viewPos.xy += c * ( p.PosSize.w * 0.5 );
            // Scene raster before the temporal resolve: jittered like every mesh (ApplyJitter = clip.xy += JitterNdc * w).
            gl_Position = cameraUB.Projection * vec4( viewPos, 1.0 );
            gl_Position.xy += cameraUB.JitterNdc * gl_Position.w;
        }
    }

    Fragment
    {
        In(0) vec2 v_UV;
        In(1) vec4 v_Color;
        Out(0) vec4 o_Color;

        void main()
        {
            // Soft round sprite: radial falloff from the quad centre.
            vec2  d    = v_UV * 2.0 - 1.0;
            float mask = smoothstep( 1.0, 0.0, dot( d, d ) );
            float a    = v_Color.a * mask;
            if ( a <= 0.0 )
                discard;
            o_Color = vec4( v_Color.rgb, a );
        }
    }
}
