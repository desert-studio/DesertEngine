-- REFERENCE first/third-person player controller — the whole gameplay POLICY lives here, in Lua.
-- The engine only executes physics: it owns the Jolt capsule and turns the intent this script sets
-- (self:move / :jump / :swim) into motion. WASD->move, mouse->look, water detection->swim are all
-- decisions, so they belong to the script, not the engine. This is the mechanism/behavior split.
--
-- Setup:
--   * Add a Character Controller component to the player entity, and attach this script.
--   * Parent a child entity with a Camera component (behind = 3rd person, at the head = 1st person).
--   * Optional swimming: somewhere set the water height once, e.g. in a level script:
--         World.set("waterLevel", 2.0)
--     There is NO hardcoded water in the engine — it's just a blackboard variable this script reads.

Properties = {
    WalkSpeed  = 4.0,   -- m/s
    RunSpeed   = 7.0,   -- m/s while holding Shift
    JumpHeight = 5.0,   -- jump launch velocity
    LookSens   = 0.0025,-- radians per mouse-pixel
    SwimSpeed  = 3.0,   -- m/s while in water
}

function OnStart()
    if not self:hasComponent("CharacterController") then
        Log.warn(self:name() .. ": PlayerController needs a Character Controller component")
    end
    Input.lockCursor() -- capture the mouse for look (Escape toggles it back)
end

function OnUpdate(dt)
    -- Look: mouse turns the body (yaw) and tilts the child camera (pitch, clamped in the engine).
    local mdx, mdy = Input.mouseDelta()
    self:addYaw(-mdx * Properties.LookSens)
    self:addCameraPitch(-mdy * Properties.LookSens)

    -- Movement intent from WASD (each axis -1..1).
    local fwd   = (Input.isKeyDown("W") and 1 or 0) - (Input.isKeyDown("S") and 1 or 0)
    local right = (Input.isKeyDown("D") and 1 or 0) - (Input.isKeyDown("A") and 1 or 0)

    -- Swim POLICY (not in the engine): are we below the (user-defined) water surface?
    local swimming = false
    if World.has("waterLevel") then
        local _, y = self:getPosition()
        swimming = y < World.get("waterLevel")
    end
    self:setSwimming(swimming)

    if swimming then
        self:move(fwd, right, Properties.SwimSpeed)
        -- Space rises, Ctrl sinks (+1 up / -1 down).
        local vertical = (Input.isKeyDown("Space") and 1 or 0) - (Input.isKeyDown("Ctrl") and 1 or 0)
        self:swim(vertical)
    else
        local speed = Input.isKeyDown("Shift") and Properties.RunSpeed or Properties.WalkSpeed
        self:move(fwd, right, speed)
        if Input.wasPressed("Space") and self:isOnGround() then
            self:jump(Properties.JumpHeight)
        end
    end
end
