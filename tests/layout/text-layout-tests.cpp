#include "text-layout.h"
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QFont>
#include <QFontMetricsF>
#include <cmath>
#include <iostream>

int main(int argc, char **argv)
{
	std::cout << "Initialize Qt font metrics" << std::endl;
	QGuiApplication app(argc, argv);
	bool passed = true;
	const auto check = [&](bool valid, const char *message) {
		passed &= valid;
		std::cout << (valid ? "PASS: " : "FAIL: ") << message << std::endl;
	};
#ifdef _WIN32
	QFont font(QStringLiteral("Microsoft YaHei"));
#else
	QFont font(QStringLiteral("PingFang SC"));
#endif
	const auto table = QStringLiteral("A\tB\tC\r\nLonger\tD\tE");
	for (int size : {16, 24, 48, 96, 192}) {
		for (bool bold : {false, true}) {
			font.setPixelSize(size);
			font.setBold(bold);
			const QFontMetricsF metrics(font);
			const auto rows = expand_text_tabs(table, font).split('\n');
			const qreal space = metrics.horizontalAdvance(QStringLiteral(" "));
			const qreal first = metrics.horizontalAdvance(rows[0].left(rows[0].indexOf('B')));
			const qreal second = metrics.horizontalAdvance(rows[1].left(rows[1].indexOf('D')));
			const qreal third = metrics.horizontalAdvance(rows[0].left(rows[0].indexOf('C')));
			const qreal fourth = metrics.horizontalAdvance(rows[1].left(rows[1].indexOf('E')));
			check(space > 0 && std::abs(first - second) <= space + 1 && std::abs(third - fourth) <= space * 2 + 1,
			      "columns retain space-level alignment across font sizes and weights");
			check(!rows[0].contains('\t') && rows[0].endsWith('\r') && table.contains('\t'),
			      "line endings and original tabs remain intact");
		}
	}
	font.setPixelSize(48);
	font.setBold(false);
	const auto chinese = QStringLiteral("\u6807\u9898\n\u6069\u5178\t\u667a\u6167\n\u751f\u547d\u6709\u9650\t\u76fc\u671b");
	auto output = expand_text_tabs(chinese, font);
	check(!output.contains('\t') && output.remove(' ') == QString(chinese).remove('\t'),
	      "Chinese cells and untabbed titles retain their content");
	check(expand_text_tabs(QStringLiteral("\tA\t\n\tA\t"), font) == QStringLiteral("    A    \n    A    "),
	      "leading and trailing empty cells keep their columns");
	check(expand_text_tabs(QString(300, '\t') + QStringLiteral("end"), font) == QString(1200, ' ') + QStringLiteral("end"),
	      "many empty columns use bounded storage");
	QString large = QString(2000, 'W') + QStringLiteral("\tx\n");
	for (int i = 0; i < 17000; ++i)
		large += QStringLiteral("\tx\n");
	check(expand_text_tabs(large, font) == QString(large).replace('\t', QLatin1Char(' ')),
	      "excessive padding falls back without amplifying the input");
	large = QString(64 * 1024 + 1, '\t');
	check(expand_text_tabs(large, font) == QString(large.size(), ' '),
	      "large pages skip font measurement and retain their character bound");
	check(expand_text_tabs(QStringLiteral("plain text"), font) == QStringLiteral("plain text"),
	      "ordinary text remains unchanged");
	std::vector<int> padding;
	const auto mapped = expand_text_tabs(QStringLiteral("A\t\tB\nLonger\tC"), font, &padding);
	check(padding.size() == 3 && mapped.size() == 10 + padding[0] + padding[1] + padding[2],
	      "TAB padding maps original point boundaries to the full-page layout");
	expand_text_tabs(large, font, &padding);
	check(padding.empty(), "bounded fallback leaves point offsets unchanged");
	QElapsedTimer timer;
	timer.start();
	bool valid = true;
	for (int i = 0; i < 10000; ++i) {
		const auto input = chinese + QString::number(i);
		const auto result = expand_text_tabs(input, font);
		valid &= !result.contains('\t') && result.endsWith(QString::number(i)) && result.size() <= input.size() + 64 * 1024;
	}
	check(valid, "10000 tab layouts retain content and bounded padding");
	std::cout << "10000 layouts ms: " << timer.elapsed() << '\n';
	return passed ? 0 : 1;
}
