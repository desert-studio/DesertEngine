-- WP-ENTITY-REFS (Scenarios/World/WP-ENTITY-REFS.scn): a turret that tracks an entity of ANOTHER World Partition cell
-- through a soft entity reference (UE TSoftObjectPtr<AActor>): while the target's cell is unloaded the property reads
-- nil and the turret holds its pose; when the cell streams in, WorldStreamer rebinds the property and it turns.

Properties = {
    Target = Property.Entity( { tooltip = "What the turret tracks." } ),
}

function OnUpdate( dt )
    local target = Properties.Target
    if target == nil or not target:valid() then
        return
    end
    local from = self:getPosition()
    local to   = target:getPosition()
    -- Yaw about world +Y toward the target; the entity's rotation is euler radians (EntityLibrary.hpp).
    local yaw = math.atan2( to.x - from.x, to.z - from.z )
    self:setRotation( vector.create( 0, yaw, 0 ) )
end
