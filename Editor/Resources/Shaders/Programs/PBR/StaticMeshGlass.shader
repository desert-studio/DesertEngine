Shader "StaticMeshGlass"
{
    // Forward transparent (glass) pass for static meshes: shares Static.glsl.vert + the Materials[] SSBO with
    // StaticMeshPBR so material data binds unchanged, but shades glass (Fresnel edge + specular + transmission)
    // and blends over the composited scene. Selected for materials with Transmission > 0.

    Vertex
    {
        In(0) vec3 a_Position;
        In(1) vec3 a_Normal;
        In(2) vec3 a_Tangent;
        In(3) vec3 a_Bitangent;
        In(4) vec2 a_TextureCoord;

        #include <Common/CameraUB.glslh>

        // Shared push-constant block. Must be byte-for-byte identical to the one in PBR.glsl.frag so the
        // reflected range (offset/size) matches across stages. The vertex stage only reads Transform; the
        // per-object material parameters are consumed by the fragment stage. Per-object data lives here
        // (not in a uniform buffer) so each draw carries its own values — a shared material UB would be
        // overwritten by later objects in the same frame (last-write-wins) before the GPU executes the draws.
        // Must match PBR.glsl.frag / Skinned.glsl.vert. Material data lives in a storage buffer (read in the
        // fragment); the vertex stage only needs Transform.
        PushConstant PushConstants
        {
        	mat4 Transform;     // offset 0
        	uint MaterialIndex; // offset 64
        } m_PushConstants;


        Out(0) Vertex
        {
        	vec3 WorldPosition;
        	vec3 Normal;
        	vec2 Texcoord;
        	mat3 TBN;
        	vec3 CameraPosition;
        } outVertex;

        void main()
        {
        	outVertex.WorldPosition  = vec3(m_PushConstants.Transform * vec4(a_Position, 1.0));
        	outVertex.Texcoord       = vec2(a_TextureCoord.x, 1.0 - a_TextureCoord.y);
        	outVertex.CameraPosition = cameraUB.CameraPos;

        	mat3 normalMatrix = transpose(inverse(mat3(m_PushConstants.Transform)));

        	vec3 T = normalize(normalMatrix * a_Tangent);
        	vec3 B = normalize(normalMatrix * a_Bitangent);
        	vec3 N = normalize(normalMatrix * a_Normal);

        	outVertex.Normal = N;
        	outVertex.TBN    = mat3(T, B, N);

        	gl_Position = cameraUB.Projection * cameraUB.View * m_PushConstants.Transform * vec4(a_Position, 1.0);
        }
    }

    Fragment
    {
        // Forward TRANSPARENT (glass) fragment. Drawn over the composited opaque scene with alpha blending. v1:
        // clear glass — Fresnel-bright reflective edges + a sun specular highlight + see-through centre (alpha from
        // transmission). Background TINT + screen-space REFRACTION are v2 (need the scene colour bound as a texture).
        //
        // IT DECLARES WHAT IT READS AND NOTHING ELSE. This file used to declare twelve descriptors it never
        // sampled — the four cascade maps, ShadowUB, the irradiance cube, the BRDF LUT, the albedo and
        // opacity maps, the two light SSBOs and the lights-metadata block — and touch every one through a
        // `keep` sum multiplied by 1e-20, so that its reflected layout would stay identical to the forward
        // mesh shader's. The comment that stood here said why: the pass was drawn with the shared
        // StaticMaterialPBR descriptor set.
        //
        // Neither half of that is true any more, and the second half stopped being true before the first.
        // The glass pass has held its OWN material since it was split out (MeshRenderer::RenderGlassManual,
        // MaterialPBR::Create(Static, Glass)), so its sets already came from this shader's reflection; the
        // only code that would have bound a forward material against the glass pipeline was reached through
        // MeshRenderer::m_GlassPass, a flag no line in the engine ever set to true. So the padding was
        // holding a layout equal for a borrow that had already stopped happening.

        In(0) Vertex
        {
        	vec3 WorldPosition;
        	vec3 Normal;
        	vec2 Texcoord;
        	mat3 TBN;
        	vec3 CameraPosition;
        } inVertex;

        Out(0) vec4 oColor;

        PushConstant PushConstants { mat4 Transform; uint MaterialIndex; } pc;

        struct GpuMaterial
        {
        	vec4 AlbedoAO;
        	vec4 MetalRoughEmission;
        	vec4 EmissionColor;
        	vec4 ExtraParams;  // z = IOR
        	vec4 GlassTint;    // rgb = tint, a = transmission
        };
        ReadBuffer(2) Materials { GpuMaterial materials[]; };

        struct DirectionLight { vec4 Direction; vec4 ColorIntensity; };
        Uniform(3) DirectionLightsUB { DirectionLight directionLights; } directionLights;

        // The specular cube is the ONE environment binding glass reads: it is a mirror term at grazing
        // angles, not an ambient (Desert/Tests/Engine/AmbientIBL says the same thing from the other side).
        // Slots keep the numbers the rest of the mesh family uses; the gaps are what this pass does not need.
        #include <Common/TangentNormal.glslh>
        Uniform(8) samplerCube u_EnvSpecularTex;
        Uniform(12) sampler2D  u_NormalTexture;
        Uniform(19) sampler2D  u_SceneColor; // copy of the composited opaque scene (for refraction)

        // THE CLOUD LAYER'S SHADOW ON THE WORLD — at the same slots as the four other mesh shaders. Glass
        // is drawn FORWARD over the deferred composite, so like the skinned meshes it never saw the map.
        // The only direct sun term this shader has is the specular hotspot below, and that is what the
        // factor multiplies: a cloud between the sun and the glass takes the highlight away with it.
        Uniform(20) sampler2D u_CloudShadowMap;
        Uniform(21) CloudShadowUB {
        	mat4 u_CloudShadowWorldToMap;
        	vec4 u_CloudShadowParams;
        };

        #include <Common/CloudShadowReceiver.glslh>

        void main()
        {
        	GpuMaterial mat = materials[pc.MaterialIndex];

        	vec3  tint         = mat.GlassTint.rgb;
        	float transmission = clamp(mat.GlassTint.a, 0.0, 1.0);
        	float ior          = max(mat.ExtraParams.z, 1.0);

        	vec3 N = normalize(inVertex.Normal);
        	const ivec2 nrmSize = textureSize(u_NormalTexture, 0);
        	if (nrmSize.x > 1 && nrmSize.y > 1)
        		N = normalize(inVertex.TBN * SampleTangentNormal(u_NormalTexture, inVertex.Texcoord));

        	vec3 V = normalize(inVertex.CameraPosition - inVertex.WorldPosition);

        	// Schlick Fresnel with the IOR-derived normal reflectance (glass F0 ~0.04, water ~0.02).
        	float f0        = pow((ior - 1.0) / (ior + 1.0), 2.0);
        	float fresnel   = f0 + (1.0 - f0) * pow(1.0 - max(dot(N, V), 0.0), 5.0);

        	// Sun specular highlight (glass is smooth -> a tight hotspot).
        	vec3  L        = normalize(-directionLights.directionLights.Direction.xyz);
        	vec3  H        = normalize(V + L);
        	float spec     = pow(max(dot(N, H), 0.0), 128.0) * directionLights.directionLights.ColorIntensity.a
        	               * CloudShadowFactor(inVertex.WorldPosition);

        	// --- Screen-space REFRACTION: sample the composited scene behind the glass, bent by the surface normal
        	// (so objects behind the sphere show through, distorted by IOR) + tinted. ---
        	vec2 screenUV   = gl_FragCoord.xy / vec2(textureSize(u_SceneColor, 0));
        	float bendScale = (1.0 - 1.0 / ior) * 0.35;                 // stronger IOR -> more bend
        	vec2  refractUV = clamp(screenUV + N.xy * bendScale, vec2(0.001), vec2(0.999));
        	vec3  refracted = texture(u_SceneColor, refractUV).rgb * tint;

        	// Environment reflection at grazing edges (skybox); Fresnel mixes refraction (centre) -> reflection (edge).
        	vec3 R       = reflect(-V, N);
        	vec3 envRefl = texture(u_EnvSpecularTex, R).rgb;

        	vec3 color = mix(refracted, envRefl, fresnel) + vec3(spec);

        	// Written opaque (the refraction already carries the background); the transmission factor only trims the
        	// reflection strength for very clear glass.
        	oColor = vec4(color, 1.0);
        }
    }
}
