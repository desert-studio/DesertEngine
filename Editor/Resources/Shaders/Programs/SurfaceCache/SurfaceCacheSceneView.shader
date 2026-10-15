// DesertAsset {"Kind":"Shader","Guid":"0a23be01087042de982924054b779f98","Versions":{"SHDR":1},"Dependencies":[]}
// The "Lumen Scene" view mode (DeferredDebugMode::SurfaceCacheScene; UE r.Lumen.Visualize "Lumen Scene"): the
// world as the surface cache holds it - nothing but the cards. Every captured card draws its front quad (the
// card plane, facing its direction); each fragment reads the card's texel through the page table, discards where
// the capture saw no surface, and moves its depth back to the captured surface (the card's depth layer), so the
// cards depth-test into the shape they captured; the colour is the card's albedo. Where the cards miss a surface
// (a hole, a too-thin card) the background shows - which is the point of the view.
Shader "SurfaceCacheSceneView"
{
    Vertex
    {
        #include <Common/SurfaceCache.glslh>
        ReadBuffer(0) SurfaceCardBuffer
        {
            SurfaceCard surfaceCards[];
        };
        PushConstant SceneViewPush
        {
            mat4  u_ViewProjection;
            vec4  u_CameraPosition;
            uvec4 u_Counts; // x = atlas side, y = card count
        };
        Out(0) vec2 v_Local;                                    // card space, [-1, 1]² over the card
        Out(1) vec4 v_Clip;                                     // clip position on the card plane
        layout(location = 2) out flat vec4 v_ClipDepthStep;      // clip offset from the plane to the far side
        layout(location = 3) out flat uvec4 v_CardInfo;          // res x | y << 16, pages x | y << 16, first page

        void main()
        {
            const SurfaceCard card    = surfaceCards[gl_InstanceIndex];
            const vec2        corners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                                   vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
            const vec2 c      = corners[gl_VertexIndex];
            const vec3 x      = card.worldToLocal[0].xyz;
            const vec3 y      = card.worldToLocal[1].xyz;
            const vec3 z      = card.worldToLocal[2].xyz;
            const vec3 origin = vec3(card.worldToLocal[0].w, card.worldToLocal[1].w, card.worldToLocal[2].w);
            const vec3 extent = card.localExtent.xyz;
            const vec3 world  = origin + x * (c.x * extent.x) + y * (c.y * extent.y) + z * extent.z;
            const bool facing = dot(z, u_CameraPosition.xyz - world) > 0.0;

            v_Local         = c;
            v_Clip          = u_ViewProjection * vec4(world, 1.0);
            v_ClipDepthStep = u_ViewProjection * vec4(z * (2.0 * extent.z), 0.0);
            const uvec2 res = uvec2(1u << (card.info.x & 0xFFu), 1u << ((card.info.x >> 8u) & 0xFFu));
            v_CardInfo      = uvec4(res.x | (res.y << 16u), card.pages.x, card.pages.y, 0u);
            // A card not captured yet, or seen from behind, collapses (all six vertices outside the clip box).
            gl_Position = SurfaceCardVisible(card) && facing ? v_Clip : vec4(0.0, 0.0, -2.0, 1.0);
        }
    }
    Fragment
    {
        #include <Common/SurfaceCache.glslh>
        ReadBuffer(1) SurfaceCardPageBuffer
        {
            SurfaceCardPage surfaceCardPages[];
        };
        Uniform(2) sampler2D u_AtlasAlbedo;
        Uniform(3) sampler2D u_AtlasDepth;
        Uniform(4) sampler2D u_AtlasOpacity;
        In(0) vec2 v_Local;
        In(1) vec4 v_Clip;
        layout(location = 2) in flat vec4 v_ClipDepthStep;
        layout(location = 3) in flat uvec4 v_CardInfo;
        Out(0) vec4 oColor;

        void main()
        {
            const uvec2 res   = uvec2(v_CardInfo.x & 0xFFFFu, v_CardInfo.x >> 16u);
            const uvec2 pages = uvec2(v_CardInfo.y & 0xFFFFu, v_CardInfo.y >> 16u);
            // Card UV: u along the card's X, v down its Y (the capture's rows run top-down).
            const vec2  uv    = vec2(0.5 + 0.5 * v_Local.x, 0.5 - 0.5 * v_Local.y);
            const uvec2 texel = min(uvec2(uv * vec2(res)), res - 1u);
            // The page holding the texel: pages of a side at or above a physical page are strided by the virtual
            // page size; a side below one is a single sub-allocated element.
            uvec2 page = uvec2(0u);
            if (res.x >= SURFACE_CACHE_PHYSICAL_PAGE_SIZE)
                page.x = min(texel.x / SURFACE_CACHE_VIRTUAL_PAGE_SIZE, pages.x - 1u);
            if (res.y >= SURFACE_CACHE_PHYSICAL_PAGE_SIZE)
                page.y = min(texel.y / SURFACE_CACHE_VIRTUAL_PAGE_SIZE, pages.y - 1u);
            const SurfaceCardPage entry = surfaceCardPages[v_CardInfo.z + page.y * pages.x + page.x];
            const vec2  atlasSize  = vec2(textureSize(u_AtlasAlbedo, 0));
            const ivec2 atlasTexel = ivec2(entry.physicalAtlasUVRect.xy * atlasSize + 0.5) +
                                     ivec2(texel - page * SURFACE_CACHE_VIRTUAL_PAGE_SIZE);
            if (texelFetch(u_AtlasOpacity, atlasTexel, 0).r < 0.5)
                discard;
            // Back to the captured surface: the depth layer runs from the card plane (0) to the far side (1).
            const vec4 clip = v_Clip - texelFetch(u_AtlasDepth, atlasTexel, 0).r * v_ClipDepthStep;
            gl_FragDepth    = clip.z / clip.w;
            oColor          = vec4(texelFetch(u_AtlasAlbedo, atlasTexel, 0).rgb, 1.0);
        }
    }
}
