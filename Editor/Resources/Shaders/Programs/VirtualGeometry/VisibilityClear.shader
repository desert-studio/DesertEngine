// DesertAsset {"Kind":"Shader","Guid":"3c6a1e9d7b2f4a58a0d4e1f7c2b9a6e3","Versions":{"SHDR":1},"Dependencies":[]}
// "VirtualGeometry: Visibility Clear" (Engine/Graphic/VirtualGeometry/VisibilityBuffer.cpp; UE Nanite's
// InitVisBuffer clear): the view's visibility buffer (Common/VisibilityBuffer.glslh) emptied for this frame's rasters
// - its header written with the view's extent, every pixel's 64-bit word set to 0 (nothing drawn). Read here as
// 32-bit words (the same bytes: header = words 0..3, pixel p = words 4 + 2p, 4 + 2p + 1), so this program needs no
// 64-bit type. One thread per pixel.
Shader "VisibilityClear"
{
    Compute
    {
        LocalSize(64, 1, 1);

        Buffer(0) Visibility
        {
            uint Words[];
        };

        PushConstant PushConstants
        {
            uvec4 Extent; // x = width, y = height (pixels)
        };

        void main()
        {
            // Groups laid out 2D (a 4K view is past the 65535 groups one dimension guarantees): row y of groups
            // continues where row y - 1 ended (VisibilityBuffer.cpp kClearGroupsRow).
            const uint pixel  = gl_GlobalInvocationID.y * ( gl_NumWorkGroups.x * 64u ) + gl_GlobalInvocationID.x;
            const uint pixels = Extent.x * Extent.y;
            if ( pixel == 0u )
            {
                Words[0] = Extent.x;
                Words[1] = Extent.y;
                Words[2] = 0u;
                Words[3] = 0u;
            }
            if ( pixel >= pixels )
                return;
            Words[4u + 2u * pixel]      = 0u;
            Words[4u + 2u * pixel + 1u] = 0u;
        }
    }
}
