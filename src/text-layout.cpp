#include "text-layout.h"
#include <QFontMetricsF>
#include <algorithm>
#include <array>
#include <cmath>

QString expand_text_tabs(const QString &text, const QFont &font, std::vector<int> *tabPadding)
{
	if (tabPadding) tabPadding->clear();
	if (!text.contains('\t'))
		return text;
	const auto fallback = [&] {
		if (tabPadding) tabPadding->clear();
		return QString(text).replace('\t', QLatin1Char(' '));
	};
	// Avoid shaping large pasted pages or multiplying their whitespace.
	if (text.size() > 64 * 1024)
		return fallback();
	const QFontMetricsF metrics(font);
	const qreal space = metrics.horizontalAdvance(QStringLiteral(" "));
	if (space <= 0)
		return fallback();

	std::array<qreal, 64> widths{};
	qsizetype start = 0;
	size_t column = 0;
	// Only cells followed by a tab determine the next column's position.
	for (qsizetype i = 0; i <= text.size(); ++i) {
		const QChar ch = i < text.size() ? text[i] : QChar();
		if (i < text.size() && ch != '\t' && ch != '\n' && ch != '\r')
			continue;
		if (ch == '\t') {
			if (column < widths.size())
				widths[column] = std::max(widths[column], metrics.horizontalAdvance(text.mid(start, i - start)));
			++column;
		} else {
			column = 0;
		}
		start = i + 1;
	}

	QString output;
	output.reserve(text.size());
	start = 0;
	column = 0;
	qsizetype added = 0;
	for (qsizetype i = 0; i <= text.size(); ++i) {
		const QChar ch = i < text.size() ? text[i] : QChar();
		if (i < text.size() && ch != '\t' && ch != '\n' && ch != '\r')
			continue;
		const auto cell = text.mid(start, i - start);
		output += cell;
		if (ch == '\t') {
			const int padding = column < widths.size() ? static_cast<int>(std::clamp(
				std::round((widths[column] - metrics.horizontalAdvance(cell)) / space) + 4.0, 4.0, 256.0)) : 4;
			added += padding - 1;
			// Bound added whitespace independently of the original page size.
			if (added > 64 * 1024)
				return fallback();
			output += QString(padding, QLatin1Char(' '));
			if (tabPadding) tabPadding->push_back(padding);
			++column;
		} else {
			if (i < text.size())
				output += ch;
			column = 0;
		}
		start = i + 1;
	}
	return output;
}
