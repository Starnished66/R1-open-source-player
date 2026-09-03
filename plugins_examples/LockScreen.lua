plugin.define({
    id = "example.lock_screen",
    name = "Lock Screen",
    version = "1.0",
    api_min = 1,
})

-- Lock Screen companion plugin.
-- Adds a "Lock Screen" row to Settings -> Display.
-- Configures a cosmetic lock screen overlay shown when screen wakes from being off:
-- Modes: Off / Album Art / Custom Image / Clock.
-- Dismissed by swiping up.

if not plugin.has_capability("ui.lock_screen") then
    plugin.show_toast("Lock Screen needs a newer player build")
    return
end

local STORAGE_KEY_MODE = "mode"
local STORAGE_KEY_IMAGE = "image_path"

local MODES = {
    { key = "off",       label = "Off" },
    { key = "album_art", label = "Album Art" },
    { key = "image",     label = "Custom Image" },
    { key = "clock",     label = "Clock" },
}

local function get_current_mode()
    return plugin.storage.get(STORAGE_KEY_MODE, "off")
end

local function set_current_mode(mode_key)
    plugin.storage.set(STORAGE_KEY_MODE, mode_key)
end

local function get_custom_image_path()
    return plugin.storage.get(STORAGE_KEY_IMAGE, "")
end

local function set_custom_image_path(path)
    plugin.storage.set(STORAGE_KEY_IMAGE, path)
end

local function trigger_lock_screen()
    local mode = get_current_mode()
    if mode == "off" then return end

    local opts = { mode = mode }
    if mode == "image" then
        local img_path = get_custom_image_path()
        if not img_path or img_path == "" then return end
        opts.image_path = img_path
    end

    plugin.show_lock_screen(opts)
end

-- Screen woke event handler
plugin.on("screen_woke", function()
    trigger_lock_screen()
end)

local function open_custom_image_picker()
    local dir_path = plugin.sd_root() .. "/.plugins/lock_images"
    local files = plugin.list_dir(dir_path)
    local image_files = {}

    for _, entry in ipairs(files) do
        if not entry.dir then
            local name_lower = entry.name:lower()
            if name_lower:match("%.png$") or name_lower:match("%.jpg$") or name_lower:match("%.jpeg$") then
                table.insert(image_files, entry.name)
            end
        end
    end

    table.sort(image_files)

    if #image_files == 0 then
        plugin.show_toast("No images in /.plugins/lock_images")
        return
    end

    local current_img = get_custom_image_path()
    local selected_idx = 0
    for i, name in ipairs(image_files) do
        if dir_path .. "/" .. name == current_img then
            selected_idx = i
            break
        end
    end

    plugin.show_list("Select Image", image_files, function(index)
        local chosen = dir_path .. "/" .. image_files[index]
        set_custom_image_path(chosen)
        set_current_mode("image")
        plugin.show_toast("Lock Image: " .. image_files[index])
    end, selected_idx > 0 and { selected = selected_idx } or nil)
end

plugin.register_list_item("display", "Lock Screen", function()
    local current_mode = get_current_mode()
    local labels = {}
    local selected_idx = 0

    for i, m in ipairs(MODES) do
        labels[i] = m.label
        if m.key == current_mode then
            selected_idx = i
        end
    end

    plugin.show_list("Lock Screen", labels, function(index)
        local chosen_mode = MODES[index].key
        if chosen_mode == "image" then
            open_custom_image_picker()
        else
            set_current_mode(chosen_mode)
            plugin.show_toast("Lock Screen: " .. MODES[index].label)
        end
    end, selected_idx > 0 and { selected = selected_idx } or nil)
end)
