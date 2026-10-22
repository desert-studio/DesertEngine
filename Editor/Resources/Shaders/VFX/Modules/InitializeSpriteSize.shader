// DesertAsset {"Kind":"Shader","Guid":"55923c5734e04fc78e068ad051c4772c","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:InitializeSpriteSize` (UE Initialize Particle's Sprite Size): the newborn's sprite width
// and height in centimetres. A Random-source range is UE's size min/max (non-uniform; a uniform range is the same
// value in both), times a Spawn from Channel Size (1 when the emitter binds none). Kept as InitialSpriteSize, the
// base SizeOverLife scales. Spawn group.
Shader "InitializeSpriteSize"
{
    Domain Particle
    Particle
    {
        Usage ParticleSpawn
        Attribute SpriteSize vec2
        Attribute InitialSpriteSize vec2
        Input Size vec2 = 10 10

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            p.SpriteSize        = max( i.Size, vec2( 0.0 ) ) * sim.ChannelScalars.y;
            p.InitialSpriteSize = p.SpriteSize;
        }
    }
}
