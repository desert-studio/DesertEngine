// DesertAsset {"Kind":"Shader","Guid":"f9d55cda8f64564cf7de9a8707e23dca","Versions":{"SHDR":1},"Dependencies":[]}
Shader "UIGlass"
{
    // Frosted-glass UI rectangle: fills with the BLURRED scene behind it instead of a texture.
    //
    // The blurred copy is a mip pyramid built by BackdropBlurRenderer right before the UI phase — the UI
    // draws into the scene target, and a shader may not sample the attachment it writes, so it samples the
    // snapshot instead. Higher LOD = blurrier.
    //
    // The rounded-rect mask is an SDF here rather than tessellated corners: this pass already runs per
    // element (each rect carries its own push constants), and an SDF edge antialiases for free.
    Vertex
    {
        In(0) vec2 a_Position;   // pixel coordinates (top-left origin)
        In(1) vec2 a_TexCoord;
        In(2) vec4 a_Color;      // tint; alpha = how much the tint covers the blur

        Out(0) vec2 v_TexCoord;
        Out(1) vec4 v_Color;

        PushConstant constants
        {
            mat4 Projection;   // pixel -> clip, same as UI2D
            vec4 Rect;         // min.xy, max.xy in the rect's OWN space (px)
            vec4 Params;       // x = corner radius px, y = blur LOD, zw = 1 / viewport size
            vec4 InvRow0;      // xyz = row 0 of screen px -> own space; w = one screen px in own space
            vec4 InvRow1;      // xyz = row 1 of the same map
        } m_PushConstants;

        void main()
        {
            v_TexCoord  = a_TexCoord;
            v_Color     = a_Color;
            gl_Position = m_PushConstants.Projection * vec4(a_Position, 0.0, 1.0);
        }
    }

    Fragment
    {
        In(0) vec2 v_TexCoord;
        In(1) vec4 v_Color;

        Uniform(0) sampler2D u_Backdrop;

        PushConstant constants
        {
            mat4 Projection;
            vec4 Rect;
            vec4 Params;
            vec4 InvRow0;
            vec4 InvRow1;
        } m_PushConstants;

        Out(0) vec4 o_Color;

        // Signed distance to a rounded box centred on the origin. Negative inside.
        float RoundedBoxSDF(vec2 p, vec2 halfSize, float radius)
        {
            vec2 q = abs(p) - halfSize + radius;
            return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
        }

        void main()
        {
            // The backdrop is a snapshot of THIS target, so screen position maps to it directly.
            vec2 screenUV = gl_FragCoord.xy * m_PushConstants.Params.zw;
            vec3 blurred  = textureLod(u_Backdrop, screenUV, m_PushConstants.Params.y).rgb;

            // Tint over the blur: alpha 0 = pure blur, 1 = flat colour (a plain panel).
            vec3 color = mix(blurred, v_Color.rgb, v_Color.a);

            vec2  center   = (m_PushConstants.Rect.xy + m_PushConstants.Rect.zw) * 0.5;
            vec2  halfSize = (m_PushConstants.Rect.zw - m_PushConstants.Rect.xy) * 0.5;
            float radius   = min(m_PushConstants.Params.x, min(halfSize.x, halfSize.y));

            // The mask is evaluated in the RECT'S OWN space, not on screen. A UI element can be rotated
            // and scaled (UILayout Rotation/Scale), and every other primitive follows because its vertices
            // move; this one is an SDF over the fragment's position, so the fragment is what has to move.
            // For an untransformed panel InvRow0/InvRow1 are the identity rows, and 1*x + 0*y + 0 is x
            // exactly in IEEE-754 -- so this path is bit-for-bit the screen-space one it replaced.
            vec3 frag  = vec3(gl_FragCoord.xy, 1.0);
            vec2 local = vec2(dot(frag, m_PushConstants.InvRow0.xyz), dot(frag, m_PushConstants.InvRow1.xyz));

            float dist = RoundedBoxSDF(local - center, halfSize, radius);
            // One SCREEN pixel of feather, measured in that own space (InvRow0.w, exactly 1 untransformed)
            // so a scaled-up panel keeps a one-pixel edge instead of a scaled-up blur.
            float feather = 0.5 * m_PushConstants.InvRow0.w;
            float mask    = 1.0 - smoothstep(-feather, feather, dist);
            if (mask <= 0.0)
                discard;

            // Opaque inside the mask: the fill already CONTAINS what is behind it, so blending it again
            // would double-count the background.
            o_Color = vec4(color, mask);
        }
    }
}
