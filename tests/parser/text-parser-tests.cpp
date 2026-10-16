#include "text-parser.h"
#include <cstdlib>
#include <iostream>

int main()
{
	std::vector<std::string> pages;
	text_error error;
	const auto check = [](bool passed, const char *name) {
		if (!passed) {
			std::cerr << "FAIL: " << name << '\n';
			std::exit(1);
		}
	};
	check(parse_slides("\xef\xbb\xbf" "one\r\ntwo\r\n\r\nthree\r\n", "", pages, error) &&
	              pages == std::vector<std::string>{"one\ntwo", "three"}, "BOM / CRLF / multiline");
	check(parse_slides("one\n\nbody\n---\ntwo\n---tail\n", "---", pages, error) &&
	              pages == std::vector<std::string>{"one\n\nbody", "two\n---tail"}, "exact delimiter / blank lines");
	check(parse_slides("one\r\rtwo", "", pages, error) && pages.size() == 2, "CR-only line endings");
	check(parse_slides("left\tright\nshort\tend\n\n\tlast\t", "", pages, error) &&
	              pages == std::vector<std::string>{"left\tright\nshort\tend", "\tlast\t"},
	      "tabs survive page parsing");
	check(parse_slides("", "", pages, error) && pages.empty(), "empty input");
	check(text_reveal_ends("Title\r\nFirst\titem\r\n\r\nLast\n") == std::vector<size_t>{5, 17, 26},
	      "reveal steps preserve CRLF, tabs, blank spacing and trailing lines");
	const std::string unicode = "\xe4\xb8\xad\xe6\x96\x87\n\xf0\x9f\x98\x80";
	check(text_reveal_ends(unicode) == std::vector<size_t>{6, 11}, "reveal steps keep UTF-8 characters intact");
	check(text_reveal_ends("\n \t\nA\n\nB") == std::vector<size_t>{5, 8}, "blank rows do not consume reveal clicks");
	check(text_reveal_ends(" \t\n") == std::vector<size_t>{3} && text_reveal_ends("") == std::vector<size_t>{0},
	      "blank pages have one bounded reveal step");
	check(text_reveal_ends("Title\r\nFirst\titem\r\n\r\nLast\n", true) == std::vector<size_t>{5, 12, 17, 26},
	      "point steps split TAB cells and CRLF rows");
	check(text_reveal_ends("\t A\t\t \tB\r\n\r\nC\t", true) == std::vector<size_t>{3, 8, 14},
	      "empty cells keep their spacing without adding point steps");
	check(text_reveal_ends(unicode + "\t" + unicode, true) == std::vector<size_t>{6, 11, 18, 23},
	      "point steps retain complete Chinese characters and emoji");
	check(text_reveal_ends(" \t\r\n", true) == std::vector<size_t>{4} &&
	      text_reveal_ends("", true) == std::vector<size_t>{0}, "blank point pages have one step");
	check(parse_slides("\nfirst\n---\n\nsecond", "---", pages, error) &&
	              pages == std::vector<std::string>{"\nfirst", "\nsecond"}, "leading blank lines");
	check(parse_slides("\xe4\xb8\xad\xe6\x96\x87\xf0\x9f\x98\x80", "", pages, error), "Chinese / emoji UTF-8");
	check(!parse_slides("\xc0\xaf", "", pages, error), "overlong UTF-8 rejection");
	check(!parse_slides("\xed\xa0\x80", "", pages, error), "surrogate UTF-8 rejection");
	check(!parse_slides("\xf0\x9f", "", pages, error), "truncated UTF-8 rejection");
	check(!parse_slides("\xff\xfe" "a", "", pages, error) && error == text_error::InvalidText, "UTF-16 rejection");
	check(!parse_slides(std::string("a\0b", 3), "", pages, error), "NUL rejection");
	check(!parse_slides(std::string(4 * 1024 * 1024 + 1, 'a'), "", pages, error), "file size bound");
	std::string many;
	for (int i = 0; i < 1001; ++i)
		many += "page\n\n";
	check(validate_text(many, error), "a whole-file page may contain many blank lines");
	check(!parse_slides(many, "", pages, error) && pages.empty(), "page count bound");
	many.resize(many.size() - 6);
	check(parse_slides(many, "", pages, error) && pages.size() == 1000, "exact page count limit");
	std::cout << "Text parser checks passed\n";
}
