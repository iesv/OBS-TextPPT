// Derived from obs-text-slideshow. Copyright (C) 2021 Joshua Wong.
// Copyright (C) 2026 iesv. SPDX-License-Identifier: GPL-2.0-or-later.
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QPointer>
#include <QString>
#include "plugin-macros.generated.h"
#include "obs-textppt-dock.h"
#include "obs-textppt.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
OBS_MODULE_AUTHOR(PLUGIN_AUTHOR)
MODULE_EXPORT const char *obs_module_description(void) { return "OBS-TextPPT native text slideshow for OBS 32"; }

void textppt_add_info(obs_properties_t *props)
{
	// OBS renders this footer as a label with a clickable author link.
	const auto author = QStringLiteral("<a href=\"%1\">%2</a>").arg(
		QString::fromUtf8(PLUGIN_WEBSITE).toHtmlEscaped(), QString::fromUtf8(PLUGIN_AUTHOR).toHtmlEscaped());
	const auto info = QString::fromUtf8(obs_module_text("Plugin.Info")).arg(QStringLiteral(PLUGIN_VERSION), author);
	const auto html = QStringLiteral("<span id=\"TextPPTInfo\">%1</span>").arg(info).toUtf8();
	auto *property = obs_properties_add_text(props, "plugin_info", html.constData(), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(property, false);
}
extern void load_textppt_native();
static QPointer<TextPPTDock> dock;
static bool frontend_ready = false;
static void frontend_event(obs_frontend_event event, void *)
{
	// Resolve references after scene loading.
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		frontend_ready = true;
	else if (event != OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED || !frontend_ready)
		return;
#ifdef _WIN32
	obs_enum_sources(textppt_reload, const_cast<char *>("obs-textppt-gdiplus"));
#else
	obs_enum_sources(textppt_reload, const_cast<char *>("obs-textppt-ft2"));
#endif
}
bool obs_module_load(void)
{
	load_textppt_native();
	obs_frontend_add_event_callback(frontend_event, nullptr);
	blog(LOG_INFO, "loaded (version %s, OBS 32, Qt %s)", PLUGIN_VERSION, qVersion());
	return true;
}
void obs_module_post_load(void)
{
	dock = new TextPPTDock;
	if (!obs_frontend_add_dock_by_id("obs-textppt-dock", "TextPPT", dock.data())) {
		delete dock.data();
		blog(LOG_ERROR, "could not register the OBS-TextPPT dock");
	}
}
void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	if (dock)
		obs_frontend_remove_dock("obs-textppt-dock");
}
