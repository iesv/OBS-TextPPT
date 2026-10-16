// OBS-TextPPT, GPL-2.0-or-later.
#include "page-list.h"
#include <QImage>
#include <QListWidget>
#include <QToolTip>
#include <iostream>
#include <utility>

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	bool passed = true;
	const auto check = [&](bool valid, const char *message) {
		passed &= valid;
		std::cout << (valid ? "PASS: " : "FAIL: ") << message << std::endl;
	};
	for (const auto &entry : {std::pair<size_t, const char *>{1, "01"}, {99, "01"}, {100, "001"},
	                          {999, "001"}, {1000, "0001"}}) {
		const auto first = PageListDelegate::label(0, entry.first, QStringLiteral("body"));
		const auto last = PageListDelegate::label(entry.first - 1, entry.first, QStringLiteral("body"));
		check(first == QString::fromLatin1(entry.second) + QStringLiteral("  body"), "first page follows total-page digit width");
		check(first.indexOf(QStringLiteral("  ")) == last.indexOf(QStringLiteral("  ")) &&
		      last.section(' ', 0, 0).toInt() == static_cast<int>(entry.first), "last page retains its full number and column width");
	}
	QListWidget list;
	PageListDelegate delegate(&list);
	auto *row = new QListWidgetItem(PageListDelegate::label(99, 100, QStringLiteral("<b>literal</b> & body")), &list);
	const auto index = list.model()->index(0, 0);
	check(row->toolTip().isEmpty() && !row->font().bold(), "rows store no tooltip and keep their normal body font");
	QStyleOptionViewItem option;
	option.initFrom(&list);
	option.font = list.font();
	option.font.setBold(false);
	option.fontMetrics = QFontMetrics(option.font, &list);
	option.widget = &list;
	option.rect = QRect(8, 8, 220, 32);
	option.displayAlignment = Qt::AlignLeft | Qt::AlignVCenter;
	option.textElideMode = Qt::ElideRight;
	option.showDecorationSelected = true;
	option.palette.setColor(QPalette::Text, Qt::black);
	option.palette.setColor(QPalette::HighlightedText, Qt::white);
	option.palette.setColor(QPalette::Highlight, QColor(40, 70, 180));
	option.palette.setColor(QPalette::Disabled, QPalette::Text, Qt::gray);
	const auto normalState = option.state | QStyle::State_Enabled | QStyle::State_Active;
	QImage normal, selected;
	for (const auto state : {normalState, normalState | QStyle::State_Selected | QStyle::State_HasFocus,
	                         normalState & ~QStyle::State_Enabled}) {
		option.state = state;
		QImage image(240, 48, QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::transparent);
		QPainter painter(&image);
		painter.setPen(Qt::red);
		const auto originalFont = painter.font();
		delegate.paint(&painter, option, index);
		check(painter.pen().color() == Qt::red && painter.font() == originalFont, "painting restores the caller's font and pen");
		painter.end();
		bool ink = false, clipped = true;
		for (int y = 0; y < image.height(); ++y)
			for (int x = 0; x < image.width(); ++x) {
				if (!qAlpha(image.pixel(x, y))) continue;
				ink = true;
				clipped &= option.rect.contains(x, y);
			}
		check(ink && clipped, "normal, selected and disabled rows paint within their bounds");
		if (state == normalState) normal = image;
		if (state & QStyle::State_Selected) selected = image;
	}
	QImage native(240, 48, QImage::Format_ARGB32_Premultiplied);
	native.fill(Qt::transparent);
	QPainter nativePainter(&native);
	option.state = normalState | QStyle::State_Selected | QStyle::State_HasFocus;
	QStyledItemDelegate nativeDelegate;
	nativeDelegate.paint(&nativePainter, option, index);
	nativePainter.end();
	check(normal != selected && selected.pixelColor(215, 10) == native.pixelColor(215, 10),
	      "selected rows retain the native highlight background");
	const auto size = delegate.sizeHint(option, index);
	check(size.width() > 0 && size.height() >= option.fontMetrics.height(), "row sizing accommodates bold digits");
	row->setToolTip(QString(500, 'x'));
	QHelpEvent tooltip(QEvent::ToolTip, QPoint(10, 10), list.mapToGlobal(QPoint(10, 10)));
	check(delegate.helpEvent(&tooltip, &list, option, index) && !QToolTip::isVisible(), "even a clipped row cannot open a content tooltip");
	check(row->text().endsWith(QStringLiteral("<b>literal</b> & body")) && !row->font().bold(),
	      "content remains plain text with no bold body role");
	row->setText(PageListDelegate::label(99, 100, QStringLiteral("MMMM projected later text")));
	const auto originalText = row->text();
	row->setData(PageListDelegate::HighlightRole, 4);
	row->setData(PageListDelegate::ProgressRole, QStringLiteral("2/3"));
	QImage reveal(240, 48, QImage::Format_ARGB32_Premultiplied);
	reveal.fill(Qt::transparent);
	QPainter revealPainter(&reveal);
	delegate.paint(&revealPainter, option, index);
	revealPainter.end();
	bool yellow = false, white = false;
	for (int y = 8; y < 40; ++y)
		for (int x = 8; x < 228; ++x) {
			yellow |= reveal.pixelColor(x, y) == QColor(0xFF, 0xFF, 0x00);
			white |= x > 130 && reveal.pixelColor(x, y) == QColor(Qt::white);
		}
	check(yellow && white && reveal.pixelColor(215, 10) == native.pixelColor(215, 10),
	      "projected text uses yellow while unrevealed text and selection retain their colors");
	check(delegate.sizeHint(option, index).height() == size.height() && row->text() == originalText,
	      "line progress preserves one-row sizing and the full-page summary");
	row->setData(PageListDelegate::HighlightRole, {});
	row->setData(PageListDelegate::ProgressRole, {});
	row->setText(PageListDelegate::label(0, 3, {}));
	row->setData(PageListDelegate::ProgressRole, QStringLiteral("1/3"));
	for (bool dark : {false, true}) {
		const QColor ink = dark ? QColor(230, 235, 240) : QColor(17, 34, 51);
		option.palette.setColor(QPalette::Text, ink);
		option.palette.setColor(QPalette::HighlightedText, ink);
		bool readable = true;
		for (const auto state : {normalState, normalState | QStyle::State_Selected,
		                         normalState & ~QStyle::State_Active}) {
			option.state = state;
			QImage image(240, 48, QImage::Format_ARGB32_Premultiplied);
			image.fill(Qt::transparent);
			QPainter painter(&image);
			delegate.paint(&painter, option, index);
			painter.end();
			bool found = false;
			for (int y = 8; y < 40; ++y)
				for (int x = 8; x < 100; ++x) found |= image.pixelColor(x, y) == ink;
			readable &= found;
		}
		check(readable, dark ? "dark themes supply number and progress colors" : "light themes supply number and progress colors");
	}
	for (const auto &text : {QString::fromUtf8(u8"  标题\t甲　乙\n尾部  "), QString(119, 'a') + QString::fromUtf8(u8"😀结尾"),
	                         QString(119, 'a') + "   end", QString(1000, ' ')}) {
		auto expected = text.simplified().left(120);
		if (!expected.isEmpty() && expected.back().isHighSurrogate()) expected.chop(1);
		check(PageListDelegate::summary(text) == expected, "bounded summaries retain native whitespace and Unicode semantics");
	}
	const QString large(4 * 1024 * 1024, 'A');
	check(PageListDelegate::summary(large) == QString(120, 'A') && PageListDelegate::revealEnds(large, true) == QVector<int>{120},
	      "large unbroken pages keep bounded summaries and point caches");
	const auto unicode = QString(119, QChar('a')) + QString::fromUtf8("\xF0\x9F\x98\x80");
	check(PageListDelegate::summary(unicode) == QString(119, QChar('a')),
	      "bounded previews never split a UTF-16 surrogate pair");
	check(PageListDelegate::revealEnds(QString::fromUtf8(u8"标题\r\n甲\t乙\r\n\r\n结尾\n")) == QVector<int>{2, 6, 9},
	      "highlight boundaries preserve Chinese, TAB columns, CRLF and blank spacing");
	check(PageListDelegate::revealEnds(QString(500, 'A') + "\nlast") == QVector<int>{120},
	      "long page summaries need only a bounded highlight cache");
	check(PageListDelegate::revealEnds(QString::fromUtf8(u8"标题\r\n甲\t\t  \t乙\r\n\r\n结尾\t"), true) == QVector<int>{2, 4, 6, 9},
	      "point highlights skip empty TAB cells and preserve the complete summary");
	check(PageListDelegate::revealEnds(QString(500, 'A') + "\tlast", true) == QVector<int>{120},
	      "point highlight caches remain bounded for long text");
	return passed ? 0 : 1;
}
