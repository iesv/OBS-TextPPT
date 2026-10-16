#pragma once
#include <string>
#include <string_view>
#include <vector>

enum class text_error { None, TooLarge, InvalidText, PageLimit };
const char *text_error_key(text_error error);

// Validate bounded UTF-8 text before parsing or displaying a file.
bool validate_text(std::string_view input, text_error &error);

// An empty delimiter splits on blank lines. Limits: 4 MiB, 1000 pages, UTF-8.
bool parse_slides(std::string_view input, std::string_view delimiter,
                  std::vector<std::string> &slides, text_error &error);

// Reveal nonblank lines or TAB cells without splitting UTF-8.
std::vector<size_t> text_reveal_ends(std::string_view text, bool points = false);
