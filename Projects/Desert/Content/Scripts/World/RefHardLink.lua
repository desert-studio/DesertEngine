-- WP-ENTITY-REFS (Scenarios/World/WP-ENTITY-REFS.scn): a HARD entity reference (UE: a reference the actor's
-- descriptor lists in GetReferences, FActorClusterContext) - the owner and Part are one streaming cluster, so the
-- World Partition planner keeps them in one cell even when they stand on both sides of a cell border.

Properties = {
    Part = Property.Entity( { hard = true, tooltip = "Streams with this entity, always in the same cell." } ),
}
