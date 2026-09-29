// DesertAsset {"Kind":"Shader","Guid":"e2095f403cbb2ba2eb81325b0cab2bb1","Versions":{"SHDR":1},"Dependencies":[]}
Shader "StaticMeshGBuffer"
{
    // Deferred G-buffer geometry pass for static meshes (writes Albedo+Metallic / Normal+Roughness MRT).
    // Shares Static.glsl.vert + the Materials[] SSBO with StaticMeshPBR so material data binds unchanged.

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
        Texture2D   u_MetallicTexture ("Metallic Map", Category("Textures"))
        Texture2D   u_RoughnessTexture ("Roughness Map", Category("Textures"))
        Texture2D   u_AOTexture ("AO Map", Category("Textures"))
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

        // The ONE engine push block (Transform, MaterialIndex, BoneOffset, wind): the parser injects the same
        // header into the fragment stage, so both stages of this pipeline reflect one range of one length.
        // This stage reads Transform only.
        #include <Common/MaterialTransport.glslh>


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

        void main()
        {

        	vec2 tiling = u_Material.UVTiling;
        	if (tiling.x <= 0.0) tiling.x = 1.0;
        	if (tiling.y <= 0.0) tiling.y = 1.0;
        	// Offset/rotation/scale come from the row; TEXCOORD_1 is not a vertex input yet, so UV set 0.
        	vec2 uv = PBRTransformUV(PBRSelectUV(inVertex.Texcoord, inVertex.Texcoord, 0), u_Material.UVOffset, tiling, u_Material.UVRotation);

        	float alphaCutoff = u_Material.AlphaCutoff;
        	if (alphaCutoff > 0.0 && texture(u_OpacityTexture, uv).r < alphaCutoff)
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

        	// No ORM texture on this path: a white texel passes the factors through (occlusion is not a GBuffer channel).
        	const vec3  orm       = PBRResolveORM(vec3(1.0), 1.0, u_Material.RoughnessFactor, u_Material.MetallicFactor);
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
        	oGBufferEmissive = vec4(PBREmission(vec3(1.0), u_Material.EmissiveColor.rgb, u_Material.EmissiveIntensity), 1.0);
        }
    }
}
