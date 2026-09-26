// DesertAsset {"Kind":"Shader","Guid":"17a68d0e77119595202bccb63ac5ddc0","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSRComposite"
{
    // Blends the resolved reflection buffer over the scene target (src-alpha): rgb = blurred reflection,
    // a = blurred reflectance. Pipeline must enable blending.

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
        // SSR composite: resolves the traced reflection buffer over the scene. A tent blur whose radius scales
        // with the surface ROUGHNESS turns the trace's jitter noise into a soft glossy reflection — rough floors
        // get blurry reflections, near-mirror surfaces stay sharp (still 1 texel min to hide trace fizz).
        // Output is BLENDED (src-alpha) over the scene target: rgb = blurred reflection, a = blurred reflectance.

        In(0) vec2 v_TexCoord;

        Uniform(1) sampler2D u_SSR;           // rgb = reflected colour, a = reflectance
        Uniform(2) sampler2D u_GBufferNormal; // rgb = world normal, a = roughness

        Out(0) vec4 oColor;

        Uniform(0) SSRCompositeUB
        {
        	vec4 u_Params; // xy = texel size, zw = unused
        };

        void main()
        {
        	vec4 gb = texture(u_GBufferNormal, v_TexCoord);
        	if (dot(gb.rgb, gb.rgb) <= 0.001) { oColor = vec4(0.0); return; } // sky

        	float roughness = gb.a;
        	// 1..3 texels: the resolve already denoised (temporal), so the composite only needs a light
        	// roughness-driven soften — stacking two wide blurs smeared reflections into mush.
        	float radius = mix(1.0, 3.0, clamp(roughness * 2.5, 0.0, 1.0));
        	vec2  t      = u_Params.xy * radius * 0.5; // offsets go -2..2 -> halve so the span = radius

        	// 5x5 tent kernel, ALPHA-WEIGHTED: a missed ray (a = 0, black) must not darken the average —
        	// neighbouring hits fill the hole instead, which is what turns the jitter speckle into a smooth
        	// reflection. Output alpha = average coverage, so partially-missed regions blend proportionally.
        	// The global sampler is REPEAT — clamp the tap UVs or the reflection wraps across screen edges.
        	vec3  colAcc = vec3(0.0);
        	float aAcc   = 0.0;
        	float wsum   = 0.0;
        	for (int y = -2; y <= 2; y++)
        		for (int x = -2; x <= 2; x++)
        		{
        			float w  = (3.0 - abs(float(x))) * (3.0 - abs(float(y)));
        			vec2  uv = clamp(v_TexCoord + vec2(x, y) * t, vec2(0.001), vec2(0.999));
        			vec4  s  = texture(u_SSR, uv);
        			colAcc += s.rgb * s.a * w;
        			aAcc   += s.a * w;
        			wsum   += w;
        		}

        	vec3  color    = aAcc > 0.001 ? colAcc / aAcc : vec3(0.0);
        	float coverage = aAcc / wsum;

        	// Confidence fade: where hits are SPARSE (grazing foreground rays, screen edges) the alpha-weighted
        	// fill would otherwise smear a few grey hits into a uniform grey wash — fade those regions out
        	// instead (the surface keeps its normal IBL look there). Dense reflections are unaffected.
        	float confidence = smoothstep(0.08, 0.35, coverage);
        	oColor = vec4(color, coverage * confidence);
        }
    }
}
