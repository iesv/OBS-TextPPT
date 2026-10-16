// OBS-TextPPT, GPL-2.0-or-later. Isolated runtime checks.
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <graphics/vec4.h>
#include <QEventLoop>
#include <QApplication>
#include <QTimer>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTextStream>
#include <QComboBox>
#include <QCheckBox>
#include <QToolButton>
#include <QPushButton>
#include <QFontDatabase>
#include <QElapsedTimer>
#include <QLayout>
#include <QListWidget>
#include <QLineEdit>
#include <QPixmap>
#include <QStyleOptionButton>
#include <QMouseEvent>
#include <QMenu>
#include <QAction>
#include <QKeyEvent>
#include <atomic>
#include <functional>
#include "page-list.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_set>

uint64_t textppt_test_memory();
bool textppt_header_probe(QWidget *, const QString &);

static void wait(int milliseconds = 180)
{
	QEventLoop loop;
	QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
	loop.exec();
	// Match the deferred cleanup between real user events.
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

static obs_source_t *active_text(obs_source_t *source)
{
	obs_source_t *transition = nullptr;
	obs_source_enum_active_sources(source, [](obs_source_t *, obs_source_t *child, void *data) {
		*static_cast<obs_source_t **>(data) = obs_source_get_ref(child);
	}, &transition);
	auto *text = transition ? obs_transition_get_active_source(transition) : nullptr;
	obs_source_release(transition);
	return text;
}

static std::string displayed(obs_source_t *source)
{
	auto *text = active_text(source);
	auto *settings = text ? obs_source_get_settings(text) : nullptr;
	const std::string value = settings ? obs_data_get_string(settings, "text") : "";
	obs_data_release(settings);
	obs_source_release(text);
	return value;
}

static int page(obs_source_t *source)
{
	calldata_t data = {};
	proc_handler_call(obs_source_get_proc_handler(source), "get_status", &data);
	const auto index = static_cast<int>(calldata_int(&data, "index"));
	calldata_free(&data);
	return index;
}

static bool ready(obs_source_t *source)
{
	calldata_t data = {};
	proc_handler_call(obs_source_get_proc_handler(source), "get_status", &data);
	const bool value = calldata_bool(&data, "ready");
	calldata_free(&data);
	return value;
}

static int wait_text(obs_source_t *source, int index, const std::string &text)
{
	QElapsedTimer timer;
	timer.start();
	while (timer.elapsed() < 1000) {
		if (ready(source) && page(source) == index && displayed(source) == text) return static_cast<int>(timer.elapsed());
		wait(20);
	}
	return -1;
}

static QImage capture(obs_source_t *source, const QSize &canvas = {})
{
	const auto width = canvas.isEmpty() ? obs_source_get_width(source) : static_cast<uint32_t>(canvas.width());
	const auto height = canvas.isEmpty() ? obs_source_get_height(source) : static_cast<uint32_t>(canvas.height());
	if (!width || !height || width > 4096 || height > 4096)
		return {};
	QImage image;
	obs_enter_graphics();
	auto *target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	auto *stage = gs_stagesurface_create(width, height, GS_RGBA);
	if (target && stage && gs_texrender_begin(target, width, height)) {
		struct vec4 clear = {};
		gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
		gs_ortho(0, static_cast<float>(width), 0, static_cast<float>(height), -100, 100);
		gs_matrix_push();
		gs_matrix_identity();
		obs_source_video_render(source);
		gs_matrix_pop();
		gs_texrender_end(target);
		gs_stage_texture(stage, gs_texrender_get_texture(target));
		uint8_t *pixels = nullptr;
		uint32_t stride = 0;
		if (gs_stagesurface_map(stage, &pixels, &stride)) {
			image = QImage(pixels, width, height, stride, QImage::Format_RGBA8888).copy();
			gs_stagesurface_unmap(stage);
		}
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(target);
	obs_leave_graphics();
	return image;
}

static bool retains_pixels(const QImage &before, const QImage &after)
{
	if (before.isNull() || before.size() != after.size())
		return false;
	size_t ink = 0;
	for (int y = 0; y < before.height(); ++y)
		for (int x = 0; x < before.width(); ++x) {
			const auto pixel = before.pixel(x, y);
			if (qAlpha(pixel)) {
				++ink;
				if (pixel != after.pixel(x, y))
					return false;
			}
		}
	return ink > 20;
}

static bool native_pixels_match(const QImage &image, const QImage &reference)
{
	if (image.isNull() || image.size() != reference.size()) return false;
#ifdef _WIN32
	return image == reference;
#else
	// Metal atlas sampling can round a color channel by one level.
	for (int row = 0; row < image.height(); ++row)
		for (int byte = 0; byte < image.width() * 4; ++byte)
			if (std::abs(int(image.constScanLine(row)[byte]) - int(reference.constScanLine(row)[byte])) > 1)
				return false;
	return true;
#endif
}

static bool dock_highlight(QWidget *dock, const QString &projected, const QString &progress)
{
	auto *pages = dock->findChild<QListWidget *>();
	for (int attempt = 0; attempt < 20; ++attempt) {
		auto *item = pages ? pages->currentItem() : nullptr;
		if (item && item->data(PageListDelegate::ProgressRole).toString() == progress) {
			const auto body = item->text().mid(item->text().indexOf("  ") + 2);
			if (progress.isEmpty() ? !item->data(PageListDelegate::HighlightRole).isValid() :
			    body.left(item->data(PageListDelegate::HighlightRole).toInt()) == PageListDelegate::summary(projected))
				return true;
		}
		wait(50);
	}
	return false;
}

static bool covers_ink(const QImage &a, const QImage &b, size_t &ink)
{
	// Native centered extents can shift antialiasing by one raster pixel.
	for (int y = 0; y < a.height(); ++y)
		for (int x = 0; x < a.width(); ++x) {
			if (qAlpha(a.pixel(x, y)) <= 16) continue;
			++ink;
			bool found = false;
			for (int dy = -1; dy <= 1 && !found; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
					if (b.rect().contains(x + dx, y + dy) && qAlpha(b.pixel(x + dx, y + dy)) > 16) {
						found = true; break;
					}
			if (!found) return false;
		}
	return true;
}

static bool matches_layout(const QImage &before, const QImage &after)
{
	if (before.isNull() || before.size() != after.size()) return false;
	size_t first = 0, second = 0;
	return covers_ink(before, after, first) && covers_ink(after, before, second) && first > 20 &&
		std::max(first, second) - std::min(first, second) <= std::max(first, second) / 50;
}

bool textppt_progressive_probe(obs_scene_t *scene, const char *kind, const QString &path, QWidget *dock)
{
	QFile report(path);
	if (!report.open(QIODevice::WriteOnly | QIODevice::Text))
		return false;
	bool passed = true;
	const auto check = [&](bool valid, const char *message) {
		passed &= valid;
		QTextStream stream(&report);
		stream << (valid ? "PASS: " : "FAIL: ") << message << '\n';
		stream.flush();
		return valid;
	};
	if (!qEnvironmentVariableIsEmpty("TEXTPPT_CJK_ONLY")) {
		const auto fixture = qEnvironmentVariable("TEXTPPT_CJK_FIXTURE");
		if (!fixture.isEmpty()) {
			QFile input(fixture);
			if (!check(input.open(QIODevice::ReadOnly), "saved Chinese source settings are readable")) return false;
			auto *settings = obs_data_create_from_json(input.readAll().constData());
			obs_data_set_string(settings, "transition", "cut");
			obs_data_set_bool(settings, "progressive", true);
			auto *source = obs_source_create(kind, "TextPPT saved Chinese source", settings, nullptr);
			obs_data_release(settings);
			auto *item = source ? obs_scene_add(scene, source) : nullptr;
			if (!check(item != nullptr, "saved Chinese source is created")) {
				obs_source_remove(source); obs_source_release(source); return false;
			}
			wait(500);
			auto *selector = dock->findChild<QComboBox *>("TextPPTSourceList");
			auto *option = dock->findChild<QCheckBox *>("TextPPTProgressive");
			if (!check(selector && option, "saved Chinese source has Dock controls")) {
				obs_sceneitem_remove(item); obs_source_remove(source); obs_source_release(source); return false;
			}
			selector->setCurrentIndex(selector->findData(QString::fromUtf8(obs_source_get_uuid(source)))); wait(300);
			const auto jump = [&](int index) {
				calldata_t data = {};
				calldata_set_int(&data, "index", index);
				proc_handler_call(obs_source_get_proc_handler(source), "dock_transition", &data);
				calldata_free(&data); wait(350);
			};
			for (int index = 0; index < 5; ++index) {
				jump(index);
				const auto stem = path + ".page" + QString::number(index + 1);
				const auto title = displayed(source);
				const auto first = capture(source);
				first.save(stem + ".first.png");
				auto *native = active_text(source);
				auto *nativeSettings = obs_source_get_settings(native);
				QTextStream(&report) << "CASE: page " << index + 1 << ", source " << obs_source_get_width(source)
					<< 'x' << obs_source_get_height(source) << ", native " << obs_source_get_base_width(native)
					<< 'x' << obs_source_get_base_height(native) << ", extents " << obs_data_get_int(nativeSettings, "extents_cx")
					<< 'x' << obs_data_get_int(nativeSettings, "extents_cy") << '\n';
				// Compare the first revealed title with the native text renderer.
				auto *referenceSettings = obs_data_get_defaults(nativeSettings);
				obs_data_apply(referenceSettings, nativeSettings);
				obs_data_set_bool(referenceSettings, "extents", false);
				auto *reference = obs_source_create_private(obs_source_get_id(native), "Chinese title reference", referenceSettings);
				obs_data_release(referenceSettings);
				obs_data_release(nativeSettings); obs_source_release(native); wait(200);
				const auto referenceImage = capture(reference);
				referenceImage.save(stem + ".reference.png");
				obs_source_release(reference);
				const auto ink = [](const QImage &image) {
					QRect bounds;
					for (int y = 0; y < image.height(); ++y)
						for (int x = 0; x < image.width(); ++x)
							if (qAlpha(image.pixel(x, y))) bounds |= QRect(x, y, 1, 1);
					return bounds;
				};
				const auto firstInk = ink(first), referenceInk = ink(referenceImage);
				check(!firstInk.isEmpty() && !referenceInk.isEmpty() &&
					std::abs(firstInk.width() - referenceInk.width()) <= 1 &&
					std::abs(firstInk.height() - referenceInk.height()) <= 1,
					"large Chinese title matches native ink bounds within one raster pixel");
				obs_source_media_next(source); wait(250);
				const auto second = capture(source);
				check(retains_pixels(first, second), "large Chinese title stays fixed when revealing the second line");
				obs_source_media_next(source); wait(250);
				const auto full = capture(source);
				full.save(stem + ".revealed.png");
				check(retains_pixels(second, full), "large Chinese lines retain their pixels");
				check(displayed(source).find(title) == 0 && displayed(source).find('\t') == std::string::npos,
					"large Chinese text and tab expansion remain complete");
				option->setChecked(false); wait(350);
				native = active_text(source);
				nativeSettings = obs_source_get_settings(native);
				check(!obs_data_get_bool(nativeSettings, "extents"), "disabling reveal restores native automatic text bounds");
				obs_data_release(nativeSettings); obs_source_release(native);
				const auto whole = capture(source);
				whole.save(stem + ".ordinary.png");
				check(matches_layout(whole, full), "large Chinese page matches ordinary layout within one raster pixel");
				option->setChecked(true); wait(350);
			}
			obs_sceneitem_remove(item); obs_source_remove(source); obs_source_release(source);
			check(passed, "SAVED CHINESE TESTS COMPLETE");
			return passed;
		}
		const std::vector<std::string> lines = {
			u8"一、认识人生的有限",
			u8"1. 认识上帝永恒不变  2. 认识人生短暂有限",
			u8"3. 认识生命脆弱无常  4. 认识自己需要智慧",
			u8"二、思想生命的价值",
			u8"1. 珍惜光阴不浪费  2. 把握机会殷勤事主",
			u8"3. 善用恩赐忠心服事  4. 追求标竿同展灵值"
		};
		const auto firstPage = lines[0] + '\n' + lines[1] + '\n' + lines[2];
		const auto secondPage = lines[3] + '\n' + lines[4] + '\n' + lines[5];
		const auto content = firstPage + "\n\n" + secondPage;
		check(QString::fromUtf8(lines[0]).startsWith(QChar(0x4e00)), "Chinese fixture contains correctly encoded Chinese characters");
		auto *settings = obs_data_create();
		obs_data_set_string(settings, "custom_text", content.c_str());
		obs_data_set_string(settings, "transition", "cut");
		auto *source = obs_source_create(kind, "TextPPT Chinese reveal probe", settings, nullptr);
		obs_data_release(settings);
		auto *item = source ? obs_scene_add(scene, source) : nullptr;
		if (!check(item != nullptr, "Chinese source is created")) {
			obs_source_remove(source); obs_source_release(source);
			return false;
		}
		wait(400);
		auto *selector = dock->findChild<QComboBox *>("TextPPTSourceList");
		auto *option = dock->findChild<QCheckBox *>("TextPPTProgressive");
		auto *next = dock->findChild<QToolButton *>("TextPPTNextPage");
		auto *previous = dock->findChild<QToolButton *>("TextPPTPreviousPage");
		if (!check(selector && option && next && previous, "Chinese source has Dock controls")) {
			obs_sceneitem_remove(item); obs_source_remove(source); obs_source_release(source);
			return false;
		}
		selector->setCurrentIndex(selector->findData(QString::fromUtf8(obs_source_get_uuid(source))));
		wait(300);
		const auto setFlag = [&](bool value) { option->setChecked(value); wait(300); };
		const auto setStyle = [&](const char *family, int size, const char *align) {
			auto *changes = obs_data_create();
			auto *font = obs_data_create();
			obs_data_set_string(font, "face", family);
			obs_data_set_string(font, "style", "Regular");
			obs_data_set_int(font, "size", size);
			obs_data_set_obj(changes, "font", font);
			obs_data_release(font);
			obs_data_set_string(changes, "align", align);
			obs_source_update(source, changes);
			obs_data_release(changes);
			wait(300);
		};
#ifdef _WIN32
		std::vector<const char *> families = {"Microsoft YaHei", "SimSun", "SimHei"};
		const char *alignments[] = {"left", "center"};
#else
		std::vector<const char *> families = {"Helvetica", "Songti SC"};
		const char *alignments[] = {"left"};
		// Compare unavailable system fonts against a fresh native source.
		auto *fresh = obs_source_get_settings(source);
		auto *freshFont = obs_data_create();
		obs_data_set_string(freshFont, "face", "PingFang SC");
		obs_data_set_string(freshFont, "style", "Regular");
		obs_data_set_int(freshFont, "size", 24);
		obs_data_set_obj(fresh, "font", freshFont);
		obs_data_set_bool(fresh, "progressive", false);
		obs_data_set_string(fresh, "text", firstPage.c_str());
		auto *nativeCheck = obs_source_create_private("text_ft2_source", "PingFang native reference", fresh);
		auto *fontCopy = obs_source_create_private(kind, "PingFang plugin reference", fresh);
		auto *fontCopyItem = obs_scene_add(scene, fontCopy); wait(350);
		auto *fontCopyNative = active_text(fontCopy);
		check(fontCopyNative && capture(fontCopyNative) == capture(nativeCheck), "fresh PingFang output matches native FreeType");
		if (obs_source_get_base_height(nativeCheck) >= 24)
			families.push_back("PingFang SC");
		else
			QTextStream(&report) << "NATIVE FONT UNAVAILABLE: PingFang SC\n";
		obs_source_release(fontCopyNative); obs_sceneitem_remove(fontCopyItem); obs_source_release(fontCopy);
		obs_data_release(freshFont); obs_data_release(fresh); obs_source_release(nativeCheck);
#endif
		for (const char *family : families) {
			check(QFontDatabase::families().contains(QString::fromUtf8(family), Qt::CaseInsensitive),
			      (std::string("Chinese font is installed: ") + family).c_str());
			for (int size : {24, 48, 72}) {
				for (const char *align : alignments) {
					QTextStream(&report) << "CASE: " << family << ' ' << size << ' ' << align << '\n';
					setFlag(false);
					obs_source_media_restart(source); wait();
					setStyle(family, size, align);
					const auto whole = capture(source);
					auto *native = active_text(source);
					auto *nativeSettings = obs_source_get_settings(native);
					auto *referenceSettings = obs_data_get_defaults(nativeSettings);
					obs_data_apply(referenceSettings, nativeSettings);
					auto *reference = obs_source_create_private(obs_source_get_id(native), "Chinese page reference", referenceSettings);
					obs_data_release(referenceSettings); obs_data_release(nativeSettings); wait(200);
					const auto nativeImage = capture(native), referenceImage = capture(reference);
					check(!nativeImage.isNull() && nativeImage == referenceImage,
					      "Chinese font output matches the native text source pixel for pixel");
					if (size == 48 && strcmp(align, "left") == 0) {
						const auto sample = path + '.' + QString::fromUtf8(family).remove(' ');
						nativeImage.save(sample + ".native.png");
						whole.save(sample + ".page.png");
					}
					obs_source_release(reference); obs_source_release(native);
					setFlag(true);
					calldata_t status = {};
					proc_handler_call(obs_source_get_proc_handler(source), "get_status", &status);
					check(calldata_int(&status, "count") == 2 && calldata_int(&status, "steps") == 3,
					      "blank lines produce two pages with three reveal steps");
					calldata_free(&status);
					check(displayed(source) == lines[0], "Chinese first line is complete UTF-8");
					const auto first = capture(source);
					next->click(); wait();
					const auto second = capture(source);
					check(displayed(source) == lines[0] + '\n' + lines[1], "Dock reveals a complete Chinese second line");
					check(retains_pixels(first, second), "Chinese first-line pixels stay fixed");
					next->click(); wait();
					const auto full = capture(source);
					check(displayed(source) == firstPage && retains_pixels(second, full),
					      "all Chinese lines remain complete and in place");
					const bool same = matches_layout(whole, full);
					check(same, "fully revealed Chinese layout matches ordinary output within one raster pixel");
					if (!same) {
						const auto stem = path + '.' + QString::fromUtf8(family).remove(' ') + '.' +
							QString::number(size) + '.' + QString::fromUtf8(align);
						whole.save(stem + ".ordinary.png"); full.save(stem + ".revealed.png");
					}
					next->click(); wait();
					check(page(source) == 1 && displayed(source) == lines[3], "page two starts with its Chinese title");
					previous->click(); wait();
					check(page(source) == 0 && displayed(source) == firstPage, "returning restores all Chinese lines on page one");
					previous->click(); wait(); previous->click(); wait();
					check(displayed(source) == lines[0], "Dock retracts Chinese lines without changing their text");
				}
			}
		}
		const auto beforeFontEdits = textppt_test_memory();
		std::unordered_set<std::string> renderers;
		QElapsedTimer fontTimer;
		fontTimer.start();
		for (int change = 0; change < 120; ++change) {
			auto *changes = obs_data_create();
			auto *font = obs_data_create();
			obs_data_set_string(font, "face", families[change % families.size()]);
			obs_data_set_string(font, "style", "Regular");
			obs_data_set_int(font, "size", 24 * (change % 3 + 1));
			obs_data_set_int(font, "flags", change % 2 ? OBS_FONT_BOLD : 0);
			obs_data_set_obj(changes, "font", font);
			obs_data_release(font);
			obs_source_update(source, changes); obs_data_release(changes);
			obs_source_media_restart(source); wait(90);
			if (auto *native = active_text(source)) {
				renderers.emplace(obs_source_get_uuid(native));
				obs_source_release(native);
			}
		}
		wait(600);
		check(displayed(source) == lines[0] && page(source) == 0,
		      "120 Chinese font edits retain the first reveal line and page");
		check(!renderers.empty() && renderers.size() <= 2, "Chinese font edits reuse at most two native renderers");
		const auto editedFirst = capture(source);
		auto *editedNative = active_text(source);
		auto *editedSettings = obs_source_get_settings(editedNative);
		auto *editedReferenceSettings = obs_data_get_defaults(editedSettings);
		obs_data_apply(editedReferenceSettings, editedSettings);
#ifndef _WIN32
		// Reserve the same full-page glyph metrics as the reveal renderer.
		obs_data_set_string(editedReferenceSettings, "text", firstPage.c_str());
#endif
		auto *editedReference = obs_source_create_private(obs_source_get_id(editedNative), "Edited font reference", editedReferenceSettings);
		obs_data_release(editedReferenceSettings); wait(200);
		obs_source_update(editedReference, editedSettings); obs_data_release(editedSettings); wait(200);
		const auto editedNativeFirst = capture(editedReference, editedFirst.size());
		editedNativeFirst.save(path + ".font-native-first.png");
		check(native_pixels_match(capture(editedNative), editedNativeFirst), "edited first-line rendering matches the native source");
		obs_source_release(editedNative);
		obs_source_media_next(source); wait();
		const auto editedSecond = capture(source);
		editedNative = active_text(source);
		editedSettings = obs_source_get_settings(editedNative);
		obs_source_update(editedReference, editedSettings); obs_data_release(editedSettings); wait(200);
		const auto editedNativeSecond = capture(editedReference, editedSecond.size());
		editedNativeSecond.save(path + ".font-native-second.png");
		check(native_pixels_match(capture(editedNative), editedNativeSecond), "edited second-line rendering matches the native source");
		obs_source_release(editedNative); obs_source_release(editedReference);
		editedFirst.save(path + ".font-first.png");
		editedSecond.save(path + ".font-second.png");
		QTextStream(&report) << "Edited reveal size: " << editedFirst.width() << 'x' << editedFirst.height()
			<< " -> " << editedSecond.width() << 'x' << editedSecond.height() << '\n';
		size_t editedInk = 0;
		check(!editedFirst.isNull() && editedFirst.size() == editedSecond.size() &&
		      covers_ink(editedFirst, editedSecond, editedInk) && editedInk > 20,
		      "Chinese line layout stays within one raster pixel after repeated font edits");
		const auto afterFontEdits = textppt_test_memory();
		check(beforeFontEdits && afterFontEdits && afterFontEdits <= beforeFontEdits + 128 * 1024 * 1024ull,
		      "Chinese reveal font edits stay within the memory growth limit");
		QTextStream(&report) << "120 reveal font edits ms: " << fontTimer.elapsed() << '\n'
			<< "Reveal font-edit delta MiB: " << (double(afterFontEdits) - double(beforeFontEdits)) / 1048576.0 << '\n';
		setFlag(false); obs_source_media_restart(source); wait();
		setStyle(families[1], 36, "left");
		setFlag(true);
		bool rapid = true;
		int slowest = 0;
		for (int repeat = 0; repeat < 100; ++repeat) {
			for (int step = 0; step < 5; ++step) obs_source_media_next(source);
			const int forward = wait_text(source, 1, secondPage);
			rapid &= forward >= 0;
			for (int step = 0; step < 5; ++step) obs_source_media_previous(source);
			const int backward = wait_text(source, 0, lines[0]);
			rapid &= backward >= 0;
			slowest = std::max({slowest, forward, backward});
		}
		QTextStream(&report) << "Slowest Chinese reveal batch ms: " << slowest << '\n';
		check(rapid, "1000 rapid Chinese reveal commands preserve lines and page boundaries");
		const auto mixed = std::string(u8"繁體中文 ABC 123，標點測試！\n短\t第一欄中文\n較長的文字\t第二欄漢字");
		settings = obs_data_create();
		obs_data_set_string(settings, "custom_text", mixed.c_str());
		obs_source_update(source, settings); obs_data_release(settings); wait(300);
		const auto mixedFirst = capture(source);
		check(dock_highlight(dock, QString::fromUtf8(u8"繁體中文 ABC 123，標點測試！"), "1/3"),
		      "Dock highlights projected Chinese, Latin text and punctuation");
		obs_source_media_next(source); wait();
		const auto mixedSecond = displayed(source);
		const auto mixedImage = capture(source);
		check(dock_highlight(dock, QString::fromUtf8(u8"繁體中文 ABC 123，標點測試！ 短 第一欄中文"), "2/3"), "Dock highlights both projected Chinese TAB rows");
		dock->grab().save(path + ".chinese-highlight.png");
		obs_source_media_next(source); wait();
		check(mixedSecond.find(u8"第一欄中文") != std::string::npos &&
		      displayed(source).find(u8"第二欄漢字") != std::string::npos &&
		      displayed(source).find('\t') == std::string::npos &&
		      retains_pixels(mixedFirst, mixedImage) && retains_pixels(mixedImage, capture(source)),
		      "traditional Chinese, Latin text, punctuation and tabs reveal without moving earlier text");
		capture(source).save(path + ".mixed.png");
		const auto mixedFull = displayed(source);
		auto *copy = obs_source_duplicate(source, "Chinese reveal copy", true);
		auto *copyItem = copy ? obs_scene_add(scene, copy) : nullptr;
		if (check(copyItem != nullptr, "saved Chinese reveal settings create a fresh source")) {
			wait(300);
			check(displayed(copy) == mixedFull, "Chinese copies inherit all currently revealed lines");
			obs_source_media_restart(copy); wait();
			const auto copyFirst = capture(copy);
			obs_source_media_next(copy); wait();
			const auto copySecond = capture(copy);
			obs_source_media_next(copy); wait();
			check(retains_pixels(copyFirst, copySecond) && retains_pixels(copySecond, capture(copy)),
			      "fresh Chinese sources retain existing pixels while revealing lines");
			check(displayed(copy) == mixedFull && displayed(source) == mixedFull,
			      "fresh Chinese copies reveal all text with independent progress");
			obs_sceneitem_remove(copyItem);
		}
		obs_source_release(copy);
		obs_sceneitem_remove(item); obs_source_remove(source); obs_source_release(source);
		check(passed, "CHINESE TESTS COMPLETE");
		return passed;
	}
	const char *deck = "Title\nA\tFirst\nLonger\tSecond\n\nNext\nLast";
	auto *settings = obs_data_create();
	obs_data_set_string(settings, "custom_text", deck);
	obs_data_set_string(settings, "transition", "cut");
	obs_data_set_bool(settings, "progressive", true);
	auto *font = obs_data_create();
#ifdef _WIN32
	obs_data_set_string(font, "face", "Arial");
#else
	obs_data_set_string(font, "face", "Helvetica");
#endif
	obs_data_set_string(font, "style", "Regular");
	obs_data_set_int(font, "size", 36);
	obs_data_set_obj(settings, "font", font);
	obs_data_release(font);
	auto *source = obs_source_create(kind, "TextPPT progressive probe", settings, nullptr);
	obs_data_release(settings);
	auto *item = source ? obs_scene_add(scene, source) : nullptr;
	if (!item) {
		obs_source_remove(source);
		obs_source_release(source);
		return false;
	}
	wait(400);
	auto *selector = dock->findChild<QComboBox *>("TextPPTSourceList");
	auto *progressive = dock->findChild<QCheckBox *>("TextPPTProgressive");
	auto *follow = dock->findChild<QToolButton *>("TextPPTFollowScene");
	auto *points = dock->findChild<QCheckBox *>("TextPPTPoints");
	auto *lock = dock->findChild<QToolButton *>("TextPPTSourceLock");
	auto *next = dock->findChild<QToolButton *>("TextPPTNextPage");
	auto *previous = dock->findChild<QToolButton *>("TextPPTPreviousPage");
	if (!check(selector && progressive && points && follow && lock && next && previous, "Dock reveal controls are available")) {
		obs_sceneitem_remove(item); obs_source_remove(source); obs_source_release(source);
		return false;
	}
	const auto previousSource = selector->currentData();
	const int selected = selector->findData(QString::fromUtf8(obs_source_get_uuid(source)));
	selector->setCurrentIndex(selected); wait(300);
	check(selected >= 0 && progressive->isChecked() && progressive->isEnabled(), "Dock reflects the selected source's saved reveal option");
	auto *pages = dock->findChild<QListWidget *>();
	auto *firstItem = pages ? pages->item(0) : nullptr;
	const auto firstSummary = firstItem ? firstItem->text() : QString();
	check(dock_highlight(dock, "Title", "1/3"), "Dock highlights the first projected line and progress");
	auto *header = dock->layout()->itemAt(0)->layout();
	check(header && header->indexOf(progressive) == header->indexOf(selector) + 1 &&
	      header->indexOf(points) == header->indexOf(progressive) + 1 &&
	      header->indexOf(follow) == header->indexOf(points) + 1 &&
	      header->indexOf(lock) == header->indexOf(follow) + 1 &&
	      progressive->geometry().center().x() > selector->geometry().center().x() &&
	      progressive->geometry().center().x() < points->geometry().center().x() &&
	      follow->geometry().center().x() < lock->geometry().center().x() &&
	      progressive->y() <= follow->geometry().bottom() && follow->y() <= progressive->geometry().bottom(),
	      "Dock header orders lines, points, scene link and source lock");
	check(textppt_header_probe(dock, path + ".header.txt"), "Dock header layout checks complete");
	check(points->toolTip().count('\n') == 1, "point help uses two concise localized lines");
	check(follow->isCheckable() && !follow->icon().isNull() && follow->text().isEmpty() &&
	      follow->size() == lock->size() && !follow->accessibleName().isEmpty(), "scene following uses an accessible compact link button");
	selector->setCurrentIndex(selector->findData(previousSource)); wait(300);
	check(!progressive->isChecked(), "another source retains its independent default reveal option");
	check(dock_highlight(dock, {}, {}), "switching to an ordinary source clears reveal hints");
	selector->setCurrentIndex(selector->findData(QString::fromUtf8(obs_source_get_uuid(source)))); wait(300);
	check(progressive->isChecked(), "changing the source selector restores the saved reveal option");
	firstItem = pages ? pages->item(0) : nullptr;
	check(dock_highlight(dock, "Title", "1/3"), "switching back restores the projected-text highlight");
	const auto update = [&](const char *key, const char *value) {
		auto *changes = obs_data_create();
		obs_data_set_string(changes, key, value);
		obs_source_update(source, changes);
		obs_data_release(changes);
		wait(300);
	};
	const auto flag = [&](bool value) {
		progressive->setChecked(value);
		wait(300);
	};
	wait(400);
	check(displayed(source) == "Title" && page(source) == 0, "new progressive sources start with the first line");
	const auto first = capture(source);
	obs_source_media_next(source); wait();
	const auto two = displayed(source);
	check(page(source) == 0 && two.find("First") != std::string::npos && two.find("Second") == std::string::npos,
	      "Next reveals the next line before changing pages");
	check(two.find('\t') == std::string::npos, "tabs use the complete page layout");
	check(retains_pixels(first, capture(source)), "revealing a line preserves existing pixels and page dimensions");
	check(dock_highlight(dock, "Title A First", "2/3") && pages && pages->item(0) == firstItem && firstItem->text() == firstSummary,
	      "forward reveal expands the highlight without rebuilding the list");
	dock->grab().save(path + ".dock-highlight.png");
	obs_source_media_next(source); wait();
	const auto full = displayed(source);
	check(full.substr(0, two.size()) == two && full.find("Second") != std::string::npos,
	      "later lines do not move earlier tab columns");
	const auto complete = capture(source);
	check(retains_pixels(first, complete), "all reveal steps keep the first line in place");
	check(dock_highlight(dock, "Title A First Longer Second", "3/3"), "Dock highlights all projected TAB rows");
	obs_source_media_next(source); wait();
	check(page(source) == 1 && displayed(source) == "Next", "Next changes pages only after revealing the complete page");
	check(dock_highlight(dock, "Next", "1/2") && firstItem->data(PageListDelegate::ProgressRole).toString().isEmpty() &&
	      firstItem->text() == firstSummary, "changing pages restores the previous page summary");
	obs_source_media_previous(source); wait();
	check(page(source) == 0 && displayed(source) == full, "Previous from the first line restores the complete previous page");
	check(dock_highlight(dock, "Title A First Longer Second", "3/3"), "returning to a page restores its complete projected highlight");
	obs_source_media_previous(source); wait();
	check(displayed(source) == two && page(source) == 0, "Previous retracts one line");
	check(dock_highlight(dock, "Title A First", "2/3"), "backward reveal retracts the projected-text highlight");
	obs_source_media_previous(source); obs_source_media_previous(source); wait();
	check(displayed(source) == "Title" && page(source) == 0, "Previous stops at the first line of the first page");
	calldata_t jump = {};
	calldata_set_int(&jump, "index", 1);
	proc_handler_call(obs_source_get_proc_handler(source), "dock_transition", &jump);
	calldata_free(&jump); wait();
	check(page(source) == 1 && displayed(source) == "Next", "Dock jumps start at the selected page's first line");
	check(dock_highlight(dock, "Next", "1/2"), "page jumps reset the projected-text highlight");
	obs_source_media_next(source); obs_source_media_next(source); wait();
	check(page(source) == 1 && displayed(source) == "Next\nLast", "Next stops at the last line of the final page");
	obs_source_media_restart(source); wait();
	check(page(source) == 0 && displayed(source) == "Title", "Restart resets the page and reveal progress");
	auto *saved = obs_save_source(source);
	auto *stored = obs_data_get_obj(saved, "settings");
	check(obs_data_get_bool(stored, "progressive") && std::string(obs_data_get_string(stored, "custom_text")) == deck,
	      "source serialization preserves the option and original tabbed text");
	obs_data_release(stored); obs_data_release(saved);
	auto *copy = obs_source_duplicate(source, "TextPPT progressive copy", true);
	auto *copyItem = copy ? obs_scene_add(scene, copy) : nullptr;
	wait();
	obs_source_media_next(source); wait();
	check(copyItem && displayed(copy) == "Title" && displayed(source) == two, "source copies keep independent reveal progress");
	if (copyItem) obs_sceneitem_remove(copyItem);
	obs_source_release(copy);
	flag(false);
	check(displayed(source) == full, "disabling the option immediately shows the complete page");
	check(dock_highlight(dock, {}, {}) && pages->item(0)->text() == firstSummary,
	      "disabling reveal restores the full-page summary");
#ifdef _WIN32
	{
		auto *text = active_text(source);
		auto *native = obs_source_get_settings(text);
		check(!obs_data_get_bool(native, "extents"), "disabling reveal restores automatic native text bounds");
		obs_data_release(native); obs_source_release(text);
	}
#endif
	flag(true);
	check(displayed(source) == "Title", "reenabling the option starts with the first line");
	obs_source_media_play_pause(source, false); wait(300);
	check(displayed(source) == full, "Play temporarily uses complete pages");
	obs_source_media_play_pause(source, true); wait(300);
	check(displayed(source) == "Title", "Pause restores manual reveal from the current page's first line");
	update("slide_mode", "mode_auto");
	check(displayed(source) == full, "automatic playback displays complete pages");
	auto *properties = obs_source_properties(source);
	settings = obs_source_get_settings(source);
	obs_properties_apply_settings(properties, settings);
	check(!obs_properties_get(properties, "progressive"), "source properties do not duplicate the Dock reveal option");
	check(!progressive->isEnabled() && progressive->isChecked(), "automatic mode disables the Dock option without losing its saved choice");
	obs_properties_destroy(properties); obs_data_release(settings);
	update("slide_mode", "mode_manual");
	update("use_custom_size", "240x100");
	const auto fixed = capture(source);
	obs_source_media_next(source); wait();
	check(retains_pixels(fixed, capture(source)), "a smaller explicit canvas keeps the same scale while revealing");
	update("use_custom_size", "auto");
#ifdef _WIN32
	for (const char *alignment : {"left", "center", "right"}) {
		update("align", alignment);
		obs_source_media_restart(source); wait();
		const auto before = capture(source);
		obs_source_media_next(source); wait();
		check(retains_pixels(before, capture(source)), "native horizontal alignment stays stable during reveal");
	}
	auto *changes = obs_data_create();
	obs_data_set_bool(changes, "gradient", true);
	obs_data_set_int(changes, "gradient_color", 0x00FF00);
	obs_data_set_double(changes, "gradient_dir", 90.0);
	obs_source_update(source, changes); obs_data_release(changes); wait(300);
	obs_source_media_restart(source); wait();
	const auto gradient = capture(source);
	obs_source_media_next(source); wait();
	check(retains_pixels(gradient, capture(source)), "native gradients keep existing text colors while revealing");
#endif
	update("custom_text", "Title\nOne\nTwo\nThree\nFour\nFive\nSix\nSeven\nEight\nNine\nTen");
	check(next->isEnabled() && !previous->isEnabled(), "a single-page deck enables Next for unrevealed lines");
	next->click(); wait(300);
	check(previous->isEnabled() && displayed(source) == "Title\nOne", "Dock Next reveals a line and enables Previous on page one");
	previous->click(); wait(300);
	check(!previous->isEnabled() && displayed(source) == "Title", "Dock Previous retracts the line and disables at the beginning");
	bool rapid = true;
	for (int repeat = 0; repeat < 50; ++repeat) {
		for (int step = 0; step < 10; ++step) obs_source_media_next(source);
		rapid &= wait_text(source, 0, "Title\nOne\nTwo\nThree\nFour\nFive\nSix\nSeven\nEight\nNine\nTen") >= 0;
		for (int step = 0; step < 10; ++step) obs_source_media_previous(source);
		rapid &= wait_text(source, 0, "Title") >= 0;
	}
	check(rapid, "1000 rapid forward and backward commands preserve reveal order");
	QTemporaryDir directory;
	QFile file(directory.filePath("page.txt"));
	if (directory.isValid() && file.open(QIODevice::WriteOnly)) {
		file.write("File title\r\nFile body\r\n"); file.close();
		update("txt_file", file.fileName().toUtf8().constData());
		update("input_mode", "file");
		check(displayed(source) == "File title", "single-file mode reveals CRLF lines correctly");
		obs_source_media_next(source); wait();
		check(displayed(source) == "File title\nFile body", "single-file parsing retains all revealed content");
		auto *files = obs_data_array_create();
		auto *entry = obs_data_create();
		obs_data_set_string(entry, "value", file.fileName().toUtf8().constData());
		obs_data_array_push_back(files, entry); obs_data_release(entry);
		settings = obs_data_create();
		obs_data_set_string(settings, "input_mode", "files");
		obs_data_set_array(settings, "files", files); obs_data_array_release(files);
		obs_source_update(source, settings); obs_data_release(settings); wait(300);
		check(displayed(source) == "File title", "multiple-file mode reveals the first line");
		obs_source_media_next(source); wait();
		check(displayed(source) == "File title\r\nFile body\r\n", "whole-file pages reveal complete CRLF text");
	} else check(false, "file fixtures can be created");
	update("input_mode", "custom");
	const auto longText = std::string(500, 'A') + u8"\n中文第二行\t后列内容";
	update("custom_text", longText.c_str());
	check(dock_highlight(dock, QString(120, 'A'), "1/2"), "long summaries keep bounded highlighting");
	obs_source_media_next(source); wait();
	check(dock_highlight(dock, QString(120, 'A'), "2/2"),
	      "long summaries stay intact while Chinese reveal progress changes");
	// Exercise point mode through the same Dock and native renderers.
	update("align", "left");
	settings = obs_data_create();
	obs_data_set_bool(settings, "gradient", false);
	font = obs_data_create();
#ifdef _WIN32
	obs_data_set_string(font, "face", "Microsoft YaHei");
#else
	obs_data_set_string(font, "face", "Songti SC");
#endif
	obs_data_set_string(font, "style", "Regular");
	obs_data_set_int(font, "size", 36);
	obs_data_set_obj(settings, "font", font);
	obs_data_release(font);
	obs_source_update(source, settings); obs_data_release(settings);
	update("custom_text", u8"标题\n第一点\t\t第二点\n第三点\t第四点\n\n下一页\t末点");
	check(!points->isChecked(), "point mode defaults to off without changing saved line mode");
	points->setChecked(true); wait(300);
	check(points->isChecked() && !progressive->isChecked() && displayed(source) == u8"标题",
	      "point mode deselects line mode and restarts on the current page");
	const auto pointSummary = pages->item(0)->text();
	const std::vector<QString> prefixes = {QString::fromUtf8(u8"标题"), QString::fromUtf8(u8"标题 第一点"),
		QString::fromUtf8(u8"标题 第一点 第二点"), QString::fromUtf8(u8"标题 第一点 第二点 第三点"),
		QString::fromUtf8(u8"标题 第一点 第二点 第三点 第四点")};
	auto pointImage = capture(source);
	std::string pointFull;
	for (size_t step = 0; step < prefixes.size(); ++step) {
		if (step) { next->click(); wait(300); }
		check(page(source) == 0 && PageListDelegate::summary(QString::fromUtf8(displayed(source).c_str())) == prefixes[step],
		      "point controls reveal complete Chinese TAB cells in order");
		check(dock_highlight(dock, prefixes[step], QStringLiteral("%1/5").arg(step + 1)) && pages->item(0)->text() == pointSummary,
		      "point highlighting retains the complete page summary and exact progress");
		const auto image = capture(source);
		if (step) check(retains_pixels(pointImage, image), "Chinese point expansion preserves existing pixels and TAB positions");
		pointImage = image;
	}
	pointFull = displayed(source);
	dock->grab().save(path + ".points.png");
	next->click(); wait(300);
	check(page(source) == 1 && displayed(source) == u8"下一页" && dock_highlight(dock, QString::fromUtf8(u8"下一页"), "1/2"),
	      "point mode changes pages after the last nonempty cell");
	previous->click(); wait(300);
	check(page(source) == 0 && displayed(source) == pointFull && dock_highlight(dock, prefixes.back(), "5/5"),
	      "backward page changes restore every point of the previous page");
	previous->click(); wait(300);
	check(dock_highlight(dock, prefixes[3], "4/5"), "Previous retracts one TAB point and its highlight");
	selector->setCurrentIndex(selector->findData(previousSource)); wait(300);
	check(!points->isChecked() && !progressive->isChecked(), "ordinary sources retain both default-off choices");
	selector->setCurrentIndex(selected); wait(300);
	check(points->isChecked() && !progressive->isChecked() && dock_highlight(dock, prefixes[3], "4/5"),
	      "switching sources restores point mode and independent progress");
	saved = obs_save_source(source);
	stored = obs_data_get_obj(saved, "settings");
	check(obs_data_get_bool(stored, "progressive_points") && !obs_data_get_bool(stored, "progressive") &&
	      std::string(obs_data_get_string(stored, "custom_text")).find('\t') != std::string::npos,
	      "point mode and original TAB separators survive serialization");
	obs_data_release(stored); obs_data_release(saved);
	flag(true);
	check(progressive->isChecked() && !points->isChecked() && dock_highlight(dock, prefixes[0], "1/3"),
	      "line mode deselects points and restores line progress");
	points->setChecked(true); wait(300);
	points->setChecked(false); wait(300);
	check(!points->isChecked() && !progressive->isChecked() && displayed(source) == pointFull && dock_highlight(dock, {}, {}),
	      "both choices can be off to restore the complete page");
	points->setChecked(true); wait(300);
	obs_source_media_play_pause(source, false); wait(300);
	check(displayed(source) == pointFull, "Play suspends point expansion and uses complete pages");
	obs_source_media_play_pause(source, true); wait(300);
	check(displayed(source) == u8"标题", "Pause resumes point expansion at the first point");
	update("slide_mode", "mode_auto");
	check(!points->isEnabled() && points->isChecked() && displayed(source) == pointFull,
	      "automatic mode preserves the point choice while disabling manual controls");
	update("slide_mode", "mode_manual");
	update("custom_text", u8"\t中文😀\t\t \tEnglish\r\n下一行\t\n\n结尾");
	check(PageListDelegate::summary(QString::fromUtf8(displayed(source).c_str())) == QString::fromUtf8(u8"中文😀") &&
	      dock_highlight(dock, QString::fromUtf8(u8"中文😀"), "1/3"), "point mode skips empty cells without splitting Unicode");
	next->click(); wait(300);
	check(dock_highlight(dock, QString::fromUtf8(u8"中文😀 English"), "2/3"), "mixed Latin and Chinese points keep leading TAB alignment");
	next->click(); wait(300);
	check(dock_highlight(dock, QString::fromUtf8(u8"中文😀 English 下一行"), "3/3"), "trailing TAB cells do not add empty steps");
	update("custom_text", (std::string(500, 'A') + u8"\t中文\t结尾").c_str());
	check(dock_highlight(dock, QString(120, 'A'), "1/3"), "long point previews keep bounded highlight caches");
	next->click(); wait(300);
	check(dock_highlight(dock, QString(120, 'A'), "2/3"), "long previews retain point progress after their visible text ends");
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write("File\tFirst\r\nSecond\tLast\r\n"); file.close();
		update("input_mode", "file");
		check(displayed(source) == "File" && dock_highlight(dock, "File", "1/4"), "file input splits TAB points and CRLF rows");
		next->click(); wait(300);
		check(dock_highlight(dock, "File First", "2/4"), "file points use the same Dock controls");
	}
	update("input_mode", "custom");
	update("custom_text", u8"标题\n第一点\t\t第二点\n第三点\t第四点\n\n终点\n结束");
	const auto pointBaseline = textppt_test_memory();
	auto pointPeak = pointBaseline;
	bool pointRapid = true;
	int slowestPoint = 0;
	for (int repeat = 0; repeat < 100; ++repeat) {
		for (int step = 0; step < 6; ++step) obs_source_media_next(source);
		const int forward = wait_text(source, 1, u8"终点\n结束");
		for (int step = 0; step < 6; ++step) obs_source_media_previous(source);
		const int backward = wait_text(source, 0, u8"标题");
		pointRapid &= forward >= 0 && backward >= 0;
		slowestPoint = std::max({slowestPoint, forward, backward});
		pointPeak = std::max(pointPeak, textppt_test_memory());
	}
	check(pointRapid, "1200 rapid Chinese point commands retain TAB cells and page boundaries");
	bool pointFonts = true;
	std::unordered_set<std::string> pointRenderers;
	for (int change = 0; change < 30; ++change) {
		settings = obs_data_create();
		font = obs_data_create();
#ifdef _WIN32
		obs_data_set_string(font, "face", "Microsoft YaHei");
#else
		obs_data_set_string(font, "face", "Songti SC");
#endif
		obs_data_set_string(font, "style", "Regular");
		obs_data_set_int(font, "size", 24 * (change % 3 + 1));
		obs_data_set_int(font, "flags", change % 2 ? OBS_FONT_BOLD : 0);
		obs_data_set_obj(settings, "font", font);
		obs_data_release(font);
		obs_source_update(source, settings); obs_data_release(settings);
		for (int attempt = 0; attempt < 150; ++attempt) {
			auto *native = active_text(source);
			auto *data = native ? obs_source_get_settings(native) : nullptr;
			auto *value = obs_data_get_obj(data, "font");
			const bool updated = obs_data_get_int(value, "size") == 24 * (change % 3 + 1) &&
				obs_data_get_int(value, "flags") == (change % 2 ? OBS_FONT_BOLD : 0);
			obs_data_release(value); obs_data_release(data); obs_source_release(native);
			if (updated && ready(source)) break;
			wait(20);
		}
		const auto title = capture(source);
		obs_source_media_next(source);
		wait_text(source, 0, std::string(u8"标题\n第一点"));
		const auto expanded = capture(source);
		const bool same = displayed(source).find(u8"第一点") != std::string::npos && retains_pixels(title, expanded);
		if (!same) {
			title.save(path + QStringLiteral(".font-%1-before.png").arg(change));
			expanded.save(path + QStringLiteral(".font-%1-after.png").arg(change));
			wait(500);
			capture(source).save(path + QStringLiteral(".font-%1-later.png").arg(change));
			QTextStream(&report) << "Point font mismatch " << change << ": " << title.width() << 'x' << title.height()
			                    << " / " << expanded.width() << 'x' << expanded.height() << '\n';
		}
		pointFonts &= same;
		if (auto *native = active_text(source)) {
			pointRenderers.emplace(obs_source_get_uuid(native));
			obs_source_release(native);
		}
		obs_source_media_previous(source);
		pointFonts &= wait_text(source, 0, u8"标题") >= 0;
		pointPeak = std::max(pointPeak, textppt_test_memory());
	}
	check(pointFonts && !pointRenderers.empty() && pointRenderers.size() <= 2,
	      "30 Chinese point font edits preserve existing pixels and reuse native renderers");
	check(pointBaseline && pointPeak <= pointBaseline + 128 * 1024 * 1024ull,
	      "Chinese point bursts and font edits stay within the memory growth limit");
	QTextStream(&report) << "Slowest point batch ms: " << slowestPoint << '\n'
		<< "Point stress peak delta MiB: " << double(pointPeak - pointBaseline) / 1048576.0 << '\n';
	update("input_mode", "groups");
	check(!progressive->isEnabled() && !points->isEnabled(), "group sources disable both Dock reveal options");
	obs_sceneitem_remove(item);
	obs_source_remove(source);
	obs_source_release(source);
	check(passed, "PROGRESSIVE TESTS COMPLETE");
	return passed;
}

static bool group_studio_probe(const char *kind, QFile &report, QWidget *dock)
{
	const bool textMode = !qEnvironmentVariableIsEmpty("TEXTPPT_NATIVE_TEXT");
	const bool doubleClick = !qEnvironmentVariableIsEmpty("TEXTPPT_DOCK_DOUBLE_CLICK");
	const bool previewClick = !qEnvironmentVariableIsEmpty("TEXTPPT_DOCK_PREVIEW");
	bool passed = true;
	const auto check = [&](bool value, const char *message) {
		QTextStream(&report) << (value ? "PASS: " : "FAIL: ") << message << '\n';
		report.flush();
		passed &= value;
		return value;
	};
	auto *config = obs_frontend_get_user_config();
	if (!check(config && !config_get_bool(config, "BasicWindow", "SwapScenesMode"),
	           "native Studio fixture has user settings and disables swapping"))
		return false;
	const bool independent = config_get_bool(config, "BasicWindow", "SceneDuplicationMode") &&
		config_get_bool(config, "BasicWindow", "EditPropertiesMode");
	auto *scene = obs_scene_create("TextPPT native group Studio");
	if (!check(scene != nullptr, "native Studio group scene is created"))
		return false;
	auto *settings = obs_data_create();
	auto *groups = obs_data_array_create();
	auto *ids = obs_data_create();
	for (int index = 0; !textMode && index < 3; ++index) {
		const auto name = "Studio group " + std::to_string(index + 1);
		auto *group = obs_scene_add_group(scene, name.c_str());
		auto *colorSettings = obs_data_create();
		obs_data_set_int(colorSettings, "width", 320);
		obs_data_set_int(colorSettings, "height", 180);
		obs_data_set_int(colorSettings, "color", 0xff000000ull | (0xffull << (index * 8)));
		const char *type = obs_get_latest_input_type_id("color_source");
		auto *color = type ? obs_source_create_private(type, name.c_str(), colorSettings) : nullptr;
		obs_data_release(colorSettings);
		if (check(group && color, "group page has a native color source")) {
			obs_scene_add(obs_sceneitem_group_get_scene(group), color);
			obs_sceneitem_set_visible(group, false);
			const char *uuid = obs_source_get_uuid(obs_sceneitem_get_source(group));
			auto *entry = obs_data_create();
			obs_data_set_string(entry, "value", name.c_str());
			obs_data_set_string(entry, "uuid", uuid);
			obs_data_array_push_back(groups, entry);
			obs_data_set_string(ids, uuid, uuid);
			obs_data_release(entry);
		}
		obs_source_release(color);
	}
	obs_data_set_string(settings, "input_mode", textMode ? "custom" : "groups");
	if (textMode) {
		obs_data_set_string(settings, "custom_text", u8"第一页\n甲\t乙\n丙\n\n第二页\n一\t\t二\n三\n\n第三页\nABC\t中文\n末行");
		obs_data_set_bool(settings, "progressive", true);
		auto *font = obs_data_create();
#ifdef _WIN32
		obs_data_set_string(font, "face", "Microsoft YaHei");
#else
		obs_data_set_string(font, "face", "Songti SC");
#endif
		obs_data_set_string(font, "style", "Regular");
		obs_data_set_int(font, "size", 36);
		obs_data_set_obj(settings, "font", font); obs_data_release(font);
	}
	obs_data_set_string(settings, "slide_mode", "mode_manual");
	obs_data_set_string(settings, "playback_behavior", "stop_restart");
	obs_data_set_string(settings, "transition", "cut");
	obs_data_set_array(settings, "groups", groups);
	obs_data_set_obj(settings, "group_ids", ids);
	obs_data_array_release(groups); obs_data_release(ids);
	auto *source = obs_source_create(kind, "TextPPT native group controller", settings, nullptr);
	obs_data_release(settings);
	check(source && obs_scene_add(scene, source), "native Studio group controller is created");
	if (passed) {
		const bool wasStudio = obs_frontend_preview_program_mode_active();
		auto *oldScene = obs_frontend_get_current_scene();
		auto *oldPreview = wasStudio ? obs_frontend_get_current_preview_scene() : nullptr;
		const int oldDuration = obs_frontend_get_transition_duration();
		auto *lock = dock->findChild<QToolButton *>("TextPPTSourceLock");
		auto *selector = dock->findChild<QComboBox *>("TextPPTSourceList");
		const bool wasLocked = lock->isChecked();
		const auto oldSelection = selector->currentData().toString();
		auto *away = previewClick ? obs_scene_create("TextPPT outside preview") : nullptr;
		if (wasStudio) obs_frontend_set_preview_program_mode(false);
		obs_frontend_set_current_scene(obs_scene_get_source(scene));
		wait(300);
		obs_frontend_set_transition_duration(80);
		obs_frontend_set_preview_program_mode(true);
		wait(300);
		selector->setCurrentIndex(selector->findData(QString::fromUtf8(obs_source_get_uuid(source))));
		lock->setChecked(true);
		QPushButton *transitionButton = nullptr;
		auto *window = static_cast<QWidget *>(obs_frontend_get_main_window());
		const auto transitionText = QString::fromUtf8(obs_frontend_get_locale_string("Transition"));
		for (auto *button : window->findChildren<QPushButton *>())
			if (button->text() == transitionText && button->isVisible()) transitionButton = button;
		check(transitionButton != nullptr, "OBS native Transition button is available");
		check(dock->findChild<QCheckBox *>("TextPPTProgressive")->isEnabled() == textMode &&
		      dock->findChild<QCheckBox *>("TextPPTPoints")->isEnabled() == textMode,
		      "native Studio reveal availability matches the input mode");
		const auto step = [](obs_source_t *source) {
			calldata_t data = {};
			proc_handler_call(obs_source_get_proc_handler(source), "get_status", &data);
			const int result = static_cast<int>(calldata_int(&data, "step"));
			calldata_free(&data);
			return result;
		};
		const auto expect = [&](obs_source_t *source, int index, int progress = -1) {
			for (int attempt = 0; attempt < 100; ++attempt) {
				if (source && ready(source) && page(source) == index && (progress < 0 || step(source) == progress)) return true;
				wait(20);
			}
			return false;
		};
		const auto acknowledged = [&] {
			calldata_t data = {};
			proc_handler_call(obs_source_get_proc_handler(source), "get_status", &data);
			const auto serial = calldata_int(&data, "acknowledged");
			calldata_free(&data);
			return serial;
		};
		const auto jump = [&](int index) {
			calldata_t data = {};
			calldata_set_int(&data, "index", index);
			proc_handler_call(obs_source_get_proc_handler(source), "dock_transition", &data);
			calldata_free(&data);
			check(expect(source, index), "preview reaches the selected group page");
		};
		const auto programScene = [] {
			auto *transition = obs_get_output_source(0);
			auto *output = obs_transition_get_active_source(transition);
			obs_source_release(transition);
			return output;
		};
		const auto program = [&] {
			struct Match { obs_source_t *result; const char *name; } match{nullptr, obs_source_get_name(source)};
			auto *output = programScene();
			if (auto *scene = obs_scene_from_source(output))
				obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *data) {
					auto *source = obs_sceneitem_get_source(item);
					auto *match = static_cast<Match *>(data);
					if (strncmp(obs_source_get_id(source), "obs-textppt-", 12) == 0 &&
					    strcmp(obs_source_get_name(source), match->name) == 0)
						match->result = obs_source_get_ref(source);
					return true;
				}, &match);
			obs_source_release(output);
			return match.result;
		};
		auto *list = dock->findChild<QListWidget *>();
		int clickSignals = 0;
		const auto clickConnection = QObject::connect(list, &QListWidget::itemClicked, dock, [&](QListWidgetItem *) { ++clickSignals; });
		const auto previewIs = [](obs_scene_t *scene) {
			auto *preview = obs_frontend_get_current_preview_scene();
			const bool same = preview == obs_scene_get_source(scene);
			obs_source_release(preview);
			return same;
		};
		const auto singleClick = [&](int row, bool held = false) {
			auto *item = list->item(row);
			if (!item) return;
			list->setFocus(Qt::MouseFocusReason);
			list->scrollToItem(item);
			const int beforeClick = clickSignals;
			const QPointF position(list->visualItemRect(item).center());
			const QPointF global(list->viewport()->mapToGlobal(position.toPoint()));
			QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
			QApplication::sendEvent(list->viewport(), &press);
			if (held) { wait(300); check(list->currentRow() == row, "status polling preserves a held mouse selection"); }
			QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(list->viewport(), &release);
			check(clickSignals == beforeClick + 1, "single mouse gesture emits exactly one item click");
		};
		const auto mouse = [&](int row, bool repeat = false) {
			auto *item = list->item(row);
			if (!item) return;
			list->scrollToItem(item);
			const QPointF position(list->visualItemRect(item).center());
			const QPointF global(list->viewport()->mapToGlobal(position.toPoint()));
			for (int count = 0; count < (repeat ? 2 : 1); ++count) {
				for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease,
				                  QEvent::MouseButtonDblClick, QEvent::MouseButtonRelease}) {
					QMouseEvent event(type, position, global, Qt::LeftButton,
						type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
					QApplication::sendEvent(list->viewport(), &event);
				}
			}
		};
		std::atomic<int> starts{0};
		auto *mainTransition = obs_frontend_get_current_transition();
		const auto countStart = +[](void *data, calldata_t *) { ++*static_cast<std::atomic<int> *>(data); };
		signal_handler_connect(obs_source_get_signal_handler(mainTransition), "transition_start", countStart, &starts);
		const int rounds = qEnvironmentVariableIsEmpty("TEXTPPT_STUDIO_STRESS") ? 2 : 100;
		const auto beforeFixtures = textppt_test_memory();
		if (previewClick) {
			// A different eligible source must not steal an explicit preview request.
			auto *decoySettings = obs_data_create();
			obs_data_set_string(decoySettings, "custom_text", "Other source");
			auto *decoy = obs_source_create(kind, "TextPPT preview following decoy", decoySettings, nullptr);
			obs_data_release(decoySettings);
			obs_scene_add(scene, decoy); obs_source_release(decoy);
			auto *privateSettings = obs_source_get_private_settings(source);
			obs_data_set_bool(privateSettings, "textppt_follow_scene", false); obs_data_release(privateSettings);
			wait(300);
			// Initialize fixed scene and readback resources before measuring repeated operations.
			capture(obs_scene_get_source(scene));
			auto *live = programScene(); capture(live); obs_source_release(live);
			wait(300);
		}
		const auto memoryBefore = textppt_test_memory();
		auto peak = memoryBefore;
		const auto widgetsBefore = QApplication::allWidgets().size();
		auto peakWidgets = widgetsBefore;
		QTextStream(&report) << "Fixed fixture/readback setup MiB: " << (double(memoryBefore) - double(beforeFixtures)) / 1048576.0 << '\n';
		int slowest = 0;
		std::vector<obs_weak_source_t *> retired;
		for (int round = 0; round < rounds && passed; ++round) {
			if (!transitionButton) break;
			// Separate intentional gestures; each gesture still includes an immediate repeated double-click.
			if (doubleClick) wait(QApplication::doubleClickInterval() + 30);
			if (textMode) {
				auto *mode = obs_data_create();
				obs_data_set_bool(mode, "progressive", round % 2 == 0);
				obs_data_set_bool(mode, "progressive_points", round % 2 != 0);
				obs_source_update(source, mode); obs_data_release(mode);
				wait(100);
			}
			const int index = 1 + round % 2;
			if (previewClick) {
				auto *live = programScene();
				const auto beforePixels = capture(live);
				const int before = starts.load();
				const auto previousSerial = acknowledged();
				lock->setChecked(round % 2 == 0);
				obs_frontend_set_current_preview_scene(obs_scene_get_source(away)); wait(100);
				selector->setCurrentIndex(selector->findData(QString::fromUtf8(obs_source_get_uuid(source))));
				singleClick(index, round % 20 == 0);
				check(expect(source, index, textMode ? 1 : 0) && previewIs(scene), "single mouse click opens its owning scene and page in preview");
				check(acknowledged() == previousSerial + 1, "one new-page click queues exactly one page command");
				singleClick(index); wait(30);
				check(acknowledged() == previousSerial + 1, "clicking the selected page queues no duplicate command");
				check(starts.load() == before, "single preview clicks never trigger a native transition");
				check(selector->currentData().toString() == QString::fromUtf8(obs_source_get_uuid(source)),
				      "manual preview selection wins over following and source-lock settings");
				check(!independent || native_pixels_match(capture(live), beforePixels), "single preview clicks preserve the complete independent program frame");
				obs_source_release(live);
			} else if (!doubleClick || textMode) jump(index);
			if (textMode) {
				if (doubleClick) wait(300);
				dock->findChild<QToolButton *>("TextPPTNextPage")->click();
				check(expect(source, index, 2), "native Studio preview reaches a partial line or TAB point");
			}
			auto expectedPixels = capture(source);
			QElapsedTimer timer; timer.start();
			// Exercise OBS's real Transition button, including source activation.
			check(transitionButton->isEnabled(), "OBS native Transition button accepts the next transition");
			if (doubleClick) {
				const int before = starts.load();
				if (previewClick && round % 2) {
					obs_frontend_set_current_preview_scene(obs_scene_get_source(away)); wait(100);
					selector->setCurrentIndex(selector->findData(QString::fromUtf8(obs_source_get_uuid(source))));
				}
				mouse(index, true);
				wait(650);
				check(expect(source, index, textMode ? 2 : 0), "mouse double-click selects the requested preview page without losing partial reveal");
				check(starts.load() <= before + 1 &&
				      (!config_get_bool(config, "BasicWindow", "SceneDuplicationMode") || starts.load() == before + 1),
				      "rapid mouse double-clicks trigger at most one native transition");
				expectedPixels = capture(source);
			} else {
				transitionButton->click();
				wait(250);
			}
			auto *copy = program();
			check(copy && (copy != source) == independent && expect(copy, index, textMode ? 2 : 0),
			      "native Studio transition preserves the selected page and progress with stop/restart enabled");
			check(copy && native_pixels_match(capture(copy), expectedPixels),
			      "native program pixels match the selected preview page");
			if (textMode) {
				dock->findChild<QToolButton *>("TextPPTNextPage")->click();
				check(expect(source, index, 3) && expect(copy, index, independent ? 2 : 3),
				      "native Studio Dock Next respects source-copy isolation");
				dock->findChild<QToolButton *>("TextPPTPreviousPage")->click();
				check(expect(source, index, 2) && expect(copy, index, 2),
				      "native Studio Dock Previous respects source-copy isolation");
			}
			jump(0);
			check(copy && expect(copy, independent ? index : 0, textMode ? (independent ? 2 : 1) : 0) &&
			      native_pixels_match(capture(copy), independent ? expectedPixels : capture(source)),
			      "preview navigation follows native source-copy isolation rules");
			if (copy) {
				auto *transition = obs_get_output_source(0);
				auto *output = obs_transition_get_active_source(transition);
				auto *outputScene = obs_scene_from_source(output);
				auto *item = outputScene ? obs_scene_find_source(outputScene, obs_source_get_name(copy)) : nullptr;
				if (check(item != nullptr, "copied group controller has a program scene item")) {
					std::atomic<bool> deactivated{false};
					const auto onDeactivate = +[](void *data, calldata_t *) { *static_cast<std::atomic<bool> *>(data) = true; };
					auto *handler = obs_source_get_signal_handler(copy);
					signal_handler_connect(handler, "deactivate", onDeactivate, &deactivated);
					obs_sceneitem_set_visible(item, false);
					for (int attempt = 0; attempt < 250 && !deactivated.load(); ++attempt) wait(20);
					signal_handler_disconnect(handler, "deactivate", onDeactivate, &deactivated);
					check(deactivated.load(), "hidden program source delivers its native deactivation callback before being shown again");
					obs_sceneitem_set_visible(item, true);
					check(expect(copy, 0, textMode ? 1 : 0), "later visibility activation still restarts the copied source at page one");
				}
				obs_source_release(output); obs_source_release(transition);
			}
			if (copy && independent) retired.push_back(obs_source_get_weak_source(copy));
			obs_source_release(copy);
			peak = std::max(peak, textppt_test_memory());
			peakWidgets = std::max(peakWidgets, QApplication::allWidgets().size());
			if ((round + 1) % 10 == 0)
				QTextStream(&report) << "Round " << round + 1 << ": memory delta MiB "
					<< (double(textppt_test_memory()) - double(memoryBefore)) / 1048576.0
					<< ", widgets " << QApplication::allWidgets().size() << '\n';
			slowest = std::max(slowest, static_cast<int>(timer.elapsed()));
		}
		if (previewClick && passed) {
			jump(0); wait(300);
			lock->setChecked(false);
			obs_frontend_set_current_preview_scene(obs_scene_get_source(away)); wait(150);
			selector->setCurrentIndex(selector->findData(QString::fromUtf8(obs_source_get_uuid(source))));
			const int before = starts.load();
			singleClick(1); singleClick(2);
			check(expect(source, 2) && previewIs(scene) &&
			      selector->currentData().toString() == QString::fromUtf8(obs_source_get_uuid(source)) && starts.load() == before,
			      "rapid cross-scene clicks keep the latest page and explicit source without transitioning");
			jump(0); wait(300);
			lock->setChecked(true);
			obs_frontend_set_current_preview_scene(obs_scene_get_source(away)); wait(150);
			auto *shared = obs_scene_create("TextPPT shared scene & 中文");
			obs_scene_add(shared, source);
			const auto sharedId = QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(shared)));
			const auto choose = [&](const QString &id, bool cancel = false, std::function<void()> before = {}) {
				QTimer::singleShot(50, qApp, [&, id, cancel, before] {
					auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
					if (!menu || menu->objectName() != "TextPPTPreviewScenes") {
						calldata_t status = {};
						proc_handler_call(obs_source_get_proc_handler(source), "get_status", &status);
						auto *preview = obs_frontend_get_current_preview_scene();
						auto *transition = obs_get_output_source(0);
						QTextStream(&report) << "Menu state: selection " << selector->currentData().toString()
							<< ", expected " << obs_source_get_uuid(source) << ", preview " << (preview ? obs_source_get_name(preview) : "none")
							<< ", backend page/revision " << calldata_int(&status, "index") << '/' << calldata_int(&status, "revision")
							<< ", list row/count/enabled/blocked " << list->currentRow() << '/' << list->count() << '/'
							<< list->isEnabled() << '/' << list->signalsBlocked() << ", transition time " << (transition ? obs_transition_get_time(transition) : -1)
							<< ", click signals " << clickSignals << '\n';
						calldata_free(&status); obs_source_release(preview); obs_source_release(transition);
					}
					if (!check(menu && menu->objectName() == "TextPPTPreviewScenes", "shared sources offer the native preview scene menu")) return;
					if (cancel) { menu->close(); return; }
					if (before) before();
					for (auto *action : menu->actions()) if (action->data().toString() == id) {
						menu->setActiveAction(action);
						QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
						QApplication::sendEvent(menu, &enter); return;
					}
					check(false, "requested preview scene is available"); menu->close();
				});
			};
			choose(sharedId); singleClick(1);
			check(expect(source, 1) && previewIs(shared), "scene-menu choice previews the requested shared scene");
			obs_frontend_set_current_preview_scene(obs_scene_get_source(away)); wait(150);
			choose({}, true); singleClick(2); wait(300);
			check(previewIs(away) && page(source) == 1, "canceling the scene menu preserves preview and page");
			choose(sharedId, false, [&] { selector->setCurrentIndex(selector->findText("TextPPT preview following decoy")); });
			singleClick(2); wait(300);
			check(previewIs(away) && page(source) == 1, "changing sources while the scene menu is open cancels its selection");
			selector->setCurrentIndex(selector->findData(QString::fromUtf8(obs_source_get_uuid(source)))); wait(200);
			choose(sharedId, false, [&] { obs_frontend_set_current_preview_scene(oldScene); });
			singleClick(2); wait(300);
			check(!previewIs(shared) && page(source) == 1, "changing preview scenes while the menu is open cancels its selection");
			obs_frontend_set_current_preview_scene(obs_scene_get_source(away)); wait(150);
			choose(sharedId, false, [&] { obs_source_remove(obs_scene_get_source(shared)); }); singleClick(2); wait(300);
			check(previewIs(away) && page(source) == 1, "a scene removed while its menu is open cannot be previewed");
			obs_scene_release(shared);
			auto *controller = obs_scene_find_source(scene, obs_source_get_name(source));
			obs_sceneitem_set_visible(controller, false); wait(200);
			singleClick(2); wait(300);
			check(previewIs(away) && page(source) == 1, "hidden sources are not made visible by a preview click");
			auto *nestedScene = obs_scene_create("TextPPT nested preview scene");
			auto *nestedGroup = obs_scene_add_group(nestedScene, "Preview parent group");
			obs_scene_add(obs_sceneitem_group_get_scene(nestedGroup), source); wait(150);
			singleClick(2);
			check(expect(source, 2) && previewIs(nestedScene), "preview lookup finds a source inside a visible nested group");
			obs_frontend_set_current_preview_scene(obs_scene_get_source(away)); wait(150);
			obs_sceneitem_set_visible(nestedGroup, false); wait(150);
			singleClick(0); wait(300);
			check(previewIs(away) && page(source) == 2, "hidden parent groups are excluded from preview lookup");
			obs_source_remove(obs_scene_get_source(nestedScene)); obs_scene_release(nestedScene);
			obs_sceneitem_set_visible(controller, true);
			obs_frontend_set_current_preview_scene(obs_scene_get_source(scene)); wait(300);
		}
		if (doubleClick && passed) {
			jump(0); wait(600);
			const auto noTransfer = [&](int before, const char *message) { wait(650); check(starts.load() == before, message); };
			int before = starts.load();
			mouse(1);
			dock->findChild<QToolButton *>("TextPPTNextPage")->click();
			noTransfer(before, "navigation cancels a pending double-click transition");
			jump(0); wait(300);
			before = starts.load();
			mouse(1);
			obs_frontend_set_current_preview_scene(oldScene);
			noTransfer(before, "changing preview scenes cancels a pending double-click transition");
			obs_frontend_set_current_preview_scene(obs_scene_get_source(scene)); wait(300);
			jump(0); wait(300);
			auto *otherSettings = obs_data_create();
			obs_data_set_string(otherSettings, "custom_text", "Unrelated source");
			auto *other = obs_source_create(kind, "Studio unrelated double-click source", otherSettings, nullptr);
			obs_data_release(otherSettings); wait(350);
			before = starts.load();
			mouse(1);
			selector->setCurrentIndex(selector->findData(QString::fromUtf8(obs_source_get_uuid(other))));
			noTransfer(before, "changing sources cancels a pending double-click transition");
			before = starts.load(); mouse(0);
			noTransfer(before, "a locked source outside preview cannot transition another scene");
			selector->setCurrentIndex(selector->findData(QString::fromUtf8(obs_source_get_uuid(source)))); wait(300);
			obs_source_remove(other); obs_source_release(other); wait(300);
			jump(0); wait(300);
			before = starts.load(); mouse(1); dock->hide();
			noTransfer(before, "hiding the Dock cancels a pending double-click transition");
			dock->show(); wait(300);
			before = starts.load();
			const QPointF rightPosition(list->visualItemRect(list->item(0)).center());
			QMouseEvent rightClick(QEvent::MouseButtonDblClick, rightPosition,
				QPointF(list->viewport()->mapToGlobal(rightPosition.toPoint())), Qt::RightButton, Qt::RightButton, Qt::NoModifier);
			QApplication::sendEvent(list->viewport(), &rightClick);
			noTransfer(before, "right-button double-clicks never trigger a transition");
			jump(0); wait(300);
			before = starts.load();
			mouse(1);
			obs_frontend_set_preview_program_mode(false);
			wait(300); before = starts.load();
			mouse(2);
			noTransfer(before, "double-clicks outside Studio Mode never trigger a scene transition");
			obs_frontend_set_preview_program_mode(true); wait(600);
			transitionButton = nullptr;
			for (auto *button : window->findChildren<QPushButton *>())
				if (button->text() == transitionText && button->isVisible()) transitionButton = button;
			if (config_get_bool(config, "BasicWindow", "SceneDuplicationMode")) {
				obs_frontend_set_transition_duration(700);
				if (check(transitionButton != nullptr, "native Transition button is recreated after reopening Studio Mode"))
					transitionButton->click();
				wait(60);
				before = starts.load();
				mouse(1, true);
				wait(900);
				check(starts.load() == before, "double-clicks during an active native transition are ignored");
			}
			obs_frontend_set_transition_duration(80);
		}
		signal_handler_disconnect(obs_source_get_signal_handler(mainTransition), "transition_start", countStart, &starts);
		obs_source_release(mainTransition);
		lock->setChecked(false);
		obs_frontend_set_preview_program_mode(false);
		obs_frontend_set_current_scene(oldScene);
		obs_frontend_set_transition_duration(oldDuration);
		if (wasStudio) {
			obs_frontend_set_preview_program_mode(true);
			if (oldPreview) obs_frontend_set_current_preview_scene(oldPreview);
		}
		wait(250);
		bool released = true;
		for (auto *ref : retired) {
			auto *copy = obs_weak_source_get_source(ref);
			released &= copy == nullptr;
			obs_source_release(copy); obs_weak_source_release(ref);
		}
		check(released, "native Studio transitions release retired source copies");
		check(memoryBefore && peak <= memoryBefore + 128 * 1024 * 1024ull,
		      "native Studio transition memory stays within the growth limit");
		QTextStream(&report) << "Native Studio rounds: " << rounds << "; independent: " << independent
			<< "; peak delta MiB: " << double(peak - memoryBefore) / 1048576.0
			<< "; slowest batch ms: " << slowest << "; widgets before/peak/after: " << widgetsBefore
			<< '/' << peakWidgets << '/' << QApplication::allWidgets().size() << '\n';
		QObject::disconnect(clickConnection);
		selector->setCurrentIndex(selector->findData(oldSelection));
		lock->setChecked(wasLocked);
		obs_source_release(oldPreview); obs_source_release(oldScene);
		if (away) { obs_source_remove(obs_scene_get_source(away)); obs_scene_release(away); }
	}
	obs_source_remove(source); obs_source_release(source);
	obs_source_remove(obs_scene_get_source(scene)); obs_scene_release(scene);
	return passed;
}

bool textppt_native_studio_probe(const char *kind, const QString &path, QWidget *dock)
{
	QFile report(path);
	if (!report.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
	const bool passed = group_studio_probe(kind, report, dock);
	QTextStream(&report) << (passed ? "PASS: " : "FAIL: ") << "NATIVE STUDIO TESTS COMPLETE\n";
	return passed;
}

bool textppt_studio_probe(const char *kind, const QString &path, QWidget *dock)
{
	QFile report(path);
	if (!report.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
	bool passed = true;
	const auto check = [&](bool value, const char *message) {
		QTextStream(&report) << (value ? "PASS: " : "FAIL: ") << message << '\n';
		report.flush();
		passed &= value;
		return value;
	};
	const auto step = [](obs_source_t *source) {
		calldata_t data = {};
		proc_handler_call(obs_source_get_proc_handler(source), "get_status", &data);
		const auto result = static_cast<int>(calldata_int(&data, "step"));
		calldata_free(&data);
		return result;
	};
	const auto expect = [&](obs_source_t *source, int index, int progress) {
		for (int attempt = 0; attempt < 50; ++attempt) {
			if (ready(source) && page(source) == index && step(source) == progress) return true;
			wait(20);
		}
		return false;
	};
	const auto press = [](QToolButton *button) {
		for (int attempt = 0; attempt < 60 && !button->isEnabled(); ++attempt) wait(20);
		button->click();
	};
	auto *selector = dock->findChild<QComboBox *>("TextPPTSourceList");
	auto *next = dock->findChild<QToolButton *>("TextPPTNextPage");
	auto *previous = dock->findChild<QToolButton *>("TextPPTPreviousPage");
	auto *restart = dock->findChild<QToolButton *>("TextPPTRestart");
	auto *lock = dock->findChild<QToolButton *>("TextPPTSourceLock");
	auto *input = dock->findChild<QLineEdit *>("TextPPTPageInput");
	auto *points = dock->findChild<QCheckBox *>("TextPPTPoints");
	auto *lines = dock->findChild<QCheckBox *>("TextPPTProgressive");
	if (!check(selector && next && previous && restart && lock && input && points && lines, "Studio controls are available"))
		return false;
	const bool wasStudio = obs_frontend_preview_program_mode_active();
	auto *oldScene = obs_frontend_get_current_scene();
	if (wasStudio) obs_frontend_set_preview_program_mode(false);
	auto *scene = obs_scene_create("TextPPT Studio probe");
	auto *sceneSource = obs_scene_get_source(scene);
	obs_frontend_set_current_scene(sceneSource);
	wait(200);
	obs_frontend_set_preview_program_mode(true);
	wait(200);
	auto *oldOutput = obs_get_output_source(0);
	auto *transition = obs_source_create_private("fade_transition", "TextPPT Studio test transition", nullptr);
	obs_transition_set(transition, sceneSource);
	obs_set_output_source(0, transition);
	const uint64_t memoryBefore = textppt_test_memory();
	for (bool byPoints : {false, true}) {
		auto *settings = obs_data_create();
		obs_data_set_string(settings, "custom_text", u8"第一页\n一\t二\n三\n\n第二页\n甲\t\t乙\n丙");
		obs_data_set_string(settings, "transition", "cut");
		obs_data_set_bool(settings, "progressive", !byPoints);
		obs_data_set_bool(settings, "progressive_points", byPoints);
		auto *font = obs_data_create();
#ifdef _WIN32
		obs_data_set_string(font, "face", "Microsoft YaHei");
#else
		obs_data_set_string(font, "face", "Songti SC");
#endif
		obs_data_set_string(font, "style", "Regular");
		obs_data_set_int(font, "size", 36);
		obs_data_set_obj(settings, "font", font); obs_data_release(font);
		auto *source = obs_source_create(kind, byPoints ? "Studio points" : "Studio lines", settings, nullptr);
		obs_data_release(settings);
		auto *item = obs_scene_add(scene, source);
		check(expect(source, 0, 1), "new preview text finishes native initialization");
		wait(300);
		selector->setCurrentIndex(selector->findData(QString::fromUtf8(obs_source_get_uuid(source))));
		lock->setChecked(true);
		calldata_t jump = {};
		calldata_set_int(&jump, "index", 1);
		proc_handler_call(obs_source_get_proc_handler(source), "dock_transition", &jump);
		calldata_free(&jump);
		obs_source_media_next(source);
		if (byPoints) obs_source_media_next(source);
		const int initialStep = byPoints ? 3 : 2;
		check(expect(source, 1, initialStep), "preview reaches a later page and partial reveal");
		wait(200);
		const auto originalPixels = capture(source);
		const auto duplicate = [&](obs_scene_t *from) {
			auto *copy = obs_scene_duplicate(from, "TextPPT Studio private copy", OBS_SCENE_DUP_PRIVATE_COPY);
			obs_transition_start(transition, OBS_TRANSITION_MODE_AUTO, 80, obs_scene_get_source(copy));
			obs_source_t *result = nullptr;
			obs_scene_enum_items(copy, [](obs_scene_t *, obs_sceneitem_t *entry, void *param) {
				auto **found = static_cast<obs_source_t **>(param);
				auto *child = obs_sceneitem_get_source(entry);
				if (strncmp(obs_source_get_id(child), "obs-textppt-", 12) == 0) *found = obs_source_get_ref(child);
				return true;
			}, &result);
			obs_scene_release(copy);
			return result;
		};
		auto *copy = duplicate(scene);
		check(copy && expect(copy, 1, initialStep), "private scene copies inherit page and reveal progress");
		wait(350);
		const auto copiedPixels = capture(copy);
		const bool samePixels = copy && native_pixels_match(copiedPixels, originalPixels);
		if (!samePixels) {
			const auto stem = path + (byPoints ? ".points" : ".lines");
			originalPixels.save(stem + ".original.png");
			copiedPixels.save(stem + ".copy.png");
			wait(500);
			capture(source).save(stem + ".original-later.png");
			capture(copy).save(stem + ".copy-later.png");
			QTextStream(&report) << "Copy dimensions: " << originalPixels.width() << 'x' << originalPixels.height()
			                    << " / " << copiedPixels.width() << 'x' << copiedPixels.height() << '\n';
		}
		check(samePixels, "copied Chinese text keeps its pixels and canvas dimensions");
		next->click();
		check(expect(source, 1, initialStep + 1) && step(copy) == initialStep,
		      "Dock Next advances preview while the program copy stays unchanged");
		check(dock_highlight(dock, QString::fromUtf8(u8"第二页 甲 乙 丙"), byPoints ? "4/4" : "3/3"),
		      "Dock highlight describes the preview's projected text");
		previous->click();
		check(expect(source, 1, initialStep) && step(copy) == initialStep, "Dock Previous retracts preview only");
		auto *copyBehavior = obs_data_create();
		obs_data_set_string(copyBehavior, "playback_behavior", "stop_restart");
		obs_source_update(copy, copyBehavior);
		obs_data_release(copyBehavior);
		wait(100);
		auto *root = obs_transition_get_active_source(transition);
		auto *nested = duplicate(obs_scene_from_source(root));
		obs_source_release(root); obs_source_release(copy);
		copy = nested;
		check(expect(copy, 1, initialStep), "stop/restart copies retain the live parent's page and partial reveal on first activation");
		wait(300);
		next->click();
		check(expect(source, 1, initialStep + 1) && step(copy) == initialStep,
		      "private copies remain independent after their parent copy is released");
		input->setText("1"); input->setModified(true);
		QMetaObject::invokeMethod(input, "returnPressed", Qt::DirectConnection);
		check(expect(source, 0, 1) && page(copy) == 1, "Dock page entry changes only preview");
		restart->click();
		check(expect(source, 0, 1) && step(copy) == initialStep, "Dock Restart resets preview reveal progress only");
		(byPoints ? lines : points)->setChecked(true);
		wait(300);
		auto *copySettings = obs_source_get_settings(copy);
		check((byPoints ? lines : points)->isChecked() && step(copy) == initialStep &&
		      obs_data_get_bool(copySettings, byPoints ? "progressive_points" : "progressive"),
		      "Dock reveal options change preview without changing the program's mode");
		obs_data_release(copySettings);
		check(native_pixels_match(capture(copy), copiedPixels), "preview navigation and mode edits preserve program pixels");
		(byPoints ? points : lines)->setChecked(true);
		wait(300);
		auto *transferred = duplicate(scene);
		check(expect(transferred, 0, 1) && native_pixels_match(capture(transferred), capture(source)),
		      "a new transition sends the updated preview page and reveal state to program");
		obs_source_release(copy);
		copy = transferred;
		obs_transition_set(transition, sceneSource);
		obs_source_release(copy);
		obs_source_media_restart(source); wait(180);
		int slowest = 0;
		bool repeated = true;
		std::vector<obs_weak_source_t *> retired;
		// Repeated private scenes must not retain old program copies.
		for (int round = 0; round < 100; ++round) {
			QElapsedTimer timer; timer.start();
			copy = duplicate(scene);
			press(next);
			repeated &= expect(source, 0, 2) && expect(copy, 0, 1);
			wait(100);
			press(previous);
			repeated &= expect(source, 0, 1) && step(copy) == 1;
			slowest = std::max(slowest, static_cast<int>(timer.elapsed()));
			retired.push_back(obs_source_get_weak_source(copy));
			obs_source_release(copy);
		}
		check(repeated, "100 copied transitions preserve output while Dock commands edit preview");
		QTextStream(&report) << "Slowest cloned transition batch ms: " << slowest << '\n';
		obs_transition_set(transition, sceneSource);
		wait(300);
		bool released = true;
		for (auto *ref : retired) {
			auto *old = obs_weak_source_get_source(ref);
			released &= old == nullptr;
			obs_source_release(old); obs_weak_source_release(ref);
		}
		check(released, "all 100 retired private sources are released after the output changes");
		next->click();
		check(expect(source, 0, 2), "without source copies, the shared scene uses preview changes directly");
		auto *unrelated = obs_source_duplicate(source, "Studio public copy", false);
		auto *otherScene = obs_scene_create_private("Studio public copy scene");
		obs_scene_add(otherScene, unrelated);
		obs_transition_set(transition, obs_scene_get_source(otherScene)); wait(250);
		next->click();
		check(expect(source, 0, 3) && step(unrelated) == 2,
		      "a locked source never redirects commands to an unrelated public duplicate");
		obs_transition_set(transition, sceneSource);
		obs_scene_release(otherScene); obs_source_remove(unrelated); obs_source_release(unrelated);
		// Private program copies outlive deletion of their preview source.
		copy = duplicate(scene); wait(150);
		obs_sceneitem_addref(item);
		obs_source_remove(source); wait(350);
		check(!lock->isChecked() && selector->currentData().toString() != QString::fromUtf8(obs_source_get_uuid(source)),
		      "deleting the original clears its Dock lock and selection");
		check(expect(copy, 0, 3), "deleting preview leaves the copied program content intact");
		obs_transition_set(transition, sceneSource);
		obs_source_release(copy);
		obs_sceneitem_remove(item);
		obs_sceneitem_release(item);
		obs_source_release(source);
	}
	obs_transition_clear(transition);
	wait(300);
	const uint64_t memoryAfter = textppt_test_memory();
	QTextStream(&report) << "Studio stress memory growth MiB: " << (double(memoryAfter) - double(memoryBefore)) / 1048576.0 << '\n';
	check(memoryAfter < memoryBefore + 128 * 1024 * 1024, "200 cloned transitions stay within the memory growth limit");
	obs_set_output_source(0, oldOutput);
	obs_source_release(oldOutput); obs_source_release(transition);
	lock->setChecked(false);
	obs_frontend_set_preview_program_mode(false);
	obs_frontend_set_current_scene(oldScene);
	obs_source_release(oldScene);
	obs_source_remove(sceneSource); obs_scene_release(scene);
	if (wasStudio) obs_frontend_set_preview_program_mode(true);
	wait(250);
	check(group_studio_probe(kind, report, dock), "native Studio group transition checks complete");
	check(passed, "STUDIO TESTS COMPLETE");
	return passed;
}
