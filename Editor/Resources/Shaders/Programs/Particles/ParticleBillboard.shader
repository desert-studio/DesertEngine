// DesertAsset {"Kind":"Shader","Guid":"367fecbc7338d8efc9d3dbd1f92fa6f2","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ParticleBillboard"
{
    Vertex
    {
        // Vertexless billboard draw: 6 vertices per particle, corners from gl_VertexIndex. Each particle is
        // read from the simulation's storage buffer by index and expanded into a camera-facing quad in view
        // space. Size + colour were baked by the compute pass, so the only bindings are the shared camera UB
        // and the particle buffer. Drawn via Renderer::SubmitVertices( pipeline, particleCount * 6, ... ).

        #include <Common/CameraUB.glslh>

        // The element layout, shared with ParticleSimulate.shader rather than restated here — the two
        // used to declare it independently, and Д26 gave Age.yzw a meaning in only one of them.
        #include <Common/ParticleState.glslh>

        ReadBuffer(1) Particles
        {
            Particle u_Particles[];
        };

        Out(0) vec2 v_UV;
        Out(1) vec4 v_Color;

        void main()
        {
            uint particleID = uint( gl_VertexIndex ) / 6u;
            uint corner     = uint( gl_VertexIndex ) % 6u;

            Particle p = u_Particles[particleID];

            // Dead / invisible particle: emit an off-screen (clipped) vertex so the quad is discarded.
            if ( p.Color.a <= 0.0 )
            {
                gl_Position = vec4( 2.0, 2.0, 2.0, 1.0 );
                v_UV        = vec2( 0.0 );
                v_Color     = vec4( 0.0 );
                return;
            }

            const vec2 corners[6] = vec2[6]( vec2( -1.0, -1.0 ), vec2( 1.0, -1.0 ), vec2( -1.0, 1.0 ),
                                             vec2( 1.0, -1.0 ), vec2( 1.0, 1.0 ), vec2( -1.0, 1.0 ) );
            vec2 c  = corners[corner];
            v_UV    = c * 0.5 + 0.5;
            v_Color = p.Color;

            // Camera-facing: offset the particle centre in VIEW space so the quad always faces the camera.
            vec3 viewPos = ( cameraUB.View * vec4( p.PosSize.xyz, 1.0 ) ).xyz;
            viewPos.xy += c * ( p.PosSize.w * 0.5 );
            gl_Position = cameraUB.Projection * vec4( viewPos, 1.0 );
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
