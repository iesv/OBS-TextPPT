// OBS-TextPPT, GPL-2.0-or-later. References existing OBS groups as pages.
#include "obs-textppt.h"
#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <unordered_map>

bool text_group_safe(obs_source_t *source, obs_source_t *controller)
{
	if (!source || obs_source_removed(source) || !obs_group_from_source(source))
		return false;
	struct search { obs_source_t *controller; bool found = false; } data{controller};
	obs_source_enum_full_tree(source, [](obs_source_t *, obs_source_t *child, void *param) {
		auto *data = static_cast<search *>(param);
		// Exclude other controllers too, so inactive references cannot form cycles later.
		data->found |= child == data->controller || strncmp(obs_source_get_unversioned_id(child), "obs-textppt-", 12) == 0;
	}, &data);
	return !data.found;
}

namespace {
const char *entry_id(obs_data_t *ids, obs_data_t *entry)
{
	const char *uuid = obs_data_get_string(ids, obs_data_get_string(entry, "uuid"));
	return *uuid ? uuid : obs_data_get_string(ids, obs_data_get_string(entry, "value"));
}

struct group_choices {
	obs_property_t *property;
	obs_source_t *controller;
	std::string path;
	std::unordered_map<std::string, std::string> seen;
	const char *target = nullptr;
	obs_source_t *current_scene = nullptr;
	obs_source_t *owner = nullptr;
};

bool collect_items(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	if (!obs_sceneitem_is_group(item))
		return true;
	auto *choices = static_cast<group_choices *>(param);
	auto *source = obs_sceneitem_get_source(item);
	if (obs_source_removed(source))
		return true;
	const std::string uuid = obs_source_get_uuid(source);
	if (!choices->seen.emplace(uuid, obs_source_get_name(source)).second)
		return true;
	if (choices->target && uuid == choices->target) {
		choices->owner = obs_source_get_ref(choices->current_scene);
		return false;
	}
	const std::string previous = choices->path;
	if (choices->property) {
		choices->path += " / ";
		choices->path += obs_source_get_name(source);
		if (text_group_safe(source, choices->controller))
			obs_property_list_add_string(choices->property, choices->path.c_str(), uuid.c_str());
	}
	obs_scene_enum_items(obs_sceneitem_group_get_scene(item), collect_items, choices);
	choices->path = previous;
	return choices->owner == nullptr;
}

bool collect_scenes(void *param, obs_source_t *source)
{
	if (obs_source_removed(source))
		return true;
	if (auto *scene = obs_scene_from_source(source)) {
		auto *choices = static_cast<group_choices *>(param);
		choices->current_scene = source;
		if (choices->property)
			choices->path = obs_source_get_name(source);
		obs_scene_enum_items(scene, collect_items, choices);
		if (choices->owner)
			return false;
	}
	return true;
}

bool layout_items(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto &signature = *static_cast<uint64_t *>(param);
	const auto mix = [&](const auto &value) {
		const auto *bytes = reinterpret_cast<const unsigned char *>(&value);
		for (size_t i = 0; i < sizeof(value); ++i)
			signature = (signature ^ bytes[i]) * 1099511628211ull;
	};
	auto *source = obs_sceneitem_get_source(item);
	const char *uuid = obs_source_get_uuid(source);
	do { mix(*uuid); } while (*uuid++);
	mix(obs_source_get_width(source));
	mix(obs_source_get_height(source));
	mix(obs_sceneitem_visible(item));
	obs_transform_info info = {};
	obs_sceneitem_get_info2(item, &info);
	mix(info.pos.x); mix(info.pos.y); mix(info.rot);
	mix(info.scale.x); mix(info.scale.y); mix(info.alignment);
	mix(info.bounds_type); mix(info.bounds_alignment);
	mix(info.bounds.x); mix(info.bounds.y); mix(info.crop_to_bounds);
	obs_sceneitem_crop crop = {};
	obs_sceneitem_get_crop(item, &crop);
	mix(crop.left); mix(crop.top); mix(crop.right); mix(crop.bottom);
	if (obs_sceneitem_is_group(item))
		obs_scene_enum_items(obs_sceneitem_group_get_scene(item), layout_items, param);
	return true;
}

uint64_t group_signature(obs_source_t *source)
{
	uint64_t signature = 14695981039346656037ull;
	obs_scene_enum_items(obs_group_from_source(source), layout_items, &signature);
	const uint64_t dimensions = (uint64_t(obs_source_get_width(source)) << 32) | obs_source_get_height(source);
	signature = (signature ^ dimensions) * 1099511628211ull;
	return signature;
}

bool add_group(void *data, obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	const std::string uuid = obs_data_get_string(settings, S_ADD_GROUP);
	if (uuid.empty())
		return false;
	obs_data_set_string(settings, S_ADD_GROUP, "");
	auto *s = static_cast<textppt_source *>(data);
	auto *source = obs_get_source_by_uuid(uuid.c_str());
	if (!text_group_safe(source, s ? s->source : nullptr)) {
		obs_source_release(source);
		return true;
	}
	auto *entries = obs_data_get_array(settings, S_GROUPS);
	if (!entries)
		entries = obs_data_array_create();
	auto *ids = obs_data_get_obj(settings, S_GROUP_IDS);
	if (!ids)
		ids = obs_data_create();
	bool duplicate = false;
	for (size_t i = 0; i < obs_data_array_count(entries); ++i) {
		auto *entry = obs_data_array_item(entries, i);
		const std::string existing = entry_id(ids, entry);
		duplicate |= uuid == existing;
		// Existing OBS editable-list row IDs survive reorder and label edits.
		const char *key = obs_data_get_string(entry, "uuid");
		if (*key && !existing.empty())
			obs_data_set_string(ids, key, existing.c_str());
		obs_data_release(entry);
	}
	if (!duplicate && obs_data_array_count(entries) < 1000) {
		const char *name = obs_source_get_name(source);
		auto *entry = obs_data_create();
		obs_data_set_string(entry, "value", name);
		obs_data_set_string(entry, "uuid", uuid.c_str());
		obs_data_array_push_back(entries, entry);
		obs_data_release(entry);
		obs_data_set_string(ids, uuid.c_str(), uuid.c_str());
	}
	obs_data_set_array(settings, S_GROUPS, entries);
	obs_data_set_obj(settings, S_GROUP_IDS, ids);
	obs_data_array_release(entries);
	obs_data_release(ids);
	obs_source_release(source);
	return true;
}
}

bool text_group_present(obs_source_t *source)
{
	if (!source)
		return false;
	group_choices groups{nullptr, nullptr, {}, {}};
	obs_enum_scenes(collect_scenes, &groups);
	return groups.seen.count(obs_source_get_uuid(source)) != 0;
}

void text_group_update_layout(textppt_source *s, obs_source_t *source)
{
	const char *uuid = obs_source_get_uuid(source);
	const uint64_t signature = group_signature(source);
	// Render the parent only when group dimensions or item transforms change.
	if (s->layout_group == uuid && s->layout_signature == signature)
		return;
	group_choices groups{nullptr, nullptr, {}, {}};
	groups.target = uuid;
	obs_enum_scenes(collect_scenes, &groups);
	if (!groups.owner)
		return;
	// OBS updates group bounds in the parent render; use a one-pixel target.
	obs_enter_graphics();
	if (!s->group_layout)
		s->group_layout = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!s->group_layout) {
		obs_leave_graphics();
		obs_source_release(groups.owner);
		return;
	}
	gs_texrender_reset(s->group_layout);
	if (gs_texrender_begin(s->group_layout, 1, 1)) {
		gs_ortho(0.0f, float(obs_source_get_width(groups.owner)), 0.0f,
		         float(obs_source_get_height(groups.owner)), -100.0f, 100.0f);
		s->updating_group_layout = true;
		obs_source_video_render(groups.owner);
		s->updating_group_layout = false;
		gs_texrender_end(s->group_layout);
		s->layout_group = uuid;
		s->layout_signature = group_signature(source);
	}
	obs_leave_graphics();
	obs_source_release(groups.owner);
}

void text_groups_read(obs_data_t *settings, std::vector<text_data> &pages)
{
	std::unordered_set<std::string> seen;
	auto *entries = obs_data_get_array(settings, S_GROUPS);
	auto *ids = obs_data_get_obj(settings, S_GROUP_IDS);
	for (size_t i = 0; i < std::min(obs_data_array_count(entries), size_t{1000}); ++i) {
		auto *entry = obs_data_array_item(entries, i);
		const char *name = obs_data_get_string(entry, "value");
		const char *uuid = entry_id(ids, entry);
		if (*uuid && seen.insert(uuid).second)
			pages.push_back({"", "", uuid, name});
		obs_data_release(entry);
	}
	obs_data_array_release(entries);
	obs_data_release(ids);
}

void text_groups_refresh(textppt_source *s, float seconds)
{
	s->group_scan_elapsed += seconds;
	if (s->group_scan_elapsed < 0.5f)
		return;
	s->group_scan_elapsed = 0.0f;
	// Scan groups only while rendering or serving status requests.
	const bool observed = s->status_observed.exchange(false);
	if (s->pages.empty() || (!obs_source_showing(s->source) && !observed))
		return;
	group_choices groups{nullptr, nullptr, {}, {}};
	obs_enum_scenes(collect_scenes, &groups);
	std::vector<std::string> labels;
	labels.reserve(s->pages.size());
	for (size_t index = 0; index < s->pages.size(); ++index) {
		const auto &page = s->pages[index];
		const auto name = groups.seen.find(page.source_uuid);
		// Only the current group needs a source reference and cycle check.
		auto *source = index == s->cur_item ? obs_get_source_by_uuid(page.source_uuid.c_str()) : nullptr;
		const bool valid = name != groups.seen.end() && (index != s->cur_item || text_group_safe(source, s->source));
		labels.push_back(valid ?
			name->second : page.source_name + " [" + T_SS_("GroupUnavailable") + "]");
		if (!valid && index == s->cur_item) {
			obs_transition_set(s->transition, nullptr);
			s->transition_running = false;
		} else if (valid && index == s->cur_item) {
			auto *active = obs_transition_get_active_source(s->transition);
			// Restore a returned group without restarting stopped or hidden playback.
			if (!active && !s->page_pending && !s->stopped && !(s->hide && s->state.load() == OBS_MEDIA_STATE_ENDED)) {
				s->page_pending = true;
				s->pending_cut = true;
			}
			obs_source_release(active);
			if (obs_source_showing(s->source))
				text_group_update_layout(s, source);
		}
		obs_source_release(source);
	}
	std::lock_guard<std::mutex> lock(s->snapshot_mutex);
	if (labels != s->snapshot) {
		s->snapshot = std::move(labels);
		++s->revision;
	}
}

void text_groups_properties(textppt_source *s, obs_properties_t *props)
{
	auto *p = obs_properties_add_list(props, S_ADD_GROUP, T_SS_("AddGroup"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_SS_("ChooseGroup"), "");
	group_choices choices{p, s ? s->source : nullptr, {}, {}};
	obs_enum_scenes(collect_scenes, &choices);
	obs_property_set_modified_callback2(p, add_group, s);
	obs_properties_add_editable_list(props, S_GROUPS, T_SS_("GroupOrder"), OBS_EDITABLE_LIST_TYPE_STRINGS, nullptr, nullptr);
}
