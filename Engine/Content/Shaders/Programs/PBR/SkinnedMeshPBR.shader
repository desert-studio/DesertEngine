// DesertAsset {"Kind":"Shader","Guid":"07c29f0359cb6851795348218fb43c0c","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SkinnedMeshPBR"
{
    // The SKINNED vertex path of the standard PBR surface — not a different material, the same one.
    // Every binding below the vertex stage is StaticMeshPBR's, slot for slot, because a surface does not
    // change when the geometry under it is skinned; the only thing this path adds is binding 1 (Bones)
    // and the BoneOffset push field. Desert/Tests/Engine/MeshVertexPath asserts exactly that against the
    // reflected SPIR-V, so a binding added to one forward variant and not the others is caught there.
    //
    // It carries no Domain/Properties block on purpose: a `.demat` names a SURFACE and never a vertex
    // path, so this shader must not appear in the material picker. StaticMeshPBR owns the schema and the
    // renderer chooses the path from the geometry (Graphic::MeshShaderFor).

    Vertex
    {
        In(0) vec3 a_Position;
        In(1) vec3 a_Normal;
        In(2) vec3 a_Tangent;
        In(3) vec3 a_Bitangent;
        In(4) vec2 a_TextureCoord;
        In(5) ivec4 a_BoneIndices;
        In(6) vec4  a_BoneWeights;

        #include <Common/CameraUB.glslh>

        // Transform + MaterialIndex are the shared mesh push block (byte-identical to StaticMeshPBR's, and
        // the fragment stage below repeats it). BoneOffset is this PATH's own field and is why one
        // material can draw every skinned mesh in the frame: the poses are packed end to end into the one
        // Bones buffer and each draw names its slice here. A push constant is snapshotted per draw, which
        // a storage buffer is not — storing the pose on the material instead made two skinned meshes
        // render with the pose of whichever was submitted last.
        PushConstant PushConstants
        {
            mat4 Transform;     // offset 0
            uint MaterialIndex; // offset 64
            uint BoneOffset;    // offset 68  first bone of THIS draw inside BoneMatrices[]
        } m_PushConstants;

        // raw-glsl: implicit (shared) layout kept — std430 would change the bone matrix offsets.
        layout(binding = 1) readonly buffer Bones
        {
            mat4 BoneMatrices[];
        } bones;

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
            // ------------------------------------------------------------
            // 1. GPU Skinning
            // ------------------------------------------------------------
            // int, not uint: a_BoneIndices is ivec4 and GLSL will not mix the two silently.
            int b = int(m_PushConstants.BoneOffset);
            mat4 skinMatrix =
                  bones.BoneMatrices[b + a_BoneIndices.x] * a_BoneWeights.x +
                  bones.BoneMatrices[b + a_BoneIndices.y] * a_BoneWeights.y +
                  bones.BoneMatrices[b + a_BoneIndices.z] * a_BoneWeights.z +
                  bones.BoneMatrices[b + a_BoneIndices.w] * a_BoneWeights.w;

            vec4 skinnedPosition = skinMatrix * vec4(a_Position, 1.0);
            vec3 skinnedNormal   = mat3(skinMatrix) * a_Normal;
            vec3 skinnedTangent  = mat3(skinMatrix) * a_Tangent;
            vec3 skinnedBitangent= mat3(skinMatrix) * a_Bitangent;

            // ------------------------------------------------------------
            // 2. World space
            // ------------------------------------------------------------
            mat4 model = m_PushConstants.Transform;
            mat3 normalMatrix = transpose(inverse(mat3(model)));

            vec4 worldPos = model * skinnedPosition;

            outVertex.WorldPosition = worldPos.xyz;
            outVertex.Normal        = normalize(normalMatrix * skinnedNormal);
            outVertex.Texcoord      = vec2(a_TextureCoord.x, 1.0 - a_TextureCoord.y);

            vec3 T = normalize(normalMatrix * skinnedTangent);
            vec3 B = normalize(normalMatrix * skinnedBitangent);
            vec3 N = normalize(normalMatrix * skinnedNormal);

            outVertex.TBN = mat3(T, B, N);
            outVertex.CameraPosition = cameraUB.CameraPos;

            // ------------------------------------------------------------
            // 3. Clip space
            // ------------------------------------------------------------
            gl_Position = cameraUB.Projection * cameraUB.View * worldPos;
        }
    }

    Fragment
    {
        #include <Mesh/PointLight.glslh>
        #include <Mesh/Spotlight.glslh>
        #include <Mesh/LightsMetadata.glslh>
        // THE direct-light BRDF, shared with the deferred composite and with the point/spot headers
        // above (which already include it). Named explicitly because this shader calls it directly.
        #include <Mesh/DirectLighting.glslh>

        In(0) Vertex
        {
        	vec3 WorldPosition;
        	vec3 Normal;
        	vec2 Texcoord;
        	mat3 TBN;
        	vec3 CameraPosition;
        } inVertex;

        const float Epsilon = 0.00001;

        const vec3 Fdielectric = vec3(0.04);


        Out(0) vec4 oColor;

        // Shared push-constant block. Must be byte-for-byte identical to the VERTEX stage's block above —
        // Vulkan requires one push-constant block per pipeline, so BoneOffset is declared here too even
        // though the fragment stage has no use for it. Per-object material data lives in the Materials[]
        // storage buffer (GPU-scene style); the push constant only carries the per-object index into it.
        PushConstant PushConstants
        {
        	mat4 Transform;     // offset 0   (vertex)
        	uint MaterialIndex; // offset 64  index into Materials[]
        	uint BoneOffset;    // offset 68  (vertex only; declared here to keep the blocks identical)
        } pc;

        // One entry per drawn object (std430). Filled on the CPU each frame (per-object / per-instance).
        struct GpuMaterial
        {
        	vec4 AlbedoAO;           // rgb = albedo, a = ambient occlusion
        	vec4 MetalRoughEmission; // x = metallic, y = roughness, z = emission strength
        	vec4 EmissionColor;      // rgb = emission color
        	vec4 ExtraParams;        // xy = UV tiling, z = IOR, w = reserved
        	vec4 GlassTint;          // rgb = glass tint, a = transmission (opaque path ignores it)
        };

        ReadBuffer(2) Materials
        {
        	GpuMaterial materials[];
        };

        struct DirectionLight
        {
        	vec4 Direction;      // xyz = normalized direction
        	vec4 ColorIntensity; // rgb = color, a = intensity
        };

        Uniform(3) DirectionLightsUB {
        	DirectionLight 		directionLights;
        } directionLights;

        // The cascaded shadow's five bindings, at slots free in THIS layout. The text that reads them is
        // one file for every shader in the engine that shades a surface with the sun —
        // Mesh/CascadedShadow.glslh, included right below because it names what is declared here.
        Uniform(5) sampler2D u_ShadowMap0;
        Uniform(13) sampler2D u_ShadowMap1;
        Uniform(14) sampler2D u_ShadowMap2;
        Uniform(15) sampler2D u_ShadowMap3;
        Uniform(7) ShadowUB {
        	mat4 u_LightViewProj[4];
        	vec4 u_ShadowParams;      // x = bias, y = enabled (>0.5), z = debug mode (0/1/2), w = cascade count
        	vec4 u_DebugParams;       // x = show normals (>0.5), y = light debug (>0.5); z,w reserved
        	vec4 u_CascadeTexelWorld; // per-cascade world size of one shadow-map texel (x..w = cascade 0..3)
        };

        #include <Mesh/CascadedShadow.glslh>

        // Environment maps
        Uniform(8) samplerCube u_EnvSpecularTex;
        Uniform(9) samplerCube u_EnvIrradianceTex;
        // The sky's look — how the two cubes above are read (Common/SkyLook.glslh). Slot 22: free in
        // the whole forward mesh family, which must keep one layout (ShaderCacheKey).
        Uniform(22) SkyLookUB
        {
            vec4 YawCosSin; // xy = (cos yaw, sin yaw) — Graphic::SkyLookGPU
            vec4 Gain;      // rgb = tint * intensity
        } skyLook;

        // BRDF LUT
        Uniform(10) sampler2D u_BRDFLUTTexture;

        #include <Common/TangentNormal.glslh>
        Uniform(11) sampler2D u_AlbedoTexture;
        Uniform(12) sampler2D u_NormalTexture;
        Uniform(18) sampler2D u_OpacityTexture; // alpha-cutout mask (foliage); unused when cutoff == 0 (16/17 = light SSBOs)

        // THE CLOUD LAYER'S SHADOW ON THE WORLD — the sun's SECOND occluder, at the same slots as in
        // StaticMeshPBR / StaticMeshPBR_Instanced / StaticMeshGlass / StaticMeshGBuffer. It matters most
        // HERE: a skinned mesh has no G-buffer variant, so it is drawn FORWARD over the deferred
        // composite in every deferred scene, and it was the one class of geometry standing in full sun
        // under a deck that shaded the ground it walks on.
        Uniform(20) sampler2D u_CloudShadowMap;
        Uniform(21) CloudShadowUB {
        	mat4 u_CloudShadowWorldToMap;
        	// x = the kilometres the map's clip z spans, y = 1 when the map is real and must be read,
        	// z = the UV width of the border fade, w = the artist's shadow strength.
        	vec4 u_CloudShadowParams;
        };

        // THE receiver, shared verbatim with the deferred composite. Included HERE because it names the
        // two bindings above.
        #include <Common/CloudShadowReceiver.glslh>

        struct Params
        {
        	vec3 AlbedoColor;
        	vec3 Normal;
        } m_Params;

        // The sun. Its Cook-Torrance response is the SAME text the deferred lighting pass compiles and
        // the same text the point and spot lights compile — Mesh/DirectLighting.glslh. What used to be
        // here was a fourth copy of that BRDF whose diffuse half read `kd * albedo` where every other
        // copy in the engine read `kd * albedo / PI`, so this path's sun was PI times too bright: both
        // against the physically-correct deferred composite and against a point light of equal
        // intensity standing beside it in this very shader.
        //
        // The loop is kept rather than collapsed to an `if`: DirectionLightsUB holds exactly ONE light
        // (Scene.cpp truncates past that and says so), and the count is the engine's own gate for
        // whether the scene has a sun at all.
        vec3 Lightning(vec3 view, vec3 N, vec3 F0, float metalness, float roughness, vec3 albedo)
        {
        	vec3 color = vec3(0);

        	for(uint i = 0; i < lightsMetadata.DirectionLightCount; i++)
        	{
        		vec3 Lradiance = directionLights.directionLights.ColorIntensity.rgb
        		               * directionLights.directionLights.ColorIntensity.a;
        		color += EvaluateDirectionalLight(directionLights.directionLights.Direction.xyz, Lradiance,
        		                                  view, N, F0, metalness, roughness, albedo);
        	}

        	return color;
        }

        // The split-sum ambient — the SAME text the deferred lighting pass compiles, so a scene shaded
        // through RenderingPath 0 and one shaded through RenderingPath 1 get one ambient model and not
        // two. Included HERE and not with the other headers at the top because it names the three
        // environment bindings declared just above.
        #include <Mesh/AmbientIBL.glslh>


        void main() {

        	GpuMaterial mat = materials[pc.MaterialIndex];

        	// Tiled UV: surface UVs * material UV-tiling (ExtraParams.xy; default {1,1} = no tiling). Guard against 0
        	// (un-set / legacy material) so the texture never collapses to a single texel.
        	vec2 tiling = mat.ExtraParams.xy;
        	if (tiling.x <= 0.0) tiling.x = 1.0;
        	if (tiling.y <= 0.0) tiling.y = 1.0;
        	vec2 uv = inVertex.Texcoord * tiling;

        	// Alpha cutout (foliage/cards): discard transparent texels per the Opacity Map. MetalRoughEmission.w is
        	// the cutoff (0 = disabled, so opaque materials are unaffected). Done first to skip lighting on discards.
        	float alphaCutoff = mat.MetalRoughEmission.w;
        	if (alphaCutoff > 0.0 && texture(u_OpacityTexture, uv).r < alphaCutoff)
        		discard;

        	m_Params.AlbedoColor = mat.AlbedoAO.rgb;
        	// Albedo maps are authored in sRGB (gamma) space; lighting must run in LINEAR space. The engine loads
        	// 8-bit textures as UNORM (no hardware sRGB sampling yet), so convert here. Normal/roughness/metallic/AO
        	// are DATA maps and are intentionally NOT converted. (Proper fix later: hardware VK_FORMAT_*_SRGB.)
        	m_Params.AlbedoColor *= pow( texture(u_AlbedoTexture, uv).rgb, vec3(2.2) );

        	// Default: use the world-space normal from the vertex shader directly.
        	m_Params.Normal = normalize(inVertex.Normal);

        	const ivec2 textureSize = textureSize(u_NormalTexture, 0);
        	if(textureSize.x > 1 && textureSize.y > 1) // real normal map — not the 1x1 fallback
        	{
        		// Transform tangent-space normal to world space via TBN.
        		vec3 tangentNormal = SampleTangentNormal(u_NormalTexture, uv);
        		m_Params.Normal = normalize(inVertex.TBN * tangentNormal);
        	}
        	// Without a normal map the TBN transform is intentionally skipped:
        	// inVertex.Normal is already in world space and needs no further transformation.

        	// Debug: visualize the final world-space normal as RGB (viewport View Mode -> Normals).
        	if (u_DebugParams.x > 0.5)
        	{
        		oColor = vec4(m_Params.Normal * 0.5 + 0.5, 1.0);
        		return;
        	}

        	const float metalness = mat.MetalRoughEmission.x;
        	// Clamp to a minimum roughness so the GGX NDF stays finite even for mirror-smooth materials.
        	const float roughness = max(mat.MetalRoughEmission.y, 0.04);
        	const float ao        = mat.AlbedoAO.a;

        	const vec3 view = normalize(inVertex.CameraPosition - inVertex.WorldPosition);

        	vec3 F0 = mix(Fdielectric, m_Params.AlbedoColor, metalness);
        	vec3 light = Lightning(view, m_Params.Normal, F0, metalness, roughness, m_Params.AlbedoColor );
        	vec3 ibl = AmbientIBL(view, m_Params.Normal, F0, metalness, roughness, m_Params.AlbedoColor);

        	vec3 pointLight = vec3(0.0);

        	for(uint i = 0; i < lightsMetadata.PointLightCount; i++)
        	{
        		PointLight light = pointLights[i];
                pointLight += CalculatePointLight(light, inVertex.WorldPosition, view,
                                                m_Params.Normal, F0, metalness,
                                                roughness, m_Params.AlbedoColor);
        	}

        	vec3 spotLight = vec3(0.0);
        	for(uint i = 0; i < lightsMetadata.SpotLightCount; i++)
        	{
                spotLight += CalculateSpotLight(spotLights[i], inVertex.WorldPosition, view,
                                                m_Params.Normal, F0, metalness,
                                                roughness, m_Params.AlbedoColor);
        	}

            // Directional (sun) light is occluded by the shadow map; IBL/point/emission are not.
            vec3  sunDir = normalize(-directionLights.directionLights.Direction.xyz); // toward the sun
            int   cascade;
            float shadow = ShadowFactor(inVertex.WorldPosition, m_Params.Normal, sunDir, cascade);
            // Per-mesh "Receive Shadows" toggle rides ExtraParams.w (1 = don't receive sun shadows).
            // It was in StaticMeshPBR and NOT here, which is the drift a per-path material class makes
            // invisible: both stages read the same GpuMaterial, and only one of them honoured this bit.
            if (mat.ExtraParams.w > 0.5)
                shadow = 1.0;

            // TWO OCCLUDERS OF ONE SUN, multiplied — exactly as the deferred composite assembles it.
            // AFTER the per-mesh toggle and not before it, because the toggle is a CASCADE toggle: the
            // G-buffer carries no such bit, so the deferred path shades every surface with the cloud
            // layer regardless, and a mesh that opted out of geometry shadows must not become the one
            // surface in the scene whose shading depends on which path drew it.
            shadow *= CloudShadowFactor(inVertex.WorldPosition);

            // Lighting debug (viewport View Mode -> Lighting): each source gets a distinct color, the
            // surface is tinted by the sources reaching it (weighted by attenuation * NdotL), brightness = light
            // strength, fully-unlit areas read black. Albedo/IBL/emission are ignored.
            if (u_DebugParams.y > 0.5)
            {
                vec3 dbg = vec3(0.0);
                // Point lights: hue cycles 0,1,2,... (red, then well-spread).
                for (uint i = 0; i < lightsMetadata.PointLightCount; i++)
                    dbg += LightDebugColor(i) * PointLightContribution(pointLights[i], inVertex.WorldPosition, m_Params.Normal);
                // Spot lights: same hue cycle but offset half a turn so spot #0 != point #0.
                for (uint i = 0; i < lightsMetadata.SpotLightCount; i++)
                    dbg += HueToRGB(fract(float(i) * 0.61803398875 + 0.5)) * SpotLightContribution(spotLights[i], inVertex.WorldPosition, m_Params.Normal);
                // Sun (directional): fixed warm yellow, occluded by its shadow — unmistakable vs the cycled hues.
                for (uint i = 0; i < lightsMetadata.DirectionLightCount; i++)
                {
                    float sunStrength = directionLights.directionLights.ColorIntensity.a
                                      * max(dot(m_Params.Normal, sunDir), 0.0) * shadow;
                    dbg += vec3(1.0, 0.85, 0.35) * sunStrength;
                }
                // Physical attenuation*NdotL is numerically dim, so a debug viz built from it reads as near-black.
                // Map it through an exposure-like curve so ANY light reaching the surface shows as a clear color,
                // while a truly unlit fragment (dbg == 0) stays black. This is a visualization, not radiometry.
                dbg = vec3(1.0) - exp(-dbg * 6.0);
                oColor = vec4(dbg, 1.0);
                return;
            }

            // Debug mode 2 (Cascades): tint by the cascade that shadows this fragment, darkened where shadowed.
            if (u_ShadowParams.z > 1.5)
            {
                oColor = vec4(cascadeDebugColor(cascade) * (shadow * 0.7 + 0.3), 1.0);
                return;
            }
            // Debug mode 1 (ShadowFactor): raw shadow factor as grayscale (1 = lit, 0 = shadowed).
            if (u_ShadowParams.z > 0.5)
            {
                oColor = vec4(vec3(shadow), 1.0);
                return;
            }

            // Ambient occlusion attenuates only the ambient (IBL) term; emission is added unlit.
            vec3 emission = mat.EmissionColor.rgb * mat.MetalRoughEmission.z;

            // The ambient, assembled by the shared header — the SAME call the deferred composite makes,
            // so the two paths cannot floor, occlude or albedo-weight it differently. The forward path
            // has no indirect-bounce gather, so it passes zero there; that is the only difference
            // between the two call sites and it is visible in the argument list.
            vec3 ambient = ComposeAmbient(ibl, m_Params.AlbedoColor, ao, vec3(0.0));

            oColor = vec4( light * shadow + ambient + pointLight + spotLight + emission, 1.0);
        }
    }
}
