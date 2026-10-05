-- Drives the UI from gameplay WITHOUT touching a single widget.
--
-- The idea: a script writes DATA, and any element carrying a UI Binding component that names the same
-- key follows it. Nothing looks an element up, so renaming or restyling a widget can never break this
-- file — and the UI keeps working in the editor (an unset key falls back to the authored value).
--
-- Showcases the bridge:
--   ui.set( key, value )      -- number / string / bool, or ( key, r, g, b ) for a colour
--   ui.get( key ) / ui.has()  -- read it back
--   ui.send( msg )            -- raise a UI message yourself
--   OnUIMessage( msg )        -- hear every button action, pointer event and drop the canvas produced
--
-- and the localisation side, which is where a sentence that CONTAINS a number has to be built:
--   loc.plural( key, n )      -- the right form for n in the current language (CLDR, three in Russian)
--   loc.text( key, { ... } )  -- named arguments, and a gender when the language needs one
--   loc.money( amount, code ) -- the locale's layout, the currency's own digit count
--
-- Attach to any entity in the MainMenu scene (Details -> Add Component -> Script) and press Play:
-- the profile name, the server load bar and its caption start moving.

Properties = {
    PlayerName = "Nico_Bellic", -- written into player.name on start
    Latency    = 32.0,          -- ms, shown in the server caption
    Friends    = 12,            -- how many are online; drives the PLURAL form of the status line
    Level      = 34,            -- shown in the profile summary
    Balance    = 1248500.0,     -- shown as money, formatted for the current language
}

local t = 0.0

function OnStart()
    ui.set( "player.name", Properties.PlayerName )
    ui.set( "server.load", 0.85 )
    RefreshLocalisedText()
    log( "[UIDataBridge] bound keys: player.name, player.status, player.summary, server.load, server.caption" )
end

-- The two labels whose text is a SENTENCE WITH A NUMBER IN IT. They cannot be authored as a plain key,
-- because the form of the sentence depends on the number: "1 друг", "2 друга", "5 друзей" are three
-- different strings and only the count decides which one. So gameplay builds them and writes the result,
-- and the label's authored key ("#menu.status.offline") is what shows before this runs.
--
-- Call it again after a language change and the sentences follow; nothing else has to be touched.
function RefreshLocalisedText()
    ui.set( "player.status", loc.plural( "menu.status.friends", Properties.Friends ) )
    ui.set( "player.summary", loc.text( "menu.profile.summary", {
        level = Properties.Level,
        money = loc.money( Properties.Balance, "USD" ),
    } ) )
end

function OnUpdate( dt )
    t = t + dt

    -- A live server load: the bar and its caption read the SAME key, formatted differently.
    local load = 0.6 + 0.25 * math.sin( t * 0.8 )
    ui.set( "server.load", load )
    ui.set( "server.caption", string.format( "%d/1000    -    %d ms", math.floor( load * 1000 ),
                                             math.floor( Properties.Latency ) ) )
end

-- Every UI message lands here: button actions (including screen:Settings from a ShowScreen button),
-- pointer enter/exit, and drag-and-drop drops.
function OnUIMessage( msg )
    log( "[UIDataBridge] ui message: " .. msg )

    -- A language change is one call and one refresh: the canvas re-resolves every keyed label by itself
    -- on the next frame, and this line is only for the two sentences gameplay owns.
    if msg == "language:ru" or msg == "language:en" then
        if loc.set_language( string.sub( msg, 10 ) ) then
            RefreshLocalisedText()
        end
        return
    end

    if msg == "screen:Settings" then
        ui.set( "player.name", "…in settings" )
    elseif msg == "screen:back" then
        ui.set( "player.name", Properties.PlayerName )
    end
end
