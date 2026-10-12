// DesertAsset {"Kind":"Shader","Guid":"910f2a02256a414e8dd5cebb9a027f5b","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VelocityView"
{
    // The Velocity view mode (DeferredDebugMode::Velocity): the view's velocity (NDC current - previous, the render
    // extent) as a colour. Hue = direction of the screen motion, brightness = its length in render pixels (full at
    // 16 px per frame), black where nothing moved. Drawn over the post input by "Debug: Velocity".

    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
        	v_TexCoord  = FullscreenTriangleUV();
        	gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }

    Fragment
    {
        In(0) vec2 v_TexCoord;

        Uniform(1) sampler2D u_Velocity;

        Out(0) vec4 oColor;

        vec3 HueToRgb(float hue)
        {
        	const vec3 k = abs(fract(vec3(hue) + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0;
        	return clamp(k, 0.0, 1.0);
        }

        void main()
        {
        	const ivec2 size     = textureSize(u_Velocity, 0);
        	const ivec2 texel    = clamp(ivec2(v_TexCoord * vec2(size)), ivec2(0), size - 1);
        	const vec2  ndc      = texelFetch(u_Velocity, texel, 0).xy;
        	const vec2  pixels   = ndc * 0.5 * vec2(size);
        	const float speed    = length(pixels);
        	const float strength = clamp(speed / 16.0, 0.0, 1.0);
        	const float hue      = atan(pixels.y, pixels.x) * (0.5 / 3.14159265) + 0.5;
        	oColor               = vec4(speed > 0.0 ? HueToRgb(hue) * strength : vec3(0.0), 1.0);
        }
    }
}
