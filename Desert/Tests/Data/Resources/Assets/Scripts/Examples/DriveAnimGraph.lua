-- Sets ONE AnimGraph parameter from gameplay code, which is the whole of T3.1: before it, nothing outside
-- the editor's own parameter sliders could move a state machine, so a graph in Play mode could only sit in
-- its entry state or auto-advance on exit time.
--
-- The graph this drives (ANIM_GraphScript.desce) has NO exit time on its only transition, so the pose in
-- the frame changes if and only if this line lands. `Param` is a property so the same script can serve as
-- its own negative control: point it at a name the graph does not declare and the engine must REFUSE,
-- name the parameter, list the ones that exist — and leave the machine where it was.

Properties = {
    Param = "Go",  -- the AnimGraph parameter to set (declared Bool in the witness scene)
    Delay = 0.5,   -- seconds of GAMEPLAY time to wait first, so the moment is the same in every run
}

local elapsed = 0.0
local sent    = false

function OnUpdate(dt)
    if sent then
        return
    end

    elapsed = elapsed + dt
    if elapsed < Properties.Delay then
        return
    end
    sent = true

    -- Returns false on every refusal (and logs which one), so a script CAN branch on it. Under `--play`
    -- dt is a fixed 1/60 s, which puts this call on frame 30 by construction rather than by luck.
    if self:setAnimParam(Properties.Param, true) then
        Log.info(self:name() .. ": set AnimGraph parameter '" .. Properties.Param .. "' = true")
    else
        Log.warn(self:name() .. ": AnimGraph parameter '" .. Properties.Param .. "' was refused")
    end
end
