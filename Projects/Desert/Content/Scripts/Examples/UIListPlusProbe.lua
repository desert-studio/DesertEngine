-- Fills the collections of the UI-LIST+ probe: a tile view (inventory), a tree view (categories whose
-- records name the collection of their children) and a dropdown whose options are a collection.
--
--   ui.list_add( key, { field = value, ... } )   -- append a record (colours are { r, g, b })
--
-- A tree record's `children` field is the KEY of another collection; only expanded rows' children are read.

Properties = {
    InventorySize = 300,
}

local names = { "Iron Ore", "Draught", "Arrow", "Coin", "Rope", "Oil", "Bread", "Map Scrap" }

local tree = {
    { from = "Weapons", items = { "Dagger", "Scimitar", "Bow", "Sling" } },
    { from = "Armour", items = { "Turban", "Robe", "Boots" } },
    { from = "Food", items = { "Dates", "Bread", "Water skin", "Figs", "Salt" } },
    { from = "Quests", items = { "Find the oasis", "Escort the caravan" } },
}

function OnStart()
    ui.list_clear( "probe.inventory" )
    for i = 1, Properties.InventorySize do
        local shade = 0.35 + 0.25 * ( ( i * 37 ) % 11 ) / 10
        ui.list_add( "probe.inventory", {
            name  = string.format( "%s %d", names[( i - 1 ) % #names + 1], i ),
            count = ( i * 13 ) % 99,
            tint  = { shade, shade * 0.9, 0.55 },
        } )
    end
    ui.set( "probe.inventory.caption", string.format( "%d items, tile view", ui.list_count( "probe.inventory" ) ) )

    ui.list_clear( "probe.tree" )
    for c, cat in ipairs( tree ) do
        local key = string.format( "probe.tree.%d", c )
        ui.list_clear( key )
        for n, item in ipairs( cat.items ) do
            -- One level deeper under the first item of each category.
            local sub = ""
            if n == 1 then
                sub = key .. ".1"
                ui.list_clear( sub )
                ui.list_add( sub, { from = "", line = item .. " (rare)" } )
                ui.list_add( sub, { from = "", line = item .. " (common)" } )
            end
            ui.list_add( key, { from = "", line = item, children = sub } )
        end
        ui.list_add( "probe.tree", { from = cat.from, line = string.format( "%d entries", #cat.items ), children = key } )
    end

    ui.list_clear( "probe.servers" )
    for _, s in ipairs( { "Oasis RP #1", "Dune Roleplay", "Sandstorm City", "Caravan Road" } ) do
        ui.list_add( "probe.servers", { label = s } )
    end
end
