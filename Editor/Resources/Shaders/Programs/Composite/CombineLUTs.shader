// DesertAsset {"Kind":"Shader","Guid":"afc48b14bdbe4c78a11490ca9d5476fa","Versions":{"SHDR":1},"Dependencies":[]}
Shader "CombineLUTs"
{
    Compute
    {
        // Ported from UE 5.8 Engine/Shaders/Private/PostProcessCombineLUTs.usf:480-494 (MainCS), adapted: one
        // dispatch of 4^3 groups per change of the grade (Graphic::System::CombineLUTsRenderer), the grade read from
        // one std430 struct instead of loose parameters.
        //
        // THE GRADE OF A VIEW, BAKED. Texel (r, g, b) of the 32^3 table holds FilmCombine of the exposed scene-linear
        // colour whose UE log encoding (FilmLinToLog) is (r, g, b) / 31: the white balance, gamut expansion, colour
        // correction, tone curve and sRGB output device of Common/FilmToneMap.glslh, evaluated once per change
        // instead of once per pixel. SceneComposite samples it trilinearly at FilmLinToLog(colour + LogToLin(0)).
        #include <Common/FilmToneMap.glslh>

        // rgba16f: the device colour is 0..1 sRGB-encoded; half carries it to a tenth of an 8-bit step.
        layout(binding = 0, rgba16f) restrict writeonly uniform image3D u_ColorGradingLUT;

        // The C++ side is Graphic::FilmGradeParams (Engine/Graphic/PostProcessing/FilmGrade.hpp), 25 vec4s.
        layout(std430, binding = 1) readonly buffer CombineLUTsBuffer
        {
            FilmGradeParams u_Grade;
        };

        LocalSize(4, 4, 4);

        void main()
        {
            ivec3 size  = imageSize(u_ColorGradingLUT);
            ivec3 texel = ivec3(gl_GlobalInvocationID);
            if (texel.x >= size.x || texel.y >= size.y || texel.z >= size.z)
                return;

            // UE: LUTEncodedColor = DispatchThreadId / (LUTSize - 1); LinearColor = LogToLin(enc) - LogToLin(0).
            vec3 encoded     = vec3(texel) / float(size.x - 1);
            vec3 linearColor = FilmLogToLin(encoded) - FilmLogToLin(vec3(0.0));

            imageStore(u_ColorGradingLUT, texel, vec4(FilmCombine(linearColor, u_Grade), 0.0));
        }
    }
}
