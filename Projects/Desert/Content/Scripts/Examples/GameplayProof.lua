-- GAMEPLAY-PROOF (Scenarios/Gameplay/GAMEPLAY-PROOF.scn): the level script of GameplayProof.desce. It is the
-- player-facing half of UE's "Player Mappable Keys" settings screen for one action: once the player's mapping
-- context is active it maps Properties.Action from DefaultKey to NewKey (Input.rebindKey, UE MapPlayerKey), and
-- the first time the action starts it says which key started it and gives the default key back (a rebind to the
-- default key removes the user override, so the user's input.json is left as it was). It also labels what the
-- scenario's frames prove: the binding, the pawn's state (moving / in the air / crouched), the death and restart
-- (OnPawnDied / OnPlayerRestarted) and the door the button opens.

Properties = {
    Context = Property.String( "IMC_Default" ), -- the mapping context the action is mapped in
    Action = Property.String( "IA_Jump" ),       -- the remapped action
    DefaultKey = Property.String( "Space" ),     -- its key in the context asset
    NewKey = Property.String( "J" ),             -- the player's key while the proof runs
    Door = Property.String( "Door" ),            -- Tag of the door the button opens
}

local WHITE = vector.create( 1, 1, 1 )
local GREEN = vector.create( 0.35, 1, 0.35 )
local AMBER = vector.create( 1, 0.8, 0.2 )

local remapped = false
local bindingLine = "binding: waiting for the player's mapping context"
local deaths = 0
local restarts = 0
local lifeLine = ""
local door = nil
local doorClosedY = 0.0

function OnStart()
    door = World.find(Properties.Door)
    if door ~= nil then
        doorClosedY = door:getPosition().y
    end
end

local function TryRemap()
    local ok, err = pcall(Input.rebindKey, Properties.Context, Properties.Action, Properties.DefaultKey,
        Properties.NewKey)
    if not ok then
        return -- the pawn's context is not active yet (first frame of Play): next frame
    end
    remapped = true
    bindingLine = string.format("%s: %s -> %s (remapped)", Properties.Action, Properties.DefaultKey, Properties.NewKey)
    Log.info("[GAMEPLAY-PROOF] " .. bindingLine)
end

function OnUpdate(dt)
    if not remapped and bindingLine:sub(1, 8) == "binding:" then
        TryRemap()
    elseif remapped and Input.actionStarted(Properties.Action) then
        local byNew = Input.isKeyDown(Properties.NewKey)
        local byDefault = Input.isKeyDown(Properties.DefaultKey)
        bindingLine = string.format("%s started by %s (new key %s, default %s %s)", Properties.Action,
            byNew and Properties.NewKey or "?", byNew and "down" or "up", Properties.DefaultKey,
            byDefault and "down" or "up")
        Log.info("[GAMEPLAY-PROOF] " .. bindingLine)
        Input.rebindKey(Properties.Context, Properties.Action, Properties.DefaultKey, Properties.DefaultKey)
        remapped = false
    end

    local pawn = gameMode.pawn()
    if pawn ~= nil then
        local p = pawn:getPosition()
        local state = string.format("pawn (%.0f, %.0f, %.0f) %s%s", p.x, p.y, p.z,
            pawn:isOnGround() and "on ground" or "IN THE AIR", pawn:isCrouched() and ", CROUCHED" or "")
        DebugDraw.text(p + vector.create(0, 150, 0), bindingLine, GREEN, 0, 1.2)
        DebugDraw.text(p + vector.create(0, 130, 0), state, WHITE, 0, 1.2)
        if lifeLine ~= "" then
            DebugDraw.text(p + vector.create(0, 110, 0), lifeLine, AMBER, 0, 1.2)
        end
    end
    if door ~= nil then
        local d = door:getPosition()
        local open = d.y - doorClosedY
        DebugDraw.text(d + vector.create(0, 180, 0), string.format("door %s (+%.0f cm)",
            open > 1 and "OPEN" or "closed", open), AMBER, 0, 1.2)
    end
end

function OnPawnDied(pawn)
    deaths = deaths + 1
    local p = pawn:getPosition()
    lifeLine = string.format("died at (%.0f, %.0f, %.0f), deaths %d", p.x, p.y, p.z, deaths)
    Log.info("[GAMEPLAY-PROOF] " .. lifeLine)
end

function OnPlayerRestarted(pawn)
    restarts = restarts + 1
    local p = pawn:getPosition()
    lifeLine = string.format("restarted at PlayerStart (%.0f, %.0f, %.0f), deaths %d, restarts %d", p.x, p.y, p.z,
        deaths, restarts)
    Log.info("[GAMEPLAY-PROOF] " .. lifeLine)
end
