plugin.define({ id = "example.async_http", name = "Async HTTP Demo", version = "1.0", api_min = 1 })

-- Demonstrates non-blocking GET, bounded responses, callback delivery on the
-- UI thread, and generation-safe logical cancellation.

local active_handle = nil

local function start_request()
    if active_handle then
        plugin.show_toast("A request is already active")
        return
    end

    local handle, err = plugin.http_request({
        url = "https://example.com/",
        method = "GET",
        verify_tls = true,
        max_response_bytes = 131072,
    }, function(status, body, request_error)
        active_handle = nil
        if request_error then
            plugin.show_toast("Request failed: " .. request_error)
        else
            plugin.show_toast("HTTP " .. status .. ": " .. #body .. " bytes")
        end
    end)

    if not handle then
        plugin.show_toast("Could not start request: " .. (err or "unknown error"))
        return
    end
    active_handle = handle
    plugin.show_toast("Request started; UI remains responsive")
end

local function cancel_request()
    if not active_handle then
        plugin.show_toast("No active request")
        return
    end
    if plugin.cancel(active_handle) then
        active_handle = nil
        plugin.show_toast("Callback cancelled")
    else
        active_handle = nil
        plugin.show_toast("Request already completed")
    end
end

local function open_demo()
    plugin.show_settings_list("Async HTTP", {
        { type = "row", label = "Start example request", on_select = start_request },
        { type = "row", label = "Cancel active request", on_select = cancel_request },
    })
end

plugin.register_list_item("system", "Async HTTP Demo", open_demo)
