--
-- Copyright (c) 2026 Michael Vandeberg
--
-- Distributed under the Boost Software License, Version 1.0. (See accompanying
-- file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
--
-- Official repository: https://github.com/cppalliance/corosio
--

-- Inject compiled reference examples into the MrDocs corpus.
--
-- Examples live as tagged regions in checked-in .cpp files under
-- test/doc/reference/, compiled by boost_corosio_doc_tests like every other
-- snippet. This transform reads those regions and injects them into the
-- matching symbol's documentation, so the reference renders an example the
-- normal build has already compiled. Compilation and injection stay
-- independent: CI compiles the files whether or not the docs build, and the
-- docs build injects them whether or not they compile.
--
-- The file name IS the mapping: a symbol's examples live in
--
--     test/doc/reference/<scope>.<kind>.cpp
--
-- where <scope> is the qualified name with `boost::corosio::` stripped and
-- `::` replaced by `__`. So boost::corosio::socket_option::no_delay (a record)
-- reads `socket_option__no_delay.record.cpp`. The kind is part of the name
-- because a name alone is ambiguous: `no_delay` also matches its constructors
-- and their overload set. Where even that is ambiguous, the symbol's unique
-- MrDocs anchor is tried first: `<scope>.<anchor>.cpp`.
--
-- Discovery works symbol-first, opening a predicted path, because MrDocs' Lua
-- sandbox blocks io.popen and Lua has no directory listing.
--
-- A file may hold several tagged regions. Each `@par !example <tag>` marker in
-- a docstring names the region it wants, and that region's code replaces the
-- marker heading where it stands, so an example renders exactly where the
-- docstring puts it. A symbol with no marker is left untouched; a marker naming
-- a region the file does not hold is an error.

local SOURCE_DEFAULT = "test/doc/reference"
local STRIP_PREFIX = "boost::corosio::"
-- Heading title that marks a position without rendering; see the anchor loop.
local SENTINEL = "!example"

-- Scalar fields copied when rebuilding a block. `sym.doc.document` accepts only
-- plain tables and rejects the userdata proxies it hands out, so appending to a
-- symbol's documentation means deep-copying every existing block first. A field
-- missing from this list is dropped silently, which is why the injection is
-- verified against a no-transform baseline rather than trusted.
-- `level` is deliberately absent: MrDocs' generic setter refuses to write it
-- ("field 'level' has a type the generic setter cannot yet write"), as an
-- integer or a float. Every heading in this corpus is level 1, which is what
-- `@par` produces and what the templates default to, so omitting it round-trips
-- unchanged -- verified by diffing the rendered corpus against a no-transform
-- baseline. A corpus using deeper headings would need this fixed upstream.
local SCALARS = {
    "kind", "literal", "lang", "title", "name", "text",
    "href", "anchor", "id", "style", "admonition", "admonish", "symbol", "href_text",
    -- ImageInline carries its target in `src`/`alt` and FootnoteReferenceInline
    -- its back-link in `label`. Omitted, an image rebuilds as `image:[]` and a
    -- footnote reference as a link with no text, orphaning its definition.
    "src", "alt", "label",
}

-- Child blocks hang off more than one field name: a paragraph uses `children`,
-- a list uses `items` (of `listItem`, which in turn uses `blocks`), and an
-- admonition uses `blocks`. Recursing only `children` silently drops list items
-- and note bodies -- caught by the baseline diff, not by any error.
local CONTAINERS = { "children", "items", "blocks" }

local function fail(msg)
    error("[reference-snippets] " .. msg, 0)
end

local function qualified_name(ctx, sym)
    local parts, cur = {}, sym
    while cur and cur.name ~= nil do
        table.insert(parts, 1, cur.name)
        cur = cur.parent and ctx.corpus.get(cur.parent) or nil
    end
    return table.concat(parts, "::")
end

-- Whether this library owns the symbol's documentation, and so owns its
-- examples. Three kinds of symbol in the corpus carry `!example` markers this
-- repository can never satisfy: a dependency's own symbols (a downstream
-- library sees every marker in its upstream's headers), the copy MrDocs
-- makes of an inherited member inside the deriving class, whose docstring --
-- marker included -- comes from the base's header, and anything else outside
-- `boost::corosio::`. All three would trip the fail-closed guard below and
-- abort the docs build. The last is the backstop: `slug()` strips only the
-- corosio prefix, so a dependency symbol MrDocs classifies as neither of the
-- first two would send the build looking for `boost_capy_X.record.cpp`.
-- Their markers are still stripped, just never resolved: a dependency's
-- symbols have no page, but an inherited copy does, and a raw
-- `!example <tag>` heading must never reach it.
local function owned(ctx, sym)
    local ok, mode = pcall(function() return sym.extraction end)
    if ok and mode ~= nil and tostring(mode) == "dependency" then return false end
    local ok2, inherited = pcall(function() return sym.isCopyFromInherited end)
    if ok2 and inherited == true then return false end
    if qualified_name(ctx, sym):sub(1, #STRIP_PREFIX) ~= STRIP_PREFIX then return false end
    return true
end

local function copy(node)
    local out = {}
    for _, f in ipairs(SCALARS) do
        local ok, v = pcall(function() return node[f] end)
        if ok and v ~= nil and type(v) ~= "userdata" and type(v) ~= "table" then
            -- Numbers read back as Lua floats (a heading's level is 1.0), and the
            -- generic setter rejects a float where the DOM holds an integer.
            if type(v) == "number" and math.tointeger(v) then v = math.tointeger(v) end
            out[f] = v
        end
    end
    for _, field in ipairs(CONTAINERS) do
        local ok, kids = pcall(function() return node[field] end)
        if ok and kids ~= nil then
            local c = {}
            for _, k in ipairs(kids) do c[#c + 1] = copy(k) end
            if #c > 0 then out[field] = c end
        end
    end
    return out
end

-- The first literal found anywhere under a node, used to identify a heading.
local function text_of(node)
    local ok, lit = pcall(function() return node.literal end)
    if ok and type(lit) == "string" and lit ~= "" then return lit end
    local ok2, kids = pcall(function() return node.children end)
    if ok2 and kids then
        for _, k in ipairs(kids) do
            local t = text_of(k)
            if t then return t end
        end
    end
    return nil
end

local function slug(qname)
    local s = qname
    if s:sub(1, #STRIP_PREFIX) == STRIP_PREFIX then s = s:sub(#STRIP_PREFIX + 1) end
    s = s:gsub("::", "__")
    -- Anything not an identifier character collapses to `_`, so a call operator
    -- (whose qualified name is literally `operator()`) yields a usable file name.
    return (s:gsub("[^%w_]+", "_"))
end

local function dedent(lines)
    local margin
    for _, l in ipairs(lines) do
        if l:match("%S") then
            local n = #(l:match("^[ \t]*"))
            if margin == nil or n < margin then margin = n end
        end
    end
    local out = {}
    for _, l in ipairs(lines) do out[#out + 1] = l:sub((margin or 0) + 1) end
    while #out > 0 and not out[1]:match("%S") do table.remove(out, 1) end
    while #out > 0 and not out[#out]:match("%S") do table.remove(out) end
    return table.concat(out, "\n")
end

-- Tagged regions in a file, in order. Returns nil when the file does not exist.
local function read_regions(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local lines = {}
    for l in f:lines() do lines[#lines + 1] = l end
    f:close()

    local out, open_tag, body = {}, nil, nil
    for i, l in ipairs(lines) do
        local t = l:match("^%s*//%s*tag::([%w_%-]+)%[%]%s*$")
        local e = l:match("^%s*//%s*end::([%w_%-]+)%[%]%s*$")
        if t then
            if open_tag then fail(path .. ":" .. i .. ": tag '" .. t .. "' opens inside '" .. open_tag .. "'") end
            open_tag, body = t, {}
        elseif e then
            if open_tag ~= e then
                fail(path .. ":" .. i .. ": end::" .. e .. "[] does not close the open tag")
            end
            out[#out + 1] = { tag = open_tag, code = dedent(body) }
            open_tag, body = nil, nil
        elseif open_tag then
            body[#body + 1] = l
        end
    end
    if open_tag then fail(path .. ": tag '" .. open_tag .. "' is never closed") end
    if #out == 0 then fail(path .. ": file exists but declares no // tag::name[] region") end
    return out
end

-- Rewrite a symbol's documentation, replacing each marker heading in `anchors`
-- with the region `picked` holds for it. A nil `picked` deletes the markers
-- instead, which is what an unowned symbol gets: this repository cannot supply
-- its example, but it must not publish the raw marker either. Returns the number
-- of examples injected. Any `@par Example` heading the marker sat under is left
-- alone -- an empty section is sparse, a literal "!example" is broken.
local function rewrite(sym, seq, anchors, picked)
    local nth, out = 0, {}
    for i, b in ipairs(seq) do
        if anchors[nth + 1] and anchors[nth + 1].at == i then
            nth = nth + 1
            if picked then
                out[#out + 1] = { kind = "code", literal = picked[nth].code }
            end
        else
            out[#out + 1] = copy(b)
        end
    end
    sym.doc.document = out
    return picked and #anchors or 0
end

mrdocs.register_transform("reference-snippets", function(ctx)
    local root = (ctx.params and ctx.params.source) or SOURCE_DEFAULT
    local injected, files, missing = 0, 0, {}

    for _, sym in ipairs(ctx.corpus.symbols) do
        if sym.name ~= nil and sym.doc then
            local doc = sym.doc.document or {}

            -- Each `@par !example <tag>` marker names the region it wants. The
            -- marker is consumed rather than rendered, so the example lands where
            -- the docstring puts it, and naming the region keeps overloads that
            -- share a file independent of the order MrDocs visits them in -- an
            -- order that differs between corpora, and that silently swapped two
            -- `armed` examples on the published site.
            local seq, anchors = {}, {}
            for _, b in ipairs(doc) do seq[#seq + 1] = b end
            for i, b in ipairs(seq) do
                if b.kind == "heading" then
                    local t = text_of(b)
                    local tag = t and t:match("^" .. SENTINEL .. "%s+(%S+)$")
                    if tag then
                        anchors[#anchors + 1] = { at = i, tag = tag }
                    elseif t and t:sub(1, #SENTINEL) == SENTINEL then
                        -- A marker that does not parse -- a tag with a space in
                        -- it, or a stray trailing character -- would otherwise
                        -- go unrecognised, survive unstripped, and publish a
                        -- heading reading literally "!example ..." while the
                        -- build reported success. Fail closed like the rest.
                        fail(string.format("%s: malformed marker heading '%s'; expected '%s <tag>' with a single-word tag",
                             qualified_name(ctx, sym), t, SENTINEL))
                    end
                end
            end
            local at = anchors[1] and anchors[1].at or nil

            if at and not owned(ctx, sym) then
                rewrite(sym, seq, anchors, nil)
            elseif at then
                local base = root .. "/" .. slug(qualified_name(ctx, sym))
                local path = base .. "." .. tostring(sym.anchor) .. ".cpp"
                local regions = read_regions(path)
                if not regions then
                    path = base .. "." .. tostring(sym.kind) .. ".cpp"
                    regions = read_regions(path)
                end
                if not regions then
                    -- Fail-closed guard: the docstring promises an example and
                    -- no file provides one, so the reference would render an
                    -- empty "Example" section.
                    missing[#missing + 1] = qualified_name(ctx, sym) ..
                        " (expected " .. base .. "." .. tostring(sym.kind) .. ".cpp)"
                else
                    files = files + 1
                    local by_tag = {}
                    for _, r in ipairs(regions) do by_tag[r.tag] = r end
                    local picked = {}
                    for k, a in ipairs(anchors) do
                        local r = by_tag[a.tag]
                        if not r then
                            fail(string.format("%s has no region tagged '%s', named by %s",
                                 path, a.tag, qualified_name(ctx, sym)))
                        end
                        picked[k] = r
                    end

                    injected = injected + rewrite(sym, seq, anchors, picked)
                end
            end
        end
    end

    if #missing > 0 then
        fail("these symbols carry an @par !example marker with no snippet file:\n    " ..
             table.concat(missing, "\n    "))
    end
    -- stderr, not print: the Antora reference extension buffers MrDocs' stdout
    -- and discards it when the command succeeds, forwarding only stderr, so a
    -- print() here never reaches the docs build log.
    io.stderr:write(string.format("[reference-snippets] injected %d example(s) from %d file(s) under %s\n",
                                  injected, files, root))
end)
