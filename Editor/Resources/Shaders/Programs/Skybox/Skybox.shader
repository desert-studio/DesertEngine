// DesertAsset {"Kind":"Shader","Guid":"ff6a2128878c91093023d2e4786d87db","Versions":{"SHDR":1},"Dependencies":[]}
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

        // A bound texture property: a separate texture read through its own sampler state `<name>Sampler`,
        // which the material writes from the property (VulkanMaterialBackend::ApplyTextureCube).
        Uniform(1) textureCube samplerCubeMap;
        Uniform(3) sampler samplerCubeMapSampler;
        #define samplerCubeMap samplerCube(samplerCubeMap, samplerCubeMapSampler)
        #include <Generated/Environment/SkyLook.glslh>
        Uniform(2) SkyLookUB
        {
            DESERT_SkyLookGPU_MEMBERS
        } skyLook;
        #include <Common/SkyLook.glslh>

        // The WORLD-SPACE VIEW RAY, not a position: a cubemap is sampled by direction, and the name
        // `v_Position` is part of why this program spent its whole life reading the camera's own
        // position as if it were one. Common/ViewRay.glslh carries the account.
        In(3) vec3 v_ViewRay;
        // ViewFrame::PreExposure from the vertex stage (CameraUB is a vertex-stage block here).
        In(4) float v_PreExposure;
        // The ray as a point at infinity through the unjittered ViewProjection / PrevViewProjection (Common/
        // Velocity.glslh DesertSkyVelocity): divided per pixel, written into the scene target's velocity slot.
        In(5) vec4 v_SkyClip;
        In(6) vec4 v_SkyPrevClip;
        // The view's velocity (ViewTargetLayouts kSceneTargetVelocitySlot): the camera's motion at infinity, so the
        // sky texels carry it for every reader and the vendor upscalers without a fix-up pass.
        Out(1) vec4 oVelocity;

        #include <Common/Velocity.glslh>

        void main()
        {
        	// The scene's look, through the SAME two calls the ambient and the reflections use — which is
        	// what makes the backdrop and the IBL the same sky now that the cube no longer carries it.
        	vec4 sky = texture(samplerCubeMap, SkyLookDirection(v_ViewRay, skyLook.YawCosSin.xy));
        	// Pre-exposed AFTER the gain and its luminance ceiling, so the ceiling stays in absolute units.
        	oColor   = vec4(ApplySkyGain(sky.rgb, skyLook.Gain.rgb) * v_PreExposure, sky.a);
        	oVelocity = DesertSkyVelocity(v_SkyClip, v_SkyPrevClip);
        }
    }

    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>
        #include <Common/CameraUB.glslh>
        #include <Common/ViewRay.glslh>

        Out(3) vec3 v_ViewRay;
        Out(4) float v_PreExposure;
        // The ray through the UNJITTERED current and previous ViewProjection as a point at infinity (w = 0): linear
        // in the ray, so the interpolated pair is the pixel's; the fragment divides (DesertSkyVelocity).
        Out(5) vec4 v_SkyClip;
        Out(6) vec4 v_SkyPrevClip;

        void main()
        {
            // z = 1.0 is the NEAR plane under reversed-Z (Core/Projection.hpp). It is not the depth
            // the sky is rasterised at (gl_Position below) -- it is the clip-space point WorldViewRay
            // unprojects.
            vec4 position = vec4(FullscreenTriangleNdc(), 1.0, 1.0);
            // Rasterised on the FAR plane (depth 0 under reversed-Z): the deferred path's sky after the G-buffer
            // tests CloserOrEqual against the scene depth and shades only the uncovered texels; the ray below is
            // still unprojected from the near-plane point.
        	gl_Position = vec4(position.xy, 0.0, 1.0);

            // WAS `inverse(Projection * View) * position`, read as .xyz with no perspective divide.
            // That carries the camera's TRANSLATION and adds cameraPos/near to every ray: with a 10 cm
            // near plane a camera 200 cm up drowned the ray under a term twenty times longer, and the
            // background showed a single direction -- the camera's own position -- on all six sides.
            // See Common/ViewRay.glslh, and Tests/Engine/DepthConvention for the rule. Through the view's temporal
            // jitter, as the geometry the upscaler resolves the backdrop with (WorldViewRayJittered).
        	v_ViewRay = WorldViewRayJittered(cameraUB.Projection, cameraUB.JitterNdc, cameraUB.View, position);
        	v_PreExposure = cameraUB.PreExposure;
        	v_SkyClip     = cameraUB.ViewProjection * vec4(v_ViewRay, 0.0);
        	v_SkyPrevClip = cameraUB.PrevViewProjection * vec4(v_ViewRay, 0.0);
        }
    }
}
