// DesertAsset {"Kind":"Shader","Guid":"a2850960813b98e5c2392c10006b5704","Versions":{"SHDR":1},"Dependencies":[]}
// A TEST FIXTURE. This shader is MEANT NOT TO COMPILE: its fragment stage assigns the vec2 UV to a vec4
// albedo, which is what makes the engine's "no compiled stages" refusal a reachable, checked path
// (ShaderService.cpp, MaterialEditorPanel::PreviewUnavailableReason). ShaderCacheKey's
// TheBrokenShaderFixtureStillDoesNotCompile is the test that holds it — repairing this file turns that
// test RED, which is the point of keeping it rather than deleting it.
//
// WHY IT IS HERE AND NOT UNDER Editor/Resources/Shaders (Г20). It used to ship among the project's own
// shaders, so AssetPreloader compiled it at EVERY editor start and printed two errors into every clean
// log — the shaderc diagnostic and ShaderService's refusal. An error that is always there means
// nothing, and a real broken shader was indistinguishable from it. A fixture has to be reachable by a
// TEST, not by everyone who opens the editor; Editor/Resources/Shaders is the project's content, not a
// test corpus.
//
// IT WAS ALREADY A FOSSIL when it moved. Its header said "GENERATED ... edit the .dgraph", and
// Assets/ShaderGraphs/MatBroken.dgraph still holds the mistyped graph — but ShaderGraph.cpp's
// ValidateGraph (Г17) now REFUSES a type-mismatched link before a line is emitted, so today's editor
// cannot produce this file from that graph at all. Keep it by hand or not at all.
//
// IT CARRIES THE SHDR 1 HEADER LINE ABOVE (MIG1). Since T7j ShaderAsset::LoadFromFile refuses a headerless
// shader BEFORE compiling it, so a generation-0 fixture would reach that refusal instead of the "no compiled
// stages" state it stands for. It was raised by `scripts/Dev/migrate.sh --write`, like every shipped shader.
Shader "MatBroken"
{
    Domain Surface

    State
    {
        Cull Back
        ZTest LEqual
        ZWrite On
    }

    Vertex
    {
        #include <Common/GraphVertex.glslh>
    }

    Fragment
    {
        layout( location = 0 ) in vec2 v_UV;
        layout( location = 0 ) out vec4 o_Color;

        void main()
        {
            vec2 n0 = v_UV;
            vec4 albedo = n0;
            o_Color = vec4( albedo.rgb + ( vec4( 0.0 ) ).rgb, albedo.a * ( 1.0 ) );
        }
    }
}
