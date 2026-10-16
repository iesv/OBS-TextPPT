// OBS-TextPPT, GPL-2.0-or-later.
#pragma once
#include <vector>
struct textppt_source;
struct text_data;
struct obs_data;
struct obs_properties;
struct obs_source;
bool text_group_safe(obs_source *, obs_source *controller);
bool text_group_present(obs_source *);
void text_group_update_layout(textppt_source *, obs_source *);
void text_groups_read(obs_data *, std::vector<text_data> &);
void text_groups_properties(textppt_source *, obs_properties *);
void text_groups_refresh(textppt_source *, float seconds);
