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

    // ONE parameter layout for every PBR pass (forward, instanced, GBuffer, skinned, glass): the renderer
    // writes one Materials[] row per object from the forward material and every pass reads it, so these
    // rows are identical by contract — ShippedShaderPasses.EveryPBRPassDeclaresTheOneRowLayout holds them equal.
    Properties Binding(2)
    {
        Color       AlbedoColor ("Albedo", Category("Surface")) = (1, 1, 1, 1)
        Float       MetallicFactor ("Metallic", Range(0,1), Category("Surface")) = 0
        Float       RoughnessFactor ("Roughness", Range(0,1), Category("Surface")) = 0.5
        Float       AOStrength ("Ambient Occlusion", Range(0,1), Category("Surface")) = 1
        Color       EmissiveColor ("Emissive", Category("Surface")) = (0, 0, 0, 1)
        Float       EmissiveIntensity ("Emissive Intensity", Range(0,100), Category("Surface")) = 1
        Float       AlphaCutoff ("Alpha Cutoff", Range(0,1), Category("Surface")) = 0
        Float       Transmission ("Transmission", Range(0,1), Category("Glass")) = 0
        Float       IOR ("IOR", Range(1,2.5), Category("Glass")) = 1.5
        Color       GlassTint ("Glass Tint", Category("Glass")) = (1, 1, 1, 1)
        Vec2        UVTiling ("UV Tiling", Category("Surface")) = (1, 1)
        Vec2        UVOffset ("UV Offset", Category("Surface")) = (0, 0)
        Float       UVRotation ("UV Rotation", Range(-3.14159,3.14159), Category("Surface")) = 0
        Float       NormalScale ("Normal Scale", Range(0,4), Category("Surface")) = 1
        Float       OcclusionStrength ("Occlusion Strength", Range(0,1), Category("Surface")) = 1
        // Which channel of u_OpacityTexture is the mask: 0 = R of a separate opacity map, 3 = A (the importer binds the
        // albedo texture itself there for a glTF MASK). Stated, never guessed from the bound texture's size.
        Float       OpacityChannel ("Opacity Channel", Range(0,3), Category("Surface")) = 0
        // Material half of the sun-shadow receive decision; the renderer also zeroes it for a mesh whose
        // Receive Shadows toggle is off, so a surface skips the sun shadow when EITHER says so.
        Float       ReceiveSunShadows ("Receive Sun Shadows", Range(0,1), Category("Shadows")) = 1
        Texture2D   u_AlbedoTexture ("Albedo Map", Category("Textures"))
        // The ONE slot whose empty state is not white. A normal map is unpacked with `2*t - 1`, so a
        // white texel decodes to a normalised (1,1,1) — a normal tilted 54 degrees off the surface —
        // whereas (0.5,0.5,1) decodes to +Z, which is what "this surface has no normal detail" means.
        // The fragment stages here, in StaticMeshGBuffer and in StaticMeshPBR_Instanced all guard with
        // `textureSize(u_NormalTexture,0).x > 1` and skip a 1x1, so this changes no pixel today; it is
        // written down so the guard is a fast path rather than the only thing standing between an empty
        // slot and a wrong normal.
        Texture2D   u_NormalTexture ("Normal Map", Category("Textures")) = "normal"
        Texture2D   u_OpacityTexture ("Opacity Map", Category("Textures"))
        // Packed glTF-style: R = occlusion, G = roughness, B = metallic, each multiplying its factor; white when empty.
        Texture2D   u_ORMTexture ("ORM Map", Category("Textures"))
        Texture2D   u_EmissiveTexture ("Emissive Map", Category("Textures"))
    }

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

        // The ONE engine push block, the same header the parser injects into the fragment stage, so both
        // stages reflect one range of one length. This stage IGNORES Transform (the instance SSBO supplies
        // the model matrix) and reads WindA/WindB (Graphic::kInstancedWindPushOffset); MaterialIndex is the
        // fragment stage's.
        #include <Common/MaterialTransport.glslh>

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



        // The surface's own three maps, at the SAME slots the forward mesh shaders use them at. The numbers
        // are deliberately unchanged and deliberately not compacted to 0,1,2: the whole mesh shader family
        // names one surface, and a reader comparing two of them should see the albedo map at the same
        // binding in both. A sparse set is not a problem for Vulkan, and the gaps are the record of what
        // this pass does NOT need.
        #include <Common/TangentNormal.glslh>
        // The source-to-surface arithmetic, the SAME text the forward StaticMeshPBR compiles.
        #include <Common/PBRSurfaceInputs.glslh>
        Uniform(11) sampler2D u_AlbedoTexture;
        Uniform(12) sampler2D u_NormalTexture;
        Uniform(18) sampler2D u_OpacityTexture;
        Uniform(23) sampler2D u_ORMTexture;      // R = occlusion, G = roughness, B = metallic (data, linear)
        Uniform(24) sampler2D u_EmissiveTexture; // sRGB colour, linearised below like the albedo

        void main()
        {

        	vec2 tiling = u_Material.UVTiling;
        	if (tiling.x <= 0.0) tiling.x = 1.0;
        	if (tiling.y <= 0.0) tiling.y = 1.0;
        	// Offset/rotation/scale come from the row; TEXCOORD_1 is not a vertex input yet, so UV set 0.
        	vec2 uv = PBRTransformUV(PBRSelectUV(inVertex.Texcoord, inVertex.Texcoord, 0), u_Material.UVOffset, tiling, u_Material.UVRotation);

        	float alphaCutoff = u_Material.AlphaCutoff;
        	// The ONE mask source: the channel OpacityChannel names of u_OpacityTexture (an empty slot is white, so no cut).
        	float mask = texture(u_OpacityTexture, uv)[int(u_Material.OpacityChannel)];
        	if (alphaCutoff > 0.0 && mask < alphaCutoff)
        		discard;

        	// No vertex-colour input in this vertex layout: the vertex colour is white.
        	vec3 albedo = PBRBaseColor(u_Material.AlbedoColor.rgb, pow(texture(u_AlbedoTexture, uv).rgb, vec3(2.2)), vec3(1.0));

        	vec3 N = normalize(inVertex.Normal);
        	const ivec2 nrmSize = textureSize(u_NormalTexture, 0);
        	if (nrmSize.x > 1 && nrmSize.y > 1)
        	{
        		// NormalScale from the row (glTF normalTexture.scale).
        		vec3 tangentNormal = PBRScaleTangentNormal(SampleTangentNormal(u_NormalTexture, uv), u_Material.NormalScale);
        		N = normalize(inVertex.TBN * tangentNormal);
        	}

        	// The ORM map against its factors (an empty slot is white, so the factors pass through unchanged).
        	const vec3  orm       = PBRResolveORM(texture(u_ORMTexture, uv).rgb, u_Material.OcclusionStrength, u_Material.RoughnessFactor, u_Material.MetallicFactor);
        	const vec3  emissiveTexel = pow(texture(u_EmissiveTexture, uv).rgb, vec3(2.2));
        	const float metallic  = orm.z;
        	const float roughness = max(orm.y, 0.04);

        	// Material-complexity proxy (heat-mapped by the DeferredLighting debug branch): count the textures
        	// this material actually samples — a bound map is a real texture, an absent one is a 1x1 dummy.
        	// Stashed in the otherwise-unused GBufferC.w so it costs no extra target.
        	int texCount = 0;
        	if (textureSize(u_AlbedoTexture, 0).x > 1)  texCount++;
        	if (nrmSize.x > 1)                          texCount++; // normal map (nrmSize computed above)
        	if (textureSize(u_OpacityTexture, 0).x > 1) texCount++;
        	if (textureSize(u_ORMTexture, 0).x > 1)      texCount++;
        	if (textureSize(u_EmissiveTexture, 0).x > 1) texCount++;

        	oGBufferA = vec4(albedo, metallic);
        	oGBufferB = vec4(N, roughness);
        	oGBufferC = vec4(inVertex.WorldPosition, float(texCount));
        	// Emissive is view-independent self-illumination; the deferred lighting resolve ADDS it, matching the
        	// forward StaticMeshPBR path so emissive materials reach the HDR composite and bloom (values > 1).
        	oGBufferEmissive = vec4(PBREmission(emissiveTexel, u_Material.EmissiveColor.rgb, u_Material.EmissiveIntensity), 1.0);
        }
    }
}
