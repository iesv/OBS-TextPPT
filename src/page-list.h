// OBS-TextPPT, GPL-2.0-or-later.
#pragma once
#include <QApplication>
#include <QHelpEvent>
#include <QPainter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStringView>
#include <QTextLayout>
#include <QVector>
#include <QWidget>
#include <algorithm>
#include <cstddef>

class PageListDelegate final : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;
	enum { LineEndsRole = Qt::UserRole + 1, HighlightRole, ProgressRole, PointEndsRole };

	static QString summary(QStringView text)
	{
		QString value;
		value.reserve(120);
		bool space = false;
		// Bound preview allocation even for a long unbroken page.
		for (const auto ch : text) {
			if (value.size() == 120) break;
			if (ch.isSpace()) {
				space = !value.isEmpty();
				continue;
			}
			if (space) value += ' ';
			if (value.size() < 120) value += ch;
			space = false;
		}
		if (!value.isEmpty() && value.at(value.size() - 1).isHighSurrogate()) value.chop(1);
		return value;
	}

	static QVector<int> revealEnds(const QString &text, bool points = false)
	{
		QVector<int> ends;
		QString prefix;
		// Cache reveal boundaries only within the bounded page summary.
		for (qsizetype start = 0; start < text.size() && prefix.size() < 120;) {
			qsizetype end = start;
			while (end < text.size() && text[end] != '\r' && text[end] != '\n' && (!points || text[end] != '\t')) ++end;
			const auto line = QStringView(text).mid(start, end - start);
			if (std::any_of(line.cbegin(), line.cend(), [](QChar ch) { return ch != ' ' && ch != '\t'; })) {
				const auto part = summary(line);
				if (!part.isEmpty()) {
					if (!prefix.isEmpty()) prefix += ' ';
					prefix += part;
				}
				prefix.truncate(120);
				ends.push_back(static_cast<int>(prefix.size()));
			}
			start = end + 1;
			if (end < text.size() && text[end] == '\r' && start < text.size() && text[start] == '\n') ++start;
		}
		if (ends.isEmpty()) ends.push_back(0);
		return ends;
	}

	static QString label(std::size_t row, std::size_t count, const QString &text)
	{
		const int digits = std::max(2, static_cast<int>(QString::number(static_cast<qulonglong>(count)).size()));
		return QStringLiteral("%1  %2").arg(static_cast<qulonglong>(row + 1), digits, 10, QChar('0')).arg(text);
	}

	void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		QStyleOptionViewItem item(option);
		initStyleOption(&item, index);
		const int separator = static_cast<int>(item.text.indexOf(QStringLiteral("  ")));
		if (separator < 0) {
			QStyledItemDelegate::paint(painter, option, index);
			return;
		}
		const QString number = item.text.left(separator);
		const QString progress = index.data(ProgressRole).toString();
		const QString text = item.text.mid(separator + 2);
		painter->save();
		painter->setClipRect(item.rect, Qt::IntersectClip);
		auto *style = item.widget ? item.widget->style() : QApplication::style();
		const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &item, item.widget);
		// Keep native selection, hover and focus rendering.
		item.text.clear();
		style->drawControl(QStyle::CE_ItemViewItem, &item, painter, item.widget);
		QFont bold(item.font);
		bold.setBold(true);
		const int width = numberWidth(QFontMetrics(bold, painter->device()), separator);
		const int gap = item.fontMetrics.horizontalAdvance(QStringLiteral("  "));
		QRect numberRect(textRect);
		numberRect.setWidth(width);
		QRect bodyRect = textRect.adjusted(width + gap, 0, 0, 0);
		const auto group = !(item.state & QStyle::State_Enabled) ? QPalette::Disabled :
			item.state & QStyle::State_Active ? QPalette::Active : QPalette::Inactive;
		const auto role = item.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text;
		painter->setClipRect(textRect, Qt::IntersectClip);
		painter->setPen(item.palette.color(group, role));
		painter->setFont(bold);
		painter->drawText(numberRect, Qt::AlignRight | Qt::AlignVCenter | Qt::TextSingleLine, number);
		painter->setFont(item.font);
		if (!progress.isEmpty()) {
			const int digits = static_cast<int>(progress.size() - progress.indexOf('/') - 1);
			const int progressWidth = numberWidth(item.fontMetrics, digits * 2) + item.fontMetrics.horizontalAdvance('/');
			QRect progressRect(bodyRect);
			progressRect.setWidth(progressWidth);
			painter->drawText(progressRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, progress);
			bodyRect.adjust(progressWidth + gap, 0, 0, 0);
		}
		if (bodyRect.width() > 0) {
			const auto visible = item.fontMetrics.elidedText(text, item.textElideMode, bodyRect.width());
			QTextLayout layout(visible, item.font, painter->device());
			if (!progress.isEmpty()) {
				QTextLayout::FormatRange range;
				range.start = 0;
				range.length = std::min(index.data(HighlightRole).toInt(), static_cast<int>(visible.size()));
				range.format.setForeground(QColor(0xFF, 0xFF, 0x00));
				layout.setFormats({range});
			}
			layout.beginLayout();
			auto line = layout.createLine();
			if (line.isValid()) line.setLineWidth(bodyRect.width());
			layout.endLayout();
			if (line.isValid())
				layout.draw(painter, QPointF(bodyRect.left(), bodyRect.top() + (bodyRect.height() - line.height()) / 2));
		}
		painter->restore();
	}

	QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		auto size = QStyledItemDelegate::sizeHint(option, index);
		QStyleOptionViewItem item(option);
		initStyleOption(&item, index);
		const int separator = static_cast<int>(item.text.indexOf(QStringLiteral("  ")));
		if (separator >= 0) {
			QFont bold(item.font);
			bold.setBold(true);
			const QFontMetrics metrics(bold, item.widget);
			size.rwidth() += numberWidth(metrics, separator) - item.fontMetrics.horizontalAdvance(item.text.left(separator));
			size.rheight() += std::max(0, metrics.height() - item.fontMetrics.height());
		}
		return size;
	}

	bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
	               const QModelIndex &index) override
	{
		// Page previews never open a tooltip, including clipped rows.
		if (event && event->type() == QEvent::ToolTip)
			return true;
		return QStyledItemDelegate::helpEvent(event, view, option, index);
	}

private:
	static int numberWidth(const QFontMetrics &metrics, int digits)
	{
		int width = 0;
		// Use a fixed column even when the font has proportional digits.
		for (char digit = '0'; digit <= '9'; ++digit)
			width = std::max(width, metrics.horizontalAdvance(QLatin1Char(digit)));
		return width * digits;
	}
};
