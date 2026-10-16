// OBS-TextPPT, GPL-2.0-or-later.
#include "obs-textppt.h"
#include "text-parser.h"
#include <obs-frontend-api.h>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTextDocument>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QMenu>
#include <QAction>
#include <QEvent>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QTimer>
#include <QThread>
#include <QShortcut>
#include <QSlider>
#include <QSignalBlocker>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QWheelEvent>
#include <QResizeEvent>
#include <cmath>
#include <cstring>

static QString label(const char *key) { return QString::fromUtf8(obs_module_text(key)); }

class EditorZoom : public QObject {
public:
	EditorZoom(QPlainTextEdit *edit, QSlider *slider, QLabel *sizeLabel)
		: QObject(edit), edit(edit), slider(slider), sizeLabel(sizeLabel)
	{
		// Capture the theme font before applying view-only zoom.
		edit->window()->ensurePolished();
		original = edit->font();
		if (original.pointSizeF() <= 0)
			original.setPointSizeF(QFontInfo(original).pointSizeF());
		original.setPointSizeF(qBound(6.0, original.pointSizeF(), 72.0));
		reset();
		connect(slider, &QSlider::valueChanged, this, [this](int size) {
			auto font = this->edit->font();
			font.setPointSize(size);
			this->edit->setFont(font);
			updateView();
		});
		edit->viewport()->installEventFilter(this);
		const auto shortcut = [&](const QList<QKeySequence> &keys, const char *name, auto action) {
			auto *key = new QShortcut(edit);
			key->setKeys(keys);
			key->setObjectName(QString::fromLatin1(name));
			// Keep zoom keys active when the slider has focus.
			key->setContext(Qt::WindowShortcut);
			connect(key, &QShortcut::activated, this, action);
		};
		shortcut(QKeySequence::keyBindings(QKeySequence::ZoomIn), "TextPPTZoomIn", [this] { zoom(1); });
		shortcut(QKeySequence::keyBindings(QKeySequence::ZoomOut), "TextPPTZoomOut", [this] { zoom(-1); });
		shortcut({QKeySequence(Qt::CTRL | Qt::Key_0)}, "TextPPTZoomReset", [this] { reset(); });
	}

	void updateTabs()
	{
		const QFontMetricsF metrics(edit->font(), edit);
		qreal width = 0;
		// Keep TAB cells in one column as the reading font changes.
		if (edit->document()->characterCount() <= 64 * 1024) {
			const auto text = edit->toPlainText();
			for (qsizetype start = 0, end = 0; end < text.size(); ++end) {
				const auto ch = text[end];
				if (ch == '\t')
					width = qMax(width, metrics.horizontalAdvance(text.mid(start, end - start)));
				if (ch == '\t' || ch == '\r' || ch == '\n') start = end + 1;
			}
		}
		const qreal distance = width + metrics.horizontalAdvance(QLatin1Char(' ')) * 4;
		if (edit->tabStopDistance() == distance) return;
		edit->setTabStopDistance(distance);
		// Refresh native scroll ranges after font and TAB layout changes.
		QResizeEvent resized(edit->viewport()->size(), edit->viewport()->size());
		QApplication::sendEvent(edit->viewport(), &resized);
	}

protected:
	bool eventFilter(QObject *object, QEvent *event) override
	{
		if (event->type() != QEvent::Wheel)
			return QObject::eventFilter(object, event);
		auto *wheel = static_cast<QWheelEvent *>(event);
		if (wheel->modifiers() != Qt::ControlModifier) {
			remainder = 0;
			return false;
		}
		// Accumulate smooth scrolling without forwarding it to the parent.
		remainder += wheel->angleDelta().y() ? wheel->angleDelta().y() : wheel->pixelDelta().y() * 3;
		zoom(remainder / 120);
		remainder %= 120;
		wheel->accept();
		return true;
	}

private:
	void zoom(int steps)
	{
		const auto size = edit->font().pointSizeF();
		steps = qBound(static_cast<int>(std::ceil(6 - size)), steps, static_cast<int>(std::floor(72 - size)));
		if (!steps) return;
		edit->zoomIn(steps);
		updateView();
	}
	void reset()
	{
		edit->setFont(original);
		remainder = 0;
		updateView();
	}
	void updateView()
	{
		updateTabs();
		const QSignalBlocker blocker(slider);
		slider->setValue(qRound(edit->font().pointSizeF()));
		sizeLabel->setText(QStringLiteral("%1 pt").arg(edit->font().pointSizeF(), 0, 'g', 3));
	}
	QPlainTextEdit *edit;
	QSlider *slider;
	QLabel *sizeLabel;
	QFont original;
	int remainder = 0;
};

static void updateEditorIcon(QPushButton *button)
{
	QIcon icon;
	// Draw in the theme's text color at common display scales.
	for (int scale = 1; scale <= 3; ++scale) {
		QPixmap image(16 * scale, 16 * scale);
		image.setDevicePixelRatio(scale);
		image.fill(Qt::transparent);
		QPainter painter(&image);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(QPen(button->palette().color(QPalette::Active, QPalette::ButtonText),
		                    1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		QPainterPath path;
		path.moveTo(7, 3); path.lineTo(3, 3); path.lineTo(3, 13);
		path.lineTo(13, 13); path.lineTo(13, 9);
		path.moveTo(8, 8); path.lineTo(13, 3);
		path.moveTo(9, 3); path.lineTo(13, 3); path.lineTo(13, 7);
		painter.drawPath(path);
		painter.end();
		icon.addPixmap(image);
	}
	button->setIcon(icon);
}

static void localizeTextMenu(QPlainTextEdit *edit)
{
	edit->setContextMenuPolicy(Qt::CustomContextMenu);
	QObject::connect(edit, &QWidget::customContextMenuRequested, edit, [edit](const QPoint &position) {
		auto *menu = edit->createStandardContextMenu();
		if (!menu)
			return;
		menu->setObjectName("TextPPTTextMenu");
		static constexpr struct {
			const char *action;
			const char *key;
		} translations[] = {
			{"edit-undo", "Editor.Undo"}, {"edit-redo", "Editor.Redo"},
			{"edit-cut", "Editor.Cut"}, {"edit-copy", "Editor.Copy"},
			{"edit-paste", "Editor.Paste"}, {"edit-delete", "Editor.Delete"},
			{"select-all", "Editor.SelectAll"},
		};
		// Keep Qt's actions, states and platform shortcuts.
		for (auto *action : menu->actions()) {
			for (const auto &entry : translations) {
				if (action->objectName() != QLatin1String(entry.action))
					continue;
				const auto text = action->text();
				const auto tab = text.indexOf('\t');
				action->setText(label(entry.key) + (tab >= 0 ? text.mid(tab) : QString()));
				break;
			}
		}
		menu->setAttribute(Qt::WA_DeleteOnClose);
		menu->popup(edit->viewport()->mapToGlobal(position));
	});
}

class TextPPTPropertiesLayout : public QObject {
	Q_OBJECT
public:
	explicit TextPPTPropertiesLayout(QWidget *view) : QObject(view) {}

public slots:
	void refresh()
	{
		auto *view = static_cast<QWidget *>(parent());
		auto *scroll = qobject_cast<QScrollArea *>(view);
		auto *container = scroll ? scroll->widget() : nullptr;
		if (!container)
			return;
		auto *form = qobject_cast<QFormLayout *>(container->layout());
		auto *edit = container->findChild<QPlainTextEdit *>();
		if (edit && edit->objectName() != "TextPPTContent") {
			edit->setObjectName("TextPPTContent");
			edit->setTabChangesFocus(false);
			localizeTextMenu(edit);
		}
		auto *caption = form && edit ? form->labelForField(edit) : nullptr;
		auto *labels = caption ? qobject_cast<QHBoxLayout *>(caption->layout()) : nullptr;
		if (labels) {
			for (auto *button : container->findChildren<QPushButton *>()) {
				if (button->text() != label("SlideShow.EditText"))
					continue;
				// Move OBS's button intact, retaining its source callback.
				const auto row = form->takeRow(button);
				delete row.labelItem;
				delete row.fieldItem;
				button->setObjectName("TextPPTEditButton");
				button->setText(QString());
				button->setToolTip(label("SlideShow.EditText"));
				button->setAccessibleName(label("SlideShow.EditText"));
				updateEditorIcon(button);
				button->installEventFilter(this);
				button->setIconSize(QSize(16, 16));
				const int size = qMax(22, button->fontMetrics().lineSpacing() + 4);
				button->setFixedSize(size, size);
				button->setAutoDefault(false);
				button->setStyleSheet("QPushButton { min-width: 0; min-height: 0; padding: 2px; }");
				labels->addSpacing(4);
				labels->addWidget(button);
				break;
			}
			// Keep the caption at the top of the multiline field.
			caption->setSizePolicy(caption->sizePolicy().horizontalPolicy(), QSizePolicy::Fixed);
			form->setAlignment(caption, Qt::AlignTop | Qt::AlignRight);
		}
		// Span the footer across the form.
		for (auto *info : container->findChildren<QLabel *>()) {
			if (!info->text().contains(QStringLiteral("id=\"TextPPTInfo\"")))
				continue;
			if (auto *infoForm = qobject_cast<QFormLayout *>(info->parentWidget()->layout())) {
				int row = -1;
				QFormLayout::ItemRole role;
				infoForm->getWidgetPosition(info, &row, &role);
				if (row >= 0 && role == QFormLayout::FieldRole) {
					infoForm->removeWidget(info);
					infoForm->setWidget(row, QFormLayout::SpanningRole, info);
				}
				infoForm->setAlignment(info, Qt::AlignRight | Qt::AlignVCenter);
			}
			info->setObjectName("TextPPTInfo");
			info->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		}
		auto *frame = view->parentWidget();
		// Restore the allocated width before painting.
		if (auto *layout = frame ? frame->layout() : nullptr) {
			layout->invalidate();
			layout->activate();
		}
	}

protected:
	bool eventFilter(QObject *object, QEvent *event) override
	{
		if (event->type() == QEvent::PaletteChange)
			updateEditorIcon(static_cast<QPushButton *>(object));
		return QObject::eventFilter(object, event);
	}
};

void textppt_prepare_properties(obs_source_t *source)
{
	if (!qApp || !source || QThread::currentThread() != qApp->thread())
		return;
	const auto title = QString::fromUtf8(obs_frontend_get_locale_string("Basic.PropertiesWindow"))
		.arg(QString::fromUtf8(obs_source_get_name(source)));
	for (auto *window : QApplication::topLevelWidgets()) {
		if (!window->inherits("OBSBasicProperties") || window->windowTitle() != title)
			continue;
		for (auto *view : window->findChildren<QWidget *>()) {
			if (!view->inherits("OBSPropertiesView") ||
			    view->findChild<QObject *>("TextPPTPropertiesLayout", Qt::FindDirectChildrenOnly))
				continue;
			auto *layout = new TextPPTPropertiesLayout(view);
			layout->setObjectName("TextPPTPropertiesLayout");
			// Observe OBS's signal without linking its private widget class.
			if (!QObject::connect(view, SIGNAL(PropertiesRefreshed()), layout, SLOT(refresh()), Qt::DirectConnection)) {
				delete layout;
				blog(LOG_WARNING, "[obs-textppt] Properties layout signal unavailable");
				continue;
			}
			// Set input height before OBS creates the fields.
			const auto rule = QStringLiteral("\nQPlainTextEdit { min-height: %1px; max-height: %1px; }")
				.arg(qMax(160, view->fontMetrics().lineSpacing() * 8 + 12));
			view->setStyleSheet(view->styleSheet() + rule);
		}
	}
}

bool textppt_edit_text(obs_properties_t *, obs_property_t *, void *data)
{
	auto *s = static_cast<textppt_source *>(data);
	if (!s)
		return false;
	auto *source = obs_source_get_ref(s->source);
	auto *settings = obs_source_get_settings(source);
	if (strcmp(textppt_input_mode(settings), INPUT_CUSTOM) != 0) {
		obs_data_release(settings);
		obs_source_release(source);
		return false;
	}
	const string delimiter = obs_data_get_bool(settings, S_CUSTOM_DELIM) ?
		obs_data_get_string(settings, S_DELIM) : "";
	QDialog dialog(QApplication::activeWindow());
	dialog.setWindowTitle(label("Editor.Title"));
	dialog.setWindowFlag(Qt::WindowMaximizeButtonHint, true);
	dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	dialog.setMinimumSize(640, 360);
	// Fit the 16:9 editor to the current screen without enlarging it.
	const auto available = dialog.screen()->availableGeometry().size();
	QSize size(1280, 720);
	size.scale(size.boundedTo(available * 0.9), Qt::KeepAspectRatio);
	dialog.resize(size);
	auto *layout = new QVBoxLayout(&dialog);
	auto *hint = new QLabel(delimiter.empty() ? label("Editor.Hint") :
		label("Editor.DelimiterHint").arg(QString::fromUtf8(delimiter.c_str())), &dialog);
	hint->setObjectName("TextPPTEditorHint");
	hint->setAlignment(Qt::AlignCenter);
	hint->setWordWrap(true);
	layout->addWidget(hint);
	auto *edit = new QPlainTextEdit(&dialog);
	edit->setObjectName("TextPPTEditor");
	edit->setTabChangesFocus(false);
	edit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
	// Keep a usable mouse target with the native theme colors.
	edit->verticalScrollBar()->setStyleSheet("QScrollBar:vertical { width: 16px; }");
	auto *zoomSlider = new QSlider(Qt::Horizontal, &dialog);
	zoomSlider->setObjectName("TextPPTZoomSlider");
	zoomSlider->setRange(6, 72);
	zoomSlider->setPageStep(4);
	zoomSlider->setFixedWidth(120);
	zoomSlider->setToolTip(label("Editor.Zoom"));
	zoomSlider->setAccessibleName(label("Editor.Zoom"));
	auto *zoomSize = new QLabel(&dialog);
	zoomSize->setObjectName("TextPPTZoomSize");
	zoomSize->setMinimumWidth(zoomSize->fontMetrics().horizontalAdvance(QStringLiteral("72.0 pt")));
	auto *zoom = new EditorZoom(edit, zoomSlider, zoomSize);
	localizeTextMenu(edit);
	edit->setPlainText(QString::fromUtf8(obs_data_get_string(settings, S_CUSTOM_TEXT)));
	layout->addWidget(edit, 1);
	auto *error = new QLabel(&dialog);
	error->setWordWrap(true);
	error->setVisible(false);
	layout->addWidget(error);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
	buttons->button(QDialogButtonBox::Save)->setText(label("Editor.Save"));
	buttons->button(QDialogButtonBox::Cancel)->setText(label("Editor.Cancel"));
	// Keep the platform Save shortcut local to this dialog.
	auto *saveShortcut = new QShortcut(QKeySequence::Save, &dialog);
	saveShortcut->setObjectName("TextPPTSave");
	saveShortcut->setContext(Qt::WidgetWithChildrenShortcut);
	saveShortcut->setAutoRepeat(false);
	buttons->button(QDialogButtonBox::Save)->setToolTip(QKeySequence(QKeySequence::Save).toString(QKeySequence::NativeText));
	QObject::connect(saveShortcut, &QShortcut::activated, buttons->button(QDialogButtonBox::Save), &QPushButton::click);
	auto *count = new QLabel(&dialog);
	count->setObjectName("TextPPTPageCount");
	auto *footer = new QHBoxLayout;
	footer->addWidget(count);
	footer->addStretch();
	footer->addWidget(zoomSlider);
	footer->addWidget(zoomSize);
	footer->addWidget(buttons);
	layout->addLayout(footer);
	// Count after typing pauses, using the same rules as playback and saving.
	QTimer countTimer;
	countTimer.setSingleShot(true);
	countTimer.setInterval(300);
	const auto updateCount = [&] {
		zoom->updateTabs();
		const auto text = edit->toPlainText().toUtf8();
		vector<string> pages;
		text_error reason;
		const bool valid = parse_slides(std::string_view(text.constData(), static_cast<size_t>(text.size())),
		                                delimiter, pages, reason);
		count->setText(valid ? label("Editor.PageCount").arg(static_cast<qulonglong>(pages.size())) :
		                       label("Editor.InvalidPageCount"));
	};
	QObject::connect(edit, &QPlainTextEdit::textChanged, &dialog, [&] {
		countTimer.start();
		error->setVisible(false);
	});
	QObject::connect(&countTimer, &QTimer::timeout, &dialog, updateCount);
	updateCount();
	QByteArray saved;
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
		countTimer.stop();
		auto *current = obs_source_get_settings(source);
		const string currentDelimiter = obs_data_get_bool(current, S_CUSTOM_DELIM) ?
			obs_data_get_string(current, S_DELIM) : "";
		const bool changed = obs_source_removed(source) || strcmp(textppt_input_mode(current), INPUT_CUSTOM) != 0 ||
			currentDelimiter != delimiter;
		obs_data_release(current);
		if (changed) {
			error->setText(label("Editor.SourceChanged"));
			error->setVisible(true);
			return;
		}
		const auto text = edit->toPlainText().toUtf8();
		vector<string> pages;
		text_error reason;
		if (!parse_slides(std::string_view(text.constData(), static_cast<size_t>(text.size())),
		                  delimiter, pages, reason)) {
			error->setText(label(text_error_key(reason)));
			error->setVisible(true);
			return;
		}
		saved = text;
		dialog.accept();
	});
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	edit->setFocus();
	if (dialog.exec() == QDialog::Accepted && !obs_source_removed(source)) {
		// Commit only content; retain concurrent style and playback edits.
		auto *changes = obs_data_create();
		obs_data_set_string(changes, S_CUSTOM_TEXT, saved.constData());
		obs_source_update(source, changes);
		obs_data_release(changes);
		obs_source_update_properties(source);
	}
	obs_data_release(settings);
	obs_source_release(source);
	return false;
}

#include "text-editor.moc"
