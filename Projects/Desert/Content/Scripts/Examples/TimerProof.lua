-- SCR-TIMERS (Scenarios/Gameplay/SCR-TIMERS.scn): the level script of TimerProof.desce. Sets one timer of each kind
-- the world's FTimerManager port offers (Timer.every / after / nextTick, pause / unpause, clear from its own
-- call, catch-up of a period shorter than a frame) and labels what each did over the picture, beside the game
-- time `t` the labels are read against.

local WHITE = vector.create(1, 1, 1)
local GREEN = vector.create(0.35, 1, 0.35)
local AMBER = vector.create(1, 0.8, 0.2)

local t = 0
local everyCalls, catchCalls, pausedCalls, clearCalls = 0, 0, 0, 0
local afterLine = "after(1.0): waiting"
local nextTickLine = "nextTick: waiting"
local clearLine = "every(0.2) clearing itself on its 3rd call: running"
local pauseLine = "every(0.1): paused at t = 1 s, unpaused at t = 2 s"
local everyHandle, catchHandle, pausedHandle, clearHandle

function OnStart()
    everyHandle = Timer.every(0.25, function() everyCalls = everyCalls + 1 end)
    -- 0.01 s is shorter than a frame: each tick calls it once per period missed (UE catch-up)
    catchHandle = Timer.every(0.01, function() catchCalls = catchCalls + 1 end)
    Timer.after(1.0, function() afterLine = string.format("after(1.0): fired once at t = %.2f s", t) end)
    Timer.nextTick(function() nextTickLine = string.format("nextTick: fired at t = %.3f s", t) end)
    pausedHandle = Timer.every(0.1, function() pausedCalls = pausedCalls + 1 end)
    Timer.after(1.0, function() Timer.pause(pausedHandle) end)
    Timer.after(2.0, function() Timer.unpause(pausedHandle) end)
    clearHandle = Timer.every(0.2, function()
        clearCalls = clearCalls + 1
        if clearCalls == 3 then
            Timer.clear(clearHandle)
            clearLine = string.format("every(0.2) cleared itself on call %d at t = %.2f s", clearCalls, t)
        end
    end, 0.0) -- firstDelay 0: the first call on the first tick
end

local function line(i, text, color)
    DebugDraw.text(vector.create(-380, 330 - i * 28, 0), text, color, 0, 1.2)
end

function OnUpdate(dt)
    t = t + dt
    line(0, string.format("t = %.2f s", t), WHITE)
    line(1, string.format("every(0.25): %d calls (t / 0.25 = %d), active %s, remaining %.2f s", everyCalls,
        math.floor(t / 0.25), tostring(Timer.isActive(everyHandle)), Timer.remaining(everyHandle)), GREEN)
    line(2, string.format("every(0.01): %d calls (t / 0.01 = %d), rate %.2f", catchCalls, math.floor(t / 0.01),
        Timer.rate(catchHandle)), GREEN)
    line(3, afterLine, AMBER)
    line(4, nextTickLine, AMBER)
    line(5, string.format("%s: %d calls, paused %s, remaining %.2f s", pauseLine, pausedCalls,
        tostring(Timer.isPaused(pausedHandle)), Timer.remaining(pausedHandle)), GREEN)
    line(6, string.format("%s (calls %d, exists %s)", clearLine, clearCalls, tostring(Timer.exists(clearHandle))),
        AMBER)
end
