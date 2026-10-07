// DesertAsset {"Kind":"Shader","Guid":"7abfca5b622c4e59b6862841e67c01c8","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:SubUVAnimation` (port of UE NiagaraStatelessModule_SubUVAnimation.ush:12-28 and its
// parameter build, NiagaraStatelessModule_SubUVAnimation.cpp:82-150): writes SubImageIndex, the float frame of a
// NumFrames-image sheet (the fraction blends two frames). Only the attribute — sampling the sheet is the sprite
// renderer's (VFX-08). Frames StartFrame..EndFrame, clamped to the sheet. Mode (UE ENSMSubUVAnimation_Mode):
//   0 DirectSet    one frame in the range, drawn once per particle (UE FrameIndex distribution; a fixed frame is
//                  StartFrame = EndFrame)
//   1 InfiniteLoop the range LoopsPerSecond times a second over the particle's age, wrapping
//   2 Linear       the range once over the particle's life
//   3 Random       a new frame in the range every RandomChangeInterval seconds of age (0: one frame for life)
// Update group.
Shader "VFX/Modules/SubUVAnimation"
{
    Domain Particle
    Particle
    {
        Attribute SubImageIndex float
        Attribute Age float
        Attribute Lifetime float
        Input Mode int
        Input NumFrames int
        Input StartFrame int
        Input EndFrame int
        Input LoopsPerSecond float
        Input RandomChangeInterval float

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            const int   frames = max( i.NumFrames, 1 );
            const float n      = float( frames );
            const float start  = float( clamp( i.StartFrame, 0, frames - 1 ) );
            const float range  = float( clamp( i.EndFrame, 0, frames - 1 ) ) - start;

            float index = start;
            if ( i.Mode == 1 )
                index = fract( ( start + p.Age * i.LoopsPerSecond * range ) / n ) * n;
            else if ( i.Mode == 2 )
                index = clamp( start + VFX_NormalizedAge( p.Age, p.Lifetime ) * range, 0.0, n );
            else if ( i.Mode == 0 || i.Mode == 3 )
            {
                const uint  epoch = i.Mode == 3 && i.RandomChangeInterval > 0.0 ? uint( p.Age / i.RandomChangeInterval ) : 0u;
                const float r     = VFX_ModuleRandomStable( sim, i.VFXModuleKey, 0u, epoch ).x;
                index             = start + min( floor( r * ( abs( range ) + 1.0 ) ), abs( range ) ) * sign( range );
            }
            p.SubImageIndex = index;
        }
    }
}
