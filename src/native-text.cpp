// Derived from obs-text-slideshow. Copyright (C) 2021 Joshua Wong.
// Copyright (C) 2026 iesv. SPDX-License-Identifier: GPL-2.0-or-later.
#include "obs-textppt.h"
#include "text-layout.h"
#include <QFont>
#include <algorithm>
#include <cstring>
#include <new>

struct text_layout {
	obs_source_t *source;
	uint32_t width = 0, height = 0;
};

static uint32_t layout_width(void *data)
{
	auto *layout = static_cast<text_layout *>(data);
	return layout->width ? layout->width : obs_source_get_base_width(obs_filter_get_parent(layout->source));
}

static uint32_t layout_height(void *data)
{
	auto *layout = static_cast<text_layout *>(data);
	return layout->height ? layout->height : obs_source_get_base_height(obs_filter_get_parent(layout->source));
}

static void layout_render(void *data, gs_effect_t *)
{
	// Reserve the full page without another texture or rendering pass.
	obs_source_skip_video_filter(static_cast<text_layout *>(data)->source);
}

void textppt_text_layout(obs_source_t *source, uint32_t width, uint32_t height)
{
	auto *filter = obs_source_get_filter_by_name(source, "TextPPT page layout");
	if (!filter && width && height) {
		filter = obs_source_create_private("textppt_page_layout", "TextPPT page layout", nullptr);
		if (filter)
			obs_source_filter_add(source, filter);
	}
	if (filter) {
		if (auto *layout = static_cast<text_layout *>(obs_obj_get_data(filter))) {
			layout->width = width;
			layout->height = height;
		}
		obs_source_release(filter);
	}
}

#ifdef _WIN32
static constexpr const char *native_id = "text_gdiplus";
static constexpr const char *source_id = "obs-textppt-gdiplus";
#else
static constexpr const char *native_id = "text_ft2_source";
static constexpr const char *source_id = "obs-textppt-ft2";
#endif

static const char *native_type()
{
	return obs_get_latest_input_type_id(native_id);
}

static obs_properties_t *native_style()
{
	// Type callbacks do not depend on the lifetime of a text buffer.
	const char *type = native_type();
	auto *props = type ? obs_get_source_properties(type) : nullptr;
	if (!props) {
		blog(LOG_WARNING, "[obs-textppt] Native text properties unavailable: %s", native_id);
		return nullptr;
	}
	for (const char *key : {"text", "file", "text_file", "read_from_file", "from_file"})
		obs_properties_remove_by_name(props, key);
	return props;
}

static void copy_style(obs_properties_t *props, obs_data_t *input, obs_data_t *output, bool defaults)
{
	if (!props || !input)
		return;
	// Copy only native style fields; never duplicate the deck or file input.
	for (auto *item = obs_data_first(input); item; obs_data_item_next(&item)) {
		const char *key = obs_data_item_get_name(item);
		if (!obs_properties_get(props, key))
			continue;
		switch (obs_data_item_gettype(item)) {
		case OBS_DATA_STRING:
			(defaults ? obs_data_set_default_string : obs_data_set_string)(output, key, obs_data_item_get_string(item));
			break;
		case OBS_DATA_NUMBER:
			if (obs_data_item_numtype(item) == OBS_DATA_NUM_DOUBLE)
				(defaults ? obs_data_set_default_double : obs_data_set_double)(output, key, obs_data_item_get_double(item));
			else
				(defaults ? obs_data_set_default_int : obs_data_set_int)(output, key, obs_data_item_get_int(item));
			break;
		case OBS_DATA_BOOLEAN:
			(defaults ? obs_data_set_default_bool : obs_data_set_bool)(output, key, obs_data_item_get_bool(item));
			break;
		case OBS_DATA_OBJECT: {
			auto *value = obs_data_item_get_obj(item);
			auto *copy = obs_data_get_defaults(value);
			obs_data_apply(copy, value);
			(defaults ? obs_data_set_default_obj : obs_data_set_obj)(output, key, copy);
			obs_data_release(copy);
			obs_data_release(value);
			break;
		}
		default:
			break;
		}
	}
}

static obs_source_t *create_text(obs_data_t *settings)
{
	const char *type = native_type();
	if (!type)
		return nullptr;
	// Each buffer owns settings so page edits cannot affect its peer.
	auto *copy = obs_data_create();
	obs_data_apply(copy, settings);
	auto *source = obs_source_create_private(type, "TextPPT text buffer", copy);
	obs_data_release(copy);
	return source;
}

string textppt_render_text(const string &text, obs_data_t *settings, vector<size_t> *ends)
{
	const auto input = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
	auto *data = obs_data_get_obj(settings, "font");
	QFont font(QString::fromUtf8(obs_data_get_string(data, "face")));
	font.setPixelSize(static_cast<int>(std::clamp<int64_t>(obs_data_get_int(data, "size"), 1, 4096)));
#ifndef _WIN32
	font.setStyleName(QString::fromUtf8(obs_data_get_string(data, "style")));
#endif
	const int64_t flags = obs_data_get_int(data, "flags");
	font.setBold((flags & OBS_FONT_BOLD) != 0);
	font.setItalic((flags & OBS_FONT_ITALIC) != 0);
	obs_data_release(data);
	// Qt measurements approximate native rendering; stored text retains its tabs.
	vector<int> padding;
	const auto output = expand_text_tabs(input, font, ends ? &padding : nullptr).toUtf8();
	// Map original cell boundaries through the complete page's TAB padding.
	if (ends && !padding.empty()) {
		size_t tab = text.find('\t'), shift = 0, index = 0;
		for (auto &end : *ends) {
			while (tab != string::npos && tab < end) {
				shift += static_cast<size_t>(padding[index++] - 1);
				tab = text.find('\t', tab + 1);
			}
			end += shift;
		}
	}
	return string(output.constData(), static_cast<size_t>(output.size()));
}

static void text_alignment(obs_source_t *transition, obs_data_t *settings)
{
	uint32_t alignment = OBS_ALIGN_CENTER;
#ifdef _WIN32
	const char *value = obs_data_get_string(settings, "align");
	alignment = strcmp(value, "center") == 0 ? OBS_ALIGN_CENTER :
		strcmp(value, "right") == 0 ? OBS_ALIGN_RIGHT : OBS_ALIGN_LEFT;
#else
	UNUSED_PARAMETER(settings);
#endif
	obs_transition_set_alignment(transition, alignment);
}

static void text_update(void *data, obs_data_t *settings)
{
	auto *style = obs_data_create();
	const char *mode = textppt_input_mode(settings);
	if (strcmp(mode, INPUT_SOURCES) != 0 && strcmp(mode, INPUT_GROUPS) != 0) {
		auto *props = native_style();
		if (!props) {
			obs_data_release(style);
			return;
		}
		copy_style(props, settings, style, false);
#ifdef _WIN32
		// Always restore native bounds after temporary reveal extents.
		obs_data_set_bool(style, "extents", obs_data_get_bool(settings, "extents"));
#endif
		obs_properties_destroy(props);
	}
	textppt_update(data, settings, style, create_text, text_alignment);
	obs_data_release(style);
}

static void text_defaults(obs_data_t *settings)
{
	textppt_defaults(settings);
	auto *props = native_style();
	auto *defaults = props ? obs_get_source_defaults(native_type()) : nullptr;
	copy_style(props, defaults, settings, true);
	obs_properties_destroy(props);
	obs_data_release(defaults);
}

static obs_properties_t *text_properties(void *data)
{
	auto *props = obs_properties_create();
	textppt_properties(data, props);
	if (auto *style = native_style()) {
		if (!obs_properties_add_group(props, S_TEXT_STYLE, obs_module_text("Text.Style"), OBS_GROUP_NORMAL, style)) {
			obs_properties_destroy(style);
			blog(LOG_WARNING, "[obs-textppt] Cannot attach native text properties");
		}
	}
	textppt_add_info(props);
	return props;
}

static const char *text_name(void *) { return obs_module_text("Source.Name"); }

void load_textppt_native()
{
	obs_source_info layout = {};
	layout.id = "textppt_page_layout";
	layout.type = OBS_SOURCE_TYPE_FILTER;
	layout.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_CAP_DISABLED;
	layout.get_name = [](void *) { return "TextPPT page layout"; };
	layout.create = [](obs_data_t *, obs_source_t *source) -> void * { return new (std::nothrow) text_layout{source}; };
	layout.destroy = [](void *data) { delete static_cast<text_layout *>(data); };
	layout.get_width = layout_width;
	layout.get_height = layout_height;
	layout.video_render = layout_render;
	obs_register_source(&layout);

	obs_source_info info = {};
	info.id = source_id;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
		OBS_SOURCE_COMPOSITE | OBS_SOURCE_CONTROLLABLE_MEDIA;
	info.get_properties = text_properties;
	info.icon_type = OBS_ICON_TYPE_SLIDESHOW;
	info.get_name = text_name;
	info.create = textppt_create;
	info.destroy = textppt_destroy;
	info.get_width = textppt_width;
	info.get_height = textppt_height;
	info.get_defaults = text_defaults;
	info.update = text_update;
	info.activate = textppt_activate;
	info.deactivate = textppt_deactivate;
	info.video_tick = textppt_video_tick;
	info.video_render = textppt_video_render;
	info.audio_render = textppt_audio_render;
	info.enum_active_sources = textppt_enum_sources;
	info.enum_all_sources = textppt_enum_sources;
	info.media_play_pause = textppt_play_pause;
	info.media_restart = textppt_restart;
	info.media_stop = textppt_stop;
	info.media_next = textppt_next_slide;
	info.media_previous = textppt_previous_slide;
	info.media_get_state = textppt_get_state;
	info.missing_files = textppt_missing_files;
	obs_register_source(&info);
}
