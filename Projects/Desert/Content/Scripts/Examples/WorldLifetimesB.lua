-- The world WL_UiB of Scenarios/Gameplay/WORLD-LIFETIMES.scn: it writes no "probe.value", so a letter there leaked.

function OnUIMessage( msg )
    ui.set( "probe.message", msg )
end
