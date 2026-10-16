// Derived from obs-text-slideshow. Copyright (C) 2021 Joshua Wong.
// Copyright (C) 2026 iesv. SPDX-License-Identifier: GPL-2.0-or-later.
#include "obs-textppt.h"
#include "text-parser.h"
#include <util/platform.h>
#include <util/dstr.h>
#include <algorithm>
#include <cstdlib>
#include <new>
#include <cstdio>
#include <cstring>

static bool load_text_from_file(string &text, const char *file_path, const char *&error)
{
	error = nullptr;
	FILE *file = os_fopen(file_path, "rb");
	if (!file) {
		error = "File.ReadError";
		return false;
	}
	string input;
	char chunk[4096];
	size_t count;
	while ((count = fread(chunk, 1, sizeof(chunk), file)) != 0) {
		input.append(chunk, count);
		if (input.size() > 4 * 1024 * 1024)
			break;
	}
	const bool failed = ferror(file) != 0;
	fclose(file);
	if (failed) {
		error = "File.ReadError";
		return false;
	}
	if (input.size() > 4 * 1024 * 1024) {
		error = "File.TooLarge";
		return false;
	}
	if (input.compare(0, 3, "\xef\xbb\xbf") == 0)
		input.erase(0, 3);
	text = std::move(input);
	return true;
}

static int64_t enqueue(textppt_source *s, slide_command type, size_t value = 0)
{
	std::lock_guard<std::mutex> lock(s->command_mutex);
	// Keep at most 64 pending commands.
	if (s->commands.size() == 64)
		s->commands.pop_front();
	const int64_t serial = ++s->next_serial;
	s->commands.push_back({type, value, serial});
	return serial;
}

static void get_texts(void *data, calldata_t *cd)
{
	auto *s = static_cast<textppt_source *>(data);
	auto *texts = static_cast<vector<string> *>(calldata_ptr(cd, "texts"));
	if (!texts)
		return;
	std::lock_guard<std::mutex> lock(s->snapshot_mutex);
	*texts = s->snapshot;
}

static void get_status(void *data, calldata_t *cd)
{
	auto *s = static_cast<textppt_source *>(data);
	s->status_observed = true;
	std::lock_guard<std::mutex> lock(s->snapshot_mutex);
	calldata_set_int(cd, "count", static_cast<int64_t>(s->snapshot.size()));
	calldata_set_int(cd, "revision", s->revision);
	calldata_set_int(cd, "step", static_cast<int64_t>(s->snapshot_step));
	calldata_set_int(cd, "steps", static_cast<int64_t>(s->snapshot_steps));
	calldata_set_bool(cd, "points", s->snapshot_points);
	calldata_set_int(cd, "width", s->cx.load());
	calldata_set_int(cd, "height", s->cy.load());
	calldata_set_bool(cd, "ready", s->ready.load());
	calldata_set_string(cd, "error", s->file_error.c_str());
	// Read acknowledgement before its page index.
	calldata_set_int(cd, "acknowledged", s->acknowledged.load());
	const size_t index = s->current.load();
	calldata_set_int(cd, "index", static_cast<int64_t>(index));
	calldata_set_string(cd, "group_uuid", index < s->snapshot_groups.size() ? s->snapshot_groups[index].c_str() : "");
}

static void dock_transition(void *data, calldata_t *cd)
{
	const int64_t index = calldata_int(cd, "index");
	if (index >= 0)
		calldata_set_int(cd, "serial", enqueue(static_cast<textppt_source *>(data),
			slide_command::Jump, static_cast<size_t>(index)));
}

static void reload_files(void *data, calldata_t *)
{
	auto *s = static_cast<textppt_source *>(data);
	auto *settings = obs_source_get_settings(s->source);
	const char *mode = textppt_input_mode(settings);
	if (strcmp(mode, INPUT_FILE) == 0 || strcmp(mode, INPUT_FILES) == 0) {
		// Request one read on the source's update thread.
		s->force_file_reload = true;
		obs_source_update(s->source, settings);
	}
	obs_data_release(settings);
}

static obs_source_t *get_transition(textppt_source *s)
{
	std::lock_guard<std::mutex> lock(s->transition_mutex);
	return obs_source_get_ref(s->transition);
}

static void hotkey(void *data, obs_hotkey_id, obs_hotkey_t *key, bool pressed)
{
	auto *s = static_cast<textppt_source *>(data);
	if (!pressed || !obs_source_showing(s->source))
		return;
	const char *name = obs_hotkey_get_name(key);
	if (strcmp(name, "SlideShow.NextSlide") == 0)
		enqueue(s, slide_command::Next);
	else if (strcmp(name, "SlideShow.PreviousSlide") == 0)
		enqueue(s, slide_command::Previous);
	else if (strcmp(name, "SlideShow.Restart") == 0)
		enqueue(s, slide_command::Restart);
	else if (strcmp(name, "SlideShow.Stop") == 0)
		enqueue(s, slide_command::Stop);
	else
		enqueue(s, slide_command::Pause, s->state.load() == OBS_MEDIA_STATE_PLAYING);
}

void *textppt_create(obs_data_t *settings, obs_source_t *source)
{
	auto *s = new (std::nothrow) textppt_source;
	if (!s)
		return nullptr;
	s->source = source;
	// OBS copies settings before creating Studio Mode source duplicates.
	const string previous_uuid = obs_data_get_string(settings, "_textppt_instance");
	if (!previous_uuid.empty() && previous_uuid != obs_source_get_uuid(source)) {
		auto *original = obs_get_source_by_uuid(previous_uuid.c_str());
		if (original && !obs_source_removed(original) &&
		    strcmp(obs_source_get_id(original), obs_source_get_id(source)) == 0) {
			calldata_t state = {};
			if (proc_handler_call(obs_source_get_proc_handler(original), "get_status", &state) &&
			    calldata_int(&state, "count") > 0) {
				s->initial_page = static_cast<size_t>(std::max<int64_t>(0, calldata_int(&state, "index")));
				s->reveal_step = static_cast<size_t>(std::max<int64_t>(1, calldata_int(&state, "step")));
				s->cx = static_cast<uint32_t>(std::max<int64_t>(1, calldata_int(&state, "width")));
				s->cy = static_cast<uint32_t>(std::max<int64_t>(1, calldata_int(&state, "height")));
			}
			calldata_free(&state);
		}
		obs_source_release(original);
	}
	obs_data_set_string(settings, "_textppt_instance", obs_source_get_uuid(source));
	const char *keys[] = {"SlideShow.PlayPause", "SlideShow.Restart", "SlideShow.Stop",
	                      "SlideShow.NextSlide", "SlideShow.PreviousSlide"};
	for (const char *key : keys)
		obs_hotkey_register_source(source, key, obs_module_text(key), hotkey, s);
	auto *handler = obs_source_get_proc_handler(source);
	proc_handler_add(handler, "void get_texts(ptr texts)", get_texts, s);
	proc_handler_add(handler, "void get_status(out int count, out int revision, out int index, out int acknowledged, out string error, out string group_uuid, out int step, out int steps, out bool points, out int width, out int height, out bool ready)",
		get_status, s);
	proc_handler_add(handler, "void dock_transition(int index, out int serial)", dock_transition, s);
	proc_handler_add(handler, "void reload_files()", reload_files, s);
	obs_source_update(source, settings);
	return s;
}

static bool is_native_text(obs_source_t *source)
{
	if (!source || obs_source_removed(source))
		return false;
	const char *id = obs_source_get_unversioned_id(source);
	return strcmp(id, "text_gdiplus") == 0 || strcmp(id, "text_ft2_source") == 0;
}

const char *textppt_input_mode(obs_data_t *settings)
{
	if (obs_data_has_user_value(settings, S_INPUT))
		return obs_data_get_string(settings, S_INPUT);
	if (obs_data_get_bool(settings, S_READ_SINGLE_FILE))
		return INPUT_FILE;
	if (obs_data_get_bool(settings, S_READ_MULTIPLE_FILES))
		return INPUT_FILES;
	return INPUT_CUSTOM;
}

static void clear_prepared(textppt_source *s)
{
	obs_source_release(s->prepared);
	s->prepared = nullptr;
	s->prepare_ticks = 0;
	s->measuring_page = false;
}

bool textppt_can_reveal(obs_data_t *settings)
{
	const char *mode = textppt_input_mode(settings);
	return strcmp(obs_data_get_string(settings, S_MODE), S_MODE_MANUAL) == 0 &&
		strcmp(mode, INPUT_SOURCES) != 0 && strcmp(mode, INPUT_GROUPS) != 0 &&
		!obs_data_get_bool(settings, "vertical") && !obs_data_get_bool(settings, "chatlog") &&
		!obs_data_get_bool(settings, "log_mode");
}

static bool revealing(const textppt_source *s) { return s->progressive && s->manual; }

static void prepare_reveal(textppt_source *s)
{
	if (!revealing(s) || s->pages.empty())
		return;
	if (s->reveal_page != s->cur_item) {
		s->reveal_text = s->pages[s->cur_item].text;
		s->reveal_ends = text_reveal_ends(s->reveal_text, s->points);
		if (s->reveal_text.find('\t') != string::npos) {
			auto *settings = obs_source_get_settings(s->buffers[s->buffer]);
			s->reveal_text = textppt_render_text(s->reveal_text, settings, &s->reveal_ends);
			obs_data_release(settings);
		}
		// Cache the full-page layout until content or style changes.
		s->reveal_page = s->cur_item;
		s->reveal_width = s->reveal_height = 0;
	}
	s->reveal_step = std::clamp(s->reveal_step, size_t{1}, s->reveal_ends.size());
}

static void set_reveal_text(textppt_source *s, obs_source_t *buffer)
{
	auto *settings = obs_source_get_settings(buffer);
	const auto text = s->reveal_text.substr(0, s->reveal_ends[s->reveal_step - 1]);
	obs_data_set_string(settings, "text", text.c_str());
#ifdef _WIN32
	// Fixed native bounds also keep alignment and gradients stable.
	const bool wrapped = obs_data_get_bool(settings, "extents") && obs_data_get_bool(settings, "extents_wrap");
	obs_data_set_bool(settings, "extents", true);
	obs_data_set_bool(settings, "extents_wrap", wrapped);
	obs_data_set_int(settings, "extents_cx", s->reveal_width);
	obs_data_set_int(settings, "extents_cy", s->reveal_height);
	obs_data_set_string(settings, "valign", "top");
#endif
	obs_source_update(buffer, settings);
	obs_data_release(settings);
	textppt_text_layout(buffer, s->reveal_width, s->reveal_height);
	s->prepared_step = s->reveal_step;
	s->prepare_ticks = 2;
}

static void clear_buffers(textppt_source *s)
{
	clear_prepared(s);
	for (auto *&buffer : s->buffers) {
		obs_source_release(buffer);
		buffer = nullptr;
	}
}

void textppt_destroy(void *data)
{
	auto *s = static_cast<textppt_source *>(data);
	if (s->transition) {
		obs_source_remove_active_child(s->source, s->transition);
		obs_source_release(s->transition);
	}
	clear_buffers(s);
	obs_enter_graphics();
	gs_texrender_destroy(s->group_layout);
	obs_leave_graphics();
	delete s;
}

static void show_page(textppt_source *s, bool cut = false, bool first_line = true)
{
	if (!s->transition || s->pages.empty())
		return;
	s->ready = false;
	s->cur_item = std::min(s->cur_item, s->pages.size() - 1);
	if (first_line)
		s->reveal_step = 1;
	prepare_reveal(s);
	{
		std::lock_guard<std::mutex> lock(s->snapshot_mutex);
		s->current = s->cur_item;
		s->snapshot_step = revealing(s) ? s->reveal_step : 0;
		s->snapshot_steps = revealing(s) ? s->reveal_ends.size() : 0;
		s->snapshot_points = s->points;
	}
	s->page_pending = true;
	s->pending_cut |= cut;
	s->stopped = false;
	s->state = s->manual ? OBS_MEDIA_STATE_PAUSED : OBS_MEDIA_STATE_PLAYING;
}

static void update_size(textppt_source *s, obs_source_t *next = nullptr, bool force = false, bool reset = false)
{
	uint32_t width = reset ? 1u : s->cx.load(), height = reset ? 1u : s->cy.load();
	// Grow automatic bounds only for visited pages.
	if (next) {
		width = std::max(width, obs_source_get_width(next));
		height = std::max(height, obs_source_get_height(next));
	}
	if (s->fixed_width && s->fixed_height) {
		width = s->fixed_width;
		height = s->fixed_height;
	} else if (s->aspect_ratio > 0.0) {
		const double ratio = s->aspect_ratio;
		if (static_cast<double>(width) / height < ratio)
			width = static_cast<uint32_t>(std::min(8192.0, height * ratio));
		else
			height = static_cast<uint32_t>(std::min(8192.0, width / ratio));
	}
	width = std::clamp(width, 1u, 8192u);
	height = std::clamp(height, 1u, 8192u);
	if (force || width != s->cx.load() || height != s->cy.load()) {
		s->cx = width;
		s->cy = height;
		obs_transition_set_size(s->transition, width, height);
	}
}

static void advance_page(textppt_source *s)
{
	if (!s->page_pending)
		return;
	// Never modify the outgoing buffer while its transition is still visible.
	if (!s->pending_cut && s->transition_running)
		return;
	if (s->prepared && s->prepared_item != s->cur_item)
		clear_prepared(s);
	if (s->prepared && revealing(s) && !s->measuring_page && s->prepared_step != s->reveal_step)
		clear_prepared(s);
	if (!s->prepared) {
		const auto &page = s->pages[s->cur_item];
		if (!page.source_uuid.empty()) {
			s->prepared = obs_get_source_by_uuid(page.source_uuid.c_str());
			bool valid = is_native_text(s->prepared);
			if (s->group_mode)
				valid = text_group_safe(s->prepared, s->source) && text_group_present(s->prepared);
			if (!valid) {
				clear_prepared(s);
				blog(LOG_WARNING, "[obs-textppt] Referenced text source unavailable: %s", page.source_name.c_str());
				obs_transition_set(s->transition, nullptr);
				s->current = s->cur_item;
				s->page_pending = false;
				s->pending_cut = false;
				s->transition_running = false;
				s->elapsed = 0.0f;
				return;
			}
			if (s->group_mode)
				text_group_update_layout(s, s->prepared);
		} else {
			if (s->pending_cut) {
				auto *current = obs_transition_get_active_source(s->transition);
				obs_transition_set(s->transition, current);
				obs_source_release(current);
				s->transition_running = false;
			}
			auto *buffer = s->buffers[1 - s->buffer];
			// Reuse measured bounds, TAB columns and the native glyph cache.
			if (revealing(s) && s->reveal_width && s->reveal_height) {
				buffer = s->buffers[s->buffer];
				s->prepared = obs_source_get_ref(buffer);
				s->prepared_item = s->cur_item;
				set_reveal_text(s, buffer);
				return;
			}
			auto *settings = obs_source_get_settings(buffer);
			if (s->progressive) {
				// Restore native settings before measuring the complete page.
				auto *style = obs_data_create_from_json(s->native_style.c_str());
				obs_data_apply(settings, style);
				obs_data_release(style);
				textppt_text_layout(buffer, 0, 0);
			}
			const bool gdiplus = strcmp(obs_source_get_unversioned_id(buffer), "text_gdiplus") == 0;
			string expanded;
			if (revealing(s))
				expanded = s->reveal_text;
			else if (page.text.find('\t') != string::npos)
				expanded = textppt_render_text(page.text, settings);
			obs_data_set_string(settings, "text", expanded.empty() ? page.text.c_str() : expanded.c_str());
			obs_data_set_bool(settings, gdiplus ? "read_from_file" : "from_file", false);
			obs_data_set_string(settings, gdiplus ? "file" : "text_file", "");
			obs_source_update(buffer, settings);
			obs_data_release(settings);
			s->prepared = obs_source_get_ref(buffer);
			// OBS defers video-source updates; wait before starting the transition.
			s->prepare_ticks = 2;
			if (revealing(s)) {
				s->measuring_page = true;
				s->prepared_step = s->reveal_step;
			}
		}
		s->prepared_item = s->cur_item;
	}
	if (s->prepare_ticks > 0) {
		--s->prepare_ticks;
		return;
	}
	if (revealing(s) && s->measuring_page) {
		s->reveal_width = obs_source_get_base_width(s->prepared);
		s->reveal_height = obs_source_get_base_height(s->prepared);
		set_reveal_text(s, s->prepared);
		s->measuring_page = false;
		return;
	}
	// Recalculate style bounds only after the native text update finishes.
	bool reset_size = s->size_pending;
	if (s->style_pending) {
		// Color and opacity edits retain the canvas when text dimensions match.
		reset_size |= s->style_width != obs_source_get_width(s->prepared) ||
			s->style_height != obs_source_get_height(s->prepared);
	}
	update_size(s, s->prepared, false, reset_size);
	s->size_pending = false;
	s->style_pending = false;
	if (s->pending_cut) {
		obs_transition_set(s->transition, s->prepared);
		s->transition_running = false;
	} else {
		s->transition_running = obs_transition_start(s->transition, OBS_TRANSITION_MODE_AUTO,
			s->tr_speed, s->prepared);
	}
	if (s->prepared == s->buffers[1 - s->buffer])
		s->buffer = 1 - s->buffer;
	s->current = s->cur_item;
	s->elapsed = 0.0f;
	s->page_pending = false;
	s->pending_cut = false;
	clear_prepared(s);
	s->ready = true;
}

void textppt_update(void *data, obs_data_t *settings, obs_data_t *style, text_source_create create_text,
                    set_text_alignment align)
{
	auto *s = static_cast<textppt_source *>(data);
	s->ready = false;
	const bool first_update = s->revision == 0;
	obs_data_set_string(settings, "_textppt_instance", obs_source_get_uuid(s->source));
	vector<text_data> pages;
	const auto failed_file = [&](const char *key, const char *path) {
		blog(LOG_WARNING, "[obs-textppt] %s: %s", obs_module_text(key), path);
		std::lock_guard<std::mutex> lock(s->snapshot_mutex);
		s->file_error = string(obs_module_text(key)) + '\n' + path;
	};
	const auto add = [&](const char *path, const char *text, const char *uuid = "", const char *name = "") {
		pages.push_back({path ? path : "", text ? text : "", uuid, name});
	};
	const char *mode = textppt_input_mode(settings);
	const bool group_mode = strcmp(mode, INPUT_GROUPS) == 0;
	const bool file_mode = strcmp(mode, INPUT_FILE) == 0 || strcmp(mode, INPUT_FILES) == 0;
	if (!file_mode) {
		std::lock_guard<std::mutex> lock(s->snapshot_mutex);
		s->file_error.clear();
	}
	string file_input;
	if (file_mode) {
		auto *input = obs_data_create();
		obs_data_set_string(input, S_INPUT, mode);
		if (strcmp(mode, INPUT_FILE) == 0) {
			obs_data_set_string(input, S_TXT_FILE, obs_data_get_string(settings, S_TXT_FILE));
			obs_data_set_bool(input, S_CUSTOM_DELIM, obs_data_get_bool(settings, S_CUSTOM_DELIM));
			obs_data_set_string(input, S_DELIM, obs_data_get_string(settings, S_DELIM));
		} else {
			auto *files = obs_data_get_array(settings, S_FILES);
			obs_data_set_array(input, S_FILES, files);
			obs_data_array_release(files);
		}
		file_input = obs_data_get_json(input);
		obs_data_release(input);
	}
	const bool force_reload = s->force_file_reload.exchange(false);
	const bool reuse_files = file_mode && file_input == s->file_input && !force_reload;
	if (reuse_files) {
		// Reuse cached files for style and playback edits.
		pages = s->pages;
	} else if (group_mode) {
		text_groups_read(settings, pages);
	} else if (strcmp(mode, INPUT_FILE) == 0) {
		const char *path = obs_data_get_string(settings, S_TXT_FILE);
		vector<string> texts;
		string input;
		const char *error;
		const char *delimiter = obs_data_get_bool(settings, S_CUSTOM_DELIM) ?
			obs_data_get_string(settings, S_DELIM) : "";
		if (*path) {
			if (!load_text_from_file(input, path, error)) {
				failed_file(error, path);
				return;
			}
			text_error reason;
			if (!parse_slides(input, delimiter, texts, reason)) {
				failed_file(text_error_key(reason), path);
				return;
			}
		}
		for (auto &text : texts)
			pages.push_back({"", std::move(text), "", ""});
	} else if (strcmp(mode, INPUT_FILES) == 0) {
		bool limited = false;
		auto *files = obs_data_get_array(settings, S_FILES);
		size_t i = 0;
		for (; i < obs_data_array_count(files) && pages.size() < 1000; ++i) {
			auto *item = obs_data_array_item(files, i);
			const char *path = obs_data_get_string(item, "value");
			if (auto *directory = os_opendir(path)) {
				vector<string> names;
				const size_t limit = 1000 - pages.size();
				// Keep the first sorted paths without storing the entire directory.
				while (auto *entry = os_readdir(directory)) {
					const char *extension = os_get_path_extension(entry->d_name);
					if (entry->directory || !extension || astrcmpi(extension, ".txt") != 0)
						continue;
					string name = string(path) + "/" + entry->d_name;
					if (names.size() < limit) {
						names.push_back(std::move(name));
						std::push_heap(names.begin(), names.end());
					} else {
						limited = true;
						if (name < names.front()) {
							std::pop_heap(names.begin(), names.end());
							names.back() = std::move(name);
							std::push_heap(names.begin(), names.end());
						}
					}
				}
				os_closedir(directory);
				std::sort_heap(names.begin(), names.end());
				for (const auto &name : names)
					add(name.c_str(), nullptr);
			} else if (os_file_exists(path))
				add(path, nullptr);
			else {
				failed_file("File.ReadError", path);
				obs_data_release(item);
				obs_data_array_release(files);
				return;
			}
			obs_data_release(item);
		}
		if (limited || i < obs_data_array_count(files))
			blog(LOG_WARNING, "[obs-textppt] Page limit reached (1000)");
		obs_data_array_release(files);
		// Replace the deck only after every file succeeds.
		size_t bytes = 0;
		for (auto &page : pages) {
			const char *error;
			if (!load_text_from_file(page.text, page.file_path.c_str(), error)) {
				failed_file(error, page.file_path.c_str());
				return;
			}
			text_error reason;
			if (!validate_text(page.text, reason)) {
				failed_file(text_error_key(reason), page.file_path.c_str());
				return;
			}
			bytes += page.text.size();
			if (bytes > 4 * 1024 * 1024) {
				failed_file("File.TooLarge", page.file_path.c_str());
				return;
			}
		}
	} else if (strcmp(mode, INPUT_SOURCES) == 0) {
		auto *sources = obs_data_get_array(settings, S_SOURCES);
		auto *ids = obs_data_get_obj(settings, S_SOURCE_IDS);
		for (size_t i = 0; i < std::min(obs_data_array_count(sources), size_t{1000}); ++i) {
			auto *item = obs_data_array_item(sources, i);
			const char *name = obs_data_get_string(item, "value");
			const char *uuid = obs_data_get_string(ids, obs_data_get_string(item, "uuid"));
			if (!*uuid)
				uuid = obs_data_get_string(ids, name);
			auto *source = *uuid ? obs_get_source_by_uuid(uuid) : obs_get_source_by_name(name);
			if (is_native_text(source)) {
				add(nullptr, nullptr, obs_source_get_uuid(source), obs_source_get_name(source));
			} else {
				// Preserve page order for a deleted reference, and display a blank page.
				add(nullptr, nullptr, *uuid ? uuid : "missing", name);
			}
			obs_source_release(source);
			obs_data_release(item);
		}
		obs_data_release(ids);
		obs_data_array_release(sources);
	} else if (obs_data_has_user_value(settings, S_CUSTOM_TEXT)) {
		vector<string> texts;
		text_error error;
		const char *delimiter = obs_data_get_bool(settings, S_CUSTOM_DELIM) ?
			obs_data_get_string(settings, S_DELIM) : "";
		if (!parse_slides(obs_data_get_string(settings, S_CUSTOM_TEXT), delimiter, texts, error)) {
			blog(LOG_WARNING, "[obs-textppt] %s", obs_module_text(text_error_key(error)));
			return;
		}
		for (auto &text : texts)
			pages.push_back({"", std::move(text), "", ""});
	} else {
		// Load saved text arrays when custom text is absent.
		auto *texts = obs_data_get_array(settings, S_TEXTS);
		size_t bytes = 0;
		for (size_t i = 0; i < std::min(obs_data_array_count(texts), size_t{1000}); ++i) {
			auto *item = obs_data_array_item(texts, i);
			string text = obs_data_get_string(item, "value");
			obs_data_release(item);
			text_error error;
			if (!validate_text(text, error)) {
				blog(LOG_WARNING, "[obs-textppt] %s", obs_module_text(text_error_key(error)));
				obs_data_array_release(texts);
				return;
			}
			bytes += text.size();
			if (bytes > 4 * 1024 * 1024) {
				blog(LOG_WARNING, "[obs-textppt] %s", obs_module_text("File.TooLarge"));
				obs_data_array_release(texts);
				return;
			}
			pages.push_back({"", std::move(text), "", ""});
		}
		obs_data_array_release(texts);
	}
	// Retain position when content and order match.
	const bool keep_position = !pages.empty() && group_mode == s->group_mode &&
		std::equal(pages.begin(), pages.end(), s->pages.begin(), s->pages.end(),
			[](const text_data &a, const text_data &b) {
				return a.file_path == b.file_path && a.text == b.text && a.source_uuid == b.source_uuid;
			});
	const bool restoring = !keep_position && s->initial_page != SIZE_MAX && !pages.empty();
	const size_t current_page = keep_position ? s->cur_item : restoring ? std::min(s->initial_page, pages.size() - 1) : 0;
	if (restoring)
		s->preserve_first_activation = true;
	const auto previous_state = s->state.load();
	const bool previous_manual = s->manual;
	const bool configured_manual = strcmp(obs_data_get_string(settings, S_MODE), S_MODE_MANUAL) == 0;
	const bool points = obs_data_get_bool(settings, S_POINTS);
	const bool progressive = (points || obs_data_get_bool(settings, S_PROGRESSIVE)) && textppt_can_reveal(settings);
	const bool progressive_changed = progressive != s->progressive || points != s->points;
	const bool keep_state = keep_position && configured_manual == s->configured_manual;
	const bool refreshing_style = s->style_pending && s->prepared &&
		s->prepared == s->buffers[s->buffer] && s->prepared_item == s->cur_item;
	const bool displayed_page = (!s->page_pending || refreshing_style) && !s->stopped;
	const bool previous_transition = s->transition_running;
	const float previous_elapsed = s->elapsed;
	const string native_style = obs_data_get_json(style);
	const bool style_changed = s->native_style != native_style;
	const bool reuse_buffers = s->buffers[0] && s->buffers[1];
	obs_source_t *buffers[2] = {};
	if (!group_mode && strcmp(mode, INPUT_SOURCES) != 0 && !pages.empty()) {
		// Keep each native renderer's font state across style changes.
		for (size_t i = 0; i < 2; ++i)
			buffers[i] = reuse_buffers ? obs_source_get_ref(s->buffers[i]) : create_text(style);
		if (!buffers[0] || !buffers[1]) {
			for (auto *buffer : buffers)
				obs_source_release(buffer);
			blog(LOG_ERROR, "[obs-textppt] Cannot create native text buffers");
			return;
		}
	}
	const char *kind = obs_data_get_string(settings, S_TRANSITION);
	const char *name = strcmp(kind, TR_CUT) == 0 ? "cut_transition" :
		strcmp(kind, TR_SWIPE) == 0 ? "swipe_transition" :
		strcmp(kind, TR_SLIDE) == 0 ? "slide_transition" : "fade_transition";
	const bool transition_changed = !s->transition || s->tr_name != name;
	if (transition_changed) {
		auto *next = obs_source_create_private(name, "OBS-TextPPT transition", nullptr);
		if (!next) {
			for (auto *buffer : buffers)
				obs_source_release(buffer);
			blog(LOG_ERROR, "[obs-textppt] Cannot create transition: %s", name);
			return;
		}
		// Keep referenced media active while replacing its transition.
		if (keep_position && s->transition) {
			auto *current = obs_transition_get_active_source(s->transition);
			obs_transition_set(next, current);
			obs_source_release(current);
		}
		obs_source_add_active_child(s->source, next);
		obs_source_t *old;
		{
			std::lock_guard<std::mutex> lock(s->transition_mutex);
			old = s->transition;
			s->transition = next;
		}
		if (old) {
			obs_source_remove_active_child(s->source, old);
			obs_source_release(old);
		}
		s->tr_name = name;
	}
	if (s->transition && !keep_position)
		obs_transition_set(s->transition, nullptr);
	if (reuse_buffers && buffers[0] && (style_changed || progressive_changed)) {
		if (!s->style_pending) {
			auto *current = obs_transition_get_active_source(s->transition);
			s->style_width = obs_source_get_width(current);
			s->style_height = obs_source_get_height(current);
			obs_source_release(current);
		}
		for (auto *buffer : buffers) {
			obs_source_update(buffer, style);
			if (!progressive)
				textppt_text_layout(buffer, 0, 0);
		}
	}
	clear_buffers(s);
	s->group_mode = group_mode;
	s->group_scan_elapsed = 0.0f;
	std::copy(std::begin(buffers), std::end(buffers), std::begin(s->buffers));
	s->native_style = buffers[0] ? native_style : "";
	s->file_input = std::move(file_input);
	if (!reuse_buffers)
		s->buffer = 0;
	s->pages = std::move(pages);
	s->manual = keep_state ? previous_manual : configured_manual;
	s->configured_manual = configured_manual;
	s->progressive = progressive;
	s->points = points;
	if (!keep_position || style_changed || progressive_changed) {
		s->reveal_page = SIZE_MAX;
		s->reveal_text.clear();
		s->reveal_ends.clear();
	}
	s->loop = obs_data_get_bool(settings, S_LOOP);
	s->hide = obs_data_get_bool(settings, S_HIDE);
	s->randomize = obs_data_get_bool(settings, S_RANDOMIZE);
	s->slide_time = static_cast<float>(std::clamp<int64_t>(obs_data_get_int(settings, S_SLIDE_TIME), 50, 3600000)) / 1000.0f;
	s->tr_speed = strcmp(kind, TR_CUT) == 0 ? 0 :
		static_cast<uint32_t>(std::clamp<int64_t>(obs_data_get_int(settings, S_TR_SPEED), 0, 60000));
	// Cache canvas bounds for rendering.
	const auto previous_width = s->fixed_width, previous_height = s->fixed_height;
	const auto previous_ratio = s->aspect_ratio;
	const char *bounds = obs_data_get_string(settings, S_CUSTOM_SIZE);
	unsigned x = 0, y = 0;
	s->fixed_width = s->fixed_height = 0;
	s->aspect_ratio = 0.0;
	if (sscanf(bounds, "%ux%u", &x, &y) == 2 && x && y) {
		s->fixed_width = x;
		s->fixed_height = y;
	} else if (sscanf(bounds, "%u:%u", &x, &y) == 2 && x && y) {
		s->aspect_ratio = static_cast<double>(x) / y;
	}
	s->style_pending |= buffers[0] && style_changed;
	s->size_pending |= previous_width != s->fixed_width || previous_height != s->fixed_height ||
		previous_ratio != s->aspect_ratio;
	const char *visibility = obs_data_get_string(settings, S_BEHAVIOR);
	s->behavior = strcmp(visibility, S_BEHAVIOR_STOP_RESTART) == 0 ? BEHAVIOR_STOP_RESTART :
		strcmp(visibility, S_BEHAVIOR_PAUSE_UNPAUSE) == 0 ? BEHAVIOR_PAUSE_UNPAUSE : BEHAVIOR_ALWAYS_PLAY;
	s->cur_item = current_page;
	if (!keep_position) {
		if (!restoring) {
			s->cx = 1;
			s->cy = 1;
		}
		s->size_pending = false;
		s->style_pending = false;
	}
	s->stopped = keep_state && previous_state == OBS_MEDIA_STATE_STOPPED;
	s->elapsed = keep_state && reuse_buffers && displayed_page && !transition_changed ? previous_elapsed : 0.0f;
	s->page_pending = false;
	s->pending_cut = false;
	s->transition_running = keep_position && reuse_buffers && displayed_page && !transition_changed && previous_transition;
	// Retain commands received before the first native update.
	if (!keep_position && !first_update) {
		std::lock_guard<std::mutex> lock(s->command_mutex);
		s->commands.clear();
	}
	if (s->transition) {
		align(s->transition, settings);
		// Fit oversized pages without enlarging smaller text.
		obs_transition_set_scale_type(s->transition, s->group_mode ?
			OBS_TRANSITION_SCALE_ASPECT : OBS_TRANSITION_SCALE_MAX_ONLY);
		update_size(s, nullptr, true);
	}
	prepare_reveal(s);
	{
		vector<string> snapshot;
		vector<string> groups;
		for (const auto &page : s->pages) {
			snapshot.push_back(!page.source_uuid.empty() ? page.source_name :
				page.file_path.empty() ? page.text : page.file_path);
			if (s->group_mode)
				groups.push_back(page.source_uuid);
		}
		std::lock_guard<std::mutex> lock(s->snapshot_mutex);
		// Publish the page index and group IDs together.
		s->current = current_page;
		s->snapshot_step = revealing(s) ? s->reveal_step : 0;
		s->snapshot_steps = revealing(s) ? s->reveal_ends.size() : 0;
		s->snapshot_points = s->points;
		s->snapshot_groups = std::move(groups);
		if (!reuse_files)
			s->file_error.clear();
		// Style changes must not rebuild the Dock or cancel page-number input.
		if (snapshot != s->snapshot || !keep_position) {
			s->snapshot = std::move(snapshot);
			++s->revision;
		}
	}
	// Retain the displayed source for playback-only changes.
	auto *active_source = obs_transition_get_active_source(s->transition);
	const bool retained_text = keep_position && reuse_buffers && displayed_page && !s->progressive && !progressive_changed &&
		active_source && active_source == s->buffers[s->buffer];
	const bool retained_reference = keep_position && active_source &&
		!s->pages[current_page].source_uuid.empty() &&
		s->pages[current_page].source_uuid == obs_source_get_uuid(active_source) &&
		(!s->group_mode || (text_group_safe(active_source, s->source) && text_group_present(active_source)));
	if ((retained_text || retained_reference) && s->size_pending && !s->style_pending) {
		update_size(s, active_source, false, true);
		s->size_pending = false;
	}
	obs_source_release(active_source);
	if (s->pages.empty())
		s->state = OBS_MEDIA_STATE_STOPPED;
	else if (keep_state && (previous_state == OBS_MEDIA_STATE_STOPPED ||
	                       (previous_state == OBS_MEDIA_STATE_ENDED && s->hide))) {
		obs_transition_set(s->transition, nullptr);
		s->state = previous_state;
	} else if (retained_reference || retained_text) {
		if (retained_text && s->style_pending) {
			const auto &text = s->pages[current_page].text;
			if (text.find('\t') != string::npos) {
				auto *current = obs_source_get_settings(s->buffers[s->buffer]);
				const auto expanded = textppt_render_text(text, current);
				if (expanded != obs_data_get_string(current, "text")) {
					obs_data_set_string(current, "text", expanded.c_str());
					obs_source_update(s->buffers[s->buffer], current);
				}
				obs_data_release(current);
			}
			// Wait for native style updates without rewriting the displayed text.
			s->prepared = obs_source_get_ref(s->buffers[s->buffer]);
			s->prepared_item = current_page;
			s->prepare_ticks = 2;
			s->page_pending = true;
			s->pending_cut = true;
		}
		s->state = keep_state ? previous_state :
			s->manual ? OBS_MEDIA_STATE_PAUSED : OBS_MEDIA_STATE_PLAYING;
		if (!keep_state)
			obs_source_media_started(s->source);
	} else {
		show_page(s, true, !restoring && (!keep_position || progressive_changed));
		if (keep_state)
			s->state = previous_state;
		else
			obs_source_media_started(s->source);
	}
	if (!s->pages.empty())
		s->initial_page = SIZE_MAX;
	s->ready = !s->page_pending;
}

void textppt_video_tick(void *data, float seconds)
{
	auto *s = static_cast<textppt_source *>(data);
	if (!s->transition)
		return;
	// Finish text-only transitions before reusing the outgoing buffer.
	if (s->transition_running && obs_transition_get_time(s->transition) >= 1.0f) {
		if (!s->group_mode) {
			auto *current = obs_transition_get_active_source(s->transition);
			obs_transition_set(s->transition, current);
			obs_source_release(current);
		}
		s->transition_running = false;
	}
	const bool active = s->active.load();
	if (active && !s->was_active) {
		if (s->group_mode)
			s->group_scan_elapsed = 0.5f;
		// Keep a copied page on first activation; later activations follow visibility settings.
		if (s->behavior == BEHAVIOR_STOP_RESTART && !s->preserve_first_activation) {
			s->cur_item = 0;
			show_page(s, true);
		}
		s->preserve_first_activation = false;
	}
	s->was_active = active;
	std::deque<queued_command> commands;
	{
		std::lock_guard<std::mutex> lock(s->command_mutex);
		commands.swap(s->commands);
	}
	for (const auto &command : commands) {
		if (s->pages.empty()) {
			s->acknowledged = command.serial;
			continue;
		}
		switch (command.type) {
		case slide_command::Jump:
			s->cur_item = std::min(command.value, s->pages.size() - 1);
			show_page(s);
			break;
		case slide_command::Next:
			prepare_reveal(s);
			if (revealing(s) && s->reveal_step < s->reveal_ends.size()) {
				++s->reveal_step;
				show_page(s, true, false);
			} else if (s->cur_item + 1 < s->pages.size()) {
				++s->cur_item;
				show_page(s);
			}
			break;
		case slide_command::Previous:
			if (revealing(s) && s->reveal_step > 1) {
				--s->reveal_step;
				show_page(s, true, false);
			} else if (s->cur_item > 0) {
				--s->cur_item;
				s->reveal_step = revealing(s) ? SIZE_MAX : 1;
				show_page(s, false, false);
			}
			break;
		case slide_command::Restart:
			s->cur_item = 0;
			show_page(s, true);
			break;
		case slide_command::Stop:
			clear_prepared(s);
			s->page_pending = false;
			s->pending_cut = false;
			s->transition_running = false;
			if (s->transition)
				obs_transition_set(s->transition, nullptr);
			s->stopped = true;
			s->state = OBS_MEDIA_STATE_STOPPED;
			s->ready = true;
			s->elapsed = 0.0f;
			break;
		case slide_command::Pause:
			if (s->progressive && s->manual != (command.value != 0)) {
				clear_prepared(s);
				s->manual = command.value != 0;
				show_page(s, true);
			}
			s->manual = command.value != 0;
			s->state = s->manual ? OBS_MEDIA_STATE_PAUSED : OBS_MEDIA_STATE_PLAYING;
			s->stopped = false;
			if (!s->page_pending && !s->prepared) {
				auto *current = obs_transition_get_active_source(s->transition);
				if (!current)
					show_page(s, true);
				obs_source_release(current);
			}
			break;
		}
		s->acknowledged = command.serial;
	}
	advance_page(s);
	if (s->group_mode)
		text_groups_refresh(s, seconds);
	if (s->pages.empty() || s->page_pending || s->manual || s->stopped ||
	    (s->behavior != BEHAVIOR_ALWAYS_PLAY && !active) ||
	    s->state.load() == OBS_MEDIA_STATE_ENDED)
		return;
	s->elapsed += seconds;
	if (s->elapsed < s->slide_time + static_cast<float>(s->tr_speed) / 1000.0f)
		return;
	if (!s->loop && s->cur_item + 1 == s->pages.size()) {
		if (s->hide)
			s->transition_running = obs_transition_start(s->transition, OBS_TRANSITION_MODE_AUTO,
				s->tr_speed, nullptr);
		s->state = OBS_MEDIA_STATE_ENDED;
		obs_source_media_ended(s->source);
		return;
	}
	if (s->randomize && s->pages.size() > 1)
		s->cur_item = (s->cur_item + 1 + static_cast<size_t>(rand()) % (s->pages.size() - 1)) % s->pages.size();
	else
		s->cur_item = (s->cur_item + 1) % s->pages.size();
	show_page(s);
}

void textppt_video_render(void *data, gs_effect_t *)
{
	auto *s = static_cast<textppt_source *>(data);
	if (s->updating_group_layout)
		return;
	if (auto *transition = get_transition(s)) {
		if (auto *current = obs_transition_get_active_source(transition)) {
			if (obs_source_removed(current))
				obs_transition_set(transition, nullptr);
			else
				update_size(s, current);
			obs_source_release(current);
		}
		obs_source_video_render(transition);
		obs_source_release(transition);
	}
}
bool textppt_audio_render(void *data, uint64_t *ts_out, obs_source_audio_mix *audio_output,
                          uint32_t mixers, size_t channels, size_t sample_rate)
{
	UNUSED_PARAMETER(sample_rate);
	auto *transition = get_transition(static_cast<textppt_source *>(data));
	if (!transition)
		return false;
	const uint64_t timestamp = obs_source_get_audio_timestamp(transition);
	const bool ready = timestamp && !obs_source_audio_pending(transition);
	if (ready) {
		obs_source_audio_mix audio = {};
		obs_source_get_audio_mix(transition, &audio);
		for (size_t mix = 0; mix < MAX_AUDIO_MIXES; ++mix)
			if (mixers & (1u << mix))
				for (size_t channel = 0; channel < channels; ++channel)
					memcpy(audio_output->output[mix].data[channel], audio.output[mix].data[channel],
					       AUDIO_OUTPUT_FRAMES * sizeof(float));
		*ts_out = timestamp;
	}
	obs_source_release(transition);
	return ready;
}

void textppt_enum_sources(void *data, obs_source_enum_proc_t callback, void *param)
{
	auto *s = static_cast<textppt_source *>(data);
	if (auto *transition = get_transition(s)) {
		callback(s->source, transition, param);
		obs_source_release(transition);
	}
}
void textppt_activate(void *data) { static_cast<textppt_source *>(data)->active = true; }
void textppt_deactivate(void *data) { static_cast<textppt_source *>(data)->active = false; }
uint32_t textppt_width(void *data) { return static_cast<textppt_source *>(data)->cx.load(); }
uint32_t textppt_height(void *data) { return static_cast<textppt_source *>(data)->cy.load(); }
void textppt_play_pause(void *data, bool pause) { enqueue(static_cast<textppt_source *>(data), slide_command::Pause, pause); }
void textppt_restart(void *data) { enqueue(static_cast<textppt_source *>(data), slide_command::Restart); }
void textppt_stop(void *data) { enqueue(static_cast<textppt_source *>(data), slide_command::Stop); }
void textppt_next_slide(void *data) { enqueue(static_cast<textppt_source *>(data), slide_command::Next); }
void textppt_previous_slide(void *data) { enqueue(static_cast<textppt_source *>(data), slide_command::Previous); }
obs_media_state textppt_get_state(void *data) { return static_cast<textppt_source *>(data)->state.load(); }
bool textppt_reload(void *param, obs_source_t *source)
{
	if (strcmp(obs_source_get_unversioned_id(source), static_cast<const char *>(param)) == 0) {
		auto *settings = obs_source_get_settings(source);
		const char *mode = textppt_input_mode(settings);
		// Only references depend on other sources finishing their load.
		if (strcmp(mode, INPUT_SOURCES) == 0 || strcmp(mode, INPUT_GROUPS) == 0)
			obs_source_update(source, settings);
		obs_data_release(settings);
	}
	return true;
}

static void missing_file_resolved(void *src, const char *path, void *index)
{
	auto *source = static_cast<obs_source_t *>(src);
	auto *settings = obs_source_get_settings(source);
	const size_t encoded = reinterpret_cast<uintptr_t>(index);
	if (!encoded) {
		obs_data_set_string(settings, S_TXT_FILE, path);
	} else {
		auto *files = obs_data_get_array(settings, S_FILES);
		if (encoded - 1 < obs_data_array_count(files)) {
			auto *item = obs_data_array_item(files, encoded - 1);
			obs_data_set_string(item, "value", path);
			obs_data_release(item);
			obs_data_set_array(settings, S_FILES, files);
		}
		obs_data_array_release(files);
	}
	obs_source_update(source, settings);
	obs_data_release(settings);
}

obs_missing_files_t *textppt_missing_files(void *data)
{
	auto *s = static_cast<textppt_source *>(data);
	auto *missing = obs_missing_files_create();
	auto *settings = obs_source_get_settings(s->source);
	const auto add = [&](const char *path, size_t index) {
		if (path && *path && !os_file_exists(path))
			obs_missing_files_add_file(missing, obs_missing_file_create(
				path, missing_file_resolved, OBS_MISSING_FILE_SOURCE, s->source,
				reinterpret_cast<void *>(index)));
	};
	if (strcmp(textppt_input_mode(settings), INPUT_FILE) == 0)
		add(obs_data_get_string(settings, S_TXT_FILE), 0);
	else if (strcmp(textppt_input_mode(settings), INPUT_FILES) == 0) {
		auto *files = obs_data_get_array(settings, S_FILES);
		for (size_t i = 0; i < obs_data_array_count(files); ++i) {
			auto *item = obs_data_array_item(files, i);
			add(obs_data_get_string(item, "value"), i + 1);
			obs_data_release(item);
		}
		obs_data_array_release(files);
	}
	obs_data_release(settings);
	return missing;
}
void textppt_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_TRANSITION, "fade");
	obs_data_set_default_int(settings, S_SLIDE_TIME, 5000);
	obs_data_set_default_int(settings, S_TR_SPEED, 300);
	obs_data_set_default_string(settings, S_CUSTOM_SIZE, "auto");
	obs_data_set_default_string(settings, S_BEHAVIOR,
				    S_BEHAVIOR_ALWAYS_PLAY);
	obs_data_set_default_string(settings, S_MODE, S_MODE_MANUAL);
	obs_data_set_default_bool(settings, S_LOOP, false);
	obs_data_set_default_bool(settings, S_PROGRESSIVE, false);
	obs_data_set_default_bool(settings, S_POINTS, false);
	obs_data_set_default_string(settings, S_INPUT, INPUT_CUSTOM);
}

static const char *aspects[] = {"16:9", "16:10", "4:3", "1:1"};

static bool input_mode_changed(obs_properties_t *props, obs_property_t *p,
			     obs_data_t *s)
{
	const char *mode = textppt_input_mode(s);
	const bool use_single_file = strcmp(mode, INPUT_FILE) == 0;
	const bool use_custom_text = strcmp(mode, INPUT_CUSTOM) == 0;
	const bool use_multiple_files = strcmp(mode, INPUT_FILES) == 0;
	const bool use_sources = strcmp(mode, INPUT_SOURCES) == 0;
	const bool use_groups = strcmp(mode, INPUT_GROUPS) == 0;
	set_vis(S_CUSTOM_DELIM, use_single_file || use_custom_text);
	set_vis(S_DELIM, (use_single_file || use_custom_text) && obs_data_get_bool(s, S_CUSTOM_DELIM));
	set_vis(S_TXT_FILE, use_single_file);
	set_vis(S_FILES, use_multiple_files);
	set_vis(S_CUSTOM_TEXT, use_custom_text);
	set_vis(S_EDIT_TEXT, use_custom_text);
	set_vis(S_ADD_SOURCE, use_sources);
	set_vis(S_SOURCES, use_sources);
	set_vis(S_TEXT_STYLE, !use_sources && !use_groups);
	set_vis(S_ADD_GROUP, use_groups);
	set_vis(S_GROUPS, use_groups);
	return true;
}

static bool collect_text_choices(void *data, obs_source_t *source)
{
	if (is_native_text(source))
		obs_property_list_add_string(static_cast<obs_property_t *>(data),
			obs_source_get_name(source), obs_source_get_uuid(source));
	return true;
}

static bool add_source_changed(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	const string uuid = obs_data_get_string(settings, S_ADD_SOURCE);
	if (uuid.empty())
		return false;
	obs_data_set_string(settings, S_ADD_SOURCE, "");
	auto *source = obs_get_source_by_uuid(uuid.c_str());
	if (!is_native_text(source)) {
		obs_source_release(source);
		return true;
	}
	auto *sources = obs_data_get_array(settings, S_SOURCES);
	if (!sources)
		sources = obs_data_array_create();
	auto *ids = obs_data_get_obj(settings, S_SOURCE_IDS);
	if (!ids)
		ids = obs_data_create();
	const char *name = obs_source_get_name(source);
	bool duplicate = false;
	for (size_t i = 0; i < obs_data_array_count(sources); ++i) {
		auto *entry = obs_data_array_item(sources, i);
		const char *key = obs_data_get_string(entry, "uuid");
		const char *existing = obs_data_get_string(ids, key);
		if (!*existing)
			existing = obs_data_get_string(ids, obs_data_get_string(entry, "value"));
		duplicate |= uuid == existing;
		// Stable row IDs preserve references after renaming or reordering.
		if (*key && *existing) {
			const string value = existing;
			obs_data_set_string(ids, key, value.c_str());
		}
		obs_data_release(entry);
	}
	if (!duplicate && obs_data_array_count(sources) < 1000) {
		auto *entry = obs_data_create();
		obs_data_set_string(entry, "value", name);
		obs_data_set_string(entry, "uuid", uuid.c_str());
		obs_data_array_push_back(sources, entry);
		obs_data_release(entry);
		obs_data_set_string(ids, uuid.c_str(), uuid.c_str());
	}
	obs_data_set_array(settings, S_SOURCES, sources);
	obs_data_set_obj(settings, S_SOURCE_IDS, ids);
	obs_data_release(ids);
	obs_data_array_release(sources);
	obs_source_release(source);
	return true;
}

static bool use_custom_delim_changed(obs_properties_t *props, obs_property_t *p,
				     obs_data_t *s)
{
	const char *mode = textppt_input_mode(s);
	bool use_custom_delim = (strcmp(mode, INPUT_FILE) == 0 || strcmp(mode, INPUT_CUSTOM) == 0) &&
		obs_data_get_bool(s, S_CUSTOM_DELIM);
	set_vis(S_DELIM, use_custom_delim);

	return true;
}

static bool playback_mode_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	const bool automatic = strcmp(obs_data_get_string(settings, S_MODE), S_MODE_AUTO) == 0;
	set_vis(S_SLIDE_TIME, automatic);
	set_vis(S_LOOP, automatic);
	set_vis(S_RANDOMIZE, automatic);
	set_vis(S_HIDE, automatic);
	return true;
}

static bool transition_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	set_vis(S_TR_SPEED, strcmp(obs_data_get_string(settings, S_TRANSITION), TR_CUT) != 0);
	return true;
}

void textppt_properties(void *data, obs_properties_t *props)
{
	if (auto *s = static_cast<textppt_source *>(data))
		textppt_prepare_properties(s->source);
	obs_property_t *p = obs_properties_add_list(props, S_INPUT, T_SS_("InputMode"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_SS_("InputMode.Custom"), INPUT_CUSTOM);
	obs_property_list_add_string(p, T_SS_("InputMode.File"), INPUT_FILE);
	obs_property_list_add_string(p, T_SS_("InputMode.Files"), INPUT_FILES);
	obs_property_list_add_string(p, T_SS_("InputMode.Sources"), INPUT_SOURCES);
	obs_property_list_add_string(p, T_SS_("InputMode.Groups"), INPUT_GROUPS);
	obs_property_set_modified_callback(p, input_mode_changed);

	p = obs_properties_add_text(props, S_CUSTOM_TEXT, T_SS_("CustomText"), OBS_TEXT_MULTILINE);
	obs_property_set_long_description(p, T_SS_("CustomTextHint"));
	obs_properties_add_button2(props, S_EDIT_TEXT, T_SS_("EditText"), textppt_edit_text, data);

	const string filter = string(T_FILTER_TEXT_FILES) + " (*.txt);;" + T_FILTER_ALL_FILES + " (*.*)";
	obs_properties_add_path(props, S_TXT_FILE, T_FILE, OBS_PATH_FILE, filter.c_str(), nullptr);
	obs_properties_add_editable_list(props, S_FILES, T_FILES, OBS_EDITABLE_LIST_TYPE_FILES, nullptr, nullptr);
	p = obs_properties_add_list(props, S_ADD_SOURCE, T_SS_("AddSource"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_SS_("ChooseSource"), "");
	obs_enum_sources(collect_text_choices, p);
	obs_property_set_modified_callback(p, add_source_changed);
	obs_properties_add_editable_list(props, S_SOURCES, T_SS_("SelectedSources"),
		OBS_EDITABLE_LIST_TYPE_STRINGS, nullptr, nullptr);
	text_groups_properties(static_cast<textppt_source *>(data), props);

	p = obs_properties_add_list(props, S_MODE, T_MODE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_long_description(p, T_SS_("SlideModeHint"));
	obs_property_list_add_string(p, T_MODE_AUTO, S_MODE_AUTO);
	obs_property_list_add_string(p, T_MODE_MANUAL, S_MODE_MANUAL);
	obs_property_set_modified_callback(p, playback_mode_changed);
	p = obs_properties_add_list(props, S_TRANSITION, T_TRANSITION, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_TR_CUT, TR_CUT);
	obs_property_list_add_string(p, T_TR_FADE, TR_FADE);
	obs_property_list_add_string(p, T_TR_SWIPE, TR_SWIPE);
	obs_property_list_add_string(p, T_TR_SLIDE, TR_SLIDE);
	obs_property_set_modified_callback(p, transition_changed);
	p = obs_properties_add_int(props, S_TR_SPEED, T_TR_SPEED, 0, 60000, 50);
	obs_property_int_set_suffix(p, " ms");
	p = obs_properties_add_int(props, S_SLIDE_TIME, T_SLIDE_TIME, 50, 3600000, 50);
	obs_property_int_set_suffix(p, " ms");
	obs_properties_add_bool(props, S_LOOP, T_LOOP);
	obs_properties_add_bool(props, S_RANDOMIZE, T_RANDOMIZE);
	obs_properties_add_bool(props, S_HIDE, T_HIDE);

	auto *advanced = obs_properties_create();
	p = obs_properties_add_bool(advanced, S_CUSTOM_DELIM, T_USE_CUSTOM_DELIM);
	obs_property_set_modified_callback(p, use_custom_delim_changed);
	obs_properties_add_text(advanced, S_DELIM, T_CUSTOM_DELIM, OBS_TEXT_DEFAULT);
	p = obs_properties_add_list(advanced, S_BEHAVIOR, T_BEHAVIOR, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_BEHAVIOR_ALWAYS_PLAY, S_BEHAVIOR_ALWAYS_PLAY);
	obs_property_list_add_string(p, T_BEHAVIOR_STOP_RESTART, S_BEHAVIOR_STOP_RESTART);
	obs_property_list_add_string(p, T_BEHAVIOR_PAUSE_UNPAUSE, S_BEHAVIOR_PAUSE_UNPAUSE);
	p = obs_properties_add_list(advanced, S_CUSTOM_SIZE, T_CUSTOM_SIZE,
		OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_CUSTOM_SIZE_AUTO, "auto");
	for (const char *aspect : aspects)
		obs_property_list_add_string(p, aspect, aspect);
	struct obs_video_info video = {};
	if (obs_get_video_info(&video)) {
		const auto size = std::to_string(video.base_width) + "x" + std::to_string(video.base_height);
		obs_property_list_add_string(p, size.c_str(), size.c_str());
	}
	obs_properties_add_group(props, S_ADVANCED, T_SS_("AdvancedSettings"), OBS_GROUP_NORMAL, advanced);
}
