-- A pressure button: while anything its trigger reports stands on it, the door named by Properties.Door slides
-- up by Properties.OpenHeight; when the last one steps off, the door slides back down. The button's Collider
-- is a trigger (Is Trigger, Overlap Characters only in the GP4 sample), so OnBeginOverlap / OnEndOverlap
-- arrive here. The door is moved through its transform; its Kinematic RigidBody follows that transform each
-- physics step, so it pushes and blocks while it moves.

Properties = {
    Door = "Door",      -- Tag of the entity to move
    OpenHeight = 250.0, -- cm the door rises when open
    Speed = 200.0,      -- cm/s
}

local door = nil
local closedY = 0.0
local inside = 0

function OnStart()
    door = World.find(Properties.Door)
    if door ~= nil and door:valid() then
        local _, y, _ = door:getPosition()
        closedY = y
    end
end

function OnBeginOverlap(other)
    inside = inside + 1
end

function OnEndOverlap(other)
    inside = math.max(inside - 1, 0)
end

function OnUpdate(dt)
    if door == nil or not door:valid() then
        return
    end
    local x, y, z = door:getPosition()
    local target = closedY
    if inside > 0 then
        target = closedY + Properties.OpenHeight
    end
    local step = Properties.Speed * dt
    if math.abs(target - y) <= step then
        y = target
    elseif target > y then
        y = y + step
    else
        y = y - step
    end
    door:setPosition(x, y, z)
end
