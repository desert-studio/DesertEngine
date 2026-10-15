// DesertAsset {"Kind":"Shader","Guid":"0ab978f2de964b3886d042fc5ccffa1d","Versions":{"SHDR":1},"Dependencies":[]}
Shader "CompositeEditorPrimitives"
{
    // "Editor: CompositeEditorPrimitives" (EditorPrimitivesCompositeRenderer). Ported from UE 5.8
    // Engine/Shaders/Private/PostProcessCompositePrimitives.usf:301-505 (ComputeDepthMask, ResolveEditorPrimitiveColor,
    // MainCompositePostProcessPrimitivesPS, the MSAA_SAMPLE_COUNT > 1 resolve), adapted: the sample count is the
    // texture's (textureSamples), the scene depth is the output-extent unjittered depth the editor depth was populated
    // from (no jitter or lens-distortion transforms: there are none at this point of our chain), no stencil (no
    // foreground-no-dithering flag and no wireframe "opaque gizmo" view here), the display encode is the scene's own
    // sRGB output device at the view's Film Gamma (UE: a fixed pow 1/2.2), and the blend is the pipeline's
    // (One, OneMinusSrcAlpha) over the display image instead of a read of it.
    //
    // The editor primitives were drawn after the tonemap into a multisampled display-linear target (premultiplied by
    // their src-alpha blend, coverage in alpha, cleared transparent) against the scene depth populated into their
    // own depth: this resolves the samples, encodes the colour for the display and lays it over the tonemapped
    // image. A primitive behind the scene (one drawn without its depth test) shows dimmed and dithered, as in UE.

    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>

        void main()
        {
        	gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }

    Fragment
    {
        Uniform(1) sampler2DMS u_EditorColor; // premultiplied display-linear colour, coverage in alpha
        Uniform(2) sampler2DMS u_EditorDepth; // the populated scene depth and what the primitives wrote over it
        Uniform(3) sampler2D   u_SceneDepth;  // the depth the editor depth was populated from (reversed Z)
        Out(0) vec4 oColor;

        Uniform(0) EditorPrimitivesCompositeUB
        {
            float u_DisplayGamma;                // the view's Film Gamma (the tonemap's output device)
            float u_OccludedDitheringPatternSize; // UE FPostProcessCompositePrimitiveParams defaults
            float u_OccludedDithering;
            float u_OccludedBrightness;
        };

        #include <Common/FilmToneMap.glslh>

        // UE ComputeDepthMask: 1 where the primitive is in front of the scene (reversed Z: larger is nearer), fading
        // to 0 over a soft device-depth bias behind it.
        float ComputeDepthMask(float sceneDeviceZ, float editorDeviceZ)
        {
        	const float kDeviceDepthFade = 0.00005;
        	return clamp(1.0 - (sceneDeviceZ - editorDeviceZ) / kDeviceDepthFade, 0.0, 1.0);
        }

        void main()
        {
        	const ivec2 pixel      = ivec2(gl_FragCoord.xy);
        	const ivec2 editorSize = textureSize(u_EditorColor);
        	const int   samples    = textureSamples(u_EditorColor);

        	// The scene texel under this pixel: the same point mapping PopulateSceneDepth filled the editor depth with.
        	const ivec2 depthSize    = textureSize(u_SceneDepth, 0);
        	const vec2  uv           = (vec2(pixel) + 0.5) / vec2(editorSize);
        	const ivec2 depthTexel   = clamp(ivec2(uv * vec2(depthSize)), ivec2(0), depthSize - ivec2(1));
        	const float sceneDeviceZ = texelFetch(u_SceneDepth, depthTexel, 0).r;

        	vec4  editor          = vec4(0.0);
        	float depthMask       = 0.0;
        	float depthMaskWeight = 0.0;
        	for (int s = 0; s < samples; ++s)
        	{
        		const vec4  c             = texelFetch(u_EditorColor, pixel, s);
        		const float editorDeviceZ = texelFetch(u_EditorDepth, pixel, s).r;
        		// A sample nothing was drawn on carries no weight (UE: "this prevents actual black pixels from being
        		// visible" - coverage is the alpha).
        		const float weight = c.a;
        		if (weight > 0.0)
        		{
        			editor          += vec4(c.rgb, weight);
        			depthMask       += ComputeDepthMask(sceneDeviceZ, editorDeviceZ) * weight;
        			depthMaskWeight += weight;
        		}
        	}
        	editor /= float(samples);
        	if (editor.a <= 0.0)
        		discard;
        	depthMask = depthMaskWeight > 0.0 ? depthMask / depthMaskWeight : 0.0;

        	// Display-linear -> the display's encoding, on the straight colour (UE ResolveEditorPrimitiveColor).
        	const vec3 straight = editor.rgb / max(editor.a, 0.0001);
        	editor.rgb          = FilmEncodeDisplay(straight, u_DisplayGamma) * editor.a;

        	// UE's NxN checker for primitives that end up behind the scene, dimmed by the occluded brightness.
        	const float patternMask =
        	     mod(floor(gl_FragCoord.x / u_OccludedDitheringPatternSize) +
        	              floor(gl_FragCoord.y / u_OccludedDitheringPatternSize),
        	         2.0) *
        	     u_OccludedBrightness;
        	const float lowContrastPatternMask = mix(1.0 - u_OccludedDithering, 1.0, patternMask);
        	const float hiddenMask             = mix(u_OccludedBrightness, 1.0, depthMask);
        	const float darkenMask             = mix(lowContrastPatternMask, 1.0, depthMask);

        	// Blended (One, OneMinusSrcAlpha): display * (1 - a) + colour * darken * hidden, UE's composite.
        	oColor = vec4(editor.rgb * (darkenMask * hiddenMask), editor.a);
        }
    }
}
