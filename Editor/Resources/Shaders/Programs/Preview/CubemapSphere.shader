// DesertAsset {"Kind":"Shader","Guid":"593d0a4c3edd7589472b5de9d7712641","Versions":{"SHDR":1},"Dependencies":[]}
// The Material Editor's cubemap-on-a-sphere: the pane's answer for a Skybox-domain material.
//
// A cubemap has no surface of its own — the engine's Skybox program shows it BY DIRECTION on the far
// plane, which is right for a world background and useless for a preview pane, where the question is
// "what does this cubemap look like as a thing I can orbit". So this draws the OTHER model: a finite
// ball at the origin, the cube sampled by the ball's own surface direction, i.e. the environment
// wrapped onto an object. Same asset, opposite presentation; UE's TextureCube editor sphere.
//
// The ball is ray-traced from a fullscreen quad rather than rasterized from sphere geometry: the
// silhouette is exact at any zoom (no tessellation facets on the one shape whose whole job is to be
// round), and the pass needs no vertex buffer at all — it is Grid.shader's trick with a sphere in
// place of the y=0 plane. Depth is written so the pass composes correctly with the preview scene
// whatever order the graph runs it in.
//
// NO Domain line ON PURPOSE: this is an engine-internal presenter the editor's preview pass draws
// directly. Give it one and it appears in the material shader picker of that domain.
Shader "CubemapSphere"
{
    Fragment
    {
        Uniform(0) CubemapSphereUB
        {
            mat4 Projection;
            mat4 View;
            mat4 InvProjection;
            mat4 InvView;
            vec4 CameraPos; // xyz; w unused
            vec4 Params;    // x = sphere radius (world units); y > 0.5 = the cube is the backdrop too; z = lod; w > 0.5 = long-lat
        } u;

        Uniform(1) samplerCube u_CubeMap;
        // The sky's look — how the two cubes above are read (Common/SkyLook.glslh). The Details
        // panel's ball beside the Rotation slider must turn with it.
        Uniform(2) SkyLookUB
        {
            vec4 YawCosSin; // xy = (cos yaw, sin yaw) — Graphic::SkyLookGPU
            vec4 Gain;      // rgb = tint * intensity
        } skyLook;
        #include <Common/SkyLook.glslh>
        #include <Common/SkyPanorama.glslh>

        In(0) vec3 v_Near;
        In(1) vec3 v_Far;
        In(2) vec2 v_Ndc;
        Out(0) vec4 o_Color;

        vec3 SkyBy( vec3 direction )
        {
            return ApplySkyGain( textureLod( u_CubeMap, SkyLookDirection( direction, skyLook.YawCosSin.xy ), u.Params.z ).rgb,
                                 skyLook.Gain.rgb );
        }

        // THE BACKDROP (the skybox viewer's mode): a ray that misses the ball sees the cube by its OWN direction,
        // the way the engine's Skybox program shows it. Depth is written just in front of the reversed-Z far
        // plane (cleared to 0), so the Closer test passes and anything real in the scene still occludes it.
        const float kBackdropDepth = 1e-7;

        void main()
        {
            // THE 2D VIEW: the cube unwrapped by the bake's own panorama mapping, so it reads as the file does.
            if ( u.Params.w > 0.5 )
            {
                o_Color      = vec4( SkyBy( PanoramaDirection( PanoramaScreenUV( v_Ndc ) ) ), 1.0 );
                gl_FragDepth = kBackdropDepth;
                return;
            }

            vec3 origin = v_Near;
            vec3 dir    = normalize( v_Far - v_Near );

            // Ray vs sphere at the origin: |origin + t*dir|^2 = R^2.
            float R = u.Params.x;
            float b = dot( origin, dir );
            float c = dot( origin, origin ) - R * R;
            float h = b * b - c;
            bool  backdrop = u.Params.y > 0.5;
            float t        = h < 0.0 ? -1.0 : -b - sqrt( h ); // near root: the face toward the camera
            if ( t <= 0.0 )
            {
                // The ray misses the ball, or the ball is behind the camera / around it.
                if ( !backdrop )
                    discard; // the scene's own backdrop stays
                o_Color      = vec4( SkyBy( dir ), 1.0 );
                gl_FragDepth = kBackdropDepth;
                return;
            }

            vec3 hit = origin + t * dir;
            vec3 n   = hit / R; // unit by construction: |hit| == R

            // The wrap itself: the cube by the sphere's outward direction. No lighting on purpose — a
            // cubemap is radiance, not a surface; the scene's post chain tonemaps it like any sky.
            o_Color = vec4( SkyBy( n ), 1.0 );

            // Zero-to-one reversed-Z device depth, no remap (Core/Projection.hpp; same as Grid.shader).
            vec4 clip    = u.Projection * u.View * vec4( hit, 1.0 );
            gl_FragDepth = clip.z / clip.w;
        }
    }

    Vertex
    {
        Uniform(0) CubemapSphereUB
        {
            mat4 Projection;
            mat4 View;
            mat4 InvProjection;
            mat4 InvView;
            vec4 CameraPos;
            vec4 Params;
        } u;

        Out(0) vec3 v_Near;
        Out(1) vec3 v_Far;
        Out(2) vec2 v_Ndc;

        vec3 Unproject( vec2 ndc, float z )
        {
            vec4 p = u.InvView * u.InvProjection * vec4( ndc, z, 1.0 );
            return p.xyz / p.w;
        }

        void main()
        {
            // Two triangles covering NDC. Drawn via Renderer::SubmitFullscreenQuad (vkCmdDraw(6)).
            const vec2 verts[6] = vec2[6](
                vec2( -1.0, -1.0 ), vec2( 1.0, -1.0 ), vec2( 1.0, 1.0 ),
                vec2( 1.0, 1.0 ), vec2( -1.0, 1.0 ), vec2( -1.0, -1.0 ) );

            vec2 ndc = verts[gl_VertexIndex];

            // Reversed-Z, zero-to-one clip depth: 1 is the near plane, 0 the far one
            // (Core/Projection.hpp).
            v_Near = Unproject( ndc, 1.0 );
            v_Far  = Unproject( ndc, 0.0 );
            v_Ndc  = ndc; // the 2D view's unwrap is placed in the fragment stage (PanoramaScreenUV)

            gl_Position = vec4( ndc, 0.0, 1.0 );
        }
    }
}
