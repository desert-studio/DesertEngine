// THE cubemap-domain program: colour per DIRECTION, geometry synthesized from gl_VertexIndex.
//
// Two consumers, one program:
//   - the engine's skybox pass (MaterialSkybox binds `samplerCubeMap` from a SkyboxComponent's
//     baked environment — the file as authored — and `SkyLookUB` from the scene's rotation, tint and
//     intensity, applied here exactly as every lit surface applies them; see Common/SkyLook.glslh);
//   - a `.demat` naming this shader — a CUBEMAP MATERIAL. The Properties block below is that
//     material's schema: one cube slot the Material Editor lets an HDR skybox asset be dropped on.
//
// The Domain line makes the second consumer classifiable: the Material Editor previews a
// Skybox-domain material as a cubemap on an orbitable sphere (see Editor's CubemapSphere.shader —
// the direction is the sphere's own surface direction there, where here it is the camera ray).
// Skybox stays OUT of IsUserAssignable() on purpose: no mesh slot can draw a program whose vertex
// stage ignores the vertex buffer, and MeshRenderer::DrawGenericMeshes refuses it by name.
Shader "Skybox"
{
    Domain Skybox

    // NO TextureBinding option: the sampler is declared by hand in the Fragment stage below, at the
    // binding the engine skybox pass has always used, so this block adds schema without moving a
    // single binding under the pass's feet.
    Properties
    {
        TextureCube samplerCubeMap ("Cubemap")
    }

    Fragment
    {
        Out(0) vec4 oColor;

        Uniform(1) samplerCube samplerCubeMap;
        Uniform(2) SkyLookUB
        {
            vec4 YawCosSin; // xy = (cos yaw, sin yaw) — Graphic::SkyLookGPU
            vec4 Gain;      // rgb = tint * intensity
        } skyLook;
        #include <Common/SkyLook.glslh>

        // The WORLD-SPACE VIEW RAY, not a position: a cubemap is sampled by direction, and the name
        // `v_Position` is part of why this program spent its whole life reading the camera's own
        // position as if it were one. Common/ViewRay.glslh carries the account.
        In(3) vec3 v_ViewRay;

        void main()
        {
        	// The scene's look, through the SAME two calls the ambient and the reflections use — which is
        	// what makes the backdrop and the IBL the same sky now that the cube no longer carries it.
        	vec4 sky = texture(samplerCubeMap, SkyLookDirection(v_ViewRay, skyLook.YawCosSin.xy));
        	oColor   = vec4(ApplySkyGain(sky.rgb, skyLook.Gain.rgb), sky.a);
        }
    }

    Vertex
    {
        #include <Common/QuadPositions.glslh>
        #include <Common/CameraUB.glslh>
        #include <Common/ViewRay.glslh>

        Out(3) vec3 v_ViewRay;

        void main()
        {
            // z = 1.0 is the NEAR plane under reversed-Z (Core/Projection.hpp). It is not a depth
            // decision here at all -- the Skybox pass runs with depth test AND depth write off
            // (SkyboxRenderer.cpp) -- it is the clip-space point WorldViewRay unprojects.
            vec4 position = vec4(QUAD_POSITIONS[gl_VertexIndex], 1.0, 1.0);
        	gl_Position = position;

            // WAS `inverse(Projection * View) * position`, read as .xyz with no perspective divide.
            // That carries the camera's TRANSLATION and adds cameraPos/near to every ray: with a 10 cm
            // near plane a camera 200 cm up drowned the ray under a term twenty times longer, and the
            // background showed a single direction -- the camera's own position -- on all six sides.
            // See Common/ViewRay.glslh, and Tests/Engine/DepthConvention for the rule.
        	v_ViewRay = WorldViewRay(cameraUB.Projection, cameraUB.View, position);
        }
    }
}
