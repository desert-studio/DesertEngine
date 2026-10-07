// DesertAsset {"Kind":"Shader","Guid":"55923c5734e04fc78e068ad051c4772c","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:InitializeSpriteSize` (UE Initialize Particle's Sprite Size): the newborn's sprite width
// and height in centimetres. A Random-source range is UE's size min/max (non-uniform; a uniform range is the same
// value in both). Kept as InitialSpriteSize, the base SizeOverLife scales. Spawn group.
Shader "InitializeSpriteSize"
{
    Domain Particle
    Particle
    {
        Attribute SpriteSize vec2
        Attribute InitialSpriteSize vec2
        Input Size vec2

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            p.SpriteSize        = max( i.Size, vec2( 0.0 ) );
            p.InitialSpriteSize = p.SpriteSize;
        }
    }
}
