-- Audiobooks plugin -- reachable from Books -> "Audio Books" (leaves the
-- app's own built-in "Books" file/Favorites feature untouched).
--
-- Install: copy this file to <SD card>/.plugins/Audiobooks.lua, then create
-- an "Audiobooks" folder on the SD card, one subfolder per book, with each
-- book's chapter files inside (any format the player already supports:
-- mp3, m4a/m4b, flac, ogg, wav, aac).
--
--   /Audiobooks/
--     Some Novel/
--       01 Chapter One.mp3
--       02 Chapter Two.mp3
--     Another Book/
--       chapter1.m4b
--
-- Tapping "Audio Books" lists book folders; tapping a book lists its
-- chapter files (sorted by name) and starts playback from whichever one is tapped,
-- continuing through the rest of the book's chapters in order.

local audiobooks_dir = plugin.sd_root() .. "/Audiobooks"

local function is_audio_file(name)
    local ext = name:match("%.([%a%d]+)$")
    if not ext then return false end
    ext = ext:lower()
    return ext == "mp3" or ext == "m4a" or ext == "m4b" or ext == "flac"
        or ext == "ogg" or ext == "wav" or ext == "aac"
end

local function open_chapters(book_dir, book_name)
    local entries = plugin.list_dir(book_dir)
    local items = {}
    for _, e in ipairs(entries) do
        if not e.dir and is_audio_file(e.name) then
            table.insert(items, { name = e.name, path = book_dir .. "/" .. e.name })
        end
    end

    if #items == 0 then
        plugin.show_toast("No chapters found in " .. book_name)
        return
    end

    table.sort(items, function(a, b) return a.name < b.name end)

    local labels, paths = {}, {}
    for i, it in ipairs(items) do
        labels[i] = it.name
        paths[i] = it.path
    end

    plugin.show_list(book_name, labels, function(index)
        plugin.play_list(paths, index)
    end)
end

local function open_books()
    local entries = plugin.list_dir(audiobooks_dir)
    local books = {}
    for _, e in ipairs(entries) do
        if e.dir then table.insert(books, e.name) end
    end

    if #books == 0 then
        plugin.show_toast("No audiobooks found -- add folders under Audiobooks/ on the SD card")
        return
    end

    table.sort(books)

    plugin.show_list("Audiobooks", books, function(index)
        local name = books[index]
        open_chapters(audiobooks_dir .. "/" .. name, name)
    end)
end

plugin.register_tile("Audiobooks", open_books, "launcher/book.png")
