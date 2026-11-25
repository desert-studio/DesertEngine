-- The world WL_UiA of Scenarios/Gameplay/WORLD-LIFETIMES.scn: what it writes must never show in another world.

function OnStart()
    ui.set( "probe.value", "A" )
end

function OnUIMessage( msg )
    ui.set( "probe.message", msg )
    if msg == "toast" then
        ui.toast( "Toasts", "toast of A" )
    end
end
