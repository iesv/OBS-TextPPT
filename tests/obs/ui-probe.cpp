// OBS-TextPPT, GPL-2.0-or-later. Test module; never installed with the plugin.
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QLineEdit>
#include <QListWidget>
#include <QLabel>
#include <QPixmap>
#include <QImage>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QUuid>
#include <QSize>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>
#include <QSignalBlocker>
#include <QToolButton>
#include <QFontMetricsF>
#include <QMenu>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QAction>
#include <QCheckBox>
#include <QFormLayout>
#include <QShortcut>
#include <QSlider>
#include <QShortcutEvent>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QStyleOptionSlider>
#include <QStyleOptionButton>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <utility>
#include <functional>
#include "plugin-macros.generated.h"

OBS_DECLARE_MODULE()
static QFile report;
static bool passed = true;
extern void textppt_stress_probe(const QString &);
extern uint64_t textppt_test_memory();
extern bool textppt_progressive_probe(obs_scene_t *, const char *, const QString &, QWidget *);
extern bool textppt_studio_probe(const char *, const QString &, QWidget *);
extern bool textppt_native_studio_probe(const char *, const QString &, QWidget *);

static bool check(bool condition, const char *message)
{
	passed &= condition;
	QTextStream stream(&report);
	stream << (condition ? "PASS: " : "FAIL: ") << message << '\n';
	stream.flush();
	return condition;
}

class PropertiesPaintProbe : public QObject {
public:
	explicit PropertiesPaintProbe(obs_source_t *source)
		: title(QString::fromUtf8(obs_frontend_get_locale_string("Basic.PropertiesWindow"))
		        .arg(QString::fromUtf8(obs_source_get_name(source))))
	{
		qApp->installEventFilter(this);
	}
	bool seen = false;
	int captionOffset = 0;

protected:
	bool eventFilter(QObject *object, QEvent *event) override
	{
		if (seen || event->type() != QEvent::Paint)
			return false;
		auto *container = qobject_cast<QWidget *>(object);
		if (!container || container->objectName() != "PropertiesContainer" ||
		    !container->window()->inherits("OBSBasicProperties") || container->window()->windowTitle() != title)
			return false;
		auto *input = container->findChild<QPlainTextEdit *>();
		if (!input)
			return false;
		auto *view = container->parentWidget();
		while (view && !view->inherits("OBSPropertiesView"))
			view = view->parentWidget();
		seen = true;
		// Inspect the first content paint, before waiting can conceal a layout jump.
		check(input->height() >= qMax(160, input->fontMetrics().lineSpacing() * 8),
		      "custom text has its full height on the first paint");
		check(view && view->parentWidget() && qAbs(view->width() - view->parentWidget()->contentsRect().width()) <= 1,
		      "properties fill their frame on the first paint");
		auto *info = container->findChild<QLabel *>("TextPPTInfo");
		auto *form = qobject_cast<QFormLayout *>(container->layout());
		auto *editButton = container->findChild<QPushButton *>("TextPPTEditButton");
		check(form && editButton && editButton->parentWidget() == form->labelForField(input) &&
		      editButton->text().isEmpty() && !editButton->icon().isNull(),
		      "editor icon shares the content label on the first paint");
		auto *caption = form ? form->labelForField(input) : nullptr;
		captionOffset = caption ? caption->y() - input->y() : 0;
		check(caption && caption->height() < input->height() / 2 && qAbs(captionOffset) <= 8,
		      "content caption stays at the top of the input on the first paint");
		int row = -1;
		QFormLayout::ItemRole role = QFormLayout::FieldRole;
		if (form && info)
			form->getWidgetPosition(info, &row, &role);
		check(info && row >= 0 && role == QFormLayout::SpanningRole &&
		      (info->alignment() & Qt::AlignRight) && info->openExternalLinks(),
		      "version and linked author span a right-aligned footer");
		check(info && form && qAbs(info->geometry().right() - form->contentsRect().right()) <= 1,
		      "the version footer reaches the right edge of the form");
		return false;
	}

private:
	QString title;
};

static void wait(int milliseconds)
{
	QEventLoop loop;
	QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
	loop.exec();
}
bool textppt_header_probe(QWidget *dock, const QString &path)
{
	QFile details(path);
	if (!details.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
	bool passed = true;
	const auto check = [&](bool value, const char *message) {
		QTextStream(&details) << (value ? "PASS: " : "FAIL: ") << message << '\n';
		passed &= value;
		return value;
	};
	auto *header = dock->layout()->itemAt(0)->layout();
	if (check(header && header->count() == 5, "Dock header has five controls")) {
		const auto original = dock->size();
		const int widths[] = {original.width(), dock->minimumSizeHint().width(), 640};
		for (int width : widths) {
			dock->resize(width, original.height()); wait(100);
			QTextStream(&details) << "Header width: " << dock->width() << '\n';
			QWidget *previous = nullptr;
			for (int index = 0; index < header->count(); ++index) {
				auto *control = header->itemAt(index)->widget();
				if (!check(control != nullptr, "Header item is a widget")) continue;
				QTextStream(&details) << control->objectName() << ": " << control->x() << '/' << control->width() << '\n';
				check(dock->rect().contains(control->geometry()), "Header control stays within the Dock");
				check(!previous || previous->geometry().right() < control->x(), "Adjacent header controls do not overlap");
				if (auto *box = qobject_cast<QCheckBox *>(control)) {
					QStyleOptionButton option;
					option.initFrom(box); option.text = box->text();
					check(box->style()->subElementRect(QStyle::SE_CheckBoxContents, &option, box).width() >=
					      box->fontMetrics().horizontalAdvance(box->text()), "Checkbox label fits its content area");
				}
				previous = control;
			}
			dock->grab().save(path + QStringLiteral(".%1.png").arg(width));
		}
		dock->resize(original); wait(100);
	}
	check(passed, "HEADER TESTS COMPLETE");
	return passed;
}
static void whenEditor(std::function<void(QDialog *)> inspect)
{
	// Cocoa can finish closing a popup after the next timer callback.
	auto *timer = new QTimer(qApp);
	timer->setInterval(50);
	QObject::connect(timer, &QTimer::timeout, timer, [timer, inspect = std::move(inspect), attempts = 0]() mutable {
		QDialog *editor = nullptr;
		for (auto *widget : QApplication::topLevelWidgets()) {
			auto *dialog = qobject_cast<QDialog *>(widget);
			auto *input = dialog ? dialog->findChild<QPlainTextEdit *>("TextPPTEditor") : nullptr;
			if (dialog && dialog->isVisible() && input && input->window() == dialog) {
				editor = dialog;
				break;
			}
		}
		if (!editor && ++attempts < 100)
			return;
		timer->stop();
		timer->deleteLater();
		inspect(editor);
	});
	timer->start();
}
static int page(obs_source_t *source)
{
	calldata_t data = {};
	proc_handler_call(obs_source_get_proc_handler(source), "get_status", &data);
	const int index = static_cast<int>(calldata_int(&data, "index"));
	calldata_free(&data);
	return index;
}
static std::vector<std::string> nativeBuffers(obs_source_t *source)
{
	std::vector<std::string> ids;
	obs_source_enum_full_tree(source, [](obs_source_t *, obs_source_t *child, void *data) {
		const char *id = obs_source_get_unversioned_id(child);
		if (strcmp(id, "text_gdiplus") == 0 || strcmp(id, "text_ft2_source") == 0)
			static_cast<std::vector<std::string> *>(data)->emplace_back(obs_source_get_uuid(child));
	}, &ids);
	return ids;
}
static void nativeSizing(obs_scene_t *scene, const char *kind)
{
	auto *settings = obs_data_create();
	obs_data_set_string(settings, "input_mode", "custom");
	obs_data_set_string(settings, "custom_text", "Font size test\nTextPPT");
	obs_data_set_string(settings, "slide_mode", "mode_manual");
	obs_data_set_string(settings, "transition", "cut");
	auto *source = obs_source_create_private(kind, "TextPPT sizing probe", settings);
	obs_data_release(settings);
	auto *item = source ? obs_scene_add(scene, source) : nullptr;
	if (!check(item != nullptr, "sizing test source is created")) {
		obs_source_release(source);
		return;
	}
	const auto dimensions = [](obs_source_t *input) {
		return QSize(obs_source_get_width(input), obs_source_get_height(input));
	};
	const auto setFont = [&](int size) {
		auto *current = obs_source_get_settings(source);
		auto *font = obs_data_get_obj(current, "font");
		auto *copy = obs_data_get_defaults(font);
		obs_data_apply(copy, font);
		obs_data_set_int(copy, "size", size);
		auto *update = obs_data_create();
		obs_data_set_obj(update, "font", copy);
		obs_source_update(source, update);
		obs_data_release(update);
		obs_data_release(copy);
		obs_data_release(font);
		obs_data_release(current);
		wait(600);
	};
	wait(600);
	const auto rendererIds = nativeBuffers(source);
	setFont(96);
	const auto large = dimensions(source);
	setFont(48);
	const auto small = dimensions(source);
	check(small.width() > 1 && small.height() > 1 &&
	      small.width() < large.width() && small.height() < large.height(),
	      "reducing the font shrinks automatic bounds");
	setFont(96);
	check(dimensions(source) == large, "enlarging the font restores automatic bounds");
	setFont(48);
	check(dimensions(source) == small, "repeated font edits do not retain larger bounds");
	check(!rendererIds.empty() && nativeBuffers(source) == rendererIds,
	      "font edits retain the displayed native renderer");
	auto *copy = obs_source_duplicate(source, "TextPPT sizing copy", true);
	auto *copyItem = copy ? obs_scene_add(scene, copy) : nullptr;
	if (check(copyItem != nullptr, "private source duplication succeeds")) {
		wait(600);
		check(dimensions(copy) == small, "copied sources retain the edited font dimensions");
		obs_sceneitem_remove(copyItem);
	}
	obs_source_release(copy);
	settings = obs_data_create();
	obs_data_set_string(settings, "use_custom_size", "800x300");
	obs_source_update(source, settings);
	wait(300);
	setFont(96);
	setFont(48);
	check(dimensions(source) == QSize(800, 300), "font edits preserve explicit canvas dimensions");
	obs_data_set_string(settings, "use_custom_size", "auto");
	obs_source_update(source, settings);
	obs_data_release(settings);
	wait(300);
	check(dimensions(source) == small, "automatic bounds shrink after leaving a fixed canvas");
	check(obs_source_media_get_state(source) == OBS_MEDIA_STATE_PAUSED && page(source) == 0,
	      "font edits preserve the page and paused state");
	settings = obs_data_create();
	obs_data_set_string(settings, "custom_text", "A wider page for layout testing\nSecond line\nThird line\n\nTiny");
	obs_source_update(source, settings);
	obs_data_release(settings);
	wait(600);
	const auto deckBounds = dimensions(source);
	obs_source_media_next(source);
	wait(600);
#ifdef _WIN32
	const char *colorKey = "color";
#else
	const char *colorKey = "color1";
#endif
	for (int64_t color : {int64_t{0xFF0000FF}, int64_t{0xFF00FF00}, int64_t{0}}) {
		settings = obs_data_create();
		obs_data_set_int(settings, colorKey, color);
#ifdef _WIN32
		obs_data_set_int(settings, "opacity", color == 0 ? 0 : 50);
		obs_data_set_bool(settings, "gradient", true);
		obs_data_set_int(settings, "gradient_color", 0xFF0000);
		obs_data_set_int(settings, "gradient_opacity", color == 0 ? 0 : 50);
		obs_data_set_int(settings, "bk_color", 0x123456);
		obs_data_set_int(settings, "bk_opacity", 25);
#endif
		obs_source_update(source, settings);
		obs_data_release(settings);
		wait(600);
		check(dimensions(source) == deckBounds && page(source) == 1 &&
		      obs_source_media_get_state(source) == OBS_MEDIA_STATE_PAUSED,
		      "color and opacity edits retain canvas, page and playback state");
		obs_source_enum_full_tree(source, [](obs_source_t *, obs_source_t *child, void *data) {
			const char *id = obs_source_get_unversioned_id(child);
			if (strcmp(id, "text_gdiplus") != 0 && strcmp(id, "text_ft2_source") != 0)
				return;
			auto *style = obs_source_get_settings(child);
#ifdef _WIN32
			const char *key = "color";
#else
			const char *key = "color1";
#endif
			const auto expected = *static_cast<int64_t *>(data);
			check(obs_data_get_int(style, key) == expected, "native text receives edited colors, including black");
#ifdef _WIN32
			check(obs_data_get_int(style, "opacity") == (expected == 0 ? 0 : 50) &&
			      obs_data_get_int(style, "gradient_opacity") == (expected == 0 ? 0 : 50) &&
			      obs_data_get_int(style, "bk_color") == 0x123456 && obs_data_get_int(style, "bk_opacity") == 25,
			      "native text receives text, gradient and background opacity");
#endif
			obs_data_release(style);
		}, &color);
	}
	obs_sceneitem_remove(item);
	obs_source_release(source);
}
static void nativeFontParity(obs_scene_t *scene, const char *kind)
{
#ifdef _WIN32
	const char *native = "text_gdiplus";
#else
	const char *native = "text_ft2_source";
#endif
	const char *text = "ABC \xE4\xB8\xAD\xE6\x96\x87\nSecond line";
	auto *settings = obs_data_create();
	const std::string deck = std::string(text) + "\n\nNext page";
	obs_data_set_string(settings, "custom_text", deck.c_str());
	obs_data_set_string(settings, "slide_mode", "mode_manual");
	obs_data_set_string(settings, "transition", "cut");
	auto *source = obs_source_create_private(kind, "TextPPT native font probe", settings);
	obs_data_release(settings);
	settings = obs_data_create();
	obs_data_set_string(settings, "text", text);
	auto *reference = obs_source_create_private(obs_get_latest_input_type_id(native), "Native font reference", settings);
	obs_data_release(settings);
	auto *item = source ? obs_scene_add(scene, source) : nullptr;
	if (!check(item && reference, "native font comparison sources are created")) {
		if (item) obs_sceneitem_remove(item);
		obs_source_release(source);
		obs_source_release(reference);
		return;
	}
	wait(600);
	const auto rendererIds = nativeBuffers(source);
	const auto matches = [&] {
		bool same = false;
		std::pair<obs_source_t *, bool *> context{reference, &same};
		obs_source_enum_full_tree(source, [](obs_source_t *, obs_source_t *child, void *data) {
			auto *context = static_cast<std::pair<obs_source_t *, bool *> *>(data);
			if (strcmp(obs_source_get_unversioned_id(child), obs_source_get_unversioned_id(context->first)) != 0)
				return;
			auto *actualSettings = obs_source_get_settings(child);
			auto *expectedSettings = obs_source_get_settings(context->first);
			auto *actualFont = obs_data_get_obj(actualSettings, "font");
			auto *expectedFont = obs_data_get_obj(expectedSettings, "font");
			*context->second = obs_source_get_width(child) == obs_source_get_width(context->first) &&
				obs_source_get_height(child) == obs_source_get_height(context->first) &&
				QJsonDocument::fromJson(obs_data_get_json_with_defaults(actualFont)) ==
					QJsonDocument::fromJson(obs_data_get_json_with_defaults(expectedFont));
			obs_data_release(actualFont);
			obs_data_release(expectedFont);
			obs_data_release(actualSettings);
			obs_data_release(expectedSettings);
		}, &context);
		return same;
	};
	check(matches(), "default text dimensions and font settings match the native source");
	auto *referenceSettings = obs_source_get_settings(reference);
	auto *originalFont = obs_data_get_obj(referenceSettings, "font");
	auto *font = obs_data_get_defaults(originalFont);
	obs_data_apply(font, originalFont);
	obs_data_release(originalFont);
	obs_data_release(referenceSettings);
	const std::string face = obs_data_get_string(font, "face");
	bool same = true;
	for (int change = 0; change < 36; ++change) {
		obs_data_set_int(font, "size", change % 3 == 0 ? 24 : change % 3 == 1 ? 48 : 96);
		obs_data_set_int(font, "flags", change % 2 ? OBS_FONT_BOLD : 0);
		obs_data_set_string(font, "face", change % 6 == 4 ? "__TextPPT_Unavailable_Font__" : face.c_str());
		settings = obs_data_create();
		obs_data_set_obj(settings, "font", font);
		obs_source_update(reference, settings);
		obs_source_update(source, settings);
		obs_data_release(settings);
		wait(150);
		QElapsedTimer settling;
		settling.start();
		bool equal = matches();
		// Native video sources apply updates on the video thread.
		while (!equal && settling.elapsed() < 2000) {
			wait(20);
			equal = matches();
		}
		if (settling.elapsed() >= 20 || !equal)
			QTextStream(&report) << "Font case " << change + 1 << ": " << obs_data_get_string(font, "face")
				<< ", size " << obs_data_get_int(font, "size") << ", flags " << obs_data_get_int(font, "flags")
				<< ", additional settling ms " << settling.elapsed() << ", native match " << equal << '\n';
		const bool stable = nativeBuffers(source) == rendererIds && page(source) == 0 &&
			obs_source_media_get_state(source) == OBS_MEDIA_STATE_PAUSED;
		const auto message = QStringLiteral("font edit %1 matches native rendering and preserves buffers and playback").arg(change + 1).toUtf8();
		same &= check(equal && stable, message.constData());
	}
	check(same, "36 font edits and unavailable fonts match native updates without replacing the renderer");
	obs_source_t *first = nullptr;
	obs_source_enum_full_tree(source, [](obs_source_t *, obs_source_t *child, void *data) {
		const char *id = obs_source_get_unversioned_id(child);
		if (strcmp(id, "text_gdiplus") == 0 || strcmp(id, "text_ft2_source") == 0)
			*static_cast<obs_source_t **>(data) = obs_source_get_ref(child);
	}, &first);
	obs_source_media_next(source);
	wait(600);
	bool isolated = false;
	std::pair<obs_source_t *, bool *> context{first, &isolated};
	obs_source_enum_full_tree(source, [](obs_source_t *, obs_source_t *child, void *data) {
		auto *context = static_cast<std::pair<obs_source_t *, bool *> *>(data);
		const char *id = obs_source_get_unversioned_id(child);
		if (!context->first || (strcmp(id, "text_gdiplus") != 0 && strcmp(id, "text_ft2_source") != 0))
			return;
		auto *previous = obs_source_get_settings(context->first);
		auto *current = obs_source_get_settings(child);
		*context->second = previous != current &&
			strcmp(obs_data_get_string(previous, "text"), "ABC \xE4\xB8\xAD\xE6\x96\x87\nSecond line") == 0 &&
			strcmp(obs_data_get_string(current, "text"), "Next page") == 0;
		obs_data_release(previous);
		obs_data_release(current);
	}, &context);
	check(isolated, "page buffers own separate settings and preserve the outgoing text");
	obs_source_release(first);
	obs_data_release(font);
	obs_sceneitem_remove(item);
	obs_source_release(source);
	obs_source_release(reference);
}
static void enterPage(QLineEdit *input, const char *number)
{
	input->setFocus();
	input->setText(number);
	input->setModified(true);
	QKeyEvent event(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
	QApplication::sendEvent(input, &event);
}
static void contextAction(QWidget *dock, const char *name = "TextPPTPropertiesAction")
{
	QTimer::singleShot(50, qApp, [name] {
		auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		if (!check(menu != nullptr, "dock context menu opens"))
			return;
		auto *action = menu->findChild<QAction *>(QString::fromLatin1(name));
		if (!check(action && action->isEnabled(), "requested context action is available")) {
			menu->close();
			return;
		}
		menu->setActiveAction(action);
		QKeyEvent event(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
		QApplication::sendEvent(menu, &event);
	});
	QMetaObject::invokeMethod(dock, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, QPoint(10, 10)));
	wait(350);
}

static void textMenu(QPlainTextEdit *edit)
{
	if (!check(edit && edit->contextMenuPolicy() == Qt::CustomContextMenu, "text input has a localized context menu"))
		return;
	{
		const QSignalBlocker blocker(edit);
		const auto content = edit->toPlainText();
		const auto cursor = edit->textCursor();
		edit->moveCursor(QTextCursor::End);
		QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
		QApplication::sendEvent(edit, &tab);
		check(edit->toPlainText() == content + QLatin1Char('\t'), "TAB inserts a character without changing focus");
		edit->undo();
		check(edit->toPlainText() == content, "undo restores content after TAB input");
		edit->setTextCursor(cursor);
	}
	auto *standard = edit->createStandardContextMenu();
	QMetaObject::invokeMethod(edit, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, QPoint(10, 10)));
	auto *menu = edit->findChild<QMenu *>("TextPPTTextMenu", Qt::FindDirectChildrenOnly);
	auto *module = obs_get_module("obs-textppt");
	if (check(menu && menu->isVisible() && module, "localized text menu opens")) {
		bool translated = true, unchanged = menu->actions().size() == standard->actions().size();
		for (const auto &name : {"Undo", "Redo", "Cut", "Copy", "Paste", "Delete", "SelectAll"}) {
			const auto actionName = strcmp(name, "SelectAll") == 0 ? QStringLiteral("select-all") :
				QStringLiteral("edit-") + QString::fromLatin1(name).toLower();
			auto *action = menu->findChild<QAction *>(actionName);
			auto *original = standard->findChild<QAction *>(actionName);
			const auto key = QByteArray("Editor.") + name;
			const char *text = nullptr;
			translated &= action && obs_module_get_locale_string(module, key.constData(), &text) && text &&
				action->text().section('\t', 0, 0) == QString::fromUtf8(text);
			unchanged &= action && original && action->isEnabled() == original->isEnabled() &&
				action->shortcut() == original->shortcut() &&
				action->text().section('\t', 1) == original->text().section('\t', 1);
		}
		check(translated, "all seven text actions use the plugin locale");
		check(unchanged, "text actions retain native states and platform shortcuts");
		check(edit->findChildren<QMenu *>("TextPPTTextMenu", Qt::FindDirectChildrenOnly).size() == 1,
		      "one context request opens one menu");
		const auto cursor = edit->textCursor();
		auto *selectAll = menu->findChild<QAction *>("select-all");
		if (selectAll && selectAll->isEnabled()) {
			selectAll->trigger();
			check(edit->textCursor().selectionEnd() - edit->textCursor().selectionStart() == edit->toPlainText().size(),
			      "translated Select all retains its native operation");
			edit->setTextCursor(cursor);
		}
	}
	if (menu)
		menu->close();
	delete standard;
	wait(50);
}

static std::vector<std::string> deck(obs_source_t *source)
{
	std::vector<std::string> texts;
	calldata_t data = {};
	calldata_set_ptr(&data, "texts", &texts);
	proc_handler_call(obs_source_get_proc_handler(source), "get_texts", &data);
	calldata_free(&data);
	return texts;
}

static std::vector<std::string> bufferText(obs_source_t *source)
{
	std::vector<std::string> texts;
	obs_source_enum_full_tree(source, [](obs_source_t *, obs_source_t *child, void *data) {
		const char *id = obs_source_get_unversioned_id(child);
		if (strcmp(id, "text_gdiplus") != 0 && strcmp(id, "text_ft2_source") != 0)
			return;
		auto *settings = obs_source_get_settings(child);
		check(!obs_data_get_bool(settings, "read_from_file") && !obs_data_get_bool(settings, "from_file"),
		      "file pages render validated text without file polling");
		static_cast<std::vector<std::string> *>(data)->emplace_back(obs_data_get_string(settings, "text"));
		obs_data_release(settings);
	}, &texts);
	return texts;
}

static void groupRecovery(obs_scene_t *scene, const char *kind)
{
	auto *groupItem = obs_scene_add_group(scene, "TextPPT Recovery Group");
	auto *group = groupItem ? obs_source_get_ref(obs_sceneitem_get_source(groupItem)) : nullptr;
	auto *settings = obs_data_create();
	obs_data_set_string(settings, "text", "Group content");
#ifdef _WIN32
	const char *native = "text_gdiplus";
#else
	const char *native = "text_ft2_source";
#endif
	auto *text = obs_source_create_private(obs_get_latest_input_type_id(native), "Recovery text", settings);
	obs_data_release(settings);
	if (!check(group && text, "group recovery fixtures are created")) {
		if (groupItem) obs_sceneitem_remove(groupItem);
		obs_source_release(group); obs_source_release(text); return;
	}
	obs_scene_add(obs_sceneitem_group_get_scene(groupItem), text);
	settings = obs_data_create();
	obs_data_set_string(settings, "input_mode", "groups");
	obs_data_set_string(settings, "transition", "cut");
	auto *entries = obs_data_array_create();
	auto *entry = obs_data_create();
	const char *uuid = obs_source_get_uuid(group);
	obs_data_set_string(entry, "value", obs_source_get_name(group));
	obs_data_set_string(entry, "uuid", uuid);
	obs_data_array_push_back(entries, entry);
	obs_data_release(entry);
	obs_data_set_array(settings, "groups", entries);
	obs_data_array_release(entries);
	auto *ids = obs_data_create();
	obs_data_set_string(ids, uuid, uuid);
	obs_data_set_obj(settings, "group_ids", ids);
	obs_data_release(ids);
	auto *controller = obs_source_create_private(kind, "Group recovery controller", settings);
	obs_data_release(settings);
	auto *controllerItem = controller ? obs_scene_add(scene, controller) : nullptr;
	const auto active = [&] {
		bool displayed = false;
		obs_source_enum_full_tree(controller, [](obs_source_t *, obs_source_t *child, void *data) {
			if (strcmp(obs_source_get_id(child), "cut_transition")) return;
			auto *current = obs_transition_get_active_source(child);
			*static_cast<bool *>(data) = current != nullptr;
			obs_source_release(current);
		}, &displayed);
		return displayed;
	};
	if (check(controllerItem != nullptr, "group recovery controller is created")) {
		wait(800);
		check(active(), "an existing group is displayed");
		obs_sceneitem_remove(groupItem); groupItem = nullptr;
		wait(800);
		check(!active(), "removing group membership clears the display");
		groupItem = obs_scene_add(scene, group);
		wait(800);
		check(active(), "restoring group membership restores the current page");
		obs_source_media_stop(controller); wait(800);
		check(!active() && obs_source_media_get_state(controller) == OBS_MEDIA_STATE_STOPPED,
		      "group refresh does not resume stopped playback");
		obs_sceneitem_remove(controllerItem);
	}
	if (groupItem) obs_sceneitem_remove(groupItem);
	obs_source_release(controller); obs_source_release(group); obs_source_release(text);
}

static void fileControls(QWidget *dock, obs_source_t *source)
{
	QTemporaryDir folder;
	if (!check(folder.isValid(), "reload test directory is created"))
		return;
	const QString path = folder.path() + "/slides.txt";
	const auto write = [](const QString &name, const QByteArray &text) {
		QFile file(name);
		return file.open(QIODevice::WriteOnly) && file.write(text) == text.size();
	};
	if (!check(write(path, "old one\n\nold two"), "initial reload fixture is written"))
		return;
	auto *settings = obs_data_create();
	obs_data_set_string(settings, "input_mode", "file");
	obs_data_set_string(settings, "txt_file", path.toUtf8().constData());
	obs_data_set_bool(settings, "custom_delim", false);
	obs_source_update(source, settings);
	obs_data_release(settings);
	wait(600);
	auto *error = dock->findChild<QLabel *>("TextPPTFileError");
	const auto old = deck(source);
	check(old == std::vector<std::string>{"old one", "old two"} && error && error->isHidden(),
	      "valid file content has no error row");
	check(write(path, "new one\n\nnew two"), "changed file content is written");
	settings = obs_data_create();
	obs_data_set_int(settings, "slide_time", 6000);
	obs_source_update(source, settings);
	obs_data_release(settings);
	wait(500);
	check(deck(source) == old, "playback edits reuse cached file content");
	contextAction(dock, "TextPPTReloadAction");
	wait(400);
	const auto updated = deck(source);
	check(updated == std::vector<std::string>{"new one", "new two"}, "reload reads changed file content");
	const auto buffers = nativeBuffers(source);
	const int current = page(source);
	check(write(path, QByteArray::fromHex("fffe6100")), "invalid UTF-16 fixture is written");
	contextAction(dock, "TextPPTReloadAction");
	wait(400);
	check(deck(source) == updated && page(source) == current && nativeBuffers(source) == buffers,
	      "invalid reload preserves the deck, position and renderers");
	check(error && !error->isHidden() && error->toolTip().contains(path.toHtmlEscaped()),
	      "file errors show a short row with the path in its tooltip");
	check(write(path, "fixed"), "corrected file fixture is written");
	contextAction(dock, "TextPPTReloadAction");
	wait(400);
	check(deck(source) == std::vector<std::string>{"fixed"} && error && error->isHidden(),
	      "successful reload clears the error row");
	check(QFile::remove(path), "missing-file fixture is prepared");
	contextAction(dock, "TextPPTReloadAction");
	wait(400);
	check(deck(source) == std::vector<std::string>{"fixed"} && error && !error->isHidden(),
	      "a missing file retains its last valid content");
	check(write(path, "first\n\nbody"), "multifile first page is written");
	const QString second = folder.path() + "/second.txt";
	check(write(second, "second"), "multifile second page is written");
	settings = obs_data_create();
	auto *files = obs_data_array_create();
	for (const auto &name : {path, second}) {
		auto *entry = obs_data_create();
		obs_data_set_string(entry, "value", name.toUtf8().constData());
		obs_data_array_push_back(files, entry);
		obs_data_release(entry);
	}
	obs_data_set_string(settings, "input_mode", "files");
	obs_data_set_array(settings, "files", files);
	obs_data_array_release(files);
	obs_source_update(source, settings);
	obs_data_release(settings);
	wait(600);
	const auto paths = deck(source);
	const auto content = bufferText(source);
	check(paths.size() == 2 && std::find(content.begin(), content.end(), "first\n\nbody") != content.end(),
	      "each validated file is one page, including its blank lines");
	check(write(path, "replacement") && write(second, QByteArray::fromHex("c0af")),
	      "partial reload failure fixture is written");
	contextAction(dock, "TextPPTReloadAction");
	wait(400);
	check(deck(source) == paths && bufferText(source) == content && error && !error->isHidden(),
	      "one invalid file prevents a partial deck replacement");
	check(write(path, QByteArray(3 * 1024 * 1024, 'a')) && write(second, QByteArray(2 * 1024 * 1024, 'b')),
	      "combined file limit fixture is written");
	contextAction(dock, "TextPPTReloadAction");
	wait(400);
	check(deck(source) == paths && bufferText(source) == content && error && !error->isHidden(),
	      "multifile content is bounded to 4 MiB per deck");
	check(write(path, "replacement") && write(second, "second"), "valid multifile reload fixture is restored");
	contextAction(dock, "TextPPTReloadAction");
	wait(400);
	const auto replacement = bufferText(source);
	check(std::find(replacement.begin(), replacement.end(), "replacement") != replacement.end() && error && error->isHidden(),
	      "valid multifile reload replaces content and clears the error");
}
static void editorScroll(QPlainTextEdit *edit)
{
	const auto content = edit->toPlainText();
	const int position = edit->textCursor().position(), anchor = edit->textCursor().anchor();
	auto *slider = edit->window()->findChild<QSlider *>("TextPPTZoomSlider");
	if (!check(slider && edit->lineWrapMode() == QPlainTextEdit::WidgetWidth,
	           "the independent editor scrolls with its default wrapping")) return;
	const auto size = edit->window()->size();
	check(size.width() <= 1280 && size.height() <= 720 && qAbs(size.width() * 9 - size.height() * 16) <= 16,
	      "the editor opens at 16:9 within its default size and screen bounds");
	edit->setPlainText(QString::fromUtf8(u8"标题\n第一点\t\t第二点\n第三点\t第四点\n\n").repeated(5));
	slider->setValue(72);
	wait(80);
	auto *bar = edit->verticalScrollBar();
	const auto state = [&] {
		QTextStream(&report) << "Editor scroll: visible=" << edit->isVisible() << " font=" << edit->font().pointSizeF()
			<< " viewport=" << edit->viewport()->width() << 'x' << edit->viewport()->height()
			<< " blocks=" << edit->document()->blockCount()
			<< " bar=" << bar->isVisible() << ',' << bar->width() << ',' << bar->minimum() << ',' << bar->maximum()
			<< " parent=" << (edit->window()->parent() ? edit->window()->parent()->metaObject()->className() : "none") << '\n';
	};
	state();
	check(bar->isVisible() && bar->width() >= 14 && bar->maximum() > bar->minimum(),
	      "wrapped Chinese text has a visible mouse-sized vertical scrollbar at maximum zoom");
	bar->setValue(bar->minimum());
	QStyleOptionSlider option;
	option.initFrom(bar);
	option.orientation = Qt::Vertical;
	option.minimum = bar->minimum(); option.maximum = bar->maximum();
	option.sliderPosition = bar->value(); option.sliderValue = bar->value();
	option.pageStep = bar->pageStep(); option.singleStep = bar->singleStep();
	const auto handle = bar->style()->subControlRect(QStyle::CC_ScrollBar, &option, QStyle::SC_ScrollBarSlider, bar);
	const auto image = bar->grab().toImage();
	const auto pixel = [&](QPoint point) { return image.pixelColor(point * bar->devicePixelRatioF()); };
	QScrollBar reference(Qt::Vertical, edit->window());
	reference.setStyleSheet("QScrollBar:vertical { width: 16px; }");
	reference.setRange(bar->minimum(), bar->maximum());
	reference.setPageStep(bar->pageStep());
	reference.resize(bar->size());
	reference.setAttribute(Qt::WA_UnderMouse, bar->underMouse());
	const auto native = reference.grab().toImage();
	check(pixel(handle.center()) == native.pixelColor(handle.center() * reference.devicePixelRatioF()),
	      "the editor scroll handle retains the native OBS theme color");
	const auto sendMouse = [bar](QEvent::Type type, QPoint position, Qt::MouseButton button, Qt::MouseButtons buttons) {
		QMouseEvent event(type, QPointF(position), QPointF(bar->mapToGlobal(position)), button, buttons, Qt::NoModifier);
		QApplication::sendEvent(bar, &event);
	};
	const QPoint track(bar->width() / 2, bar->height() - 8);
	sendMouse(QEvent::MouseButtonPress, track, Qt::LeftButton, Qt::LeftButton);
	sendMouse(QEvent::MouseButtonRelease, track, Qt::LeftButton, Qt::NoButton);
	check(bar->value() > bar->minimum(), "clicking the native scroll track moves the editor");
	bar->setValue(bar->minimum());
	sendMouse(QEvent::MouseButtonPress, handle.center(), Qt::LeftButton, Qt::LeftButton);
	sendMouse(QEvent::MouseMove, track, Qt::NoButton, Qt::LeftButton);
	sendMouse(QEvent::MouseButtonRelease, track, Qt::LeftButton, Qt::NoButton);
	check(bar->value() > bar->minimum(), "dragging the native scroll handle moves the editor");
	bar->setValue(bar->minimum());
	QWheelEvent wheel(QPointF(20, 20), QPointF(edit->mapToGlobal(QPoint(20, 20))), {}, QPoint(0, -120),
	                  Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
	QApplication::sendEvent(edit->viewport(), &wheel);
	check(bar->value() > bar->minimum(), "the mouse wheel scrolls wrapped editor text");
	bar->setValue(bar->maximum()); wait(50);
	QTextCursor last(edit->document());
	last.movePosition(QTextCursor::End);
	const auto lastY = edit->cursorRect(last).center().y();
	check(lastY >= 0 && lastY < edit->viewport()->height(), "mouse-scrollable editor content includes the final Chinese line");
	static int snapshot = 0;
	edit->window()->grab().save(report.fileName() + QStringLiteral(".editor-mouse-scroll-%1.png").arg(++snapshot));
	slider->setValue(6);
	wait(50);
	check(bar->maximum() == bar->minimum(), "shrinking the reading font removes an obsolete scroll range");
	edit->setPlainText(content);
	auto *reset = edit->findChild<QShortcut *>("TextPPTZoomReset");
	QShortcutEvent resetEvent(reset->key(), reset);
	QApplication::sendEvent(reset, &resetEvent);
	QTextCursor restored(edit->document());
	restored.setPosition(anchor);
	restored.setPosition(position, QTextCursor::KeepAnchor);
	edit->setTextCursor(restored);
	wait(350);
}

static void nativeEditorTabs(QPlainTextEdit *input)
{
#ifdef _WIN32
	const char *kind = "text_gdiplus";
#else
	const char *kind = "text_ft2_source";
#endif
	auto *source = obs_source_create(obs_get_latest_input_type_id(kind), "TextPPT native input comparison", nullptr, nullptr);
	if (!check(source != nullptr, "native text input reference is created")) return;
	obs_frontend_open_source_properties(source);
	wait(350);
	const auto title = QString::fromUtf8(obs_frontend_get_locale_string("Basic.PropertiesWindow"))
		.arg(QString::fromUtf8(obs_source_get_name(source)));
	QDialog *dialog = nullptr;
	for (auto *window : QApplication::topLevelWidgets())
		if (window->inherits("OBSBasicProperties") && window->windowTitle() == title)
			dialog = qobject_cast<QDialog *>(window);
	auto *reference = dialog ? dialog->findChild<QPlainTextEdit *>() : nullptr;
	if (check(input && reference, "native and TextPPT properties expose the OBS text widget")) {
		check(input->font() == reference->font() && input->tabStopDistance() == reference->tabStopDistance() &&
		      input->lineWrapMode() == reference->lineWrapMode() && input->wordWrapMode() == reference->wordWrapMode(),
		      "inline text inherits native OBS font, TAB spacing and wrapping");
		const auto content = input->toPlainText();
		const auto wrap = input->lineWrapMode();
		const QSignalBlocker blocker(input), referenceBlocker(reference);
		const auto text = QString::fromUtf8(u8"甲\t乙\n甲\t\t丙");
		input->setPlainText(text); reference->setPlainText(text);
		input->setLineWrapMode(QPlainTextEdit::NoWrap); reference->setLineWrapMode(QPlainTextEdit::NoWrap);
		bool equal = true;
		for (int position = 0; position <= text.size(); ++position) {
			QTextCursor actual(input->document()), expected(reference->document());
			actual.setPosition(position); expected.setPosition(position);
			equal &= input->cursorRect(actual).x() == reference->cursorRect(expected).x();
		}
		check(equal, "Chinese text with single and consecutive TABs matches native OBS input positions");
		input->setPlainText(content); input->setLineWrapMode(wrap);
	}
	if (dialog) dialog->reject();
	wait(100);
	obs_source_remove(source);
	obs_source_release(source);
}

static void editorZoom(QPlainTextEdit *edit, obs_source_t *source)
{
	editorScroll(edit);
	const auto initial = edit->font();
	const auto content = edit->toPlainText();
	const auto cursor = edit->textCursor();
	auto *settings = obs_source_get_settings(source);
	auto *font = obs_data_get_obj(settings, "font");
	const std::string sourceFont = obs_data_get_json(font);
	obs_data_release(font); obs_data_release(settings);
	auto *larger = edit->findChild<QShortcut *>("TextPPTZoomIn");
	auto *smaller = edit->findChild<QShortcut *>("TextPPTZoomOut");
	auto *reset = edit->findChild<QShortcut *>("TextPPTZoomReset");
	if (!check(larger && smaller && reset, "editor exposes scoped native zoom shortcuts")) return;
	check(larger->keys() == QKeySequence::keyBindings(QKeySequence::ZoomIn) &&
	      smaller->keys() == QKeySequence::keyBindings(QKeySequence::ZoomOut) &&
	      reset->key() == QKeySequence(Qt::CTRL | Qt::Key_0) &&
	      larger->context() == Qt::WindowShortcut,
	      "zoom uses platform keys within the editor");
	const auto press = [](QShortcut *shortcut) {
		QShortcutEvent event(shortcut->key(), shortcut);
		QApplication::sendEvent(shortcut, &event);
	};
	const auto wheel = [edit](int delta, Qt::KeyboardModifiers modifiers) {
		QWheelEvent event(QPointF(20, 20), QPointF(edit->mapToGlobal(QPoint(20, 20))), {}, QPoint(0, delta),
		                  Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
		QApplication::sendEvent(edit->viewport(), &event);
	};
	auto *slider = edit->window()->findChild<QSlider *>("TextPPTZoomSlider");
	auto *sizeLabel = edit->window()->findChild<QLabel *>("TextPPTZoomSize");
	if (!check(slider && sizeLabel && slider->minimum() == 6 && slider->maximum() == 72,
	           "editor exposes a native reading-size slider with bounded values")) return;
	check(slider->y() > edit->geometry().bottom() && !slider->accessibleName().isEmpty(),
	      "the accessible reading-size slider is in the editor footer");
	slider->setValue(24);
	check(edit->font().pointSizeF() == 24 && sizeLabel->text().startsWith("24"), "the slider changes the reading font and size label");
	press(larger);
	check(slider->value() == 25 && edit->font().pointSizeF() == 25, "keyboard zoom synchronizes the size slider");
	wheel(-120, Qt::ControlModifier);
	check(slider->value() == 24 && edit->font().pointSizeF() == 24, "wheel zoom synchronizes the size slider");
	slider->setValue(-100); check(edit->font().pointSizeF() == 6, "the slider clamps its lower reading-size bound");
	slider->setValue(1000); check(edit->font().pointSizeF() == 72, "the slider clamps its upper reading-size bound");
	press(reset);
	check(edit->font() == initial && slider->value() == qRound(initial.pointSizeF()), "reset synchronizes the themed font and slider");
	wheel(120, Qt::ControlModifier);
	check(edit->font().pointSizeF() > initial.pointSizeF() &&
	      qAbs(edit->tabStopDistance() - QFontMetricsF(edit->font(), edit).horizontalAdvance(QLatin1Char(' ')) * 4) < 0.01,
	      "control wheel enlarges text and scales tab spacing");
	press(smaller);
	check(edit->font() == initial, "zoom out reverses a wheel step");
	for (int i = 0; i < 4; ++i) wheel(30, Qt::ControlModifier);
	check(edit->font().pointSizeF() > initial.pointSizeF(), "smooth wheel deltas accumulate into one zoom step");
	press(reset);
	wheel(120, Qt::NoModifier);
	check(edit->font() == initial, "ordinary scrolling keeps the editor font");
	for (int i = 0; i < 200; ++i) press(larger);
	check(edit->font().pointSizeF() <= 72, "repeated zoom in stays within a safe reading size");
	for (int i = 0; i < 400; ++i) press(smaller);
	check(edit->font().pointSizeF() >= 6, "repeated zoom out keeps text readable");
	press(reset);
	QTextStream(&report) << "Editor font: " << initial.toString() << " -> " << edit->font().toString() << '\n';
	check(edit->font() == initial, "reset restores the themed editor font");
	check(edit->toPlainText() == content &&
	      edit->textCursor().position() == cursor.position() && edit->textCursor().anchor() == cursor.anchor(),
	      "zoom preserves text and selection");
	settings = obs_source_get_settings(source); font = obs_data_get_obj(settings, "font");
	check(sourceFont == obs_data_get_json(font), "editor zoom leaves the rendered source font unchanged");
	obs_data_release(font); obs_data_release(settings);
	{
		const int savedPosition = edit->textCursor().position(), savedAnchor = edit->textCursor().anchor();
		const auto wrap = edit->lineWrapMode();
		const auto fixture = QString::fromUtf8(u8"一、认识人生的有限\n1. 认识上帝永恒不变\t2. 认识人生短暂有限\n3. 认识生命脆弱无常\t4. 认识自己需要智慧\n\n四、学习数算的功课\n1. 学习省察自己的生命\t2. 学习珍惜宝贵的光阴\n3. 学习遵行上帝的旨意\t4. 学习忠心完成主托付");
		{
			const QSignalBlocker blocker(edit);
			edit->setPlainText(fixture);
			edit->setLineWrapMode(QPlainTextEdit::NoWrap);
			edit->document()->setModified(false);
		}
		press(reset);
		edit->window()->grab().save(report.fileName() + ".editor-tabs-small.png");
		const auto columnsAligned = [&] {
			int column = -1;
			const qreal gap = QFontMetricsF(edit->font(), edit).horizontalAdvance(' ') * 4;
			const auto text = edit->toPlainText();
			for (qsizetype tab = text.indexOf('\t'); tab >= 0; tab = text.indexOf('\t', tab + 1)) {
				QTextCursor position(edit->document());
				position.setPosition(static_cast<int>(tab));
				const int before = edit->cursorRect(position).left();
				position.setPosition(static_cast<int>(tab + 1));
				const int after = edit->cursorRect(position).left();
				if ((column >= 0 && qAbs(column - after) > 1) || after - before < gap - 1) return false;
				column = after;
			}
			return column >= 0;
		};
		bool aligned = columnsAligned();
		for (int step = 0; step < 200; ++step) press(smaller);
		for (int step = 0; step <= 66; ++step) {
			aligned &= columnsAligned();
			press(larger);
		}
		wait(80);
		auto *scroll = edit->verticalScrollBar();
		check(scroll->isVisible() && scroll->width() >= 8 && scroll->maximum() > scroll->minimum(),
		      "zoomed Chinese text keeps a visible vertical scrollbar with a usable range");
		scroll->setValue(scroll->maximum()); wait(50);
		QTextCursor last(edit->document());
		last.movePosition(QTextCursor::End);
		const int lastY = edit->cursorRect(last).center().y();
		check(lastY >= 0 && lastY < edit->viewport()->height(),
		      "scrolling to the bottom reaches the final Chinese line at maximum zoom");
		edit->window()->grab().save(report.fileName() + ".editor-scrollbar.png");
		scroll->setValue(scroll->minimum());
		wheel(-120, Qt::NoModifier); wait(50);
		check(scroll->value() > scroll->minimum(), "ordinary wheel scrolling still reaches overflowing editor content");
		for (int step = 0; step < 66; ++step) {
			press(smaller);
			aligned &= columnsAligned();
		}
		check(aligned, "real Chinese TAB columns keep a full gap without jumping stops at 6-72 points");
		check(!edit->document()->isModified() && !edit->document()->isUndoAvailable() && edit->toPlainText() == fixture,
		      "TAB layout and zoom preserve text, modified state and undo history");
		press(reset);
		for (int step = 0; step < 4; ++step) press(larger);
		edit->window()->grab().save(report.fileName() + ".editor-tabs-large.png");
		const auto beforeEdit = edit->tabStopDistance();
		edit->moveCursor(QTextCursor::Start);
		edit->moveCursor(QTextCursor::Down);
		edit->insertPlainText(QString::fromUtf8(u8"新增内容使这一列更长"));
		wait(450);
		check(edit->tabStopDistance() > beforeEdit && columnsAligned() && edit->document()->isUndoAvailable(),
		      "typing updates TAB columns after the existing debounce without consuming Undo");
		edit->undo(); wait(450);
		check(edit->toPlainText() == fixture && columnsAligned(), "Undo restores Chinese text and its TAB columns");
		{
			edit->setPlainText(content);
			edit->setLineWrapMode(wrap);
			QTextCursor restored(edit->document());
			restored.setPosition(savedAnchor);
			restored.setPosition(savedPosition, QTextCursor::KeepAnchor);
			edit->setTextCursor(restored);
		}
		press(reset);
		wait(350);
	}
	auto *hint = edit->window()->findChild<QLabel *>("TextPPTEditorHint");
	check(hint && hint->geometry().bottom() < edit->y() && !hint->text().contains('\n') && !hint->text().contains("Ctrl"),
	      "the editor keeps only its short pagination hint above the text");
}

static void editor(obs_source_t *source, bool customDelimiter)
{
	whenEditor([source, customDelimiter](QDialog *dialog) {
		auto *edit = dialog ? dialog->findChild<QPlainTextEdit *>("TextPPTEditor") : nullptr;
		auto *count = dialog ? dialog->findChild<QLabel *>("TextPPTPageCount") : nullptr;
		if (check(edit && count, "independent editor exposes the page counter")) {
			editorZoom(edit, source);
			textMenu(edit);
			check(QRegularExpression("^\\D*3\\D*$").match(count->text()).hasMatch(), "initial page count is three");
			if (customDelimiter) {
				edit->setPlainText("first\n\nbody\n---\nsecond");
				wait(400);
				check(QRegularExpression("^\\D*2\\D*$").match(count->text()).hasMatch(), "custom separators preserve internal blank lines");
			} else {
				edit->setPlainText("one\n\ntwo\n\nthree\n\nfour");
				wait(400);
				check(QRegularExpression("^\\D*4\\D*$").match(count->text()).hasMatch(), "page count updates after editing");
				edit->clear();
				wait(400);
				check(QRegularExpression("^\\D*0\\D*$").match(count->text()).hasMatch(), "empty text has zero pages");
			}
		}
		if (dialog) dialog->reject();
	});
	auto *properties = obs_source_properties(source);
	obs_property_button_clicked(obs_properties_get(properties, "edit_text"), nullptr);
	obs_properties_destroy(properties);
}
class ImageVisibilityProbe : public QObject {
public:
	int changes = 0;
protected:
	bool eventFilter(QObject *, QEvent *event) override
	{
		if (event->type() == QEvent::Show || event->type() == QEvent::Hide)
			++changes;
		return false;
	}
};

struct ImageSlideEvents {
	std::atomic<int> count{0};
	std::atomic<int64_t> last{-1};
	static void changed(void *data, calldata_t *event)
	{
		auto *events = static_cast<ImageSlideEvents *>(data);
		events->last = calldata_int(event, "index");
		++events->count;
	}
};

static void imageCollections(QWidget *dock, obs_data_t *slides)
{
	char *name = obs_frontend_get_current_scene_collection();
	const std::string original = name ? name : "";
	bfree(name);
	const std::string alternate = (QStringLiteral("TextPPT Images ") + QUuid::createUuid().toString(QUuid::Id128)).toStdString();
	if (!check(!original.empty() && obs_frontend_add_scene_collection(alternate.c_str()), "image collection fixture is created"))
		return;
	auto *controls = dock->findChild<QWidget *>("TextPPTImageControls");
	bool reset = true;
	for (int cycle = 0; cycle < 6; ++cycle) {
		auto *scene = obs_frontend_get_current_scene();
		auto *group = obs_scene_add_group(obs_scene_from_source(scene), "Collection image group");
		auto *slide = obs_source_create(obs_get_latest_input_type_id("slideshow"), "Collection image slide", slides, nullptr);
		obs_scene_add(obs_sceneitem_group_get_scene(group), slide);
		auto *groupSource = obs_sceneitem_get_source(group);
		const char *uuid = obs_source_get_uuid(groupSource);
		auto *settings = obs_data_create();
		auto *groups = obs_data_array_create();
		auto *entry = obs_data_create();
		auto *ids = obs_data_create();
		obs_data_set_string(entry, "value", obs_source_get_name(groupSource));
		obs_data_set_string(entry, "uuid", uuid);
		obs_data_set_string(ids, uuid, uuid);
		obs_data_array_push_back(groups, entry);
		obs_data_set_array(settings, "groups", groups);
		obs_data_set_obj(settings, "group_ids", ids);
		obs_data_set_string(settings, "input_mode", "groups");
		obs_data_set_string(settings, "transition", "cut");
#ifdef _WIN32
		const char *kind = "obs-textppt-gdiplus";
#else
		const char *kind = "obs-textppt-ft2";
#endif
		auto *source = obs_source_create(kind, "Collection image controller", settings, nullptr);
		obs_scene_add(obs_scene_from_source(scene), source);
		obs_data_release(settings); obs_data_release(ids);
		obs_data_release(entry); obs_data_array_release(groups);
		wait(500);
		auto *selector = dock->findChild<QComboBox *>("TextPPTSourceList");
		const QString controllerId = QString::fromUtf8(obs_source_get_uuid(source));
		selector->setCurrentIndex(selector->findData(controllerId));
		reset &= !controls->isHidden();
		// Release test references before OBS clears the collection.
		obs_source_release(source); obs_source_release(slide); obs_source_release(scene);
		obs_frontend_set_current_scene_collection(cycle % 2 ? alternate.c_str() : original.c_str());
		wait(350);
		reset &= selector->currentData().toString() != controllerId;
	}
	obs_frontend_set_current_scene_collection(original.c_str());
	wait(350);
	check(reset, "six collection switches release active image watchers and controls");
}

static void imageControls(QWidget *dock)
{
	auto *controls = dock->findChild<QWidget *>("TextPPTImageControls");
	auto *previous = dock->findChild<QToolButton *>("TextPPTPreviousImage");
	auto *next = dock->findChild<QToolButton *>("TextPPTNextImage");
	if (!check(controls && previous && next, "image controls are registered"))
		return;
	QTemporaryDir folder;
	auto *settings = obs_data_create();
	auto *files = obs_data_array_create();
	for (int i = 0; i < 3; ++i) {
		QImage image(64, 32, QImage::Format_RGB32);
		image.fill(QColor::fromHsv(i * 100, 200, 200));
		const auto path = folder.path() + QString("/%1.png").arg(i);
		check(image.save(path), "image slide fixture is written");
		auto *entry = obs_data_create();
		obs_data_set_string(entry, "value", path.toUtf8().constData());
		obs_data_array_push_back(files, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(settings, "files", files);
	obs_data_array_release(files);
	obs_data_set_string(settings, "slide_mode", "mode_manual");
	obs_data_set_string(settings, "transition", "cut");
	auto *first = obs_source_create(obs_get_latest_input_type_id("slideshow"), "Image probe A", settings, nullptr);
	auto *second = obs_source_create(obs_get_latest_input_type_id("slideshow"), "Image probe B", settings, nullptr);
	obs_data_release(settings);
	auto *host = obs_scene_create("TextPPT image controls probe");
	auto *empty = obs_scene_add_group(host, "No image slides");
	auto *group = obs_scene_add_group(host, "Image slides");
	auto *nested = obs_scene_add_group(obs_sceneitem_group_get_scene(group), "Nested image slides");
	auto *firstItem = obs_scene_add(obs_sceneitem_group_get_scene(nested), first);
	const auto slideIndex = [](obs_source_t *source) {
		calldata_t data = {};
		proc_handler_call(obs_source_get_proc_handler(source), "current_index", &data);
		const int index = static_cast<int>(calldata_int(&data, "current_index"));
		calldata_free(&data);
		return index;
	};
	settings = obs_data_create();
	obs_data_set_string(settings, "input_mode", "groups");
	obs_data_set_string(settings, "slide_mode", "mode_manual");
	obs_data_set_string(settings, "transition", "cut");
	auto *groups = obs_data_array_create();
	auto *ids = obs_data_create();
	for (auto *item : {empty, group}) {
		auto *source = obs_sceneitem_get_source(item);
		const char *uuid = obs_source_get_uuid(source);
		auto *entry = obs_data_create();
		obs_data_set_string(entry, "value", obs_source_get_name(source));
		obs_data_set_string(entry, "uuid", uuid);
		obs_data_array_push_back(groups, entry);
		obs_data_set_string(ids, uuid, uuid);
		obs_data_release(entry);
		obs_sceneitem_set_visible(item, false);
	}
	obs_data_set_array(settings, "groups", groups);
	obs_data_set_obj(settings, "group_ids", ids);
	obs_data_array_release(groups);
	obs_data_release(ids);
#ifdef _WIN32
	const char *kind = "obs-textppt-gdiplus";
#else
	const char *kind = "obs-textppt-ft2";
#endif
	auto *controller = obs_source_create(kind, "TextPPT image controller", settings, nullptr);
	obs_data_release(settings);
	obs_scene_add(host, controller);
	obs_frontend_set_current_scene(obs_scene_get_source(host));
	wait(1000);
	auto *sources = dock->findChild<QComboBox *>("TextPPTSourceList");
	sources->setCurrentIndex(sources->findData(QString::fromUtf8(obs_source_get_uuid(controller))));
	const auto jump = [&](int index) {
		calldata_t data = {};
		calldata_set_int(&data, "index", index);
		proc_handler_call(obs_source_get_proc_handler(controller), "dock_transition", &data);
		calldata_free(&data);
		wait(650);
	};
	check(controls->isHidden(), "groups without image slides hide both buttons");
	jump(1);
	check(!controls->isHidden() && !previous->icon().isNull() && !next->icon().isNull(),
	      "nested image slides show distinct icon controls");
	next->click(); wait(150);
	check(slideIndex(first) == 1 && page(controller) == 1, "next image advances the native slideshow without changing groups");
	previous->click(); wait(150);
	check(slideIndex(first) == 0, "previous image uses the native slideshow control");
	obs_sceneitem_set_visible(nested, false); wait(400);
	check(controls->isHidden(), "hidden nested groups are excluded");
	obs_sceneitem_set_visible(nested, true); wait(400);
	check(!controls->isHidden(), "showing a nested group restores image controls");
	// Program and preview may select different group pages.
	const bool wasStudio = obs_frontend_preview_program_mode_active();
	if (!wasStudio) obs_frontend_set_preview_program_mode(true);
	auto *oldOutput = obs_get_output_source(0);
	auto *program = obs_scene_create_private("Image controls program");
	auto *copy = obs_source_duplicate(controller, "Image controls copy", true);
	obs_scene_add(program, copy);
	auto *transition = obs_source_create_private("cut_transition", "Image controls transition", nullptr);
	obs_transition_set(transition, obs_scene_get_source(program));
	auto *lock = dock->findChild<QToolButton *>("TextPPTSourceLock");
	const bool wasLocked = lock->isChecked();
	lock->setChecked(true);
	obs_set_output_source(0, transition);
	wait(700);
	jump(0);
	check(page(copy) == 1 && page(controller) == 0 && controls->isHidden(),
	      "an empty preview group hides image controls independently of program");
	jump(1);
	obs_source_media_previous(copy); wait(700);
	check(page(copy) == 0 && page(controller) == 1 && !controls->isHidden(),
	      "Studio image controls follow preview even when program has no images");
	next->click(); wait(200);
	check(slideIndex(first) == 1 && page(controller) == 1 && page(copy) == 0,
	      "Studio Next image controls preview without changing the program page");
	previous->click(); wait(200);
	check(slideIndex(first) == 0, "Studio Previous image reverses the preview slideshow");
	obs_set_output_source(0, oldOutput);
	obs_source_release(oldOutput);
	obs_source_release(transition);
	obs_scene_release(program);
	obs_source_release(copy);
	if (!wasStudio) obs_frontend_set_preview_program_mode(false);
	lock->setChecked(wasLocked);
	wait(500);
	check(!controls->isHidden(), "leaving Studio Mode restores the original group's image controls");
	auto *restart = dock->findChild<QToolButton *>("TextPPTRestart");
	auto *previousPage = dock->findChild<QToolButton *>("TextPPTPreviousPage");
	auto *nextPage = dock->findChild<QToolButton *>("TextPPTNextPage");
	if (check(restart && previousPage && nextPage, "page control groups are registered")) {
		const auto right = [dock](QWidget *widget) { return widget->mapTo(dock, QPoint(widget->width(), 0)).x(); };
		const int gapA = previousPage->mapTo(dock, QPoint()).x() - right(restart);
		const int gapB = previous->mapTo(dock, QPoint()).x() - right(nextPage);
		QTextStream(&report) << "Dock control gaps: " << gapA << ',' << gapB << "; width: " << dock->width()
		                    << "; minimum: " << dock->layout()->minimumSize().width() << '\n';
		check(dock->width() >= dock->layout()->minimumSize().width(), "Dock keeps enough width for every visible control");
		check(gapA == restart->width() && gapB == restart->width(), "both control-group gaps equal one button width");
	}
	if (!qEnvironmentVariableIsEmpty("TEXTPPT_IMAGE_STRESS")) {
		ImageVisibilityProbe visibility;
		controls->installEventFilter(&visibility);
		const uint64_t baseline = textppt_test_memory();
		uint64_t peak = baseline;
		ImageSlideEvents events;
		signal_handler_connect(obs_source_get_signal_handler(first), "slide_changed", ImageSlideEvents::changed, &events);
		QElapsedTimer clock;
		clock.start();
		for (int i = 0; i < 10000; ++i) {
			(i % 2 ? previous : next)->click();
			if (i % 100 == 99) { wait(5); peak = std::max(peak, textppt_test_memory()); }
		}
		signal_handler_disconnect(obs_source_get_signal_handler(first), "slide_changed", ImageSlideEvents::changed, &events);
		const int finalImage = slideIndex(first);
		check(events.count > 0 && finalImage >= 0 && finalImage < 3 && finalImage == events.last && page(controller) == 1,
		      "10000 image clicks match native slide events without leaving the group");
		check(visibility.changes == 0, "image clicks do not hide or rebuild visible controls");
		QTextStream(&report) << "10000 image clicks ms: " << clock.elapsed() << "; native changes: " << events.count.load() << '\n';
		obs_source_media_restart(first); wait(150);
		bool paced = true;
		for (int i = 0; i < 12; ++i) { next->click(); wait(100); paced &= slideIndex(first) == (i + 1) % 3; }
		for (int i = 0; i < 12; ++i) { previous->click(); wait(100); paced &= slideIndex(first) == (2 - i % 3); }
		check(paced && slideIndex(first) == 0, "24 paced image clicks advance and reverse every requested slide");
		for (int i = 0; i < 500; ++i) {
			calldata_t data = {};
			calldata_set_int(&data, "index", i % 2);
			proc_handler_call(obs_source_get_proc_handler(controller), "dock_transition", &data);
			calldata_free(&data);
			wait(10);
		}
		wait(500);
		check(page(controller) == 1 && !controls->isHidden(), "500 rapid group changes retain the final image controls");
		for (int i = 0; i < 200; ++i) { obs_sceneitem_set_visible(nested, i % 2 != 0); wait(5); }
		wait(400);
		check(!controls->isHidden(), "200 nested visibility changes retain the final visible slideshow");
		auto *slideSettings = obs_source_get_settings(first);
		for (int i = 0; i < 100; ++i) {
			auto *temporary = obs_source_create(obs_get_latest_input_type_id("slideshow"), "Temporary image slide", slideSettings, nullptr);
			auto *item = obs_scene_add(obs_sceneitem_group_get_scene(group), temporary);
			wait(5);
			obs_sceneitem_remove(item); obs_source_remove(temporary); obs_source_release(temporary);
			if (i % 10 == 9) { wait(50); peak = std::max(peak, textppt_test_memory()); }
		}
		obs_data_release(slideSettings);
		wait(500);
		check(!controls->isHidden() && slideIndex(first) == 0, "100 slideshow create/delete cycles preserve the surviving target");
		for (int i = 0; i < 20; ++i) { dock->hide(); wait(10); dock->show(); wait(10); }
		wait(400);
		check(!controls->isHidden(), "20 Dock reopenings restore image controls");
		peak = std::max(peak, textppt_test_memory());
		check(baseline != 0 && peak <= baseline + 128 * 1024 * 1024ull, "image stress memory growth remains below 128 MiB");
		QTextStream(&report) << "Image stress baseline MiB: " << double(baseline) / 1048576.0
		                    << "\nImage stress peak MiB: " << double(peak) / 1048576.0 << '\n';
	}
	auto *duplicate = obs_scene_add(obs_sceneitem_group_get_scene(group), first);
	wait(400);
	next->click(); wait(150);
	check(slideIndex(first) == 1, "a shared slideshow is controlled once without a selection menu");
	obs_sceneitem_remove(duplicate);
	auto *secondItem = obs_scene_add(obs_sceneitem_group_get_scene(group), second);
	wait(400);
	const QString secondId = QString::fromUtf8(obs_source_get_uuid(second));
	const auto choose = [secondId] {
		auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		if (!check(menu && menu->objectName() == "TextPPTImageMenu" && menu->actions().size() == 2,
		           "multiple image slides require an explicit target")) {
			if (menu) menu->close();
			return;
		}
		for (auto *action : menu->actions())
			if (action->data().toString() == secondId) {
				menu->setActiveAction(action);
				QKeyEvent event(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
				QApplication::sendEvent(menu, &event);
				return;
			}
		menu->close();
	};
	QTimer::singleShot(80, qApp, choose);
	next->click(); wait(150);
	check(slideIndex(first) == 1 && slideIndex(second) == 1, "only the chosen slideshow advances");
	if (!qEnvironmentVariableIsEmpty("TEXTPPT_IMAGE_STRESS")) {
		for (int i = 0; i < 50; ++i) {
			QTimer::singleShot(5, qApp, choose);
			(i % 2 ? previous : next)->click();
			wait(50);
		}
		check(slideIndex(first) == 1 && slideIndex(second) == 1, "50 target-menu actions affect only the selected slideshow");
	}
	QTimer::singleShot(80, qApp, [&] { jump(0); choose(); });
	next->click(); wait(150);
	check(slideIndex(second) == 1 && page(controller) == 0, "a page change during the menu cancels its pending control");
	jump(1);
	auto *collectionSlides = obs_source_get_settings(first);
	if (!qEnvironmentVariableIsEmpty("TEXTPPT_IMAGE_STRESS")) {
		QTimer::singleShot(20, qApp, [&] {
			obs_sceneitem_remove(secondItem);
			secondItem = nullptr;
			obs_source_remove(second);
			choose();
		});
		next->click();
		check(slideIndex(second) == 1, "removing a target during its menu cancels the image action");
	}
	if (secondItem) obs_sceneitem_remove(secondItem);
	obs_source_remove(second); obs_source_release(second);
	obs_sceneitem_remove(firstItem);
	obs_source_remove(first); obs_source_release(first);
	wait(500);
	check(controls->isHidden(), "removing image slides hides controls without stale references");
	obs_source_remove(controller); obs_source_release(controller);
	obs_source_remove(obs_scene_get_source(host)); obs_scene_release(host);
	wait(400);
	check(controls->isHidden(), "removing the controller clears image controls");
	if (!qEnvironmentVariableIsEmpty("TEXTPPT_IMAGE_STRESS"))
		imageCollections(dock, collectionSlides);
	obs_data_release(collectionSlides);
}

static void run()
{
	report.setFileName(qEnvironmentVariable("TEXTPPT_UI_REPORT"));
	if (!report.open(QIODevice::WriteOnly | QIODevice::Text)) return;
	QWidget *dock = nullptr;
	for (auto *widget : QApplication::allWidgets())
		if (widget->objectName() == "TextPPTDock") dock = widget;
	auto *sources = dock ? dock->findChild<QComboBox *>("TextPPTSourceList") : nullptr;
	auto *status = dock ? dock->findChild<QLabel *>("TextPPTPageStatus") : nullptr;
	auto *follow = dock ? dock->findChild<QToolButton *>("TextPPTFollowScene") : nullptr;
	auto *sourceLock = dock ? dock->findChild<QToolButton *>("TextPPTSourceLock") : nullptr;
	auto *input = dock ? dock->findChild<QLineEdit *>("TextPPTPageInput") : nullptr;
	if (!check(sources && status && follow && sourceLock && input, "TextPPT dock is registered")) {
		check(false, "UI TESTS COMPLETE"); report.close(); return;
	}
	auto *window = static_cast<QWidget *>(obs_frontend_get_main_window());
	window->setAttribute(Qt::WA_DontShowOnScreen);
	window->show();
	dock->parentWidget()->show();
	dock->show();
	if (!qEnvironmentVariableIsEmpty("TEXTPPT_IMAGE_TEST")) {
		imageControls(dock);
		check(passed, "IMAGE TESTS COMPLETE");
		report.close();
		return;
	}
	auto *settings = obs_data_create();
	obs_data_set_string(settings, "input_mode", "custom");
	const std::string content = "<b>one</b>" + std::string(300, 'x') + "\n\ntwo\n\nthree";
	obs_data_set_string(settings, "custom_text", content.c_str());
	obs_data_set_string(settings, "slide_mode", "mode_manual");
	obs_data_set_string(settings, "transition", "cut");
#ifdef _WIN32
	const char *kind = "obs-textppt-gdiplus";
#else
	const char *kind = "obs-textppt-ft2";
#endif
	if (!qEnvironmentVariableIsEmpty("TEXTPPT_NATIVE_STUDIO_ONLY")) {
		obs_data_release(settings);
		check(textppt_native_studio_probe(kind, report.fileName() + ".native-studio.txt", dock), "native Studio checks complete");
		check(passed, "UI TESTS COMPLETE"); report.close(); return;
	}
	if (!qEnvironmentVariableIsEmpty("TEXTPPT_STUDIO_ONLY")) {
		obs_data_release(settings);
		check(textppt_studio_probe(kind, report.fileName() + ".studio.txt", dock), "Studio Mode copy checks complete");
		check(passed, "UI TESTS COMPLETE"); report.close(); return;
	}
	auto *source = obs_source_create(kind, "TextPPT UI Probe", settings, nullptr);
	obs_data_release(settings);
	auto *scene = obs_frontend_get_current_scene();
	auto *item = source && scene ? obs_scene_add(obs_scene_from_source(scene), source) : nullptr;
	if (!check(item != nullptr, "UI test source is created")) {
		obs_source_release(source); obs_source_release(scene);
		check(false, "UI TESTS COMPLETE"); report.close(); return;
	}
	wait(700);
	if (!qEnvironmentVariableIsEmpty("TEXTPPT_HEADER_ONLY")) {
		check(textppt_header_probe(dock, report.fileName() + ".header.txt"), "Dock header layout checks complete");
		obs_sceneitem_remove(item); obs_source_remove(source);
		obs_source_release(source); obs_source_release(scene);
		check(passed, "UI TESTS COMPLETE"); report.close(); return;
	}
	obs_source_enum_full_tree(source, [](obs_source_t *, obs_source_t *child, void *) {
		const char *id = obs_source_get_unversioned_id(child);
		if (strcmp(id, "text_gdiplus") == 0 || strcmp(id, "text_ft2_source") == 0)
			check(strcmp(obs_source_get_id(child), obs_get_latest_input_type_id(id)) == 0,
			      "text buffers use the current native renderer");
	}, nullptr);
	auto *properties = obs_source_properties(source);
	auto *sourceSettings = obs_source_get_settings(source);
	obs_properties_apply_settings(properties, sourceSettings);
	check(obs_property_visible(obs_properties_get(properties, "advanced_settings")), "advanced settings stay expanded");
	check(!obs_properties_get(properties, "advanced_button") && !obs_properties_get(properties, "style_more_button"), "collapse buttons are removed");
	check(!obs_properties_get(properties, "group_hint"), "group help text is removed");
	const auto info = QString::fromUtf8(obs_property_description(obs_properties_get(properties, "plugin_info")));
	check(info.contains(PLUGIN_VERSION) && info.contains("href=\"https://haliluya.net\""), "properties show the version and linked author");
	auto *font = obs_data_get_obj(sourceSettings, "font");
#ifdef _WIN32
	const char *native = "text_gdiplus";
#else
	const char *native = "text_ft2_source";
#endif
	auto *nativeDefaults = obs_get_source_defaults(obs_get_latest_input_type_id(native));
	auto *nativeFont = obs_data_get_obj(nativeDefaults, "font");
	check(font && nativeFont && QJsonDocument::fromJson(obs_data_get_json_with_defaults(font)) ==
	      QJsonDocument::fromJson(obs_data_get_json_with_defaults(nativeFont)),
	      "font defaults match the platform's native text source");
	obs_data_release(nativeFont);
	obs_data_release(nativeDefaults);
	obs_data_release(font);
	check(obs_data_get_json_with_defaults(sourceSettings) != nullptr, "native style defaults remain valid for serialization");
	auto *style = obs_properties_get(properties, "text_style");
	check(style && obs_property_group_type(style) == OBS_GROUP_NORMAL, "native text styles stay expanded");
	auto *styleFields = obs_property_group_content(style);
	check(obs_properties_get(styleFields, "font") && obs_properties_get(styleFields, "antialiasing") &&
	      obs_properties_get(styleFields, "outline"), "native style controls are reused");
	bool ownContent = true;
	for (const char *key : {"text", "file", "text_file", "read_from_file", "from_file"})
		ownContent &= obs_properties_get(styleFields, key) == nullptr;
	check(ownContent, "native content fields do not duplicate TextPPT input");
#ifdef _WIN32
	auto *callbackSettings = obs_data_create();
	// Update styles while keeping their original properties open.
	for (int change = 0; change < 8; ++change) {
		auto *update = obs_data_create();
		obs_data_set_int(update, "color", 0xFFFFFF - change);
		obs_source_update(source, update);
		obs_data_release(update);
		wait(80);
	}
	obs_data_set_bool(callbackSettings, "outline", true);
	obs_property_modified(obs_properties_get(styleFields, "outline"), callbackSettings);
	check(obs_property_visible(obs_properties_get(styleFields, "outline_size")), "native style callbacks work inside the group");
	obs_data_set_bool(callbackSettings, "outline", false);
	obs_property_modified(obs_properties_get(styleFields, "outline"), callbackSettings);
	check(!obs_property_visible(obs_properties_get(styleFields, "outline_size")), "native style callbacks hide inactive options");
	obs_data_set_bool(callbackSettings, "gradient", true);
	obs_property_modified(obs_properties_get(styleFields, "gradient"), callbackSettings);
	check(obs_property_visible(obs_properties_get(styleFields, "gradient_color")), "native gradient callback survives style updates");
	obs_data_release(callbackSettings);
#endif
	obs_data_release(sourceSettings); obs_properties_destroy(properties);
	check(textppt_progressive_probe(obs_scene_from_source(scene), kind, report.fileName() + ".progressive.txt", dock),
	      "progressive display checks complete");
	if (!qEnvironmentVariableIsEmpty("TEXTPPT_STRESS") || !qEnvironmentVariableIsEmpty("TEXTPPT_STUDIO_ONLY"))
		check(textppt_studio_probe(kind, report.fileName() + ".studio.txt", dock), "Studio Mode copy checks complete");
	nativeSizing(obs_scene_from_source(scene), kind);
	nativeFontParity(obs_scene_from_source(scene), kind);
	if (!qEnvironmentVariableIsEmpty("TEXTPPT_PROGRESSIVE_ONLY")) {
		obs_sceneitem_remove(item);
		obs_source_remove(source);
		obs_source_release(source);
		obs_source_release(scene);
		check(passed, "UI TESTS COMPLETE");
		report.close();
		if (passed && !qEnvironmentVariableIsEmpty("TEXTPPT_STRESS"))
			textppt_stress_probe(report.fileName() + ".stress.txt");
		return;
	}
	groupRecovery(obs_scene_from_source(scene), kind);
	wait(200);
	const auto playbackBuffers = nativeBuffers(source);
	for (int change = 0; change < 8; ++change) {
		auto *update = obs_data_create();
		obs_data_set_int(update, "slide_time", 5000 + change * 50);
		obs_data_set_bool(update, "loop", change % 2 != 0);
		obs_source_update(source, update);
		obs_data_release(update);
		wait(60);
	}
	check(!playbackBuffers.empty() && nativeBuffers(source) == playbackBuffers,
	      "playback settings reuse native text buffers");
	sources->setCurrentIndex(sources->findData(QString::fromUtf8(obs_source_get_uuid(source))));
	wait(300);
	auto controls = dock->findChildren<QToolButton *>();
	controls.removeAll(sourceLock);
	controls.removeAll(follow);
	controls.removeAll(dock->findChild<QToolButton *>("TextPPTPreviousImage"));
	controls.removeAll(dock->findChild<QToolButton *>("TextPPTNextImage"));
	check(controls.size() == 4, "dock has four compact controls");
	auto *imageButtons = dock->findChild<QWidget *>("TextPPTImageControls");
	check(imageButtons && imageButtons->isHidden(), "custom text hides the group image controls");
	if (controls.size() == 4)
		check(controls[1]->x() - controls[0]->geometry().right() >= 12, "restart is separated at the left of the toolbar");
	auto *pages = dock->findChild<QListWidget *>();
	check(pages && pages->count() == 3 && pages->item(0)->toolTip().isEmpty() &&
	      pages->item(0)->text().startsWith("01  <b>one</b>"), "page previews retain literal markup without content tooltips");
	check(follow->isChecked(), "scene following is enabled by default");
	auto *header = dock->layout()->itemAt(0)->layout();
	check(header && header->indexOf(follow) == header->indexOf(sourceLock) - 1 &&
	      follow->geometry().center().x() < sourceLock->geometry().center().x() &&
	      follow->y() <= sourceLock->geometry().bottom() && sourceLock->y() <= follow->geometry().bottom(),
	      "scene following is immediately left of the source lock");
	check(status->y() > sources->y() && status->x() > input->x(), "page counter is at the right of the footer");
	for (auto *control : controls)
		check(!control->icon().isNull() && control->text().isEmpty() && !control->accessibleName().isEmpty() &&
		      control->size() == QSize(28, 24), "controls use compact icons with accessible labels");
	check(input->isEnabled(), "page input is enabled for a nonempty source");
	enterPage(input, "3"); wait(500);
	check(page(source) == 2, "entering page three jumps to the last page");
	check(!QApplication::activeModalWidget(), "page entry does not open a dialog");
	input->setText("1"); input->setModified(true); wait(350);
	check(page(source) == 2 && input->text() == "1", "typing alone neither jumps nor gets overwritten");
	QFocusEvent leave(QEvent::FocusOut);
	QApplication::sendEvent(input, &leave); wait(100);
	check(page(source) == 2 && input->text() == "3", "leaving an unfinished entry restores the current page");
	enterPage(input, "4"); wait(350);
	check(page(source) == 2 && input->text() == "3", "out-of-range entry preserves the current page");
	settings = obs_data_create();
	input->setText("1"); input->setModified(true);
	obs_data_set_string(settings, "transition", "fade");
	obs_data_set_int(settings, "transition_speed", 150);
	obs_source_update(source, settings); obs_data_release(settings); wait(500);
	check(input->text() == "1" && input->isModified(), "settings edits preserve unfinished page input");
	QApplication::sendEvent(input, &leave); wait(100);
	check(page(source) == 2, "transition edits preserve the current page");
	check(obs_source_media_get_state(source) == OBS_MEDIA_STATE_PAUSED, "settings edits preserve the paused state");
	editor(source, false);
	check(page(source) == 2, "cancelling the editor preserves playback");
	settings = obs_data_create();
	obs_data_set_bool(settings, "custom_delim", true);
	obs_data_set_string(settings, "delim", "---");
	obs_data_set_string(settings, "custom_text", "one\n\nbody\n---\ntwo\n---\nthree");
	obs_source_update(source, settings); obs_data_release(settings); wait(500);
	check(page(source) == 0, "changing content returns to the first page");
	editor(source, true);
	// Verify scene following with a few isolated fixtures, without a stress loop.
	auto *a = obs_scene_create("TextPPT UI Scene A");
	auto *b = obs_scene_create("TextPPT UI Scene B");
	auto *empty = obs_scene_create("TextPPT UI Empty Scene");
	settings = obs_data_create();
	const std::string tabContent = "left\tright\nlonger\tend";
	obs_data_set_string(settings, "custom_text", tabContent.c_str());
	auto *other = obs_source_create(kind, "TextPPT UI Second", settings, nullptr);
	// OBS retains the settings object; each source needs its own mutable instance.
	obs_data_release(settings);
	settings = obs_data_create();
	obs_data_set_string(settings, "custom_text", "third");
	auto *third = obs_source_create(kind, "TextPPT UI Third", settings, nullptr);
	obs_data_release(settings);
	if (check(a && b && empty && other && third, "scene-follow fixtures are created")) {
		obs_scene_add(a, source);
		obs_scene_add(a, third);
		auto *group = obs_scene_add_group(b, "TextPPT UI Nested Group");
		obs_scene_add(obs_sceneitem_group_get_scene(group), other);
		wait(350);
		obs_frontend_set_current_scene(obs_scene_get_source(b)); wait(350);
		check(sources->currentData().toString() == QString::fromUtf8(obs_source_get_uuid(other)), "scene following finds TextPPT inside a group");
		check(input->text() == "1", "source changes reset the page input");
		const auto rendered = bufferText(other);
		check(std::any_of(rendered.begin(), rendered.end(), [](const auto &text) {
			return text.find('\t') == std::string::npos && text.find("right") != std::string::npos &&
				text.find("longer") != std::string::npos;
		}), "native text buffers receive expanded tab columns");
		auto *tabSettings = obs_source_get_settings(other);
		check(obs_data_get_string(tabSettings, "custom_text") == tabContent, "tab rendering leaves saved content unchanged");
		obs_data_release(tabSettings);
		obs_frontend_set_current_scene(obs_scene_get_source(a)); wait(350);
		sources->setCurrentIndex(sources->findData(QString::fromUtf8(obs_source_get_uuid(third))));
		obs_frontend_set_current_scene(obs_scene_get_source(b)); wait(350);
		obs_frontend_set_current_scene(obs_scene_get_source(a)); wait(350);
		check(sources->currentData().toString() == QString::fromUtf8(obs_source_get_uuid(third)), "multiple-source scenes remember the manual choice");
		const QString thirdUuid = QString::fromUtf8(obs_source_get_uuid(third));
		const QString sourceUuid = QString::fromUtf8(obs_source_get_uuid(source));
		const QString otherUuid = QString::fromUtf8(obs_source_get_uuid(other));
		check(follow->isChecked(), "each source defaults to scene following");
		check(!sourceLock->isChecked(), "the Dock starts unlocked");
		sourceLock->click();
		check(sourceLock->isChecked() && !sources->isEnabled() && follow->isChecked(),
		      "locking prevents selection changes without unchecking scene following");
		obs_frontend_set_current_scene(obs_scene_get_source(b)); wait(350);
		check(sources->currentData().toString() == thirdUuid, "scene changes retain the locked source");
		obs_source_set_name(third, "TextPPT UI Locked Source"); wait(350);
		check(sourceLock->isChecked() && sources->currentData().toString() == thirdUuid,
		      "renaming a locked source preserves the lock by UUID");
		dock->hide(); dock->show(); wait(350);
		check(sourceLock->isChecked() && sources->currentData().toString() == thirdUuid,
		      "reopening the Dock preserves its source lock");
		sourceLock->click();
		check(!sourceLock->isChecked() && sources->isEnabled() && sources->currentData().toString() == otherUuid,
		      "unlocking immediately follows the current scene");
		obs_frontend_set_current_scene(obs_scene_get_source(a)); wait(350);
		check(sources->currentData().toString() == thirdUuid && follow->isChecked(),
		      "locking does not replace remembered scene choices");
		const auto buffers = nativeBuffers(third);
		follow->setChecked(false); wait(200);
		check(sources->currentData().toString() == thirdUuid && !follow->isChecked(),
		      "unchecking a source keeps the current selection");
		check(!buffers.empty() && nativeBuffers(third) == buffers,
		      "changing eligibility retains native text buffers");
		// Check the same metadata that OBS saves and loads with the source.
		auto *saved = obs_save_source(third);
		auto *roundtrip = obs_data_create_from_json(obs_data_get_json(saved));
		auto *metadata = obs_data_get_obj(roundtrip, "private_settings");
		check(obs_data_has_user_value(metadata, "textppt_follow_scene") &&
		      !obs_data_get_bool(metadata, "textppt_follow_scene"), "unchecked eligibility survives source serialization");
		obs_data_release(metadata); obs_data_release(roundtrip); obs_data_release(saved);
		sources->setCurrentIndex(sources->findData(sourceUuid));
		check(follow->isChecked(), "another source retains its own checked state");
		sources->setCurrentIndex(sources->findData(thirdUuid));
		check(!follow->isChecked(), "reselecting a source restores its unchecked state");
		obs_source_set_name(third, "TextPPT UI Third Renamed"); wait(350);
		check(sources->currentData().toString() == thirdUuid && !follow->isChecked(),
		      "renaming an unchecked source preserves selection and eligibility");
		obs_frontend_set_current_scene(obs_scene_get_source(b)); wait(350);
		check(sources->currentData().toString() == otherUuid && follow->isChecked(),
		      "an unchecked source does not stop other sources from following scenes");
		obs_frontend_set_current_scene(obs_scene_get_source(a)); wait(350);
		check(sources->currentData().toString() == sourceUuid,
		      "scene following skips the remembered unchecked source");
		obs_frontend_set_current_scene(obs_scene_get_source(b)); wait(350);
		follow->setChecked(false);
		obs_frontend_set_current_scene(obs_scene_get_source(a)); wait(350);
		obs_frontend_set_current_scene(obs_scene_get_source(b)); wait(350);
		check(sources->currentData().toString() == sourceUuid,
		      "scenes containing only unchecked sources retain manual selection");
		sources->setCurrentIndex(sources->findData(otherUuid));
		check(!follow->isChecked(), "unchecked sources remain available for manual control");
		follow->setChecked(true);
		check(sources->currentData().toString() == otherUuid, "checking a source keeps the current selection");
		obs_frontend_set_current_scene(obs_scene_get_source(a)); wait(350);
		sources->setCurrentIndex(sources->findData(thirdUuid));
		follow->setChecked(true);
		obs_frontend_set_current_scene(obs_scene_get_source(b)); wait(350);
		obs_frontend_set_current_scene(obs_scene_get_source(a)); wait(350);
		check(sources->currentData().toString() == thirdUuid && follow->isChecked(),
		      "reenabling a source takes effect on the next scene switch");
		obs_frontend_set_current_scene(obs_scene_get_source(empty)); wait(350);
		check(sources->currentData().toString() == thirdUuid && status->isEnabled(),
		      "scenes without TextPPT retain manual playback controls");
		check(follow->isEnabled(), "the retained source can still change its eligibility");
		obs_frontend_set_current_scene(scene); wait(350);
		obs_frontend_set_preview_program_mode(true); wait(350);
		const QString programSource = sources->currentData().toString();
		obs_frontend_set_current_preview_scene(obs_scene_get_source(b)); wait(350);
		check(sources->currentData().toString() == otherUuid && sources->currentData().toString() != programSource,
		      "Studio preview changes select the preview source independently of program");
		obs_frontend_set_current_preview_scene(obs_scene_get_source(empty)); wait(350);
		check(sources->currentData().toString() == otherUuid, "an empty Studio preview retains manual source selection");
		obs_frontend_set_preview_program_mode(false); wait(350);
		dock->hide();
		obs_frontend_set_current_scene(obs_scene_get_source(a)); wait(350);
		dock->show(); wait(350);
		check(sources->currentData().toString() == thirdUuid, "reopening the dock follows the current scene");
		const bool previewEnabled = obs_frontend_preview_enabled();
		obs_frontend_set_preview_enabled(false);
		obs_frontend_set_current_scene(obs_scene_get_source(b)); wait(350);
		check(sources->currentData().toString() == otherUuid, "scene following works with preview rendering disabled");
		obs_frontend_set_preview_enabled(previewEnabled);
		obs_frontend_set_current_scene(obs_scene_get_source(a)); wait(350);
		sourceLock->click();
		obs_source_remove(third); wait(350);
		check(!sourceLock->isChecked() && sources->isEnabled() && sources->currentData().toString() == sourceUuid && follow->isChecked(),
		      "removing a locked source unlocks and selects an eligible source");
		obs_frontend_set_current_scene(scene); wait(350);
	}
	sources->setCurrentIndex(sources->findData(QString::fromUtf8(obs_source_get_uuid(source))));
	settings = obs_data_create();
	obs_data_set_bool(settings, "custom_delim", false);
	obs_source_update(source, settings);
	obs_data_release(settings);
	wait(400);
	QTimer::singleShot(50, qApp, [] {
		auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		if (check(menu && menu->actions().size() == 2 && menu->findChild<QAction *>("TextPPTEditAction") &&
		          !menu->findChild<QAction *>("TextPPTReloadAction"), "custom text menu exposes properties and editing")) {
			check(menu->actions().first()->isEnabled(), "source properties action is available");
		}
		if (menu)
			menu->close();
	});
	QMetaObject::invokeMethod(dock, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, QPoint(10, 10)));
	whenEditor([source](QDialog *dialog) {
		auto *edit = dialog ? dialog->findChild<QPlainTextEdit *>("TextPPTEditor") : nullptr;
		auto *save = dialog ? dialog->findChild<QShortcut *>("TextPPTSave") : nullptr;
		if (check(edit && save, "dock editing opens the existing independent editor")) {
			check(save->key() == QKeySequence(QKeySequence::Save) && save->context() == Qt::WidgetWithChildrenShortcut,
			      "save uses the platform shortcut within the editor");
			auto *changes = obs_data_create();
			obs_data_set_int(changes, "color", 0xEDEDED);
			obs_data_set_int(changes, "slide_time", 4500);
			obs_source_update(source, changes);
			obs_data_release(changes);
			wait(100);
			QString invalid;
			for (int index = 0; index < 1001; ++index)
				invalid += "page\n\n";
			edit->setPlainText(invalid);
			QShortcutEvent shortcut(save->key(), save);
			QApplication::sendEvent(save, &shortcut);
			check(dialog->isVisible(), "shortcut saving applies the page limit before closing");
			edit->setPlainText("saved first\n\nsaved second");
			QApplication::sendEvent(save, &shortcut);
			if (!check(!dialog->isVisible(), "valid shortcut saving closes the editor"))
				dialog->reject();
		} else if (dialog) {
			dialog->reject();
		}
	});
	contextAction(dock, "TextPPTEditAction");
	wait(400);
	check(deck(source) == std::vector<std::string>{"saved first", "saved second"}, "save shortcut commits edited content");
	settings = obs_source_get_settings(source);
	check(obs_data_get_int(settings, "color") == 0xEDEDED && obs_data_get_int(settings, "slide_time") == 4500,
	      "editor saving retains concurrent style and playback changes");
	obs_data_release(settings);
	whenEditor([source](QDialog *dialog) {
		auto *edit = dialog ? dialog->findChild<QPlainTextEdit *>("TextPPTEditor") : nullptr;
		auto *save = dialog ? dialog->findChild<QShortcut *>("TextPPTSave") : nullptr;
		if (check(edit && save, "editor reopens for changed-setting validation")) {
			edit->setPlainText("unsaved text");
			auto *changes = obs_data_create();
			obs_data_set_bool(changes, "custom_delim", true);
			obs_data_set_string(changes, "delim", "---");
			obs_source_update(source, changes);
			obs_data_release(changes);
			wait(100);
			QShortcutEvent shortcut(save->key(), save);
			QApplication::sendEvent(save, &shortcut);
			check(dialog->isVisible(), "changed page separators prevent a stale editor save");
		}
		if (dialog) dialog->reject();
	});
	contextAction(dock, "TextPPTEditAction");
	settings = obs_source_get_settings(source);
	check(strcmp(obs_data_get_string(settings, "custom_text"), "saved first\n\nsaved second") == 0,
	      "rejected editor saves preserve the content");
	obs_data_release(settings);
	settings = obs_data_create();
	obs_data_set_bool(settings, "custom_delim", false);
	obs_source_update(source, settings);
	obs_data_release(settings); wait(400);
	PropertiesPaintProbe firstPaint(source);
	contextAction(dock);
	check(firstPaint.seen, "source properties received a first content paint");
	QDialog *sourceDialog = nullptr;
	for (auto *widget : QApplication::topLevelWidgets())
		if (widget->inherits("OBSBasicProperties")) sourceDialog = qobject_cast<QDialog *>(widget);
	if (check(sourceDialog != nullptr, "context action opens native source properties")) {
		auto *inlineText = sourceDialog->findChild<QPlainTextEdit *>();
		auto *form = inlineText ? qobject_cast<QFormLayout *>(inlineText->parentWidget()->layout()) : nullptr;
		auto *caption = form ? form->labelForField(inlineText) : nullptr;
		check(caption && caption->y() - inlineText->y() == firstPaint.captionOffset &&
		      caption->height() < inlineText->height() / 2,
		      "content caption does not shift after the initial paint");
		sourceDialog->grab().save(report.fileName() + ".properties.png");
		check(inlineText && inlineText->height() >= inlineText->fontMetrics().lineSpacing() * 8,
		      "custom text input shows at least eight lines");
		textMenu(inlineText);
		nativeEditorTabs(inlineText);
		QWidget *view = inlineText;
		while (view && !view->inherits("OBSPropertiesView"))
			view = view->parentWidget();
		if (check(view != nullptr, "inline input belongs to the native properties view")) {
			const auto style = view->styleSheet();
			for (int index = 0; index < 3; ++index) {
				check(QMetaObject::invokeMethod(view, "ReloadProperties", Qt::DirectConnection),
				      "native properties reload completes");
				check(qAbs(view->width() - view->parentWidget()->contentsRect().width()) <= 1,
				      "reloaded properties keep their allocated width immediately");
				auto *scroll = qobject_cast<QScrollArea *>(view);
				auto *container = scroll ? scroll->widget() : nullptr;
				check(container && container->findChildren<QPushButton *>("TextPPTEditButton").size() == 1,
				      "properties reloads retain one compact editor button");
			}
			check(view->styleSheet() == style, "properties reloads do not append height rules");
			check(view->findChildren<QObject *>("TextPPTPropertiesLayout", Qt::FindDirectChildrenOnly).size() == 1,
			      "properties reloads keep one layout connection");
			auto *scroll = qobject_cast<QScrollArea *>(view);
			auto *container = scroll ? scroll->widget() : nullptr;
			textMenu(container ? container->findChild<QPlainTextEdit *>("TextPPTContent") : nullptr);
			auto *button = container ? container->findChild<QPushButton *>("TextPPTEditButton") : nullptr;
			if (check(button != nullptr, "compact editor button is available after reloads")) {
				whenEditor([](QDialog *dialog) {
					check(dialog && dialog->findChild<QPlainTextEdit *>("TextPPTEditor"),
					      "compact editor button opens the independent editor");
					if (dialog) {
						editorScroll(dialog->findChild<QPlainTextEdit *>("TextPPTEditor"));
						dialog->reject();
					}
				});
				button->click();
			}
		}
		sourceDialog->reject(); wait(350);
		obs_frontend_save(); wait(350);
		check(page(source) == 0, "closing properties and saving keeps the current page");
	}
	fileControls(dock, source);
	// Exercise the file limit through the plugin, with reverse creation order.
	QTemporaryDir folder;
	if (check(folder.isValid(), "file-mode test directory is created")) {
		bool written = true;
		for (int index = 1004; index >= 0; --index) {
			QFile file(folder.path() + QString("/page-%1.TXT").arg(index, 4, 10, QChar('0')));
			written &= file.open(QIODevice::WriteOnly) && file.write("File page") == 9;
		}
		if (check(written, "1005 test files are written")) {
			settings = obs_data_create();
			auto *files = obs_data_array_create();
			auto *entry = obs_data_create();
			const auto path = folder.path().toUtf8();
			obs_data_set_string(entry, "value", path.constData());
			obs_data_array_push_back(files, entry);
			obs_data_release(entry);
			obs_data_set_string(settings, "input_mode", "files");
			obs_data_set_array(settings, "files", files);
			obs_data_array_release(files);
			obs_source_update(source, settings);
			obs_data_release(settings);
			wait(700);
			std::vector<std::string> paths;
			calldata_t data = {};
			calldata_set_ptr(&data, "texts", &paths);
			proc_handler_call(obs_source_get_proc_handler(source), "get_texts", &data);
			calldata_free(&data);
			check(paths.size() == 1000 && QString::fromStdString(paths.front()).endsWith("/page-0000.TXT") &&
			      QString::fromStdString(paths.back()).endsWith("/page-0999.TXT"),
			      "directory mode keeps the first 1000 files sorted by name");
		}
	}
	QPixmap image(dock->size());
	dock->render(&image);
	image.save(report.fileName() + ".png");
	for (auto *fixture : {a, b, empty}) {
		if (fixture) obs_source_remove(obs_scene_get_source(fixture));
		obs_scene_release(fixture);
	}
	obs_source_remove(other); obs_source_remove(third);
	obs_source_release(other); obs_source_release(third);
	obs_sceneitem_remove(item); obs_source_remove(source);
	obs_source_release(source); obs_source_release(scene);
	if (!qEnvironmentVariableIsEmpty("TEXTPPT_IMAGE_STRESS"))
		imageControls(dock);
	check(passed, "UI TESTS COMPLETE");
	report.close();
	if (passed && !qEnvironmentVariableIsEmpty("TEXTPPT_STRESS"))
		textppt_stress_probe(report.fileName() + ".stress.txt");
}
static void event(obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) QTimer::singleShot(13000, qApp, run);
}
bool obs_module_load()
{
	if (qEnvironmentVariableIsEmpty("TEXTPPT_UI_REPORT")) return false;
	obs_frontend_add_event_callback(event, nullptr);
	return true;
}
void obs_module_unload() { obs_frontend_remove_event_callback(event, nullptr); }
