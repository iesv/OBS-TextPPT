// Derived from obs-text-slideshow. Copyright (C) 2021 Joshua Wong.
// Copyright (C) 2026 iesv. SPDX-License-Identifier: GPL-2.0-or-later.
#include "obs-textppt-dock.h"
#include "obs-textppt.h"
#include "page-list.h"
#include <obs-module.h>
#include <QComboBox>
#include <QListWidget>
#include <QLabel>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QShowEvent>
#include <QHideEvent>
#include <QSignalBlocker>
#include <QLineEdit>
#include <QIntValidator>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QMenu>
#include <QSettings>
#include <QCheckBox>
#include <QApplication>
#include <QToolTip>
#include <string>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

static const char *sourceSignals[] = {"source_create", "source_remove", "source_destroy", "source_rename"};
static constexpr const char *followSetting = "textppt_follow_scene";
static const char *imageSceneSignals[] = {"item_add", "item_remove", "item_visible", "reorder"};
static QString label(const char *key) { return QString::fromUtf8(obs_module_text(key)); }

static bool isTextPPT(obs_source_t *source)
{
	if (!source || obs_source_removed(source))
		return false;
	const char *id = obs_source_get_unversioned_id(source);
	return strcmp(id, "obs-textppt-gdiplus") == 0 || strcmp(id, "obs-textppt-ft2") == 0;
}

static bool followsScene(obs_source_t *source)
{
	if (!source)
		return false;
	auto *data = obs_source_get_private_settings(source);
	// Sources without metadata follow scenes by default.
	const bool enabled = !obs_data_has_user_value(data, followSetting) || obs_data_get_bool(data, followSetting);
	obs_data_release(data);
	return enabled;
}

struct SceneSources {
	QString scene;
	std::vector<std::pair<QString, bool>> pages;
	std::unordered_map<std::string, bool> visited;
	bool visible = true;
	bool onlyFollowing = false;
};

static bool collectSceneSources(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto &found = *static_cast<SceneSources *>(param);
	auto *source = obs_sceneitem_get_source(item);
	const bool parentVisible = found.visible;
	const bool visible = parentVisible && obs_sceneitem_visible(item);
	const QString uuid = QString::fromUtf8(obs_source_get_uuid(source));
	if (isTextPPT(source) && (!found.onlyFollowing || followsScene(source))) {
		auto entry = std::find_if(found.pages.begin(), found.pages.end(), [&](const auto &page) { return page.first == uuid; });
		if (entry == found.pages.end())
			found.pages.emplace_back(uuid, visible);
		else
			entry->second = entry->second || visible;
	}
	auto *scene = obs_sceneitem_is_group(item) ? obs_sceneitem_group_get_scene(item) : obs_scene_from_source(source);
	if (scene) {
		auto [entry, inserted] = found.visited.emplace(obs_source_get_uuid(source), visible);
		// Revisit a shared scene only if its other path was hidden.
		if (inserted || (visible && !entry->second)) {
			entry->second = entry->second || visible;
			found.visible = visible;
			obs_scene_enum_items(scene, collectSceneSources, &found);
			found.visible = parentVisible;
		}
	}
	return true;
}

static SceneSources currentSceneSources(bool onlyFollowing = false, const QString &skipScene = {})
{
	SceneSources found;
	found.onlyFollowing = onlyFollowing;
	// Studio Mode edits the preview; OBS owns the program copy.
	auto *source = obs_frontend_preview_program_mode_active() ?
		obs_frontend_get_current_preview_scene() : obs_frontend_get_current_scene();
	if (source) {
		found.scene = QString::fromUtf8(obs_source_get_uuid(source));
		found.visited.emplace(obs_source_get_uuid(source), true);
		// Coalesce scene and transition events for the same scene.
		if (found.scene != skipScene)
			if (auto *scene = obs_scene_from_source(source))
				obs_scene_enum_items(scene, collectSceneSources, &found);
		obs_source_release(source);
	}
	return found;
}

static bool containsVisible(const SceneSources &found, const QString &uuid)
{
	return std::any_of(found.pages.begin(), found.pages.end(), [&](const auto &page) {
		return page.first == uuid && page.second;
	});
}

static std::vector<std::pair<QString, QString>> previewScenes(const QString &uuid)
{
	std::vector<std::pair<QString, QString>> choices;
	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t index = 0; index < scenes.sources.num; ++index) {
		auto *source = scenes.sources.array[index];
		auto *scene = obs_scene_from_source(source);
		if (!scene || obs_source_removed(source)) continue;
		SceneSources found;
		found.visited.emplace(obs_source_get_uuid(source), true);
		obs_scene_enum_items(scene, collectSceneSources, &found);
		if (containsVisible(found, uuid))
			choices.emplace_back(QString::fromUtf8(obs_source_get_uuid(source)), QString::fromUtf8(obs_source_get_name(source)));
	}
	obs_frontend_source_list_free(&scenes);
	return choices;
}

static bool sceneTransitionRunning()
{
	auto *source = obs_get_output_source(0);
	const float time = source ? obs_transition_get_time(source) : 0.0f;
	const bool running = !source || (time > 0.0f && time < 1.0f);
	obs_source_release(source);
	return running;
}

enum class MediaIcon { Previous, Play, Pause, Next, Restart, Lock, Unlock, Link, Unlink, ImagePrevious, ImageNext };
static QIcon mediaIcon(MediaIcon kind, QColor color)
{
	QIcon icon;
	// Render icons for common display scales.
	for (int scale = 1; scale <= 3; ++scale) {
		QPixmap image(24 * scale, 24 * scale);
		image.setDevicePixelRatio(scale);
		image.fill(Qt::transparent);
		QPainter painter(&image);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(QPen(color, 2.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		QPainterPath path;
		if (kind == MediaIcon::Previous || kind == MediaIcon::Next) {
			const bool previous = kind == MediaIcon::Previous;
			path.moveTo(previous ? 15 : 9, 5);
			path.lineTo(previous ? 8 : 16, 12);
			path.lineTo(previous ? 15 : 9, 19);
			painter.drawPath(path);
		} else if (kind == MediaIcon::ImagePrevious || kind == MediaIcon::ImageNext) {
			painter.setPen(QPen(color, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			painter.drawRoundedRect(QRectF(4, 2, 16, 14), 1, 1);
			painter.drawPoint(QPointF(16, 6));
			path.moveTo(6, 13); path.lineTo(10, 8); path.lineTo(15, 13); path.lineTo(18, 10);
			painter.drawPath(path);
			painter.drawLine(QPointF(5, 21), QPointF(19, 21));
			const bool back = kind == MediaIcon::ImagePrevious;
			path = QPainterPath();
			path.moveTo(back ? 8 : 16, 18); path.lineTo(back ? 5 : 19, 21); path.lineTo(back ? 8 : 16, 23);
			painter.drawPath(path);
		} else if (kind == MediaIcon::Play) {
			painter.setPen(Qt::NoPen); painter.setBrush(color);
			path.moveTo(8, 4); path.lineTo(20, 12); path.lineTo(8, 20); path.closeSubpath();
			painter.drawPath(path);
		} else if (kind == MediaIcon::Pause) {
			painter.setPen(Qt::NoPen); painter.setBrush(color);
			painter.drawRoundedRect(QRectF(6, 4, 4, 16), 0.8, 0.8);
			painter.drawRoundedRect(QRectF(14, 4, 4, 16), 0.8, 0.8);
		} else if (kind == MediaIcon::Link || kind == MediaIcon::Unlink) {
			painter.setPen(QPen(color, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			painter.translate(12, 12);
			painter.rotate(-45);
			if (kind == MediaIcon::Link) {
				painter.drawRoundedRect(QRectF(-10, -4, 12, 8), 4, 4);
				painter.drawRoundedRect(QRectF(-2, -4, 12, 8), 4, 4);
			} else {
				painter.drawArc(QRectF(-11, -4, 9, 8), 45 * 16, 270 * 16);
				painter.drawArc(QRectF(2, -4, 9, 8), 225 * 16, 270 * 16);
				painter.drawLine(QPointF(-2, -7), QPointF(0, -9));
				painter.drawLine(QPointF(0, 9), QPointF(2, 7));
			}
		} else if (kind == MediaIcon::Lock || kind == MediaIcon::Unlock) {
			painter.drawRoundedRect(QRectF(6, 11, 12, 10), 1.5, 1.5);
			const bool locked = kind == MediaIcon::Lock;
			painter.drawArc(QRectF(locked ? 8 : 12, 3, 8, 12), 0, 180 * 16);
			painter.drawLine(QPointF(locked ? 8 : 12, 9), QPointF(locked ? 8 : 12, 11));
			if (locked)
				painter.drawLine(QPointF(16, 9), QPointF(16, 11));
			painter.drawLine(QPointF(12, 15), QPointF(12, 17));
		} else {
			painter.drawArc(QRectF(5, 5, 14, 14), 40 * 16, -285 * 16);
			path.moveTo(19, 4); path.lineTo(19, 10); path.lineTo(13, 10);
			painter.drawPath(path);
		}
		painter.end();
		icon.addPixmap(image);
	}
	return icon;
}

static bool collectSources(void *data, obs_source_t *source)
{
	if (isTextPPT(source))
		static_cast<std::vector<obs_weak_source_t *> *>(data)->push_back(obs_source_get_weak_source(source));
	return true;
}

TextPPTDock::TextPPTDock(QWidget *parent) : QWidget(parent)
{
	setObjectName("TextPPTDock");
	char *path = obs_module_config_path("dock.ini");
	settings = new QSettings(QString::fromUtf8(path), QSettings::IniFormat, this);
	bfree(path);
	settings->remove("followScene");
	setContextMenuPolicy(Qt::CustomContextMenu);
	connect(this, &QWidget::customContextMenuRequested, this, &TextPPTDock::showContextMenu);
	auto *layout = new QVBoxLayout(this);
	// Keep controls and group spacing within the native layout minimum.
	layout->setSizeConstraint(QLayout::SetMinimumSize);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);
	sources = new QComboBox(this);
	sources->setObjectName("TextPPTSourceList");
	sources->setMinimumContentsLength(4);
	sources->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	sources->setToolTip(label("Dock.SourceHint"));
	status = new QLabel(this);
	status->setObjectName("TextPPTPageStatus");
	status->setToolTip(label("Dock.CurrentHint"));
	pageInput = new QLineEdit(this);
	pageInput->setObjectName("TextPPTPageInput");
	pageInput->setAlignment(Qt::AlignCenter);
	pageInput->setMaxLength(4);
	pageInput->setValidator(new QIntValidator(1, 1000, pageInput));
	pageInput->setFixedSize(pageInput->fontMetrics().horizontalAdvance(QStringLiteral("1000")) + 12, 24);
	pageInput->setToolTip(label("Dock.CurrentHint"));
	auto *header = new QHBoxLayout;
	header->addWidget(sources, 1);
	follow = new QToolButton(this);
	follow->setObjectName("TextPPTFollowScene");
	follow->setToolTip(label("Dock.FollowScene") + '\n' + label("Dock.FollowSceneHint"));
	follow->setAccessibleName(label("Dock.FollowScene"));
	follow->setCheckable(true);
	follow->setToolButtonStyle(Qt::ToolButtonIconOnly);
	follow->setIconSize(QSize(18, 18));
	follow->setFixedSize(24, 24);
	follow->setChecked(true);
	follow->setEnabled(false);
	progressive = new QCheckBox(label("Dock.Progressive"), this);
	progressive->setObjectName("TextPPTProgressive");
	progressive->setToolTip(label("Dock.ProgressiveHint"));
	progressive->setEnabled(false);
	header->addWidget(progressive);
	points = new QCheckBox(label("Dock.Points"), this);
	points->setObjectName("TextPPTPoints");
	points->setToolTip(label("Dock.PointsHint"));
	points->setEnabled(false);
	header->addWidget(points);
	header->addWidget(follow);
	sourceLock = new QToolButton(this);
	sourceLock->setObjectName("TextPPTSourceLock");
	sourceLock->setCheckable(true);
	sourceLock->setToolButtonStyle(Qt::ToolButtonIconOnly);
	sourceLock->setIconSize(QSize(18, 18));
	sourceLock->setFixedSize(24, 24);
	sourceLock->setEnabled(false);
	header->addWidget(sourceLock);
	// Keep themed controls within their widget bounds on every platform.
	for (int index = 0; index < header->count(); ++index)
		header->itemAt(index)->widget()->setAttribute(Qt::WA_LayoutUsesWidgetRect);
	layout->addLayout(header);
	fileError = new QLabel(this);
	fileError->setObjectName("TextPPTFileError");
	fileError->setTextFormat(Qt::PlainText);
	fileError->setWordWrap(true);
	fileError->setVisible(false);
	layout->addWidget(fileError);
	texts = new QListWidget(this);
	texts->setItemDelegate(new PageListDelegate(texts));
	texts->viewport()->installEventFilter(this);
	texts->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(texts, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
		showContextMenu(mapFromGlobal(texts->viewport()->mapToGlobal(pos)));
	});
	layout->addWidget(texts, 1);
	auto *buttons = new QHBoxLayout;
	buttons->setContentsMargins(0, 0, 0, 0);
	buttons->setSpacing(4);
	const auto button = [&](const char *key, QHBoxLayout *row) {
		auto *item = new QToolButton(this);
		item->setToolTip(label(key));
		item->setAccessibleName(label(key));
		item->setToolButtonStyle(Qt::ToolButtonIconOnly);
		item->setIconSize(QSize(18, 18));
		item->setFixedSize(28, 24);
		row->addWidget(item);
		return item;
	};
	restart = button("SlideShow.Restart", buttons);
	restart->setObjectName("TextPPTRestart");
	buttons->addSpacing(restart->width() - buttons->spacing());
	previous = button("SlideShow.PreviousSlide", buttons);
	previous->setObjectName("TextPPTPreviousPage");
	play = button("Dock.Play", buttons);
	next = button("SlideShow.NextSlide", buttons);
	next->setObjectName("TextPPTNextPage");
	buttons->addSpacing(restart->width() - buttons->spacing());
	imageControls = new QWidget(this);
	imageControls->setObjectName("TextPPTImageControls");
	auto *imageButtons = new QHBoxLayout(imageControls);
	imageButtons->setContentsMargins(0, 0, 0, 0);
	imageButtons->setSpacing(4);
	imagePrevious = button("Dock.PreviousImage", imageButtons);
	imagePrevious->setObjectName("TextPPTPreviousImage");
	imageNext = button("Dock.NextImage", imageButtons);
	imageNext->setObjectName("TextPPTNextImage");
	imageControls->hide();
	buttons->addWidget(imageControls);
	buttons->addStretch();
	buttons->addWidget(pageInput);
	buttons->addWidget(status);
	updateIcons();
	layout->addLayout(buttons);
	connect(pageInput, &QLineEdit::returnPressed, this, &TextPPTDock::jumpToPage);
	connect(follow, &QToolButton::toggled, this, [this](bool enabled) {
		if (closing)
			return;
		updateIcons();
		if (auto *source = selected()) {
			// Save with the source without updating its text renderers.
			auto *data = obs_source_get_private_settings(source);
			obs_data_set_bool(data, followSetting, enabled);
			obs_data_release(data);
			obs_source_release(source);
			obs_frontend_save();
		}
	});
	connect(sources, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
		rememberSelection();
		selectionChanged();
	});
	const auto setReveal = [this](bool enabled, bool byPoints) {
		if (closing)
			return;
		cancelPageRequest();
		// Save both choices in one update; never render an intermediate mode.
		const QSignalBlocker lineBlocker(progressive), pointBlocker(points);
		if (enabled) (byPoints ? progressive : points)->setChecked(false);
		if (auto *source = selected()) {
			auto *data = obs_data_create();
			obs_data_set_bool(data, S_PROGRESSIVE, progressive->isChecked());
			obs_data_set_bool(data, S_POINTS, points->isChecked());
			obs_source_update(source, data);
			obs_data_release(data);
			obs_source_release(source);
			obs_frontend_save();
		}
	};
	connect(progressive, &QCheckBox::toggled, this, [setReveal](bool enabled) { setReveal(enabled, false); });
	connect(points, &QCheckBox::toggled, this, [setReveal](bool enabled) { setReveal(enabled, true); });
	connect(sourceLock, &QToolButton::toggled, this, [this](bool locked) {
		if (closing)
			return;
		updateIcons();
		if (locked || !followCurrentScene(true))
			updateStatus();
	});
	connect(texts, &QListWidget::currentRowChanged, this, [this](int row) {
		if (!pageMouseDown) previewPage(row);
	});
	connect(texts, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) { previewPage(texts->row(item)); });
	connect(texts, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
		transitionPage(texts->row(item));
	});
	connect(previous, &QToolButton::clicked, this, [this] { control(0); });
	connect(next, &QToolButton::clicked, this, [this] { control(1); });
	connect(play, &QToolButton::clicked, this, [this] { control(2); });
	connect(restart, &QToolButton::clicked, this, [this] { control(3); });
	connect(imagePrevious, &QToolButton::clicked, this, [this] { controlImage(false); });
	connect(imageNext, &QToolButton::clicked, this, [this] { controlImage(true); });
	timer = new QTimer(this);
	timer->setInterval(250);
	connect(timer, &QTimer::timeout, this, &TextPPTDock::updateStatus);
	pageInput->installEventFilter(this);
	obs_frontend_add_event_callback(frontendEvent, this);
	for (const char *signal : sourceSignals)
		signal_handler_connect(obs_get_signal_handler(), signal, sourceEvent, this);
}

TextPPTDock::~TextPPTDock()
{
	timer->stop();
	closing = true;
	clearImageControls();
	bindSceneTransition();
	obs_frontend_remove_event_callback(frontendEvent, this);
	for (const char *signal : sourceSignals)
		signal_handler_disconnect(obs_get_signal_handler(), signal, sourceEvent, this);
	releaseSources();
}
void TextPPTDock::releaseSources()
{
	for (auto *ref : refs)
		obs_weak_source_release(ref);
	refs.clear();
}
obs_source_t *TextPPTDock::selected()
{
	const int index = sources->currentIndex();
	if (index < 0 || static_cast<size_t>(index) >= refs.size())
		return nullptr;
	auto *source = obs_weak_source_get_source(refs[index]);
	if (source && obs_source_removed(source)) {
		obs_source_release(source);
		return nullptr;
	}
	return source;
}
void TextPPTDock::sourceEvent(void *data, calldata_t *cd)
{
	auto *dock = static_cast<TextPPTDock *>(data);
	auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (source && strncmp(obs_source_get_unversioned_id(source), "obs-textppt-", 12) == 0)
		dock->scheduleRefresh();
	if (source && (strcmp(obs_source_get_unversioned_id(source), "slideshow") == 0 ||
	               obs_group_from_source(source) || obs_scene_from_source(source)))
		dock->imageDirty = true;
}
void TextPPTDock::transitionEvent(void *data, calldata_t *)
{
	auto *dock = static_cast<TextPPTDock *>(data);
	QMetaObject::invokeMethod(dock, [dock] {
		dock->cancelPageRequest();
		dock->transitionGuard.start();
		if (!dock->closing && dock->isVisible())
			dock->followCurrentScene();
	}, Qt::QueuedConnection);
}
void TextPPTDock::bindSceneTransition()
{
	auto *next = closing ? nullptr : obs_frontend_get_current_transition();
	if (next == sceneTransition) {
		obs_source_release(next);
		return;
	}
	if (sceneTransition)
		signal_handler_disconnect(obs_source_get_signal_handler(sceneTransition), "transition_start", transitionEvent, this);
	obs_source_release(sceneTransition);
	sceneTransition = next;
	if (sceneTransition)
		signal_handler_connect(obs_source_get_signal_handler(sceneTransition), "transition_start", transitionEvent, this);
}
void TextPPTDock::frontendEvent(obs_frontend_event event, void *data)
{
	auto *dock = static_cast<TextPPTDock *>(data);
	QMetaObject::invokeMethod(dock, [dock, event] {
		const bool expectedPreview = event == OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED && dock->pageRequest.isValid() &&
			currentSceneSources().scene == dock->requestScene;
		if ((!expectedPreview && event == OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED) || event == OBS_FRONTEND_EVENT_SCENE_CHANGED ||
		    event == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED || event == OBS_FRONTEND_EVENT_TRANSITION_CHANGED ||
		    event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING || event == OBS_FRONTEND_EVENT_EXIT)
			dock->cancelPageRequest();
		if (event == OBS_FRONTEND_EVENT_EXIT) {
			dock->closing = true;
			dock->timer->stop();
			dock->clearImageControls();
		} else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING) {
			dock->closing = true;
			dock->clearImageControls();
			dock->sourceLock->setChecked(false);
			dock->sourceLock->setEnabled(false);
			dock->sources->setEnabled(false);
			dock->updateIcons();
			dock->followedScene.clear();
			dock->follow->setEnabled(false);
			dock->progressive->setEnabled(false);
			dock->points->setEnabled(false);
			dock->bindSceneTransition();
			dock->releaseSources();
			dock->sources->clear();
			dock->texts->clear();
			dock->fileError->setVisible(false);
			dock->enableControls(false);
			dock->pageInput->clear();
			dock->pageInput->setEnabled(false);
			dock->status->setText(label("Dock.TotalPages").arg(0));
			dock->status->setEnabled(false);
		} else if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING ||
		           event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) {
			dock->closing = false;
			dock->scheduleRefresh();
		} else if (event == OBS_FRONTEND_EVENT_TRANSITION_CHANGED && !dock->closing) {
			dock->bindSceneTransition();
		} else if (!dock->closing && dock->isVisible() &&
		           (event == OBS_FRONTEND_EVENT_SCENE_CHANGED ||
		            event == OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED ||
		            event == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED ||
		            event == OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED)) {
			// Follow scene changes while preview rendering is disabled.
			dock->followCurrentScene();
		}
	}, Qt::QueuedConnection);
}
void TextPPTDock::scheduleRefresh()
{
	if (refreshPending.exchange(true))
		return;
	QMetaObject::invokeMethod(this, [this] {
		refreshPending = false;
		if (!closing && isVisible())
			refreshSources();
	}, Qt::QueuedConnection);
}
void TextPPTDock::refreshSources()
{
	if (closing)
		return;
	bindSceneTransition();
	const QString previous = sources->currentData().toString();
	const QSignalBlocker blocker(sources);
	releaseSources();
	sources->clear();
	std::vector<obs_weak_source_t *> found;
	obs_enum_sources(collectSources, &found);
	for (auto *ref : found) {
		auto *source = obs_weak_source_get_source(ref);
		if (source && !obs_source_removed(source)) {
			sources->addItem(QString::fromUtf8(obs_source_get_name(source)),
			                 QString::fromUtf8(obs_source_get_uuid(source)));
			refs.push_back(ref);
		} else {
			obs_weak_source_release(ref);
		}
		obs_source_release(source);
	}
	if (refs.empty()) {
		sources->addItem(label("Dock.NoSources"));
	} else {
		const int oldIndex = sources->findData(previous);
		sources->setCurrentIndex(oldIndex >= 0 ? oldIndex : 0);
	}
	// Selection is restored by UUID; deleted sources release the Dock lock.
	if (sourceLock->isChecked() && sources->currentData().toString() != previous) {
		const QSignalBlocker lockBlocker(sourceLock);
		sourceLock->setChecked(false);
		updateIcons();
	}
	// Rebuild the page list only after the final source has been selected.
	if (followCurrentScene(sources->currentData().toString() != previous))
		return;
	if (sources->currentData().toString() != previous)
		selectionChanged();
	else
		updateStatus();
}
void TextPPTDock::selectionChanged()
{
	cancelPageRequest();
	pageInput->setModified(false);
	pageInput->clearFocus();
	revision = -1;
	pendingRow = -1;
	pendingSerial = 0;
	updateStatus();
}
void TextPPTDock::rememberSelection(const QString &scene)
{
	if (closing)
		return;
	const QString uuid = sources->currentData().toString();
	QString target = scene;
	if (target.isEmpty()) {
		const auto found = currentSceneSources();
		if (found.scene.isEmpty() || std::none_of(found.pages.begin(), found.pages.end(),
			[&](const auto &page) { return page.first == uuid; })) return;
		target = found.scene;
	}
	const auto key = "scenes/" + target;
	if (settings->value(key).toString() != uuid) settings->setValue(key, uuid);
}
bool TextPPTDock::followCurrentScene(bool force)
{
	// Manual preview requests and the Dock lock take priority over following.
	if (closing || sourceLock->isChecked() || pageRequest.isValid())
		return false;
	const auto found = currentSceneSources(true, force ? QString() : followedScene);
	// Apply source eligibility when the scene changes.
	if (!force && found.scene == followedScene)
		return false;
	// Keep manual control when the scene has no eligible source.
	if (found.pages.empty()) {
		followedScene = found.scene;
		return false;
	}
	const QString saved = settings->value("scenes/" + found.scene).toString();
	QString chosen;
	const auto contains = [&](const QString &uuid) {
		return std::any_of(found.pages.begin(), found.pages.end(), [&](const auto &page) { return page.first == uuid; });
	};
	if (contains(saved))
		chosen = saved;
	else if (contains(sources->currentData().toString()))
		chosen = sources->currentData().toString();
	else {
		const auto visible = std::find_if(found.pages.begin(), found.pages.end(), [](const auto &page) { return page.second; });
		chosen = visible != found.pages.end() ? visible->first : found.pages.front().first;
	}
	const int index = sources->findData(chosen);
	if (index < 0) {
		// Retry scene selection after the source list is refreshed.
		followedScene.clear();
		return false;
	}
	followedScene = found.scene;
	if (sources->currentIndex() != index) {
		const QSignalBlocker blocker(sources);
		sources->setCurrentIndex(index);
		selectionChanged();
		return true;
	}
	return false;
}
void TextPPTDock::showContextMenu(const QPoint &pos)
{
	if (closing)
		return;
	auto *source = selected();
	QMenu menu(this);
	auto *properties = menu.addAction(label("Dock.Properties"));
	properties->setObjectName("TextPPTPropertiesAction");
	properties->setEnabled(source != nullptr);
	QAction *edit = nullptr;
	QAction *reload = nullptr;
	if (source) {
		auto *data = obs_source_get_settings(source);
		const char *mode = textppt_input_mode(data);
		if (strcmp(mode, INPUT_CUSTOM) == 0) {
			edit = menu.addAction(label("Dock.EditText"));
			edit->setObjectName("TextPPTEditAction");
		} else if (strcmp(mode, INPUT_FILE) == 0 || strcmp(mode, INPUT_FILES) == 0) {
			reload = menu.addAction(label("Dock.ReloadFiles"));
			reload->setObjectName("TextPPTReloadAction");
		}
		obs_data_release(data);
	}
	auto *action = menu.exec(mapToGlobal(pos));
	if (action && source && !closing && !obs_source_removed(source)) {
		if (action == properties) {
			obs_frontend_open_source_properties(source);
		} else if (action == reload) {
			calldata_t data = {};
			proc_handler_call(obs_source_get_proc_handler(source), "reload_files", &data);
			calldata_free(&data);
		} else if (action == edit) {
			auto *data = obs_source_get_settings(source);
			const bool custom = strcmp(textppt_input_mode(data), INPUT_CUSTOM) == 0;
			obs_data_release(data);
			if (custom) {
				// Reuse the source's editor callback and its lifetime checks.
				auto *props = obs_source_properties(source);
				obs_property_button_clicked(obs_properties_get(props, S_EDIT_TEXT), source);
				obs_properties_destroy(props);
			}
		}
	}
	obs_source_release(source);
}
void TextPPTDock::enableControls(bool enabled)
{
	for (auto *button : {previous, play, next, restart})
		button->setEnabled(enabled);
}
void TextPPTDock::updateStatus()
{
	if (closing || !isVisible())
		return;
	// Block selection signals during status updates.
	const QSignalBlocker blocker(texts);
	auto *source = selected();
	sources->setEnabled(source != nullptr && !sourceLock->isChecked());
	sourceLock->setEnabled(source != nullptr);
	{
		const QSignalBlocker followBlocker(follow);
		follow->setEnabled(source != nullptr);
		const bool following = source ? followsScene(source) : true;
		if (follow->isChecked() != following) {
			follow->setChecked(following);
			updateIcons();
		}
	}
	{
		// Refresh the selected source's option without changing its playback.
		const QSignalBlocker progressiveBlocker(progressive);
		const QSignalBlocker pointsBlocker(points);
		auto *data = source ? obs_source_get_settings(source) : nullptr;
		progressive->setEnabled(data && textppt_can_reveal(data));
		points->setEnabled(progressive->isEnabled());
		points->setChecked(data && obs_data_get_bool(data, S_POINTS));
		progressive->setChecked(data && !points->isChecked() && obs_data_get_bool(data, S_PROGRESSIVE));
		obs_data_release(data);
	}
	enableControls(source != nullptr);
	if (!source) {
		cancelPageRequest();
		updateImageControls({});
		fileError->setVisible(false);
		texts->clear();
		highlightRow = -1;
		status->setEnabled(false);
		status->setText(label("Dock.TotalPages").arg(0));
		pageInput->clear();
		pageInput->setEnabled(false);
		pendingRow = -1;
		pendingSerial = 0;
		return;
	}
	auto *handler = obs_source_get_proc_handler(source);
	calldata_t cd = {};
	proc_handler_call(handler, "get_status", &cd);
	const int64_t nextRevision = calldata_int(&cd, "revision");
	const int64_t count = calldata_int(&cd, "count");
	const int64_t index = calldata_int(&cd, "index");
	const int64_t acknowledged = calldata_int(&cd, "acknowledged");
	const int64_t step = calldata_int(&cd, "step");
	const int64_t steps = calldata_int(&cd, "steps");
	const bool byPoints = calldata_bool(&cd, "points");
	const bool ready = calldata_bool(&cd, "ready");
	const QString error = QString::fromUtf8(calldata_string(&cd, "error"));
	const QString group = QString::fromUtf8(calldata_string(&cd, "group_uuid"));
	calldata_free(&cd);
	updateImageControls(group);
	fileError->setText(error.section('\n', 0, 0));
	fileError->setToolTip(QStringLiteral("<span>%1</span>").arg(error.toHtmlEscaped().replace('\n', QStringLiteral("<br>"))));
	fileError->setVisible(!error.isEmpty());
	enableControls(count > 0);
	status->setEnabled(count > 0);
	pageInput->setEnabled(count > 0);
	pageInput->setAccessibleName(label("Dock.PageNumber").arg(count));
	if (nextRevision != revision) {
		pageInput->setModified(false);
		pageInput->clearFocus();
		std::vector<std::string> pages;
		cd = {};
		calldata_set_ptr(&cd, "texts", &pages);
		proc_handler_call(handler, "get_texts", &cd);
		calldata_free(&cd);
		texts->clear();
		highlightRow = -1;
		for (size_t row = 0; row < pages.size(); ++row) {
			const auto text = QString::fromUtf8(pages[row].c_str());
			auto *item = new QListWidgetItem(PageListDelegate::label(row, pages.size(), PageListDelegate::summary(text)), texts);
			item->setData(PageListDelegate::LineEndsRole, QVariant::fromValue(PageListDelegate::revealEnds(text)));
			item->setData(PageListDelegate::PointEndsRole, QVariant::fromValue(PageListDelegate::revealEnds(text, true)));
		}
		revision = nextRevision;
		pendingRow = -1;
		pendingSerial = 0;
	}
	if (pendingSerial > 0 && acknowledged >= pendingSerial) {
		pendingRow = -1;
		pendingSerial = 0;
	}
	// Keep the clicked row while the video thread is still consuming the command.
	const int row = pendingRow >= 0 ? pendingRow : count ? static_cast<int>(index) : -1;
	if (!pageMouseDown && texts->currentRow() != row)
		texts->setCurrentRow(row);
	const int revealRow = row == index && step > 0 && steps > 0 ? row : -1;
	// Only the current row changes during reveal.
	if (highlightRow != revealRow) {
		if (auto *item = texts->item(highlightRow)) {
			item->setData(PageListDelegate::HighlightRole, {});
			item->setData(PageListDelegate::ProgressRole, {});
		}
		highlightRow = revealRow;
	}
	if (auto *item = texts->item(highlightRow)) {
		const auto ends = item->data(byPoints ? PageListDelegate::PointEndsRole : PageListDelegate::LineEndsRole).value<QVector<int>>();
		item->setData(PageListDelegate::HighlightRole, ends.value(static_cast<int>(std::min<int64_t>(step - 1, ends.size() - 1))));
		item->setData(PageListDelegate::ProgressRole, QStringLiteral("%1/%2").arg(step).arg(steps));
	}
	status->setText(label("Dock.TotalPages").arg(count));
	// Do not overwrite a number while the user is typing.
	const QString number = QString::number(count ? row + 1 : 0);
	if (!pageInput->isModified() && pageInput->text() != number)
		pageInput->setText(number);
	previous->setEnabled(row > 0 || (row >= 0 && step > 1));
	next->setEnabled(row >= 0 && (row + 1 < count || step < steps));
	const bool playing = obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING;
	if (showingPause != playing) {
		showingPause = playing;
		play->setIcon(playing ? pauseIcon : playIcon);
	}
	play->setToolTip(label(playing ? "Dock.Pause" : "Dock.Play"));
	play->setAccessibleName(play->toolTip());
	bool transfer = false;
	if (pageRequest.isValid()) {
		if (pageRequest.elapsed() > 2000 || sources->currentData().toString() != requestSource ||
		    nextRevision != requestRevision || (requestSerial >= 0 && acknowledged > requestSerial) ||
		    !obs_frontend_preview_program_mode_active()) {
			cancelPageRequest();
		} else {
			const auto preview = currentSceneSources();
			if (preview.scene == requestScene && containsVisible(preview, requestSource)) {
				// Queue the page only after OBS has selected its preview scene.
				if (requestSerial < 0) {
					if (index != requestRow) sendPage(source, requestRow);
					requestSerial = pendingSerial > 0 ? pendingSerial : acknowledged;
				}
				if (ready && acknowledged == requestSerial && index == requestRow) {
					transfer = pageTransfer && !sceneTransitionRunning();
					followedScene = requestScene;
					rememberSelection(requestScene);
					cancelPageRequest();
				}
			} else if (requestSerial >= 0) {
				cancelPageRequest();
			}
		}
	}
	obs_source_release(source);
	if (transfer) {
		transitionGuard.start();
		obs_frontend_preview_program_trigger_transition();
	}
}
void TextPPTDock::updateIcons()
{
	const QColor color = palette().color(QPalette::ButtonText);
	follow->setIcon(mediaIcon(follow->isChecked() ? MediaIcon::Link : MediaIcon::Unlink, color));
	previous->setIcon(mediaIcon(MediaIcon::Previous, color));
	next->setIcon(mediaIcon(MediaIcon::Next, color));
	restart->setIcon(mediaIcon(MediaIcon::Restart, color));
	imagePrevious->setIcon(mediaIcon(MediaIcon::ImagePrevious, color));
	imageNext->setIcon(mediaIcon(MediaIcon::ImageNext, color));
	playIcon = mediaIcon(MediaIcon::Play, color);
	pauseIcon = mediaIcon(MediaIcon::Pause, color);
	play->setIcon(showingPause ? pauseIcon : playIcon);
	const bool locked = sourceLock->isChecked();
	sourceLock->setIcon(mediaIcon(locked ? MediaIcon::Lock : MediaIcon::Unlock, color));
	sourceLock->setToolTip(label(locked ? "Dock.UnlockSource" : "Dock.LockSource"));
	sourceLock->setAccessibleName(sourceLock->toolTip());
}
void TextPPTDock::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	if (event->type() == QEvent::PaletteChange && previous && play && next && restart)
		updateIcons();
}
bool TextPPTDock::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == texts->viewport()) {
		if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) {
			if (static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) pageMouseDown = true;
		} else if (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::UngrabMouse) {
			pageMouseDown = false;
		}
	}
	if (watched == texts->viewport() && event->type() == QEvent::MouseButtonDblClick &&
	    static_cast<QMouseEvent *>(event)->button() != Qt::LeftButton)
		return true;
	// Leaving an unfinished entry restores the actual page number.
	if (watched == pageInput && event->type() == QEvent::FocusOut) {
		pageInput->setModified(false);
		QTimer::singleShot(0, this, &TextPPTDock::updateStatus);
	}
	// Opening the menu must not change the playing page.
	if (watched == texts->viewport() && event->type() == QEvent::MouseButtonPress &&
	    static_cast<QMouseEvent *>(event)->button() == Qt::RightButton)
		return true;
	return QWidget::eventFilter(watched, event);
}
void TextPPTDock::jumpToPage()
{
	if (closing)
		return;
	auto *source = selected();
	if (!source)
		return;
	calldata_t cd = {};
	proc_handler_call(obs_source_get_proc_handler(source), "get_status", &cd);
	const int count = static_cast<int>(calldata_int(&cd, "count"));
	calldata_free(&cd);
	obs_source_release(source);
	bool valid = false;
	const int number = pageInput->text().toInt(&valid);
	pageInput->setModified(false);
	if (valid && number > 0 && number <= count)
		jump(number - 1);
	else
		updateStatus();
	pageInput->selectAll();
}
void TextPPTDock::jump(int row)
{
	cancelPageRequest();
	if (row < 0)
		return;
	if (auto *source = selected()) {
		sendPage(source, row);
		updateStatus();
		obs_source_release(source);
	}
}
void TextPPTDock::sendPage(obs_source_t *source, int row)
{
	calldata_t data = {};
	calldata_set_int(&data, "index", row);
	if (proc_handler_call(obs_source_get_proc_handler(source), "dock_transition", &data)) {
		pendingRow = row;
		pendingSerial = calldata_int(&data, "serial");
	}
	calldata_free(&data);
}
void TextPPTDock::cancelPageRequest()
{
	if (pageRequest.isValid() && pendingSerial == 0) pendingRow = -1;
	pageRequest.invalidate();
	pageTransfer = false;
}
void TextPPTDock::previewPage(int row)
{
	if (closing || !isVisible() || row < 0) return;
	const auto uuid = sources->currentData().toString();
	// A double-click's final release must preserve its pending transfer.
	if (pageRequest.isValid() && requestSource == uuid && requestRow == row) return;
	cancelPageRequest();
	auto *source = selected();
	if (!source) return;
	calldata_t data = {};
	proc_handler_call(obs_source_get_proc_handler(source), "get_status", &data);
	const int64_t current = calldata_int(&data, "index"), nextRevision = calldata_int(&data, "revision");
	const bool valid = row < calldata_int(&data, "count") && revision == nextRevision;
	calldata_free(&data);
	obs_source_release(source);
	if (!valid) { updateStatus(); return; }
	const bool change = pendingRow >= 0 ? pendingRow != row : current != row;
	if (!obs_frontend_preview_program_mode_active()) {
		if (change) jump(row);
		return;
	}
	if (sceneTransitionRunning()) { updateStatus(); return; }
	const auto preview = currentSceneSources();
	if (containsVisible(preview, uuid)) {
		if (change) jump(row);
		return;
	}
	// Search only on a user action; shared sources need an explicit scene choice.
	const auto choices = previewScenes(uuid);
	if (choices.empty()) {
		QToolTip::showText(texts->mapToGlobal(QPoint(8, 8)), label("Dock.NoPreviewScene"), texts);
		updateStatus();
		return;
	}
	QString target = choices.front().first;
	if (choices.size() > 1) {
		QMenu menu(this);
		menu.setObjectName("TextPPTPreviewScenes");
		menu.addSection(label("Dock.ChoosePreviewScene"));
		for (const auto &choice : choices)
			menu.addAction(QString(choice.second).replace('&', QStringLiteral("&&")))->setData(choice.first);
		auto *action = menu.exec(texts->viewport()->mapToGlobal(texts->visualItemRect(texts->item(row)).bottomLeft()));
		if (!action) { updateStatus(); return; }
		target = action->data().toString();
	}
	// Menus can outlive their source, scene or page list.
	if (closing || !isVisible()) return;
	source = selected();
	data = {};
	if (source) proc_handler_call(obs_source_get_proc_handler(source), "get_status", &data);
	const bool unchanged = source && uuid == sources->currentData().toString() &&
		nextRevision == calldata_int(&data, "revision") && row < calldata_int(&data, "count");
	calldata_free(&data);
	obs_source_release(source);
	auto *scene = obs_get_source_by_uuid(target.toUtf8().constData());
	if (!unchanged || closing || !isVisible() || !obs_frontend_preview_program_mode_active() ||
	    currentSceneSources().scene != preview.scene || sceneTransitionRunning() || !scene ||
	    obs_source_removed(scene) || !obs_scene_from_source(scene)) {
		obs_source_release(scene);
		updateStatus();
		return;
	}
	SceneSources found;
	found.visited.emplace(obs_source_get_uuid(scene), true);
	obs_scene_enum_items(obs_scene_from_source(scene), collectSceneSources, &found);
	if (containsVisible(found, uuid)) {
		requestSource = uuid;
		requestScene = target;
		requestRow = row;
		requestRevision = nextRevision;
		requestSerial = -1;
		pendingRow = row;
		pendingSerial = 0;
		// Keep manual selection across queued scene events and superseding page clicks.
		followedScene = target;
		rememberSelection(target);
		pageRequest.start();
		obs_frontend_set_current_preview_scene(scene);
	}
	obs_source_release(scene);
	updateStatus();
}
void TextPPTDock::transitionPage(int row)
{
	if (closing || !isVisible() || row < 0 || row != texts->currentRow() ||
	    !obs_frontend_preview_program_mode_active() || sceneTransitionRunning() ||
	    (transitionGuard.isValid() && transitionGuard.elapsed() < QApplication::doubleClickInterval()))
		return;
	const auto uuid = sources->currentData().toString();
	if (pageRequest.isValid()) {
		if (requestSource == uuid && requestRow == row) pageTransfer = true;
		return;
	}
	const auto preview = currentSceneSources();
	if (!containsVisible(preview, uuid)) return;
	auto *source = selected();
	if (!source)
		return;
	calldata_t data = {};
	proc_handler_call(obs_source_get_proc_handler(source), "get_status", &data);
	requestSerial = pendingSerial > 0 ? pendingSerial : calldata_int(&data, "acknowledged");
	calldata_free(&data);
	obs_source_release(source);
	requestSource = uuid;
	requestScene = preview.scene;
	requestRevision = revision;
	requestRow = row;
	pageTransfer = true;
	// Reuse status polling until the selected page is ready for OBS's native transition.
	pageRequest.start();
}
void TextPPTDock::control(int action)
{
	cancelPageRequest();
	if (auto *source = selected()) {
		switch (action) {
		case 0: obs_source_media_previous(source); break;
		case 1: obs_source_media_next(source); break;
		case 2: obs_source_media_play_pause(source, obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING); break;
		case 3: obs_source_media_restart(source); break;
		}
		obs_source_release(source);
	}
}

void TextPPTDock::imageSceneEvent(void *data, calldata_t *)
{
	static_cast<TextPPTDock *>(data)->imageDirty = true;
}

struct ImageSearch {
	TextPPTDock *dock;
	std::unordered_set<std::string> scenes;
	std::unordered_set<std::string> slides;
};

bool TextPPTDock::collectImageSlides(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	if (!obs_sceneitem_visible(item))
		return true;
	auto &search = *static_cast<ImageSearch *>(param);
	auto *source = obs_sceneitem_get_source(item);
	if (obs_source_removed(source))
		return true;
	const char *uuid = obs_source_get_uuid(source);
	if (strcmp(obs_source_get_unversioned_id(source), "slideshow") == 0) {
		if (search.slides.insert(uuid).second)
			search.dock->imageSlides.push_back(QString::fromUtf8(uuid));
	} else if (auto *scene = obs_sceneitem_is_group(item) ?
		obs_sceneitem_group_get_scene(item) : obs_scene_from_source(source)) {
		if (search.scenes.insert(uuid).second) {
			search.dock->imageScenes.push_back(obs_source_get_weak_source(source));
			for (const char *signal : imageSceneSignals)
				signal_handler_connect(obs_source_get_signal_handler(source), signal, imageSceneEvent, search.dock);
			obs_scene_enum_items(scene, collectImageSlides, &search);
		}
	}
	return true;
}

void TextPPTDock::clearImageControls(bool hide)
{
	for (auto *ref : imageScenes) {
		if (auto *source = obs_weak_source_get_source(ref)) {
			for (const char *signal : imageSceneSignals)
				signal_handler_disconnect(obs_source_get_signal_handler(source), signal, imageSceneEvent, this);
			obs_source_release(source);
		}
		obs_weak_source_release(ref);
	}
	imageScenes.clear();
	imageSlides.clear();
	imageGroup.clear();
	if (hide)
		imageControls->hide();
}

void TextPPTDock::updateImageControls(const QString &group)
{
	// Reuse the status timer; rescan only after page or scene-item changes.
	const bool dirty = imageDirty.exchange(false);
	if (!dirty && group == imageGroup)
		return;
	clearImageControls(false);
	imageGroup = group;
	if (group.isEmpty()) {
		imageControls->hide();
		return;
	}
	auto *source = obs_get_source_by_uuid(group.toUtf8().constData());
	if (source && !obs_source_removed(source)) {
		if (auto *scene = obs_group_from_source(source)) {
			ImageSearch search{this, {obs_source_get_uuid(source)}, {}};
			imageScenes.push_back(obs_source_get_weak_source(source));
			for (const char *signal : imageSceneSignals)
				signal_handler_connect(obs_source_get_signal_handler(source), signal, imageSceneEvent, this);
			obs_scene_enum_items(scene, collectImageSlides, &search);
		}
	}
	obs_source_release(source);
	imageControls->setVisible(!imageSlides.empty());
}

static QString currentGroup(obs_source_t *source)
{
	calldata_t data = {};
	proc_handler_call(obs_source_get_proc_handler(source), "get_status", &data);
	const QString group = QString::fromUtf8(calldata_string(&data, "group_uuid"));
	calldata_free(&data);
	return group;
}

void TextPPTDock::controlImage(bool forward)
{
	if (closing)
		return;
	auto *source = selected();
	if (!source)
		return;
	const QString controller = QString::fromUtf8(obs_source_get_uuid(source));
	const QString group = currentGroup(source);
	obs_source_release(source);
	updateImageControls(group);
	if (imageSlides.empty())
		return;
	QString chosen = imageSlides.front();
	if (imageSlides.size() > 1) {
		QMenu menu(this);
		menu.setObjectName("TextPPTImageMenu");
		for (const auto &uuid : imageSlides) {
			auto *slide = obs_get_source_by_uuid(uuid.toUtf8().constData());
			if (slide && !obs_source_removed(slide)) {
				auto *action = menu.addAction(QString::fromUtf8(obs_source_get_name(slide)).replace('&', "&&"));
				action->setData(uuid);
			}
			obs_source_release(slide);
		}
		auto *button = forward ? imageNext : imagePrevious;
		auto *action = menu.exec(button->mapToGlobal(QPoint(0, button->height())));
		if (!action || closing)
			return;
		chosen = action->data().toString();
	}
	// Menus can outlive a page or source; validate before forwarding the click.
	source = selected();
	const bool same = source && controller == QString::fromUtf8(obs_source_get_uuid(source)) && group == currentGroup(source);
	obs_source_release(source);
	if (!same)
		return;
	updateImageControls(group);
	if (!imageSlides.contains(chosen))
		return;
	auto *slide = obs_get_source_by_uuid(chosen.toUtf8().constData());
	if (slide && !obs_source_removed(slide)) {
		if (forward)
			obs_source_media_next(slide);
		else
			obs_source_media_previous(slide);
	}
	obs_source_release(slide);
}

void TextPPTDock::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	refreshSources();
	timer->start();
}
void TextPPTDock::hideEvent(QHideEvent *event)
{
	pageMouseDown = false;
	cancelPageRequest();
	timer->stop();
	clearImageControls();
	QWidget::hideEvent(event);
}
