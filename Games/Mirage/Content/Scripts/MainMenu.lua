-- Main menu: the title and the studio come from the project's settings (Mirage.deproj Name,
-- Config/Game.json Company) — no script or string table names the game.
function OnStart()
    ui.set( "game.title", string.upper( project.name() ) )
    ui.set( "game.studio", project.company() )
end

-- The Language tab's buttons raise language:<tag>; the canvas re-resolves every keyed label itself.
function OnUIMessage( msg )
    local tag = string.match( msg, "^language:(%a+)$" )
    if tag then
        loc.set_language( tag )
    end
end
