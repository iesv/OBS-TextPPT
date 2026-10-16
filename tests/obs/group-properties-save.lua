-- Run with OBS's Lua scripting support in an isolated test configuration.
-- Exercises adding a group, reopening properties and saving the scene.
obs = obslua
local source, scene_source, item, group_item, group_source
local iteration, wait = 0, 0
local source_kind = package.config:sub(1, 1) == "\\" and "obs-textppt-gdiplus" or "obs-textppt-ft2"

local function cleanup()
    obs.timer_remove(tick)
    if item ~= nil then obs.obs_sceneitem_remove(item); item = nil end
    if group_item ~= nil then obs.obs_sceneitem_remove(group_item); group_item = nil end
    if source ~= nil then
        obs.obs_source_remove(source)
        obs.obs_source_release(source)
        source = nil
    end
    if scene_source ~= nil then obs.obs_source_release(scene_source); scene_source = nil end
end

function tick()
    local ok, reason = pcall(function()
        if source == nil then
            local existing = obs.obs_get_source_by_name("TextPPT Settings Save Test")
            if existing ~= nil then
                obs.obs_source_release(existing)
                error("Test source already exists; use a clean isolated scene")
            end
            scene_source = obs.obs_frontend_get_current_scene()
            assert(scene_source ~= nil, "No current test scene")
            group_item = obs.obs_scene_add_group(obs.obs_scene_from_source(scene_source), "TextPPT Save Test Group")
            group_source = obs.obs_sceneitem_get_source(group_item)
            local settings = obs.obs_data_create()
            obs.obs_data_set_string(settings, "input_mode", "groups")
            source = obs.obs_source_create(source_kind, "TextPPT Settings Save Test", settings, nil)
            obs.obs_data_release(settings)
            assert(source ~= nil, "TextPPT source could not be created")
            item = obs.obs_scene_add(obs.obs_scene_from_source(scene_source), source)
        end
        if iteration == 20 then
            -- Let deferred OBS scene saves finish before declaring success.
            wait = wait + 1
            if wait == 4 then
                obs.script_log(obs.LOG_INFO, "[TextPPT Save Regression] PASS: 20 group selections, property closes and scene saves")
                cleanup()
            end
            return
        end
        local settings = obs.obs_source_get_settings(source)
        obs.obs_data_set_string(settings, "add_group", obs.obs_source_get_uuid(group_source))
        local properties = obs.obs_source_properties(source)
        obs.obs_properties_apply_settings(properties, settings)
        assert(obs.obs_properties_get(properties, "group_scene") == nil, "Single-scene selector must be removed")
        obs.obs_property_modified(obs.obs_properties_get(properties, "add_group"), settings)
        local ids = obs.obs_data_get_obj(settings, "group_ids")
        assert(ids ~= nil, "Cleared group IDs must be a valid object")
        obs.obs_data_release(ids)
        local entries = obs.obs_data_get_array(settings, "groups")
        assert(entries ~= nil and obs.obs_data_array_count(entries) == 1, "Group picker must keep one page without duplicating it")
        obs.obs_data_array_release(entries)
        assert(obs.obs_data_get_json(settings) ~= nil, "Source settings cannot be serialized")
        obs.obs_properties_destroy(properties)
        obs.obs_data_release(settings)
        obs.obs_frontend_save()
        iteration = iteration + 1
    end)
    if not ok then
        obs.script_log(obs.LOG_ERROR, "[TextPPT Save Regression] FAIL: " .. tostring(reason))
        cleanup()
    end
end

function script_description()
    return "TextPPT group properties/save regression. Run only in an isolated OBS test configuration."
end

function script_load(settings)
    obs.timer_add(tick, 500)
end

function script_unload()
    cleanup()
end
