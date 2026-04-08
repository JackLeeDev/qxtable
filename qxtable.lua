local c = require "qxtable.core"

local rawset = rawset
local rawget = rawget
local pairs = pairs
local select = select
local assert = assert
local setmetatable = setmetatable
local c_index = c.index
local c_next = c.next
local c_len = c.len

local cache_config = {}
local meta = {}
local weak_meta = {__mode = "v"}

local _M = {}

local function wrap_root(name, ud)
    return setmetatable({__ud = assert(ud), __name = name}, meta)
end

local function wrap_table(ud)
    return setmetatable({__ud = assert(ud)}, meta)
end

local function qxtable_next(t, k)
    local nextk,nextv,nextud = c_next(t.__ud, k)
    if nextud then
        return nextk,t[nextk]
    else
        return nextk,nextv
    end
end

meta.__index = function(t, k)
    local v,ud = c_index(t.__ud, k)
    if ud then
        local value = wrap_table(ud)
        rawset(t, k, value)
        return value
    else
        return v
    end
end

meta.__newindex = function(t, k, v)
    error("Cannot modify a read-only table")
end

meta.__len = function(t)
    return c_len(t.__ud)
end

meta.__pairs = function(t)
    return qxtable_next,t,nil
end

meta.__ipairs = function(t)
    return qxtable_next,t,nil
end

for k,v in pairs(meta) do
    weak_meta[k] = v
end

function _M.reload()
    local ud_list = c.reload()
    for name,ud in pairs(ud_list) do
        local cache = cache_config[name]
        if not cache or cache.__ud ~= ud then
            cache_config[name] = wrap_root(name, ud)
        end
    end
end

function _M.find(name, ...)
    local conf = cache_config[name]
    if not conf then
        return
    end
    local val = conf
    local n = select("#", ...)
    for i =1,n do
        val = val[select(i, ...)]
        if val == nil then
            return nil
        end
    end
    return val
end

function _M.update(confs)
    assert(type(confs) == "table")
    for name,conf in pairs(confs) do
        assert(type(name) == "string")
        assert(type(conf) == "table")
    end
    c.update(confs)
end

function _M.md5(t)
    local md5 = require "md5"
    return md5.sumhexa(c.tostring(t.__ud))
end

function _M.memory()
    return c.memory()
end

function _M.gc()
    for _,cache in pairs(cache_config) do
        setmetatable(cache, weak_meta)
    end
    collectgarbage("collect")
    for _,cache in pairs(cache_config) do
        setmetatable(cache, meta)
    end
end

return _M