#include "text-parser.h"

// Split reveal steps at ASCII separators, preserving UTF-8 characters.
std::vector<size_t> text_reveal_ends(std::string_view text, bool points)
{
	std::vector<size_t> ends;
	for (size_t start = 0; start < text.size();) {
		const auto found = text.find_first_of(points ? "\t\r\n" : "\r\n", start);
		const size_t end = found == std::string_view::npos ? text.size() : found;
		if (text.substr(start, end - start).find_first_not_of(" \t") != std::string_view::npos)
			ends.push_back(end);
		start = end + 1;
		if (end < text.size() && text[end] == '\r' && start < text.size() && text[start] == '\n')
			++start;
	}
	if (ends.empty())
		ends.push_back(text.size());
	else
		ends.back() = text.size();
	return ends;
}

const char *text_error_key(text_error error)
{
	switch (error) {
	case text_error::None: return "";
	case text_error::TooLarge: return "File.TooLarge";
	case text_error::InvalidText: return "File.InvalidText";
	case text_error::PageLimit: return "File.PageLimit";
	}
	return "";
}

bool validate_text(std::string_view input, text_error &error)
{
	error = text_error::None;
	if (input.size() > 4 * 1024 * 1024) {
		error = text_error::TooLarge;
		return false;
	}
	if (input.size() >= 2 &&
	    ((static_cast<unsigned char>(input[0]) == 0xff && static_cast<unsigned char>(input[1]) == 0xfe) ||
	     (static_cast<unsigned char>(input[0]) == 0xfe && static_cast<unsigned char>(input[1]) == 0xff))) {
		error = text_error::InvalidText;
		return false;
	}
	if (input.find('\0') != std::string_view::npos) {
		error = text_error::InvalidText;
		return false;
	}
	// Reject malformed and noncanonical UTF-8.
	for (size_t i = 0; i < input.size();) {
		const auto first = static_cast<unsigned char>(input[i++]);
		if (first < 0x80)
			continue;
		const int length = first >= 0xc2 && first <= 0xdf ? 2 :
			first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
		unsigned value = first & (length == 2 ? 0x1f : length == 3 ? 0x0f : 0x07);
		bool valid = length != 0 && input.size() - i >= static_cast<size_t>(length - 1);
		for (int j = 1; valid && j < length; ++j) {
			const auto next = static_cast<unsigned char>(input[i++]);
			valid = (next & 0xc0) == 0x80;
			value = (value << 6) | (next & 0x3f);
		}
		if (!valid || (length == 3 && value < 0x800) || (length == 4 && value < 0x10000) ||
		    value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
			error = text_error::InvalidText;
			return false;
		}
	}
	return true;
}

bool parse_slides(std::string_view input, std::string_view delimiter,
                  std::vector<std::string> &slides, text_error &error)
{
	slides.clear();
	if (!validate_text(input, error))
		return false;
	if (input.substr(0, 3) == "\xef\xbb\xbf")
		input.remove_prefix(3);

	std::string page;
	bool hasLine = false;
	size_t position = 0;
	while (position < input.size()) {
		const size_t end = input.find_first_of("\r\n", position);
		const size_t length = (end == std::string_view::npos ? input.size() : end) - position;
		const std::string_view line = input.substr(position, length);
		const bool separator = delimiter.empty() ? line.empty() : line == delimiter;
		if (separator) {
			if (!page.empty()) {
				slides.push_back(std::move(page));
				page.clear();
			}
			hasLine = false;
		} else {
			if (hasLine)
				page += '\n';
			page.append(line);
			hasLine = true;
		}
		if (slides.size() > 1000) {
			error = text_error::PageLimit;
			slides.clear();
			return false;
		}
		if (end == std::string_view::npos)
			break;
		position = end + 1;
		if (input[end] == '\r' && position < input.size() && input[position] == '\n')
			++position;
	}
	if (!page.empty())
		slides.push_back(std::move(page));
	if (slides.size() > 1000) {
		error = text_error::PageLimit;
		slides.clear();
		return false;
	}
	return true;
}
