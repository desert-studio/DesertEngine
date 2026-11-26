-- The world WL_UiA of Scenarios/Gameplay/WORLD-LIFETIMES.scn: what it writes must never show in another world.

function OnStart()
    ui.set( "probe.value", "A" )
    -- With document B active: A's travel must load into A (the first session of the scenario stops before this).
    Timer.after( 12.0, function() level.open( "Content/Scenes/WL_UiB.desce" ) end )
end

function OnUIMessage( msg )
    ui.set( "probe.message", msg )
    if msg == "toast" then
        ui.toast( "Toasts", "toast of A" )
    end
end
