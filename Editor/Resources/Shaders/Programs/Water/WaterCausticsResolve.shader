// DesertAsset {"Kind":"Shader","Guid":"87549a76d4fc4ef38a0cd750e9f1d6fe","Versions":{"SHDR":1},"Dependencies":[]}
Shader "WaterCausticsResolve"
{
    Compute
    {
        // THE CAUSTICS' SECOND HALF (WATER-CAUSTICS, Common/WaterCaustics.glslh): WaterCausticsSplat's landed
        // light counts -> the map, in units of still-water irradiance (1 = still water), through a 3x3 tent
        // (the splat's bilinear footprint is one texel; the tent takes the last of its grid's aliasing off the
        // lines). Read by the deferred direct pass (WaterCausticsFactor).
        LocalSize(8, 8, 1);

        #include <Common/WaterCaustics.glslh>

        Buffer(0) WaterCausticsEnergy
        {
            uint Energy[WATER_CAUSTICS_SIZE * WATER_CAUSTICS_SIZE];
        };
        layout(binding = 1, r32f) writeonly uniform image2D WaterCausticsImage;

        float Landed( ivec2 t )
        {
            const ivec2 c = clamp( t, ivec2( 0 ), ivec2( WATER_CAUSTICS_SIZE - 1 ) );
            return float( Energy[c.y * WATER_CAUSTICS_SIZE + c.x] ) * ( 1.0 / WATER_CAUSTICS_ENERGY_SCALE );
        }

        void main()
        {
            const ivec2 texel = ivec2( gl_GlobalInvocationID.xy );
            if ( any( greaterThanEqual( texel, ivec2( WATER_CAUSTICS_SIZE ) ) ) )
                return;
            float sum = 0.0;
            for ( int y = -1; y <= 1; ++y )
                for ( int x = -1; x <= 1; ++x )
                    sum += Landed( texel + ivec2( x, y ) ) * float( ( 2 - abs( x ) ) * ( 2 - abs( y ) ) );
            imageStore( WaterCausticsImage, texel, vec4( sum * ( 1.0 / 16.0 ), 0.0, 0.0, 0.0 ) );
        }
    }
}
