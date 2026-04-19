-- scripts/example.lua
-- Every .lua file under scripts/ is loaded once at startup. Define any of
-- the optional global functions below to react to game events. Remove or
-- rename this file to disable it; broken scripts log an error and are
-- skipped so they can't break the engine.
--
-- Event hooks fired by the engine:
--   OnInit()                 once, after all scripts have loaded
--   OnSpawn(ch)              when a dino / the player is (re)spawned
--   OnDamage(ch, amount)     when the player's shot reduces dino HP
--   OnFire(weaponIndex)      when the player successfully fires
--
-- `ch` is a read-only snapshot table:
--   ch.id      index into the Characters[] array (use with oc.* helpers)
--   ch.ctype   numeric creature-type id (stable within a mod)
--   ch.name    string name from _res.txt ("Trex", "Parasaur", ...)
--   ch.ai      AI behaviour enum
--   ch.health  current HP (post-damage for OnDamage)
--   ch.state   current state enum
--   ch.x, ch.y, ch.z, ch.alpha, ch.beta
--
-- Helpers in the `oc` global table:
--   oc.log(msg)              write to Log.txt
--   oc.message(msg)          show an in-game HUD message
--   oc.setHealth(id, hp)     write hp back (0 = kill)
--   oc.getHealth(id)         read hp; -1 if id invalid
--   oc.playerPos()           returns x, y, z
--   oc.dinoCount()           number of live entries in Characters[]
--   oc.getCharacter(id)      pushes the same snapshot table as the hooks

local shotCount = 0

function OnInit()
    oc.log("example.lua: hello from Lua " .. _VERSION)
end

function OnSpawn(ch)
    -- Announce big predators so the player knows to be careful.
    if ch.name == "Trex" or ch.name == "Ceratosaurus" then
        oc.message("A " .. ch.name .. " is nearby...")
    end
end

function OnDamage(ch, amount)
    -- Headshot-ish rewards: large damage chunks get a HUD shout.
    if amount >= 40 and ch.health > 0 then
        oc.message(string.format("Solid hit on %s (-%d HP)", ch.name or "target", amount))
    end
end

function OnFire(weapon)
    shotCount = shotCount + 1
    if shotCount % 10 == 0 then
        oc.log(string.format("example.lua: %d shots fired", shotCount))
    end
end
