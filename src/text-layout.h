#pragma once
#include <QString>
#include <vector>

class QFont;

// Align tab-separated cells approximately, using the complete page.
QString expand_text_tabs(const QString &text, const QFont &font, std::vector<int> *tabPadding = nullptr);
