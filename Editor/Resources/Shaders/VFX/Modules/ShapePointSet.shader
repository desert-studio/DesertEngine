// DesertAsset {"Kind":"Shader","Guid":"20893564213944cab1cf09dc53f7667e","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:ShapePointSet` (VFX-15, UE UNiagaraDataInterfaceArrayPosition read by a shape-location
// module: NiagaraDataInterfaceArrayFloat.h:55, PositionData :65): the particle is born at one point of a set
// painted onto the level in VFX Paint mode (VFXComponentData::PointSets). Location is bound to
// PointSet.<set>: per particle the compiler reads one row of the set's weight CDF at a uniform random index, so
// points come out in proportion to their weights; the row is the point relative to the placement, rotated and
// scaled by its transform. Bound to a Value instead, Location is a fixed offset. Centimetres. Spawn group.
Shader "ShapePointSet"
{
    Domain Particle
    Particle
    {
        Usage ParticleSpawn
        Attribute Position vec3
        Input Location vec3 = 0 0 0

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            p.Position += i.Location;
        }
    }
}
