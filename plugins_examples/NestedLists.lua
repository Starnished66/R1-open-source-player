plugin.define({ id = "example.nested_lists", name = "Nested Lists", version = "1.0", api_min = 1 })

-- Demonstrates per-screen callback ownership. Open a child, press Back, and
-- select another parent row: it still invokes the parent's callback rather
-- than the most recently created child callback.

local function open_parent()
    local parent_rows = { "Open child list", "Prompt for a label", "Show parent callback" }
    plugin.show_list("Nested List Demo", parent_rows, function(parent_index)
        if parent_index == 1 then
            local child_rows = { "Child A", "Child B", "Child C" }
            plugin.show_list("Child List", child_rows, function(child_index)
                plugin.show_toast("Selected " .. child_rows[child_index])
            end, { width = 360 })
        elseif parent_index == 2 then
            local ok, err = plugin.show_text_input("Demo Label", "Example", false, function(text)
                plugin.show_toast("Entered: " .. text)
            end)
            if not ok then plugin.show_toast(err or "Text input unavailable") end
        else
            plugin.show_toast("Parent callback is still active")
        end
    end)
end

plugin.register_list_item("settings", "Nested List Demo", open_parent, { width = 420 })
