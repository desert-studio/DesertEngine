// DesertAsset {"Kind":"Shader","Guid":"4a47504c1f437ed7a269a9c1ccbbab75","Versions":{"SHDR":1},"Dependencies":[]}
Shader "StaticMeshGBuffer_Instanced"
{
    // The (Instanced x GBuffer) cell of MeshShaderFor's table: the deferred G-buffer write, with the
    // INSTANCED vertex stage.
    //
    // WHY IT EXISTS. This cell was a deliberate hole, and the reason written beside it -- "instancing is
    // disabled in the G-buffer pass, every static takes the per-object path there" -- was true of the
    // AUTO-BATCHED statics (they fall back to per-object draws) and false of an InstancedStaticMesh
    // entity, which has no per-object path to fall back to. So every ISM entity was dropped, without a
    // line in the log, in the deferred path -- which is the default and what 81 of the repository's 88
    // scenes state. Measured on Resources/Assets/Scenes/G26_ISMProbe.desce: nine instances invisible in
    // Deferred, all nine drawn in Forward, from the same file.
    //
    // It is the instanced vertex of StaticMeshPBR_Instanced over the fragment of StaticMeshGBuffer, and
    // nothing else: Desert/Tests/Engine/MeshVertexPath asserts that relation against the reflected
    // SPIR-V, so the two cells of one pass cannot drift into two different G-buffer writes.

    Vertex
    {
        In(0) vec3 a_Position;
        In(1) vec3 a_Normal;
        In(2) vec3 a_Tangent;
        In(3) vec3 a_Bitangent;
        In(4) vec2 a_TextureCoord;

        #include <Common/CameraUB.glslh>

        // Per-instance world transforms — the model matrix comes from here (indexed by gl_InstanceIndex), instead
        // of the per-draw push-constant Transform used by the non-instanced Static.glsl.vert. One instanced draw
        // (instanceCount = N) renders all N transforms.
        // Anonymous block (no instance name) so shader reflection registers it under the BLOCK name
        // "InstanceTransforms" — matching MeshRenderer's Get<StorageBufferProperty>("InstanceTransforms").
        // (SPIRV-Cross names an SSBO by its instance/variable name when present; the Materials SSBO works the
        // same way precisely because it is anonymous.) Members are accessed in global scope: transforms[i].
        // Binding 17 because that is what MeshPathOwnBinding(Instanced) claims for this path, and the
        // whole point of that function is that ONE slot means InstanceTransforms in every cell of the
        // path. The G-buffer fragment below declares no lights at all, so 16 is free here — using it
        // would still be wrong, because the forward cell of this path cannot (PBR.glsl.frag holds 16 for
        // SpotLightsUB) and two cells of one path that number their own binding differently is exactly
        // the drift Desert/Tests/Engine/MeshVertexPath exists to refuse.
        ReadBuffer(17) InstanceTransforms
        {
        	mat4 transforms[];
        };

        // Kept byte-identical to Static.glsl.vert / PBR.glsl.frag so the reflected push range matches. The
        // instanced vertex IGNORES Transform (the instance SSBO supplies the model matrix); MaterialIndex is still
        // used by the fragment stage.
        PushConstant PushConstants
        {
        	mat4 Transform;     // offset 0  — unused here
        	uint MaterialIndex; // offset 64
        	vec4 WindA;         // offset 80 — Graphic::kInstancedWindPushOffset (Common/FoliageWind.glslh)
        	vec4 WindB;         // offset 96
        } m_PushConstants;

        #include <Common/FoliageWind.glslh>

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
        	mat4 model = transforms[gl_InstanceIndex];

        	vec3 worldPosition = InstancedWorldPosition(model, a_Position, m_PushConstants.WindA, m_PushConstants.WindB);

        	outVertex.WorldPosition  = worldPosition;
        	outVertex.Texcoord       = vec2(a_TextureCoord.x, 1.0 - a_TextureCoord.y);
        	outVertex.CameraPosition = cameraUB.CameraPos;

        	mat3 normalMatrix = transpose(inverse(mat3(model)));

        	vec3 T = normalize(normalMatrix * a_Tangent);
        	vec3 B = normalize(normalMatrix * a_Bitangent);
        	vec3 N = normalize(normalMatrix * a_Normal);

        	outVertex.Normal = N;
        	outVertex.TBN    = mat3(T, B, N);

        	gl_Position = cameraUB.Projection * cameraUB.View * vec4(worldPosition, 1.0);
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
        // The source-to-surface arithmetic, the SAME text the forward StaticMeshPBR compiles.
        #include <Common/PBRSurfaceInputs.glslh>
        Uniform(11) sampler2D  u_AlbedoTexture;
        Uniform(12) sampler2D  u_NormalTexture;
        Uniform(18) sampler2D  u_OpacityTexture;

        void main()
        {
        	GpuMaterial mat = materials[pc.MaterialIndex];

        	vec2 tiling = mat.ExtraParams.xy;
        	if (tiling.x <= 0.0) tiling.x = 1.0;
        	if (tiling.y <= 0.0) tiling.y = 1.0;
        	// The transport (GpuMaterial) carries a scale only: offset/rotation at their identities, UV set 0.
        	vec2 uv = PBRTransformUV(PBRSelectUV(inVertex.Texcoord, inVertex.Texcoord, 0), vec2(0.0), tiling, 0.0);

        	float alphaCutoff = mat.MetalRoughEmission.w;
        	if (alphaCutoff > 0.0 && texture(u_OpacityTexture, uv).r < alphaCutoff)
        		discard;

        	// No vertex-colour input in this vertex layout: the vertex colour is white.
        	vec3 albedo = PBRBaseColor(mat.AlbedoAO.rgb, pow(texture(u_AlbedoTexture, uv).rgb, vec3(2.2)), vec3(1.0));

        	vec3 N = normalize(inVertex.Normal);
        	const ivec2 nrmSize = textureSize(u_NormalTexture, 0);
        	if (nrmSize.x > 1 && nrmSize.y > 1)
        	{
        		// The transport carries no normal scale: glTF's default 1 changes nothing.
        		vec3 tangentNormal = PBRScaleTangentNormal(SampleTangentNormal(u_NormalTexture, uv), 1.0);
        		N = normalize(inVertex.TBN * tangentNormal);
        	}

        	// No ORM texture on this path: a white texel passes the factors through (occlusion is not a GBuffer channel).
        	const vec3  orm       = PBRResolveORM(vec3(1.0), 1.0, mat.MetalRoughEmission.y, mat.MetalRoughEmission.x);
        	const float metallic  = orm.z;
        	const float roughness = max(orm.y, 0.04);

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
        	// No emissive texture is bound: its texel is white.
        	oGBufferEmissive = vec4(PBREmission(vec3(1.0), mat.EmissionColor.rgb, mat.MetalRoughEmission.z), 1.0);
        }
    }
}
