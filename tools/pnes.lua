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

-- The hashtable onExec actually checks against, keyed by LMA -- each value is
-- a LIST of entries, not a single entry: log()/pause() both cost zero real
-- instructions (see this file's own header comment), so two adjacent call
-- sites routinely resolve to the exact same "nearest real instruction" LMA
-- (confirmed on this project's own demo: a log() immediately followed by a
-- pause() on the very next line both resolve to the same address). A single
-- entry per LMA would let whichever entry loads last silently overwrite
-- (and permanently hide) every earlier one sharing that address.
local logByLma = {}

-- Populated from logdata.lua's `memory` table (see tools/luaLogBuilder/
-- main.cpp) once it loads -- nil until then, and permanently nil for a build
-- that has no stack/heap reservation to report.
local memoryLayout = nil

-- ---------------------------------------------------------------------
-- C-stack high-water mark + heap touch-bitfield tracking. Defined here,
-- ahead of tryLoadLogData below, because tryLoadLogData calls
-- startMemoryTracking the moment logdata.lua's `memory` table loads --
-- which can happen on this script's very first, synchronous attempt if
-- logdata.lua is already sitting there from an earlier build.
--
-- The C stack pointer, on every mos-platform target including this one, is
-- the 16-bit little-endian pair at CPU zero-page $00/$01 ("imaginary
-- register 0") -- confirmed against a real build's __do_init_stack, which
-- stores __stack's low/high bytes there before main() ever runs. It grows
-- DOWN from stackTop, same top-of-region convention as demo/link.ld's own
-- __stack comment.
--
-- Watching writes to $00/$01 (rather than sampling once a frame) catches a
-- genuine all-time-high the instant it happens, including a deep call chain
-- that both descends and returns within a single frame -- a once-per-frame
-- sample would silently miss that.
-- ---------------------------------------------------------------------

-- Returns a `reset` function: tryLoadLogData calls it right when its own
-- deferred emu.reset() fires, so whatever ran between Mesen attaching this
-- script and that reset (already-discarded, pre-power-on-observed activity)
-- doesn't inflate the high-water mark before real measurement even starts.
--
-- silentStackAmount (from logger.hpp/logger.cpp, baked into logdata.lua by
-- lua_log_builder -- same mechanism as the heap tracker's silentHeapAmount)
-- suppresses the "cstack: new high-water mark" line until depth exceeds it.
-- Defaults to 0xffff (see logger.cpp's own comment on why that, not the
-- region's own size), i.e. silent out of the box until a project overrides
-- it.
local function startStackTracking(stackTop, stackBottom, silentStackAmount)
    local totalBytes = stackTop - stackBottom + 1
    local maxDepth = 0

    local function onStackPointerWrite()
        local sp = emu.read16(0, emu.memType.nesMemory, false)
        -- $00/$01 is two independent 8-bit writes, so a low-byte store that
        -- carries into the high byte is briefly visible here as a bogus
        -- combined value (e.g. high byte not yet decremented) -- reject
        -- anything outside the known stack region rather than treat it as a
        -- real new low point.
        if sp > stackTop or sp < stackBottom then return end

        local depth = stackTop - sp
        if depth > maxDepth then
            maxDepth = depth
            if maxDepth > silentStackAmount then
                local pct = math.floor(100 * maxDepth / totalBytes)
                emu.log(string.format("cstack: new high-water mark -- %d/%d bytes (%d%%)",
                    maxDepth, totalBytes, pct))
            end
        end
    end

    emu.addMemoryCallback(onStackPointerWrite, emu.callbackType.write,
        0, 1, emu.cpuType.nes, emu.memType.nesMemory)

    return function() maxDepth = 0 end
end

-- ---------------------------------------------------------------------
-- Reserved-memory-overwrite guard: catches the stack settling outside its
-- own reserved region -- below stackBottom (overflowed DOWN into
-- .data/.bss/.noinit) or above stackTop (underflowed UP into the heap; this
-- project's link.ld pins __stack = __heap_start - 1, so stack and heap sit
-- back-to-back with no gap between them at all).
--
-- Deliberately NOT the same per-write callback the high-water-mark tracker
-- uses: $00/$01 is two independent 8-bit writes, so a multi-byte SP update
-- is often visible here mid-update (e.g. low byte stored, high byte not yet
-- adjusted) -- fine for "was this ever the new deepest point", useless for
-- "is this actually overflowing" without flagging that same harmless
-- transient constantly. Sampling once per (fully-settled-between-frames)
-- frame sidesteps that architecturally rather than trying to debounce it.
--
-- `hunks` -- named, contiguous regions of prg_ram, built once in
-- startMemoryTracking -- names whichever region a stray value has wandered
-- into. Deliberately doesn't attempt the reverse (heap writing into the
-- stack hunk): unlike the stack pointer, an ordinary heap write carries no
-- signal that lets it be told apart from a perfectly normal stack write to
-- the same address -- both are just "a byte got written here". Catching
-- that side for real would mean hooking malloc's own return pointer/
-- requested size, not just watching raw writes.
local function hunkNameFor(hunks, address)
    for _, hunk in ipairs(hunks) do
        if address <= hunk.upTo and (not hunk.start or address >= hunk.start) then
            return hunk.name
        end
    end
    return "unmapped memory"
end

local function startStackOverflowGuard(hunks, stackTop, stackBottom)
    local violated = false

    local function checkOnce()
        local sp = emu.read16(0, emu.memType.nesMemory, false)
        local isViolation = sp < stackBottom or sp > stackTop
        if isViolation and not violated then
            violated = true
            emu.log(string.format(
                "app: STACK OVERFLOW -- stack pointer settled at 0x%x, outside its reserved "
                .. "region [0x%x, 0x%x] -- now inside the %s region",
                sp, stackBottom, stackTop, hunkNameFor(hunks, sp)))
        elseif violated and not isViolation then
            violated = false
            emu.log(string.format("app: stack back within its reserved region (SP = 0x%x)", sp))
        end
    end

    emu.addEventCallback(checkOnce, emu.eventType.startFrame)

    return function() violated = false end
end

-- Heap: one boolean per reserved byte, set the first time that byte is ever
-- written. A bit staying set after malloc's free() reuses that byte is
-- intentional, not a bug -- this bitfield tracks cumulative footprint ever
-- touched (the real answer to "how big does the heap reservation need to
-- be"), not bytes live right now. Spotting live fragmentation would mean
-- walking the allocator's own free list instead; not attempted here.
--
-- BOUNDARY_TAG_BYTES excludes this platform's malloc's own bookkeeping from
-- both ends: confirmed by disassembling a real build's malloc -- its
-- one-time init writes a 2-byte header (size+flags) at the very start of the
-- initial free chunk, and FreeChunk::insert writes a matching 2-byte footer
-- (a duplicate of the chunk's size, for O(1) backward-coalescing on free) at
-- that chunk's very end. With one giant initial free chunk spanning the
-- whole arena, those land exactly at the heap's own first/last 2 bytes.
--
-- THIS IS ONLY EXACT FOR THAT INITIAL STATE. Every free chunk gets its own
-- header+footer, not just the first one -- once allocations/frees fragment
-- the heap into more than one free chunk, later headers/footers can land
-- anywhere inside the arena, not just these fixed 4 bytes, and this
-- correction will no longer fully account for them (it'll undercount total
-- overhead, not overcount). Treated as an acceptable approximation for now;
-- exact accounting would mean hooking malloc's own argument/return values
-- instead of just watching raw writes.
local BOUNDARY_TAG_BYTES = 2 -- at EACH end (header, footer)

-- Same 8-byte minimum this malloc clamps every chunk to (BOUNDARY_TAG_BYTES's
-- own comment cites the exact disassembly: d63e cpx #$7 / d640 bcs / d642 ldx
-- #$8) -- reused here as the frag report's granule, not just the boundary
-- trim above. A used/empty gap SHORTER than one full chunk-granule can never
-- be a real reusable hole: nothing this allocator ever hands out is smaller
-- than HEAP_GRANULE bytes, so a 1- or 2-byte gap is just quantization slop
-- inside/between chunks, not memory a future allocation could ever actually
-- land in. Reporting it as "fragmentation" alongside genuine reusable gaps
-- (a gap of a full chunk-or-more) overstates what's actually wrong.
local HEAP_GRANULE = 8

-- True if any byte in `touched`'s [start, stop] range has ever been written.
local function heapCellTouched(touched, start, stop)
    for i = start, stop do
        if touched[i] then return true end
    end
    return false
end

-- Run-length-encodes `touched` into alternating used/empty runs of whole
-- HEAP_GRANULE-byte cells -- a.k.a. qwords, this tracker's whole unit of
-- measurement from here on (see HEAP_GRANULE's own comment on why 8 bytes
-- is the natural unit: it's the smallest chunk this malloc ever hands out,
-- so anything finer than that can never be a real, independently-reusable
-- piece of heap). "u(5) e(11)" means 5 used qwords then 11 empty ones, NOT
-- bytes -- the very last qword is short (heapSize isn't always a multiple
-- of HEAP_GRANULE) but still counts as one whole qword here. A cell counts
-- as "used" the moment ANY one of its bytes has ever been touched, so this
-- reports real fragmentation (gaps of a full chunk-or-more between live
-- allocations) without also flagging the sub-chunk padding HEAP_GRANULE
-- itself creates -- see HEAP_GRANULE's own comment on why that padding was
-- never a real, reusable hole to begin with.
--
-- Also returns usedQwords -- how many qwords the ALLOCATOR has actually
-- committed to chunks, not how many your code happened to write into. This
-- is deliberately NOT the same as counting `touched` directly: a chunk's
-- un-written padding bytes (the reason HEAP_GRANULE exists at all) are
-- memory the allocator has reserved and can never hand to anyone else, so
-- the whole qword they sit in counts as "used" even though nothing ever
-- wrote every byte of it.
local function heapRunLengthEncoding(touched, heapSize)
    local parts = {}
    local usedRuns, usedQwords = 0, 0
    local runIsUsed, runLen = nil, 0
    local i = 1
    while i <= heapSize do
        local cellEnd = math.min(i + HEAP_GRANULE - 1, heapSize)
        local isUsed = heapCellTouched(touched, i, cellEnd)
        if isUsed then usedQwords = usedQwords + 1 end
        if runIsUsed == nil then
            runIsUsed, runLen = isUsed, 1
        elseif isUsed == runIsUsed then
            runLen = runLen + 1
        else
            if runIsUsed then usedRuns = usedRuns + 1 end
            parts[#parts + 1] = string.format("%s(%d)", runIsUsed and "u" or "e", runLen)
            runIsUsed, runLen = isUsed, 1
        end
        i = cellEnd + 1
    end
    if runIsUsed ~= nil then
        if runIsUsed then usedRuns = usedRuns + 1 end
        parts[#parts + 1] = string.format("%s(%d)", runIsUsed and "u" or "e", runLen)
    end
    return table.concat(parts, " "), usedRuns, usedQwords
end

-- Returns a `reset` function; see startStackTracking's comment above for why.
--
-- silentHeapAmount (from demo/link.ld, baked into logdata.lua by
-- lua_log_builder) suppresses the "app: heap usage"/"app: heap frag" lines
-- until cumulative usage exceeds it -- defaults to the entire reservation,
-- i.e. silent out of the box until a project deliberately lowers it. The
-- exhaustion warning ignores this: it's a hard error condition, not noise.
local function startHeapTracking(heapStart, heapSize, silentHeapAmount)
    -- Usable-heap window this tracker actually watches, with the initial
    -- chunk's header/footer (see BOUNDARY_TAG_BYTES's own comment) excluded
    -- from both ends. Falls back to the whole region, untrimmed, for a heap
    -- too small to have any room left after excluding both -- better to
    -- overcount usage on a tiny heap than watch a negative-length range.
    local usableStart = heapStart + BOUNDARY_TAG_BYTES
    local usableSize = heapSize - 2 * BOUNDARY_TAG_BYTES
    if usableSize <= 0 then
        usableStart = heapStart
        usableSize = heapSize
    end

    -- Everything from here on is in qwords (HEAP_GRANULE-byte units), not
    -- bytes -- see heapRunLengthEncoding's own comment on why. silentHeapAmount
    -- arrives from logdata.lua as a byte count (lua_log_builder's job is
    -- converting a project's override fraction to bytes, nothing more); it's
    -- converted to qwords exactly once, here, ceiling so a nonzero byte
    -- threshold never rounds down into an always-warn 0-qword one.
    local totalQwords = math.ceil(usableSize / HEAP_GRANULE)
    local silentQwords = math.ceil(silentHeapAmount / HEAP_GRANULE)

    local touched = {}
    -- Last-REPORTED qword usage, not a raw touched-byte count -- see
    -- heapRunLengthEncoding's own comment on usedQwords. Touching a second
    -- byte inside an already-committed qword changes nothing an
    -- allocator-level view cares about, so it must not re-print either.
    local lastReportedQwords = 0

    local function onHeapWrite(address)
        local offset = address - usableStart + 1
        if touched[offset] then return end
        touched[offset] = true

        local pattern, usedRuns, usedQwords = heapRunLengthEncoding(touched, usableSize)
        if usedQwords ~= lastReportedQwords then
            lastReportedQwords = usedQwords
            if usedQwords > silentQwords then
                local pct = math.floor(100 * usedQwords / totalQwords)
                emu.log(string.format("app: heap usage (%d/%d qwords) %d%%",
                    usedQwords, totalQwords, pct))
                if usedRuns > 1 then
                    emu.log("app: heap frag " .. pattern)
                end
            end

            if usedQwords == totalQwords then
                emu.log("app: heap exhausted -- every usable qword is now committed to some "
                    .. "chunk; grow __heap_default_limit (see demo/link.ld) or reduce allocations.")
            end
        end
    end

    emu.addMemoryCallback(onHeapWrite, emu.callbackType.write,
        usableStart, usableStart + usableSize - 1, emu.cpuType.nes, emu.memType.nesMemory)

    return function()
        touched = {}
        lastReportedQwords = 0
    end
end

-- Named, contiguous carve-up of prg_ram, in ascending-address order -- see
-- startStackOverflowGuard's own comment on why "static" only needs an upper
-- bound (nothing here ever checks whether a value has gone too far the OTHER
-- way, off the bottom of prg_ram entirely).
local function buildHunks(memory)
    return {
        { name = "static (.data/.bss/.noinit)", upTo = memory.stackBottom - 1 },
        { name = "stack", start = memory.stackBottom, upTo = memory.stackTop },
        { name = "heap", start = memory.heapStart, upTo = memory.heapStart + memory.heapSize - 1 },
    }
end

-- Returns a `reset` function combining all trackers', for tryLoadLogData to
-- call once its own deferred power-on reset actually fires.
local function startMemoryTracking(memory)
    emu.log(string.format("app: memory tracking active -- stack [0x%x, 0x%x] (%d bytes), heap [0x%x, 0x%x] (%d bytes)",
        memory.stackBottom, memory.stackTop, memory.stackTop - memory.stackBottom + 1,
        memory.heapStart, memory.heapStart + memory.heapSize - 1, memory.heapSize))
    local hunks = buildHunks(memory)
    -- memory.silentStackAmount can be absent from a logdata.lua generated by
    -- an older lua_log_builder -- fall back to the region's own total size
    -- (fully silent), same as memory.silentHeapAmount's own fallback below.
    local stackTotalBytes = memory.stackTop - memory.stackBottom + 1
    local resetStack = startStackTracking(memory.stackTop, memory.stackBottom,
        memory.silentStackAmount or stackTotalBytes)
    local resetOverflowGuard = startStackOverflowGuard(hunks, memory.stackTop, memory.stackBottom)
    -- memory.silentHeapAmount can be absent from a logdata.lua generated by
    -- an older lua_log_builder -- fall back to fully silent, same default
    -- the builder itself uses when the ELF has no silentHeapAmount symbol.
    local resetHeap = startHeapTracking(memory.heapStart, memory.heapSize,
        memory.silentHeapAmount or memory.heapSize)
    return function()
        resetStack()
        resetOverflowGuard()
        resetHeap()
    end
end

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
    local ok, root = pcall(dofile, logDataPath())
    if not ok then
        return false, root -- root is the pcall error message here
    end
    if type(root) ~= "table" or type(root.logs) ~= "table" then
        return false, "logdata.lua did not return { logs = {...} }"
    end
    for _, entry in ipairs(root.logs) do
        local bucket = logByLma[entry.lma]
        if not bucket then
            bucket = {}
            logByLma[entry.lma] = bucket
        end
        table.insert(bucket, entry)
    end
    -- Sort each address's bucket by source line: entries.lua_log_builder's
    -- emission order tracks ELF symbol-table order, not necessarily source
    -- order, but firing them in source order is what a reader actually
    -- expects when several call sites collapse onto one address (e.g. a
    -- log() immediately followed by a pause() -- the log's message should
    -- print before execution actually halts).
    for _, bucket in pairs(logByLma) do
        table.sort(bucket, function(a, b) return a.line < b.line end)
    end
    emu.log("app: found " .. logDataPath() .. " -- " .. #root.logs .. " log point(s) active.")

    local resetMemoryTracking = nil
    if root.memory then
        memoryLayout = root.memory
        resetMemoryTracking = startMemoryTracking(memoryLayout)
    else
        emu.log("app: logdata.lua has no `memory` table -- stack/heap tracking disabled for this build.")
    end

    -- MesenCE only loads a script once the ROM is already running, so any log
    -- point hit before this point is unavoidably missed -- reset once, so the
    -- run the user actually watches starts from power-on with logging already
    -- live. emu.reset() (like emu.rewind()) can only be called from inside a
    -- callback, not from this function's own top-level/retry-callback call
    -- site directly, so it's deferred one startFrame -- confirmed the hard
    -- way: calling it right here threw "This function cannot be called
    -- outside a callback" from Mesen.
    --
    -- Same reason resetMemoryTracking is called right here: whatever the
    -- stack/heap trackers observed between attaching (mid-run) and this
    -- actual reset is about to be thrown away along with everything else, so
    -- it must not count toward the high-water marks either.
    local resetRef
    resetRef = emu.addEventCallback(function()
        emu.removeEventCallback(resetRef, emu.eventType.startFrame)
        if resetMemoryTracking then resetMemoryTracking() end
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
    local bucket = logByLma[lma]
    if not bucket then return end

    -- All of them, in source order (see the loader's own comment on why
    -- there can be more than one) -- a pause() sharing this address must not
    -- stop earlier entries in the bucket from printing first.
    local shouldBreak = false
    for _, entry in ipairs(bucket) do
        if entry.kind == "pause" then
            -- pause() (logger.hpp) is unconditional on NES -- this is the only
            -- place it ever has any effect, same as log(). No formatting, no
            -- overlay message: just stop, right here, right now.
            emu.log("app: pause() hit at " .. entry.file .. ":" .. entry.line)
            shouldBreak = true
        else
            local message = formatMessage(entry.message, entry.args)
            emu.log("log: " .. message)
            emu.displayMessage("log", message)
        end
    end
    if shouldBreak then emu.breakExecution() end
end

emu.addMemoryCallback(refreshWindowMap, emu.callbackType.write,
    WINDOW_BASE, WINDOW_END, emu.cpuType.nes, emu.memType.nesMemory)

emu.addMemoryCallback(onExec, emu.callbackType.exec,
    WINDOW_BASE, WINDOW_END, emu.cpuType.nes, emu.memType.nesMemory)
