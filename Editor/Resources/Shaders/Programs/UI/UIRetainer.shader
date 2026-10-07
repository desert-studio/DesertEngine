// DesertAsset {"Kind":"Shader","Guid":"db794aa4fdbe4ff2a7cd42c5241725f7","Versions":{"SHDR":1},"Dependencies":[]}
Shader "UIRetainer"
{
    // The composite of one retained UI layer (UE Retainer Box + its effect material). Render2D rendered the
    // element's subtree into u_Content (premultiplied: a transparent target with the UI's "over" blend) and,
    // when the element names a mask, the mask element's subtree into u_Mask over the same rectangle. This
    // pass shows the layer through every effect at once — the effect is data, not a pipeline per effect.
    //
    // The math is Engine/Graphic/Render2D/RetainerEffect.hpp line for line (hash constants included), so a
    // CPU suite can say what a pixel does.
    Vertex
    {
        In(0) vec2 a_Position;   // screen px (top-left origin)
        In(1) vec2 a_TexCoord;   // 0..1 across the retained rect
        In(2) vec4 a_Color;      // tint; alpha multiplies the layer

        Out(0) vec2 v_TexCoord;
        Out(1) vec4 v_Color;

        PushConstant constants
        {
            mat4 Projection;   // pixel -> clip, same as UI2D
            vec4 Rect;         // min.xy, max.xy, screen px
            vec4 Uv;           // xy = layer extent in target UV, zw = 1 / target size px
            vec4 Mask;         // x = mask on, y = invert, z = opacity, w = haze on
            vec4 Haze;         // x = amplitude px, y = cell px, z = cells per second, w = view time s
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

        Uniform(0) sampler2D u_Content;
        Uniform(1) sampler2D u_Mask;

        PushConstant constants
        {
            mat4 Projection;
            vec4 Rect;
            vec4 Uv;
            vec4 Mask;
            vec4 Haze;
        } m_PushConstants;

        Out(0) vec4 o_Color;

        float RetainerHash(int x, int y)
        {
            uint h = uint(x) * 374761393u + uint(y) * 668265263u;
            h      = (h ^ (h >> 13u)) * 1274126177u;
            h ^= h >> 16u;
            return float(h & 0x00FFFFFFu) / 16777216.0;
        }

        float RetainerNoise(vec2 p)
        {
            vec2  i = floor(p);
            vec2  f = p - i;
            vec2  u = f * f * (3.0 - 2.0 * f);
            int   x = int(i.x);
            int   y = int(i.y);
            float a = RetainerHash(x, y);
            float b = RetainerHash(x + 1, y);
            float c = RetainerHash(x, y + 1);
            float d = RetainerHash(x + 1, y + 1);
            return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
        }

        void main()
        {
            vec2 size    = m_PushConstants.Rect.zw - m_PushConstants.Rect.xy;
            vec2 localPx = v_TexCoord * size;

            // Heat haze: sample the layer from a noise-displaced pixel. The mask is NOT displaced — the dune
            // stays put while the air above it shimmers.
            vec2 offset = vec2(0.0);
            if (m_PushConstants.Mask.w > 0.5 && m_PushConstants.Haze.x > 0.0 && m_PushConstants.Haze.y > 0.0)
            {
                vec2 p = localPx / m_PushConstants.Haze.y + vec2(0.0, m_PushConstants.Haze.w * m_PushConstants.Haze.z);
                vec2 n = vec2(RetainerNoise(p), RetainerNoise(p + vec2(17.0, 5.0)));
                offset = (n * 2.0 - 1.0) * m_PushConstants.Haze.x;
            }

            // Clamped half a texel inside the layer's extent: the pooled target is larger than the layer, and
            // its unused part must never bleed in.
            vec2 halfTexel = 0.5 * m_PushConstants.Uv.zw;
            vec2 uvContent = clamp((localPx + offset) * m_PushConstants.Uv.zw, halfTexel, m_PushConstants.Uv.xy - halfTexel);
            vec4 texel     = texture(u_Content, uvContent);

            float coverage = 1.0;
            if (m_PushConstants.Mask.x > 0.5)
            {
                float m  = clamp(texture(u_Mask, localPx * m_PushConstants.Uv.zw).a, 0.0, 1.0);
                coverage = m_PushConstants.Mask.y > 0.5 ? 1.0 - m : m;
            }

            float alpha = texel.a * coverage * m_PushConstants.Mask.z * v_Color.a;
            if (alpha <= 0.0)
                discard;
            // Premultiplied layer -> the straight colour the UI blend expects.
            vec3 color = texel.rgb / max(texel.a, 1e-5);
            o_Color    = vec4(color * v_Color.rgb, alpha);
        }
    }
}
