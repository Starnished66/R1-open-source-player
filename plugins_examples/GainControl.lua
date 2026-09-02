plugin.define({
  id = "org.hiby.r1_gain_control",
  name = "Gain Control",
  version = "1.0.0",
  api_min = 10,
})

if not plugin.has_capability("audio.gain") then
  plugin.register_list_item("playback", "Gain Control", function()
    plugin.show_toast("Gain Control requires a newer player build")
  end)
else
  local STORAGE_KEY = "mode"
  local mode = plugin.storage.get(STORAGE_KEY, nil)

  if mode == "low" or mode == "high" then
    plugin.set_gain(mode)
  else
    mode = nil
  end

  local function select_gain(new_mode)
    plugin.set_gain(new_mode)

    mode = new_mode
    local ok = plugin.storage.set(STORAGE_KEY, mode)
    if ok then
      plugin.show_toast(new_mode == "low" and "Low Gain" or "High Gain")
    else
      plugin.show_toast((new_mode == "low" and "Low Gain" or "High Gain") .. " (not saved)")
    end
  end

  plugin.register_list_item("playback", "Gain Control", function()
    local low_label = "Low Gain"
    local high_label = "High Gain"

    if mode == "low" then
      low_label = "Low Gain"
    elseif mode == "high" then
      high_label = "High Gain"
    end

    plugin.show_list("Gain Control", {
      low_label,
      high_label,
    }, function(index)
      if index == 1 then
        select_gain("low")
      elseif index == 2 then
        select_gain("high")
      end
    end)
  end)
end
