// DesertAsset {"Kind":"Shader","Guid":"7af7111f31514a8aa10a02c2e777a591","Versions":{"SHDR":1},"Dependencies":[]}
// The Int/Bool row-parameter probe (Projects/Desert/Content/Scenes/MAT_ProbeIntParam.desce). An Int and a Bool
// in a Binding() row are declared `int` in the generated struct, so the row must hold int bytes
// (Core::Formats::MaterialRowSlotValue). Each Mode is its own flat colour and any other value is magenta, so a
// row that carried the float's bits (1.0 -> 1065353216) draws magenta for every Mode but 0.
Shader "MatProbeIntParam"
{
    Domain Surface

    Properties Binding(2)
    {
        Int   Mode   ("Mode", Range(0, 3)) = 2
        Bool  Darken ("Darken") = 0
    }

    State
    {
        Cull Back
        ZTest LEqual
        ZWrite On
    }

    ShadingModel Unlit

    Surface
    {
        SurfaceOutput EvaluateSurface( SurfaceInput i )
        {
            vec3 colour;
            switch ( Material.Mode )
            {
                case 0: colour = vec3( 0.9, 0.1, 0.1 ); break;
                case 1: colour = vec3( 0.1, 0.9, 0.1 ); break;
                case 2: colour = vec3( 0.1, 0.2, 0.95 ); break;
                case 3: colour = vec3( 0.95, 0.85, 0.1 ); break;
                default: colour = vec3( 1.0, 0.0, 1.0 ); break;
            }
            if ( Material.Darken != 0 )
                colour *= 0.25;
            SurfaceOutput s = DefaultSurfaceOutput();
            s.BaseColor = vec3( 0.0 );
            s.Emissive  = colour;
            s.Opacity   = 1.0;
            return s;
        }
    }
}
