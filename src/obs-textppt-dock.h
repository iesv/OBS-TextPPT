// Derived from obs-text-slideshow (Joshua Wong), GPL-2.0-or-later.
#pragma once
#include <obs-frontend-api.h>
#include <QWidget>
#include <QIcon>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <atomic>
#include <vector>

class QComboBox;
class QListWidget;
class QLabel;
class QToolButton;
class QTimer;
class QSettings;
class QLineEdit;
class QCheckBox;

class TextPPTDock : public QWidget {
	Q_OBJECT
public:
	explicit TextPPTDock(QWidget *parent = nullptr);
	~TextPPTDock() override;
protected:
	bool eventFilter(QObject *, QEvent *) override;
	void changeEvent(QEvent *) override;
	void showEvent(QShowEvent *) override;
	void hideEvent(QHideEvent *) override;
private:
	QComboBox *sources = nullptr;
	QListWidget *texts = nullptr;
	QLabel *status = nullptr;
	QLineEdit *pageInput = nullptr;
	QToolButton *follow = nullptr;
	QCheckBox *progressive = nullptr;
	QCheckBox *points = nullptr;
	QToolButton *sourceLock = nullptr;
	QLabel *fileError = nullptr;
	QToolButton *previous = nullptr;
	QToolButton *play = nullptr;
	QToolButton *next = nullptr;
	QToolButton *restart = nullptr;
	QWidget *imageControls = nullptr;
	QToolButton *imagePrevious = nullptr;
	QToolButton *imageNext = nullptr;
	QString imageGroup;
	QStringList imageSlides;
	std::vector<obs_weak_source_t *> imageScenes;
	std::atomic<bool> imageDirty{true};
	QIcon playIcon, pauseIcon;
	bool showingPause = false;
	std::vector<obs_weak_source_t *> refs;
	int64_t revision = -1;
	int pendingRow = -1;
	int highlightRow = -1;
	int64_t pendingSerial = 0;
	QElapsedTimer pageRequest, transitionGuard;
	QString requestSource, requestScene;
	int requestRow = -1;
	int64_t requestSerial = -1, requestRevision = -1;
	bool pageMouseDown = false, pageTransfer = false;
	QTimer *timer = nullptr;
	QSettings *settings = nullptr;
	QString followedScene;
	obs_source_t *sceneTransition = nullptr;
	std::atomic<bool> refreshPending{false};
	bool closing = false;
	static void frontendEvent(obs_frontend_event, void *);
	static void sourceEvent(void *, calldata_t *);
	static void transitionEvent(void *, calldata_t *);
	static void imageSceneEvent(void *, calldata_t *);
	static bool collectImageSlides(obs_scene_t *, obs_sceneitem_t *, void *);
	void scheduleRefresh();
	void refreshSources();
	void updateStatus();
	void jump(int row);
	void sendPage(obs_source_t *, int row);
	void previewPage(int row);
	void cancelPageRequest();
	void transitionPage(int row);
	void jumpToPage();
	void control(int action);
	void controlImage(bool forward);
	void updateImageControls(const QString &group);
	void clearImageControls(bool hide = true);
	void updateIcons();
	void enableControls(bool);
	void selectionChanged();
	bool followCurrentScene(bool force = false);
	void bindSceneTransition();
	void rememberSelection(const QString &scene = {});
	void showContextMenu(const QPoint &);
	obs_source_t *selected();
	void releaseSources();
};
