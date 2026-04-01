# qxtable
A lua cross-thread excel config library from quick engine

✨ **Core Features**  
- **Cross-thread safety** 
- **82% reduction in memory usage**
- **Support for incremental hot updates, 99% memory reduction**
- **QQ Group**: 1075858599
  
# Cases:
```lua 
-- Resource Management Service (loads configurations)
local excels = {
    monster = {
        [101] = {id = 101, name = "Slime",  atk = 10, def = 5,  maxhp = 10000},
        [102] = {id = 102, name = "Dragon", atk = 50, def = 20, maxhp = 50000},
    }
}
qxtable.update(excels)

-- Worker Services (access configurations)
qxtable.reload()  -- Refresh to latest configuration
local monster = qxtable.find("monster")
assert(monster)

-- Access configuration data
local data = monster[101]
print(data.name, data.atk, data.maxhp)

-- Iterate through all monsters
for id, data in pairs(monster) do
    print(string.format("Monster %s: ATK=%d, HP=%d", data.name, data.atk, data.maxhp))
end

```
