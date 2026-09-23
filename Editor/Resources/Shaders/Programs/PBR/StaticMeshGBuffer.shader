Shader "StaticMeshGBuffer"
{
    // Deferred G-buffer geometry pass for static meshes (writes Albedo+Metallic / Normal+Roughness MRT).
    // Shares Static.glsl.vert + the Materials[] SSBO with StaticMeshPBR so material data binds unchanged.

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
        // Deferred G-buffer WRITE pass. It shades NOTHING — it writes the material attributes into four MRT targets:
        //   GBufferA (RGBA8F)  = Albedo.rgb + Metallic.a
        //   GBufferB (RGBA32F) = world Normal.rgb + Roughness.a
        //   GBufferC (RGBA32F) = world position.xyz + texture count
        //   GBufferEmissive    = HDR self-illumination
        //
        // IT DECLARES WHAT IT READS AND NOTHING ELSE, which is a recent state of affairs. Until the deferred pass
        // got a material of its own, MeshRenderer drew it with the (Static x Forward) material — whose descriptor
        // sets are allocated from StaticMeshPBR's reflection — bound against a pipeline layout built from THIS
        // shader's. Vulkan requires those to be compatible, and SPIR-V reflection drops unreferenced bindings, so
        // this file had to declare four cascade maps, three environment samplers, two light SSBOs, the lights
        // metadata block, the directional light block, ShadowUB and the cloud-shadow pair — fourteen descriptors it
        // never reads — and touch every one of them through a `keep` sum scaled by 1e-20 so the optimiser could not
        // remove them again. MaterialService now keys a runtime material by (asset x vertex path x PASS), the
        // G-buffer pass binds its own cell's sets, and none of that is needed.
        //
        // The relation that replaced it is asserted, not commented: Desert/Tests/Engine/ShaderCacheKey checks that
        // this shader declares EXACTLY the surface inputs a G-buffer write needs, and Desert/Tests/Engine/
        // MeshVertexPath checks that no mesh shader declares a binding whose data no material of its own cell
        // could supply.

        In(0) Vertex
        {
        	vec3 WorldPosition;
        	vec3 Normal;
        	vec2 Texcoord;
        	mat3 TBN;
        	vec3 CameraPosition;
        } inVertex;

        Out(0) vec4 oGBufferA; // Albedo.rgb, Metallic.a
        Out(1) vec4 oGBufferB; // Normal.rgb, Roughness.a
        Out(2) vec4 oGBufferC; // WorldPosition.xyz, texCount.w
        Out(3) vec4 oGBufferEmissive; // Emissive.rgb (HDR, self-illumination added in the deferred resolve)

        PushConstant PushConstants
        {
        	mat4 Transform;
        	uint MaterialIndex;
        } pc;

        struct GpuMaterial
        {
        	vec4 AlbedoAO;
        	vec4 MetalRoughEmission;
        	vec4 EmissionColor;
        	vec4 ExtraParams;
        	vec4 GlassTint; // rgb = tint, a = transmission (G-buffer path is opaque; used to SKIP glass, not shade it)
        };
        ReadBuffer(2) Materials { GpuMaterial materials[]; };

        // The surface's own three maps, at the SAME slots the forward mesh shaders use them at. The numbers
        // are deliberately unchanged and deliberately not compacted to 0,1,2: the whole mesh shader family
        // names one surface, and a reader comparing two of them should see the albedo map at the same
        // binding in both. A sparse set is not a problem for Vulkan, and the gaps are the record of what
        // this pass does NOT need.
        #include <Common/TangentNormal.glslh>
        Uniform(11) sampler2D  u_AlbedoTexture;
        Uniform(12) sampler2D  u_NormalTexture;
        Uniform(18) sampler2D  u_OpacityTexture;

        void main()
        {
        	GpuMaterial mat = materials[pc.MaterialIndex];

        	vec2 tiling = mat.ExtraParams.xy;
        	if (tiling.x <= 0.0) tiling.x = 1.0;
        	if (tiling.y <= 0.0) tiling.y = 1.0;
        	vec2 uv = inVertex.Texcoord * tiling;

        	float alphaCutoff = mat.MetalRoughEmission.w;
        	if (alphaCutoff > 0.0 && texture(u_OpacityTexture, uv).r < alphaCutoff)
        		discard;

        	vec3 albedo = mat.AlbedoAO.rgb * pow(texture(u_AlbedoTexture, uv).rgb, vec3(2.2));

        	vec3 N = normalize(inVertex.Normal);
        	const ivec2 nrmSize = textureSize(u_NormalTexture, 0);
        	if (nrmSize.x > 1 && nrmSize.y > 1)
        	{
        		vec3 tangentNormal = SampleTangentNormal(u_NormalTexture, uv);
        		N = normalize(inVertex.TBN * tangentNormal);
        	}

        	const float metallic  = mat.MetalRoughEmission.x;
        	const float roughness = max(mat.MetalRoughEmission.y, 0.04);

        	// Material-complexity proxy (heat-mapped by the DeferredLighting debug branch): count the textures
        	// this material actually samples — a bound map is a real texture, an absent one is a 1x1 dummy.
        	// Stashed in the otherwise-unused GBufferC.w so it costs no extra target.
        	int texCount = 0;
        	if (textureSize(u_AlbedoTexture, 0).x > 1)  texCount++;
        	if (nrmSize.x > 1)                          texCount++; // normal map (nrmSize computed above)
        	if (textureSize(u_OpacityTexture, 0).x > 1) texCount++;

        	oGBufferA = vec4(albedo, metallic);
        	oGBufferB = vec4(N, roughness);
        	oGBufferC = vec4(inVertex.WorldPosition, float(texCount));
        	// Emissive is view-independent self-illumination; the deferred lighting resolve ADDS it, matching the
        	// forward StaticMeshPBR path so emissive materials reach the HDR composite and bloom (values > 1).
        	oGBufferEmissive = vec4(mat.EmissionColor.rgb * mat.MetalRoughEmission.z, 1.0);
        }
    }
}
