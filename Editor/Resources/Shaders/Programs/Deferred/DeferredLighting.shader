Shader "DeferredLighting"
{
    // Deferred lighting + G-buffer debug visualization (fullscreen). Consumes the scene renderer's MRT G-buffer.

    Vertex
    {
        #include <Common/QuadPositions.glslh>
        #include <Common/QuadTextureCoords.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
        	v_TexCoord  = QUAD_TEXTURE_COORDINATES[gl_VertexIndex];
        	gl_Position = vec4(QUAD_POSITIONS[gl_VertexIndex], 0.0, 1.0);
        }
    }

    Fragment
    {
        // Deferred lighting + G-buffer DEBUG visualization (fullscreen). Reads the G-buffer (albedo/metallic,
        // world-normal/roughness, world-position) and shades it with the directional sun (cascaded shadow-mapped) +
        // all point & spot lights (the deferred payoff: many dynamic lights in screen space). In a debug mode it
        // instead shows a raw G-buffer channel full-screen. Non-geometry texels are discarded so the LOADed forward
        // scene (real procedural sky / skybox + grid) shows through.

        #include <Mesh/PointLight.glslh>      // binding 6  (SSBO PointLightsUB) + CalculatePointLight + PBRFunctions
        #include <Mesh/Spotlight.glslh>       // binding 16 (SSBO SpotLightsUB)  + CalculateSpotLight
        #include <Mesh/LightsMetadata.glslh>  // binding 4  (UB LightsMetadata: point/spot/dir counts)
        // THE direct-light BRDF, shared with the forward mesh shaders and with the point/spot headers
        // above (which already include it). Named explicitly because this pass calls it directly.
        #include <Mesh/DirectLighting.glslh>
        // The one-bounce estimate GatherIndirectGI below is built from — direct light seen a second time,
        // so it is the SAME BRDF, reached through the same text. Must follow DirectLighting.glslh, which it
        // calls.
        #include <Mesh/IndirectBounce.glslh>

        In(0) vec2 v_TexCoord;

        Uniform(1) sampler2D u_GBufferC; // rgb = world position
        Uniform(2) sampler2D u_GBufferA; // rgb = albedo, a = metallic
        Uniform(3) sampler2D u_GBufferB; // rgb = world normal, a = roughness
        Uniform(8) sampler2D u_SSAO;     // r = ambient-occlusion factor (1 = lit)
        Uniform(9) sampler2D u_GBufferEmissive; // rgb = HDR emissive (self-illumination, added below)
        // RSM GI mode only: one-bounce indirect light PRE-RESOLVED into its own buffer by the GIResolve pass
        // (already temporally denoised there). Unused — and left bound to its dummy — in the other GI modes.
        Uniform(10) sampler2D u_GI;

        // THE BAKED ENVIRONMENT — the same trio the forward mesh shaders bind, and the whole point of
        // this pass having an ambient term at all. Until 2026-09-03 the deferred composite read NEITHER
        // cube and floored its ambient at a flat vec3(0.08), so 51 of the repository's 53 scenes lit
        // their ordinary static opaque geometry from a constant while the skinned meshes, the
        // custom-shader meshes and the glass drawn forward over this very composite lit themselves from
        // the sky. One material was shaded two ways depending on whether its mesh had a skeleton.
        //
        // New slots (17..19) rather than the forward path's 8..10: those three are already taken here by
        // u_SSAO / u_GBufferEmissive / u_GI, and this pass shares no descriptor layout with the mesh
        // shaders — it is a fullscreen quad with its own material. Only the NAMES have to match, because
        // the material binds by name.
        Uniform(17) samplerCube u_EnvIrradianceTex; // diffuse irradiance (cosine-convolved sky)
        Uniform(18) samplerCube u_EnvSpecularTex;   // GGX-prefiltered radiance, roughness across mips
        Uniform(19) sampler2D   u_BRDFLUTTexture;   // split-sum BRDF integration (cosLo, roughness)
        // The sky's look — how the two cubes above are read (Common/SkyLook.glslh). Slot 20: the
        // first free one in this layout.
        Uniform(20) SkyLookUB
        {
            vec4 YawCosSin; // xy = (cos yaw, sin yaw) — Graphic::SkyLookGPU
            vec4 Gain;      // rgb = tint * intensity
        } skyLook;

        Out(0) vec4 oColor;

        const vec3 Fdielectric = vec3(0.04); // base reflectance for dielectrics (matches PBR.glsl.frag)

        // The ambient model itself, shared verbatim with StaticMeshPBR / StaticMeshPBR_Instanced /
        // SkinnedMeshPBR. Included after the three bindings above because it names them.
        #include <Mesh/AmbientIBL.glslh>

        // The directional (sun) Cook-Torrance contribution used to be a local copy of the BRDF right here,
        // with a comment recording that the raw albedo*NdotL*intensity Lambert made the sun ~PI× too bright
        // and washed out the point/spot lights. The forward mesh shaders had shipped exactly that raw form
        // for as long, so the comment described a defect that was live one file away. The copy is gone: the
        // sun is now EvaluateDirectionalLight from Mesh/DirectLighting.glslh, which the forward path and the
        // point/spot headers compile too. Included at the top with Mesh/PointLight.glslh, which pulls it in.

        Uniform(0) DeferredUB
        {
        	vec4 u_LightDir;   // xyz = direction the light travels (away from the sun); w unused
        	vec4 u_LightColor; // rgb = colour, a = intensity
        	vec4 u_Params;     // x = debug mode (0..9), y = GI intensity (0 = off), z = SSAO enabled, w = GI mode
        	vec4 u_CameraPos;  // xyz = camera world position (for the view vector); w unused
        };

        float ssgiHash(vec2 p)
        {
        	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
        	p3 += dot(p3, p3.yzx + 33.33);
        	return fract((p3.x + p3.y) * p3.z);
        }

        // One-bounce screen-space GI: gather nearby G-buffer texels, work out how much of the SUN each of
        // them reflects TOWARD this surface, and accumulate it. Screen-space only (no extra passes / no
        // lit-colour feedback); misses off-screen and point-lit bounces.
        //
        // HOW LITTLE THIS DELIVERS IN A ROOM, measured on CornellDemo (the scene that existed to show
        // colour bleed) at --camera 0,300,1400, 715x764, noise floor 0 pixels:
        //   * the whole feature on vs off:  mean 0.11/255, max 16/255, 13.4 % of pixels;
        //   * the isolated indirect buffer: 0.000 in every statistic on the floor, near the walls and at
        //     the centre alike, with a faint band only where a SUNLIT wall meets the floor;
        //   * aiming the sun at the unlit wall: no change whatsoever.
        // Three divisors stack. A neighbour the sun does not reach returns exactly vec3(0) from the
        // shared BRDF; BounceFalloff is 1 + d²/(1 m)², which is 9.4 across a 290 cm room; and the sum is
        // divided by SAMPLES whether or not a sample found an emitter, so WIDENING the radius makes the
        // bleed weaker rather than longer-ranged — measured at 0.45 instead of 0.12, the one non-zero
        // floor reading fell from 0.001 to 0.000. RADIUS is therefore not the knob it looks like.
        //
        // What would change the answer is bouncing the point and spot lights as well as the sun, at a
        // price of SAMPLES × lights BRDF evaluations per pixel inside the pass that produces the lit
        // colour. That is a design decision for the owner, not a constant for whoever is passing.
        //
        // The per-sample estimate is Mesh/IndirectBounce.glslh, which shades the bouncing neighbour through the
        // SAME Mesh/DirectLighting.glslh the sun below is shaded with. It used to shade it here, inline, as
        // `nAlb * max(0.0, dot(nN, sunL)) * sunRadiance` — the un-normalized Lambert this engine deleted from
        // its forward mesh shaders — which made every emitter PI times brighter than the same surface is when
        // the main pass shades it. The G-buffer already carries the neighbour's metallic (GBufferA.a) and
        // roughness (GBufferB.a), so nothing extra is sampled to shade it properly.
        //
        // WHAT THE ESTIMATE DELIBERATELY DOES NOT MODEL, stated so the omission is a property and not an
        // oversight: the neighbour's own occlusion. The sun below is attenuated by ShadowFactor *
        // CloudShadowFactor; re-deriving that per gather sample means a cascade choice plus nine PCF taps
        // TWELVE more times per pixel, and there is no lit-colour buffer to read it back from because this
        // gather runs inside the very pass that produces the lit colour. A neighbour standing in shadow
        // therefore bounces as though it stood in sun.
        vec3 GatherIndirectGI(vec2 uv, vec3 worldPos, vec3 N, vec3 sunL, vec3 sunRadiance)
        {
        	const int   SAMPLES = 12;     // GI sample count (perf/quality knob)
        	// NOT a range knob, measured: because the sum below is divided by SAMPLES whether or not a
        	// sample found an emitter, and BounceFalloff grows with distance, a WIDER radius spreads the
        	// same fixed budget over dimmer contributions. At 0.45 instead of 0.12 the only non-zero
        	// reading on CornellDemo's floor fell from 0.001 to 0.000. The line used to say "wider =
        	// longer-range bleed, but noisier"; the first half of that is false.
        	const float RADIUS  = 0.12;   // screen-space gather radius (UV)
        	const float GOLDEN  = 2.3999632; // golden angle for an even spiral

        	float ang      = ssgiHash(uv * 2048.0) * 6.2831853;
        	vec3  indirect = vec3(0.0);

        	for (int i = 0; i < SAMPLES; i++)
        	{
        		float r = RADIUS * sqrt((float(i) + 0.5) / float(SAMPLES));
        		float a = ang + float(i) * GOLDEN;
        		vec2  suv = uv + vec2(cos(a), sin(a)) * r;
        		if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;

        		vec4 nb = texture(u_GBufferB, suv);
        		if (dot(nb.rgb, nb.rgb) <= 0.001) continue; // sky texel -> no bounce

        		vec4  na   = texture(u_GBufferA, suv);
        		vec3  nPos = texture(u_GBufferC, suv).rgb;
        		vec3  nF0  = mix(Fdielectric, na.rgb, na.a);

        		indirect += EvaluateBounceSample(worldPos, N, nPos, normalize(nb.rgb), na.rgb, nF0, na.a,
        		                                 max(nb.a, 0.04), sunL, sunRadiance);
        	}
        	return indirect / float(SAMPLES);
        }

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

        // THE CLOUD LAYER'S SHADOW ON THE WORLD — a second, independent occluder of the same sun.
        // Declared here, evaluated by the shared receiver text below: the wrapper used to live in this
        // file and this file ALONE, which is exactly why no forward-shaded surface in the engine ever
        // received a cloud shadow. See Common/CloudShadowReceiver.glslh.
        Uniform(11) sampler2D u_CloudShadowMap;
        Uniform(12) CloudShadowUB {
        	mat4 u_CloudShadowWorldToMap;
        	// x = the kilometres the map's clip z spans, y = 1 when the map is real and must be read,
        	// z = the UV width of the border fade, w = the artist's shadow strength.
        	vec4 u_CloudShadowParams;
        };

        // THE receiver, shared verbatim with the forward mesh shaders, the glass pass and the terrain.
        // Included after the two bindings above because it names them.
        #include <Common/CloudShadowReceiver.glslh>

        // Heat ramp for the Light-Complexity debug view: 0 -> dark blue, up through cyan/green/yellow -> red.
        // Standard "jet"-style piecewise map so overlapping light volumes read as hotter pixels.
        vec3 HeatColor(float t)
        {
        	t = clamp(t, 0.0, 1.0);
        	return clamp(vec3(1.5 - abs(4.0 * t - 3.0),
        	                  1.5 - abs(4.0 * t - 2.0),
        	                  1.5 - abs(4.0 * t - 1.0)), 0.0, 1.0);
        }

        void main()
        {
        	vec4 ga = texture(u_GBufferA, v_TexCoord);
        	vec4 gb = texture(u_GBufferB, v_TexCoord);
        	vec4 gc = texture(u_GBufferC, v_TexCoord);

        	vec3  albedo    = ga.rgb;
        	float metallic  = ga.a;
        	vec3  normal    = gb.rgb;
        	float roughness = max(gb.a, 0.04);
        	vec3  worldPos  = gc.rgb;

        	const int dbg = int(u_Params.x + 0.5);

        	// No geometry here (zero normal — the G-buffer is cleared to 0): discard so the LOADed forward scene
        	// (real procedural sky / skybox + grid) shows through.
        	const bool hasGeometry = dot(normal, normal) > 0.001;
        	if (!hasGeometry)
        	{
        		discard;
        	}

        	float ao = (u_Params.z > 0.5) ? texture(u_SSAO, v_TexCoord).r : 1.0; // z = SSAO enabled

        	// --- Debug channels (shown on the geometry, over the sky) ---
        	if (dbg == 1) { oColor = vec4(albedo, 1.0); return; }                          // Albedo
        	if (dbg == 2) { oColor = vec4(normalize(normal) * 0.5 + 0.5, 1.0); return; }   // Normal
        	if (dbg == 3) { oColor = vec4(vec3(metallic),  1.0); return; }                 // Metallic
        	if (dbg == 4) { oColor = vec4(vec3(roughness), 1.0); return; }                 // Roughness
        	if (dbg == 5) { oColor = vec4(vec3(ao),        1.0); return; }                 // Ambient Occlusion

        	// Light Complexity: count how many point/spot light VOLUMES cover this pixel (by radius/range +
        	// cone), then map the count to a heat colour. Cheap in deferred — the light data is already bound.
        	if (dbg == 7)
        	{
        		uint lights = 0u;
        		for (uint i = 0u; i < lightsMetadata.PointLightCount; i++)
        			if (distance(pointLights[i].position, worldPos) < pointLights[i].radius)
        				lights++;
        		for (uint i = 0u; i < lightsMetadata.SpotLightCount; i++)
        		{
        			vec3  toL = spotLights[i].position - worldPos;
        			if (length(toL) < spotLights[i].range && SpotConeFactor(spotLights[i], normalize(toL)) > 0.0)
        				lights++;
        		}
        		// Normalize by a fixed budget (8 overlapping lights = full red) so the scale is stable.
        		oColor = vec4(HeatColor(float(lights) / 8.0), 1.0);
        		return;
        	}

        	// Material Complexity: the G-buffer pass stashed the material's sampled-texture count in GBufferC.w
        	// (0..3). Heat-map it as a proxy for per-pixel shading cost (UE-style shader/material complexity).
        	if (dbg == 9) { oColor = vec4(HeatColor(gc.w / 3.0), 1.0); return; }

        	// --- Lit: shadow-mapped directional sun (N·L) + full PBR point/spot lights ---
        	vec3 N    = normalize(normal);
        	vec3 view = normalize(u_CameraPos.xyz - worldPos);
        	vec3 F0   = mix(Fdielectric, albedo, metallic);

        	// Directional sun (energy-normalized PBR), occluded by the cascaded shadow map.
        	// TWO OCCLUDERS OF ONE SUN, multiplied: the cascaded maps for opaque geometry and the cloud
        	// layer's own volumetric transmittance. Only the DIRECTIONAL term is attenuated — the ambient
        	// below is the whole sky dome, which a cloud deck occludes with a different geometry
        	// (a hemisphere, not a direction) and which UE answers with a separate Sky AO volume. Folding
        	// a directional occlusion into an omnidirectional term would darken the shaded side of every
        	// object under a cloud by an amount nothing in the world justifies.
        	vec3  L        = normalize(-u_LightDir.xyz);
        	// `cascade` is the chosen cascade index, which this pass has no debug view for — the forward
        	// mesh shaders tint by it under Scene Settings -> Debug -> Cascades and the deferred composite's
        	// debug modes are a different, G-buffer-driven set. Read and dropped rather than given a second
        	// ShadowFactor overload, because a second overload is how the four copies started.
        	int   cascade  = -1;
        	float shadow   = ShadowFactor(worldPos, N, L, cascade) * CloudShadowFactor(worldPos);
        	vec3  radiance = u_LightColor.rgb * u_LightColor.a;
        	vec3  result   = EvaluateDirectionalLight(u_LightDir.xyz, radiance, view, N, F0, metallic, roughness,
        	                                          albedo)
        	               * shadow;

        	// Point lights (the city payoff): every source contributes full Cook-Torrance PBR (not shadowed yet).
        	for (uint i = 0u; i < lightsMetadata.PointLightCount; i++)
        		result += CalculatePointLight(pointLights[i], worldPos, view, N, F0, metallic, roughness, albedo);

        	// Spot lights (street lamps / headlights).
        	for (uint i = 0u; i < lightsMetadata.SpotLightCount; i++)
        		result += CalculateSpotLight(spotLights[i], worldPos, view, N, F0, metallic, roughness, albedo);

        	// One-bounce GI (D6). Two interchangeable sources, picked by u_Params.w:
        	//  1 = SCREEN-SPACE: gather from sun-lit G-buffer neighbours. Cheap and self-contained, but only
        	//      geometry currently ON SCREEN can bounce, and there is no denoiser.
        	//  2 = RSM: read the GIResolve buffer, which bounced light from everything the SUN sees (off-screen
        	//      included) and was temporally accumulated. Costs an extra shadow-style pass + two fullscreen
        	//      passes. Its intensity is already applied in GIResolve, so it is NOT scaled again here.
        	// The RSM buffer is a jittered gather even after temporal accumulation, so read it through a 5x5
        	// tent — single-tap leaves visible grain that glass refraction and SSR then magnify. UVs are
        	// clamped because the global sampler is REPEAT.
        	vec3  indirect    = vec3(0.0);
        	float giIntensity = u_Params.y;
        	int   giMode      = int(u_Params.w + 0.5);
        	if (giMode == 1 && giIntensity > 0.0)
        	{
        		indirect = GatherIndirectGI(v_TexCoord, worldPos, N, L, radiance) * giIntensity;
        	}
        	else if (giMode == 2)
        	{
        		vec2  texel = 1.0 / vec2(textureSize(u_GI, 0));
        		vec3  giAcc = vec3(0.0);
        		float wsum  = 0.0;
        		for (int gy = -2; gy <= 2; gy++)
        			for (int gx = -2; gx <= 2; gx++)
        			{
        				float w  = (3.0 - abs(float(gx))) * (3.0 - abs(float(gy))); // 5x5 tent
        				vec2  uv = clamp(v_TexCoord + vec2(gx, gy) * texel, vec2(0.001), vec2(0.999));
        				giAcc += texture(u_GI, uv).rgb * w;
        				wsum  += w;
        			}
        		indirect = giAcc / wsum;
        	}

        	if (dbg == 6) { oColor = vec4(indirect, 1.0); return; } // Indirect GI only

        	// Ambient = the BAKED ENVIRONMENT (split-sum IBL: diffuse irradiance + prefiltered specular)
        	// plus the one-bounce indirect, attenuated by SSAO — assembled by the SAME shared function the
        	// forward mesh shaders call, so a scene rendered through either RenderingPath gets one ambient
        	// model. The only argument that differs there is `indirect`, which the forward path passes as
        	// zero because it has no bounce gather.
        	vec3 ibl     = AmbientIBL(view, N, F0, metallic, roughness, albedo);
        	vec3 ambient = ComposeAmbient(ibl, albedo, ao, indirect);

        	// Self-illumination (view-independent) — added here (not lit) so HDR emissive reaches the composite
        	// and blooms, matching the forward path. GBufferEmissive is 0 where the material has none.
        	vec3 emissive = texture(u_GBufferEmissive, v_TexCoord).rgb;

        	oColor = vec4(result + ambient + emissive, 1.0);
        }
    }
}
