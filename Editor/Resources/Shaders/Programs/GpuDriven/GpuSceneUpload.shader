// DesertAsset {"Kind":"Shader","Guid":"1ac9b4da6a70497493e46edec1e4a939","Versions":{"SHDR":1},"Dependencies":[]}
Shader "GpuSceneUpload"
{
    Compute
    {
        // "GPUScene: Upload <table>" (Engine/Graphic/GpuDriven/GpuScene.cpp, UE FGPUScene::Update's
        // FScatterUploadBuffer / ScatterCopyCS): copies the dirty rows of one of the scene's persistent tables
        // (Common/GpuScene.glslh rows, the bone palettes) from the frame's upload buffer into the table. One
        // invocation per 32-bit word of the rows it carries.
        LocalSize(64, 1, 1);

        // Words 0-3: row count N, words per row W, 1 = indexed (words 4..4+N name each row's destination row,
        // then the rows) / 0 = contiguous (row i lands at row i, the rows follow at word 4), unused.
        Buffer(0) GpuSceneUploadSource
        {
            uint Source[];
        };

        Buffer(1) GpuSceneUploadDestination
        {
            uint Destination[];
        };

        void main()
        {
            const uint word = (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * 64u +
                              gl_LocalInvocationIndex;
            const uint rows     = Source[0];
            const uint rowWords = Source[1];
            const bool indexed  = Source[2] != 0u;
            if (word >= rows * rowWords)
                return;
            const uint row      = word / rowWords;
            const uint column   = word - row * rowWords;
            const uint first    = 4u + (indexed ? rows : 0u);
            const uint target   = indexed ? Source[4u + row] : row;
            Destination[target * rowWords + column] = Source[first + word];
        }
    }
}
