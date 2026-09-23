-- Destroys its own entity after `Delay` seconds of gameplay time.
--
-- Written for PHYS_DestroyWitness.desce (WP6): the platform this sits on is destroyed before the box above
-- it lands, so the box must fall straight through to the floor. Before WP6 the platform's Jolt body
-- outlived its entity, and the box came to rest on nothing, in mid-air.

Properties = {
    Delay = 0.25, -- seconds; under `--play` dt is a fixed 1/60 s, so this is frame 15 in every run
}

local elapsed = 0.0

function OnUpdate(dt)
    elapsed = elapsed + dt
    if elapsed >= Properties.Delay then
        Log.info(self:name() .. ": destroying itself")
        self:destroy()
    end
end
