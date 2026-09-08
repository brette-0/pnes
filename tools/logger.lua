-- logger.lua -- fixed, reusable Mesen script. Not regenerated per build; the
-- host-side `logger` tool (see tools/logger/, once written) regenerates
-- logdata.lua next to this file after every NES build instead.
--
-- Loads logdata.lua's array of { lma = <flat PRG-ROM byte offset>, message =
-- <string> } entries -- LMA meaning the physical, bank-switching-agnostic
-- position of that log point in the PRG-ROM image (see this project's design
-- notes on why a plain CPU address can't identify a log point once a mapper
-- is involved) -- and fires the matching message whenever the CPU actually
-- executes the instruction currently sitting at a logged LMA.
--
-- log() call sites (include/platform-nes/logger.hpp) cost zero ROM bytes and
-- zero CPU cycles on real hardware -- this script is the only place they ever
-- have any effect at all, and only here, inside Mesen.
--
-- Requires the script's "Allow access to I/O and OS functions" option enabled
-- (dofile needs it) -- Mesen resolves relative paths against this script's
-- own directory.

-- The hashtable onExec actually checks against, keyed by LMA.
local logByLma = {}

-- dofile is gated behind the script's "Allow access to I/O and OS functions"
-- option -- there's no separate API to ask whether that's enabled, so the
-- only way to find out is to actually try it. Mesen doesn't reload a running
-- script when that option is toggled, but the ROM/logdata.lua can also
-- legitimately not exist yet (a build in progress) -- either way, retrying
-- once a second on startFrame until it succeeds covers both cases without
-- spamming dofile every single frame.
local RETRY_INTERVAL_FRAMES = 60
local framesUntilRetry = 0
local retryCallbackRef = nil

-- logdata.lua lives next to the running ROM (this project's NES build writes
-- demo.nes and logdata.lua to the same output directory), not necessarily
-- next to this script -- derive its path from the ROM's own path rather than
-- assuming a fixed location.
local function logDataPath()
    local romPath = emu.getRomInfo().path
    local romDir = romPath:match("^(.*[/\\])") or ""
    return romDir .. "logdata.lua"
end

local function tryLoadLogData()
    local ok, entries = pcall(dofile, logDataPath())
    if not ok then
        return false, entries -- entries is the pcall error message here
    end
    if type(entries) ~= "table" then
        return false, "logdata.lua did not return a table"
    end
    for _, entry in ipairs(entries) do
        logByLma[entry.lma] = entry
    end
    emu.log("app: found " .. logDataPath() .. " -- " .. #entries .. " log point(s) active.")

    -- MesenCE only loads a script once the ROM is already running, so any log
    -- point hit before this point is unavoidably missed -- reset once, so the
    -- run the user actually watches starts from power-on with logging already
    -- live. emu.reset() (like emu.rewind()) can only be called from inside a
    -- callback, not from this function's own top-level/retry-callback call
    -- site directly, so it's deferred one startFrame -- confirmed the hard
    -- way: calling it right here threw "This function cannot be called
    -- outside a callback" from Mesen.
    local resetRef
    resetRef = emu.addEventCallback(function()
        emu.removeEventCallback(resetRef, emu.eventType.startFrame)
        emu.reset()
    end, emu.eventType.startFrame)

    return true
end

local function onStartFrame()
    framesUntilRetry = framesUntilRetry - 1
    if framesUntilRetry > 0 then return end
    framesUntilRetry = RETRY_INTERVAL_FRAMES

    local loaded, err = tryLoadLogData()
    if loaded then
        emu.removeEventCallback(retryCallbackRef, emu.eventType.startFrame)
        retryCallbackRef = nil
    else
        -- logdata.lua is expected to exist by the time the ROM is running --
        -- it's a build output, not something that should ever be missing in
        -- normal use -- so keep saying so on every retry, not just the first.
        emu.log("app: still can't load " .. logDataPath() .. " -- " .. tostring(err)
            .. " (build the NES target first, or enable \"Allow access to I/O "
            .. "and OS functions\" for this script if that's what's blocking it)")
    end
end

local loaded, err = tryLoadLogData()
if not loaded then
    emu.log("app: " .. logDataPath() .. " not found -- " .. tostring(err)
        .. ". It should exist before the script runs (produced by the NES build); "
        .. "will keep retrying.")
    retryCallbackRef = emu.addEventCallback(onStartFrame, emu.eventType.startFrame)
end

-- CPU windows that can ever carry PRG-ROM under this project's mappers:
-- $6000-$7FFF (homebrew ROM-in-$6000, per this project's own mapper support),
-- then the four $2000 windows spanning $8000-$FFFF (switchable and/or fixed,
-- depending on the mapper/board). Each window is tracked independently since
-- a bank switch only ever changes one window's mapping at a time.
local WINDOW_BASE  = 0x6000
local WINDOW_COUNT = 5
local WINDOW_SIZE  = 0x2000
local WINDOW_END   = WINDOW_BASE + WINDOW_COUNT * WINDOW_SIZE - 1

-- windowLmaBase[i] is the flat PRG-ROM LMA currently mapped to window i's CPU
-- base address, or nil when that window isn't presently backed by PRG-ROM at
-- all (e.g. plain PRG-RAM sitting at $6000). Refreshed on writes rather than
-- on every instruction: bank switches happen via register writes, not via
-- ordinary execution, so the exec callback below never has to call into the
-- emulator itself -- just a table lookup.
local windowLmaBase = {}

local function refreshWindowMap()
    for i = 0, WINDOW_COUNT - 1 do
        local cpuAddr = WINDOW_BASE + i * WINDOW_SIZE
        local mapped = emu.convertAddress(cpuAddr, emu.memType.nesMemory, emu.cpuType.nes)
        if mapped and mapped.memType == emu.memType.nesPrgRom then
            windowLmaBase[i] = mapped.address
        else
            windowLmaBase[i] = nil
        end
    end
end

refreshWindowMap()

-- log()'s %-args (see logger.hpp) are never formatted on NES -- only an
-- address/size/signedness triple survives into logdata.lua. The actual
-- value only exists here, read live out of Mesen's own memory the instant
-- the log point fires, which is also the only way it CAN be read: NES RAM
-- isn't bank-switched the way the PRG-ROM code windows are (see
-- refreshWindowMap above), so entry.args[i].address is used directly, with
-- no LMA-style resolution needed.
local function readArgValue(arg)
    if arg.size == 1 then return emu.read(arg.address, emu.memType.nesMemory, arg.signed)
    elseif arg.size == 2 then return emu.read16(arg.address, emu.memType.nesMemory, arg.signed)
    else return emu.read32(arg.address, emu.memType.nesMemory, arg.signed)
    end
end

-- Replaces each %d/%u/%x/%X in order with the next arg's live value.
-- Anything else in the format string (including a stray leftover specifier
-- if fewer args were logged than specifiers appear) passes through as-is.
local function formatMessage(message, args)
    if not args or #args == 0 then return message end
    local i = 0
    return (message:gsub("%%[duxX]", function(spec)
        i = i + 1
        local arg = args[i]
        if not arg then return spec end
        local value = readArgValue(arg)
        if spec == "%x" then return string.format("%x", value) end
        if spec == "%X" then return string.format("%X", value) end
        return tostring(value)
    end))
end

local function onExec(address)
    local index  = math.floor((address - WINDOW_BASE) / WINDOW_SIZE)
    local base   = windowLmaBase[index]
    if not base then return end

    local lma = base + ((address - WINDOW_BASE) % WINDOW_SIZE)
    local entry = logByLma[lma]
    if entry then
        local message = formatMessage(entry.message, entry.args)
        emu.log("log: " .. message)
        emu.displayMessage("log", message)
    end
end

emu.addMemoryCallback(refreshWindowMap, emu.callbackType.write,
    WINDOW_BASE, WINDOW_END, emu.cpuType.nes, emu.memType.nesMemory)

emu.addMemoryCallback(onExec, emu.callbackType.exec,
    WINDOW_BASE, WINDOW_END, emu.cpuType.nes, emu.memType.nesMemory)
