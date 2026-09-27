-- Fills two collections a UIListView is bound to (UIL1): an inventory of ten thousand records and a chat
-- that grows while the scene plays. Nothing here touches an element — a list names its collection, its one
-- child is the entry template, and the template's UI Bindings name the record's fields.
--
--   ui.list_add( key, { field = value, ... } )   -- append a record (colours are { r, g, b })
--   ui.list_set( key, index, field, value )      -- change one field of one record (1-based index)
--   ui.list_insert / ui.list_remove / ui.list_clear / ui.list_count

Properties = {
    InventorySize = 10000,
    ChatEvery     = 0.25, -- seconds between chat lines while playing
}

local names  = { "Iron Ore", "Healing Draught", "Arrow", "Copper Coin", "Rope", "Lantern Oil", "Bread", "Map Scrap" }
local people = { "Ann", "Bo", "Cyra", "Dmitri", "Eun" }
local lines  = { "anyone near the oasis?", "selling rope", "gg", "need a healer", "dune worm at the north gate" }
local t      = 0.0
local next   = 0.0
local said   = 0

function OnStart()
    ui.list_clear( "probe.inventory" )
    for i = 1, Properties.InventorySize do
        local shade = 0.55 + 0.35 * ( ( i * 37 ) % 11 ) / 10
        ui.list_add( "probe.inventory", {
            name  = string.format( "%s #%d", names[( i - 1 ) % #names + 1], i ),
            count = ( i * 13 ) % 250,
            tint  = { shade, shade * 0.95, 0.8 },
        } )
    end
    ui.set( "probe.inventory.caption", string.format( "%d records", ui.list_count( "probe.inventory" ) ) )
    ui.list_clear( "probe.chat" )
    for _ = 1, 12 do
        Say()
    end
end

function Say()
    said = said + 1
    ui.list_add( "probe.chat", { from = people[said % #people + 1], line = string.format( "%s (%d)", lines[said % #lines + 1], said ) } )
end

function OnUpdate( dt )
    t = t + dt
    if t >= next then
        next = t + Properties.ChatEvery
        Say()
        -- One record of the inventory changes every tick too: only its row's text moves on screen.
        ui.list_set( "probe.inventory", 3, "count", said )
    end
end
