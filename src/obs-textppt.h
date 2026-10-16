// Derived from obs-text-slideshow. Copyright (C) 2021 Joshua Wong.
// Copyright (C) 2026 iesv. SPDX-License-Identifier: GPL-2.0-or-later.
#pragma once

#include "obs-module.h"
#include <atomic>
#include <mutex>
#include <deque>
#include <string>
#include <vector>
#include "group-source.h"

using std::string;
using std::vector;

#define S_TR_SPEED "transition_speed"
#define S_CUSTOM_SIZE "use_custom_size"
#define S_SLIDE_TIME "slide_time"
#define S_TRANSITION "transition"
#define S_RANDOMIZE "randomize"
#define S_LOOP "loop"
#define S_HIDE "hide"
#define S_TEXTS "texts"
#define S_CUSTOM_TEXT "custom_text"
#define S_BEHAVIOR "playback_behavior"
#define S_BEHAVIOR_STOP_RESTART "stop_restart"
#define S_BEHAVIOR_PAUSE_UNPAUSE "pause_unpause"
#define S_BEHAVIOR_ALWAYS_PLAY "always_play"
#define S_MODE "slide_mode"
#define S_MODE_AUTO "mode_auto"
#define S_MODE_MANUAL "mode_manual"
#define S_PROGRESSIVE "progressive"
#define S_POINTS "progressive_points"

#define S_READ_SINGLE_FILE "read_single_file"
#define S_CUSTOM_DELIM "custom_delim"
#define S_DELIM "delim"
#define S_TXT_FILE "txt_file"
#define S_READ_MULTIPLE_FILES "read_multiple_files"
#define S_FILES "files"
#define S_INPUT "input_mode"
#define INPUT_CUSTOM "custom"
#define INPUT_FILE "file"
#define INPUT_FILES "files"
#define INPUT_SOURCES "sources"
#define INPUT_GROUPS "groups"
#define S_GROUPS "groups"
#define S_GROUP_IDS "group_ids"
#define S_ADD_GROUP "add_group"
#define S_SOURCES "sources"
#define S_SOURCE_IDS "source_ids"
#define S_ADD_SOURCE "add_source"
#define S_TEXT_STYLE "text_style"
#define S_EDIT_TEXT "edit_text"
#define S_ADVANCED "advanced_settings"

#define TR_CUT "cut"
#define TR_FADE "fade"
#define TR_SWIPE "swipe"
#define TR_SLIDE "slide"

#define T_SS_(text) obs_module_text("SlideShow." text)
#define T_TR_SPEED T_SS_("TransitionSpeed")
#define T_CUSTOM_SIZE T_SS_("CustomSize")
#define T_CUSTOM_SIZE_AUTO T_SS_("CustomSize.Auto")
#define T_SLIDE_TIME T_SS_("SlideTime")
#define T_TRANSITION T_SS_("Transition")
#define T_RANDOMIZE T_SS_("Randomize")
#define T_LOOP T_SS_("Loop")
#define T_HIDE T_SS_("HideWhenDone")
#define T_BEHAVIOR T_SS_("PlaybackBehavior")
#define T_BEHAVIOR_STOP_RESTART T_SS_("PlaybackBehavior.StopRestart")
#define T_BEHAVIOR_PAUSE_UNPAUSE T_SS_("PlaybackBehavior.PauseUnpause")
#define T_BEHAVIOR_ALWAYS_PLAY T_SS_("PlaybackBehavior.AlwaysPlay")
#define T_MODE T_SS_("SlideMode")
#define T_MODE_AUTO T_SS_("SlideMode.Auto")
#define T_MODE_MANUAL T_SS_("SlideMode.Manual")

#define T_USE_CUSTOM_DELIM T_SS_("UseCustomDelim")
#define T_CUSTOM_DELIM T_SS_("CustomDelim")
#define T_FILE T_SS_("TextFile")
#define T_FILTER_TEXT_FILES T_SS_("Filter.TextFiles")
#define T_FILTER_ALL_FILES T_SS_("Filter.AllFiles")
#define T_FILES T_SS_("Files")

#define T_TR_(text) obs_module_text("SlideShow.Transition." text)
#define T_TR_CUT T_TR_("Cut")
#define T_TR_FADE T_TR_("Fade")
#define T_TR_SWIPE T_TR_("Swipe")
#define T_TR_SLIDE T_TR_("Slide")

#define set_vis(val, show)                          \
	do {                                        \
		p = obs_properties_get(props, val); \
		obs_property_set_visible(p, show);  \
	} while (false)
struct text_data {
	string file_path;
	string text;
	string source_uuid;
	string source_name;
};
enum behavior { BEHAVIOR_STOP_RESTART, BEHAVIOR_PAUSE_UNPAUSE, BEHAVIOR_ALWAYS_PLAY };
enum class slide_command { Jump, Next, Previous, Restart, Stop, Pause };
struct queued_command {
	slide_command type;
	size_t value = 0;
	int64_t serial = 0;
};
struct textppt_source {
	bool group_mode = false;
	float group_scan_elapsed = 0.0f;
	gs_texrender_t *group_layout = nullptr;
	string layout_group;
	uint64_t layout_signature = 0;
	bool updating_group_layout = false;
	obs_source_t *source = nullptr;
	obs_source_t *transition = nullptr;
	// Pages hold data only. Two native text sources are reused for rendering.
	vector<text_data> pages;
	obs_source_t *buffers[2] = {};
	string native_style;
	string file_input;
	std::atomic<bool> force_file_reload{false};
	obs_source_t *prepared = nullptr;
	size_t prepared_item = 0;
	int buffer = 0;
	int prepare_ticks = 0;
	bool page_pending = false;
	bool pending_cut = false;
	bool size_pending = false;
	bool style_pending = false;
	uint32_t style_width = 0, style_height = 0;
	bool transition_running = false;
	string tr_name;
	uint32_t fixed_width = 0, fixed_height = 0;
	double aspect_ratio = 0.0;
	bool randomize = false;
	bool loop = false;
	bool manual = true;
	bool configured_manual = true;
	bool progressive = false;
	bool points = false;
	size_t reveal_step = 1;
	size_t initial_page = SIZE_MAX;
	size_t reveal_page = SIZE_MAX;
	string reveal_text;
	vector<size_t> reveal_ends;
	uint32_t reveal_width = 0, reveal_height = 0;
	size_t prepared_step = 0;
	bool measuring_page = false;
	bool hide = false;
	bool stopped = false;
	bool was_active = false;
	bool preserve_first_activation = false;
	float elapsed = 0.0f;
	float slide_time = 5.0f;
	uint32_t tr_speed = 300;
	size_t cur_item = 0;
	enum behavior behavior = BEHAVIOR_ALWAYS_PLAY;
	std::atomic<bool> active{false};
	std::atomic<uint32_t> cx{1}, cy{1};
	std::atomic<obs_media_state> state{OBS_MEDIA_STATE_PAUSED};
	std::mutex transition_mutex;
	std::mutex command_mutex;
	std::deque<queued_command> commands;
	int64_t next_serial = 0;
	std::atomic<int64_t> acknowledged{0};
	std::atomic<bool> status_observed{false};
	std::atomic<bool> ready{false};
	std::mutex snapshot_mutex;
	vector<string> snapshot;
	vector<string> snapshot_groups;
	size_t snapshot_step = 0, snapshot_steps = 0;
	bool snapshot_points = false;
	string file_error;
	int64_t revision = 0;
	std::atomic<size_t> current{0};
};

typedef obs_source_t *(*text_source_create)(obs_data_t *);
typedef void (*set_text_alignment)(obs_source_t *, obs_data_t *);
void *textppt_create(obs_data_t *, obs_source_t *);
void textppt_destroy(void *);
void textppt_update(void *, obs_data_t *, obs_data_t *, text_source_create, set_text_alignment);
void textppt_activate(void *);
void textppt_deactivate(void *);
void textppt_video_tick(void *, float);
void textppt_video_render(void *, gs_effect_t *);
bool textppt_audio_render(void *, uint64_t *, obs_source_audio_mix *, uint32_t mixers, size_t channels, size_t sample_rate);
void textppt_enum_sources(void *, obs_source_enum_proc_t, void *);
uint32_t textppt_width(void *);
uint32_t textppt_height(void *);
void textppt_defaults(obs_data_t *);
void textppt_properties(void *, obs_properties_t *);
void textppt_prepare_properties(obs_source_t *);
void textppt_add_info(obs_properties_t *);
const char *textppt_input_mode(obs_data_t *);
bool textppt_can_reveal(obs_data_t *);
string textppt_render_text(const string &, obs_data_t *, vector<size_t> *ends = nullptr);
void textppt_text_layout(obs_source_t *, uint32_t, uint32_t);
bool textppt_edit_text(obs_properties_t *, obs_property_t *, void *);
void textppt_play_pause(void *, bool);
void textppt_restart(void *);
void textppt_stop(void *);
void textppt_next_slide(void *);
void textppt_previous_slide(void *);
obs_media_state textppt_get_state(void *);
bool textppt_reload(void *, obs_source_t *);
obs_missing_files_t *textppt_missing_files(void *);
