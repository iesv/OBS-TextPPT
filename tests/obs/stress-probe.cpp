// OBS-TextPPT, GPL-2.0-or-later. Runs only in an isolated test host.
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QEventLoop>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <QTemporaryDir>
#include <QApplication>
#include <QComboBox>
#include <QWidget>
#include <QToolButton>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QUuid>
#include <algorithm>
#include <string>
#include <vector>
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <mach/mach.h>
#endif

static void wait(int milliseconds)
{
	QEventLoop loop;
	QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
	loop.exec();
	// Dispatch deferred cleanup between simulated user actions.
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
uint64_t textppt_test_memory()
{
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS_EX counters = {};
	if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters)))
		return counters.PrivateUsage;
#else
	// Measure the process footprint, excluding shared mapped code.
	task_vm_info_data_t info = {};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
		return info.phys_footprint;
#endif
	return 0;
}
static int index(obs_source_t *source)
{
	calldata_t data = {};
	proc_handler_call(obs_source_get_proc_handler(source), "get_status", &data);
	const int result = static_cast<int>(calldata_int(&data, "index"));
	calldata_free(&data);
	return result;
}
static int64_t jump(obs_source_t *source, int page)
{
	calldata_t data = {};
	calldata_set_int(&data, "index", page);
	proc_handler_call(obs_source_get_proc_handler(source), "dock_transition", &data);
	const int64_t serial = calldata_int(&data, "serial");
	calldata_free(&data);
	return serial;
}
void textppt_stress_probe(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
		return;
	QTextStream report(&file);
	bool passed = true;
	const auto check = [&](bool valid, const char *message) {
		passed &= valid;
		report << (valid ? "PASS: " : "FAIL: ") << message << '\n';
		report.flush();
		return valid;
	};
#ifdef _WIN32
	const char *kind = "obs-textppt-gdiplus";
#else
	const char *kind = "obs-textppt-ft2";
#endif
	// Keep stress geometry fixed while testing native defaults separately.
	auto *defaults = obs_get_source_defaults(kind);
	auto *defaultFont = obs_data_get_obj(defaults, "font");
	auto *fixtureFont = obs_data_get_defaults(defaultFont);
	obs_data_apply(fixtureFont, defaultFont);
	obs_data_set_int(fixtureFont, "size", 48);
	obs_data_release(defaultFont);
	obs_data_release(defaults);
	wait(100);
	const uint64_t baseline = textppt_test_memory();
	check(baseline != 0, "process memory is readable");
	std::string text;
	for (int page = 0; page < 1000; ++page)
		text += "Stress page " + std::to_string(page + 1) + "\tRight column\nSecond line\tEnd\n\n";
	auto *settings = obs_data_create();
	obs_data_set_obj(settings, "font", fixtureFont);
	obs_data_set_string(settings, "custom_text", text.c_str());
	obs_data_set_string(settings, "transition", "cut");
	obs_data_set_string(settings, "slide_mode", "mode_manual");
	obs_data_set_string(settings, "use_custom_size", "640x360");
	auto *source = obs_source_create(kind, "TextPPT Stress", settings, nullptr);
	obs_data_release(settings);
	auto *scene = obs_frontend_get_current_scene();
	auto *item = source && scene ? obs_scene_add(obs_scene_from_source(scene), source) : nullptr;
	if (!item) {
		check(false, "stress source is created");
		obs_source_release(source); obs_source_release(scene);
		obs_data_release(fixtureFont);
		check(false, "STRESS TEST COMPLETE");
		return;
	}
	wait(1000);
	calldata_t data = {};
	proc_handler_call(obs_source_get_proc_handler(source), "get_status", &data);
	check(calldata_int(&data, "count") == 1000, "1000-page TAB deck loads");
	calldata_free(&data);
	uint64_t peak = textppt_test_memory();
	const auto memoryReport = [&](const char *phase) {
		const auto memory = textppt_test_memory();
		peak = std::max(peak, memory);
		report << "Memory MiB after " << phase << ": " << double(memory) / 1048576.0 << '\n';
		report << "Peak MiB: " << double(peak) / 1048576.0 << '\n';
		report << "Qt widgets: " << QApplication::allWidgets().size() << '\n';
#ifndef _WIN32
		task_vm_info_data_t info = {};
		mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
		if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
			report << "Resident MiB: " << double(info.resident_size) / 1048576.0 << '\n';
#endif
		report.flush();
	};
	memoryReport("source creation");
	int64_t lastSerial = 0;
	// Bursts exceed the queue bound; only the latest command must be retained.
	for (int batch = 0; batch < 100; ++batch) {
		for (int command = 0; command < 100; ++command)
			lastSerial = jump(source, (batch * 100 + command) % 1000);
		wait(25);
		peak = std::max(peak, textppt_test_memory());
	}
	wait(500);
	data = {};
	proc_handler_call(obs_source_get_proc_handler(source), "get_status", &data);
	check(calldata_int(&data, "acknowledged") >= lastSerial && index(source) == 999,
	      "10000 rapid TAB-page jumps acknowledge the latest page");
	calldata_free(&data);
	memoryReport("page jumps");
	for (int command = 0; command < 100; ++command) obs_source_media_next(source);
	wait(150);
	check(index(source) == 999, "rapid next-page commands stop at the last page");
	jump(source, 0); wait(150);
	for (int command = 0; command < 100; ++command) obs_source_media_previous(source);
	wait(150);
	check(index(source) == 0, "rapid previous-page commands stop at the first page");
	jump(source, 499); wait(150);
	const char *transitions[] = {"cut", "fade", "swipe", "slide"};
	for (int change = 0; change < 100; ++change) {
		settings = obs_data_create();
		obs_data_set_string(settings, "transition", transitions[change % 4]);
#ifdef _WIN32
		obs_data_set_int(settings, "color", 0xFFFFFF - change);
#else
		obs_data_set_int(settings, "color1", 0xFFFFFF - change);
#endif
		obs_source_update(source, settings);
		obs_data_release(settings);
		wait(40);
		peak = std::max(peak, textppt_test_memory());
	}
	wait(250);
	check(index(source) == 499 && obs_source_media_get_state(source) == OBS_MEDIA_STATE_PAUSED,
	      "100 settings changes retain the page and paused state");
	memoryReport("transition and color changes");
	settings = obs_source_get_settings(source);
	auto *font = obs_data_get_obj(settings, "font");
	auto *fontCopy = obs_data_get_defaults(font);
	obs_data_apply(fontCopy, font);
	obs_data_release(font);
	obs_data_release(settings);
	const auto rendererIds = [&] {
		std::vector<std::string> ids;
		obs_source_enum_full_tree(source, [](obs_source_t *, obs_source_t *child, void *data) {
			const char *id = obs_source_get_unversioned_id(child);
			if (strcmp(id, "text_gdiplus") == 0 || strcmp(id, "text_ft2_source") == 0)
				static_cast<std::vector<std::string> *>(data)->emplace_back(obs_source_get_uuid(child));
		}, &ids);
		return ids;
	};
	const auto fontRendererIds = rendererIds();
	QElapsedTimer fontTimer;
	fontTimer.start();
	for (int change = 0; change < 120; ++change) {
		obs_data_set_int(fontCopy, "size", change % 3 == 0 ? 24 : change % 3 == 1 ? 48 : 96);
		obs_data_set_int(fontCopy, "flags", change % 2 ? OBS_FONT_BOLD : 0);
		settings = obs_data_create();
		obs_data_set_obj(settings, "font", fontCopy);
		obs_source_update(source, settings);
		obs_data_release(settings);
		wait(40);
		peak = std::max(peak, textppt_test_memory());
	}
	obs_data_release(fontCopy);
	wait(500);
	check(index(source) == 499 && obs_source_media_get_state(source) == OBS_MEDIA_STATE_PAUSED,
	      "120 TAB font-size and weight changes retain playback state");
	check(!fontRendererIds.empty() && rendererIds() == fontRendererIds,
	      "120 TAB font edits reuse the displayed native renderer");
	bool expanded = false;
	obs_source_enum_full_tree(source, [](obs_source_t *, obs_source_t *child, void *data) {
		const char *id = obs_source_get_unversioned_id(child);
		if (strcmp(id, "text_gdiplus") && strcmp(id, "text_ft2_source")) return;
		auto *settings = obs_source_get_settings(child);
		const std::string text = obs_data_get_string(settings, "text");
		*static_cast<bool *>(data) |= text.find("Right column") != std::string::npos && text.find('\t') == std::string::npos;
		obs_data_release(settings);
	}, &expanded);
	check(expanded, "TAB pages remain expanded after repeated native font changes");
	report << "120 TAB font changes ms: " << fontTimer.elapsed() << '\n';
	memoryReport("font changes");
	const uint64_t beforeCycles = textppt_test_memory();
	for (int cycle = 0; cycle < 100; ++cycle) {
		settings = obs_data_create();
		obs_data_set_string(settings, "custom_text", "first\n\nsecond\n\nthird");
		obs_data_set_obj(settings, "font", fixtureFont);
		auto *temporary = obs_source_create(kind, "TextPPT Stress Temporary", settings, nullptr);
		obs_data_release(settings);
		if (!temporary) { check(false, "temporary source is created"); break; }
		wait(30);
		obs_source_remove(temporary); obs_source_release(temporary);
		wait(30);
		peak = std::max(peak, textppt_test_memory());
	}
	wait(1000);
	const uint64_t afterCycles = textppt_test_memory();
	check(afterCycles <= beforeCycles + 64 * 1024 * 1024ull, "create/delete cycles stay within the memory growth limit");
	memoryReport("create/delete cycles");
	QTemporaryDir files;
	check(files.isValid(), "file reload stress directory is created");
	if (files.isValid()) {
		const auto path = files.path() + "/reload.txt";
		bool reloaded = true;
		for (int cycle = 0; cycle < 100; ++cycle) {
			QFile input(path);
			const auto text = QString("Reload %1\tRight\n\nlast\tEnd").arg(cycle).toUtf8();
			reloaded &= input.open(QIODevice::WriteOnly) && input.write(text) == text.size();
			input.close();
			if (cycle == 0) {
				settings = obs_data_create();
				obs_data_set_string(settings, "input_mode", "file");
				obs_data_set_string(settings, "txt_file", path.toUtf8().constData());
				obs_source_update(source, settings);
				obs_data_release(settings);
			} else {
				data = {};
				reloaded &= proc_handler_call(obs_source_get_proc_handler(source), "reload_files", &data);
				calldata_free(&data);
			}
			wait(25);
			peak = std::max(peak, textppt_test_memory());
		}
		wait(500);
		std::vector<std::string> content;
		data = {};
		calldata_set_ptr(&data, "texts", &content);
		proc_handler_call(obs_source_get_proc_handler(source), "get_texts", &data);
		calldata_free(&data);
		check(reloaded && content == std::vector<std::string>{"Reload 99\tRight", "last\tEnd"},
		      "100 TAB file reloads retain the latest original content");
	}
	auto *a = obs_scene_create("TextPPT Stress A");
	memoryReport("file reloads");
	auto *b = obs_scene_create("TextPPT Stress B");
	settings = obs_data_create();
	obs_data_set_string(settings, "custom_text", "Scene B");
	obs_data_set_obj(settings, "font", fixtureFont);
	auto *other = obs_source_create(kind, "TextPPT Stress Other", settings, nullptr);
	obs_data_release(settings);
	auto *previousTransition = obs_frontend_get_current_transition();
	obs_frontend_source_list transitionsList = {};
	obs_frontend_get_transitions(&transitionsList);
	for (size_t i = 0; i < transitionsList.sources.num; ++i)
		if (strcmp(obs_source_get_id(transitionsList.sources.array[i]), "cut_transition") == 0)
			obs_frontend_set_current_transition(transitionsList.sources.array[i]);
	obs_frontend_source_list_free(&transitionsList);
	// Compare scene-switch pressure with the platform's native text source.
	const char *nativeKind = obs_get_latest_input_type_id(
#ifdef _WIN32
		"text_gdiplus"
#else
		"text_ft2_source"
#endif
	);
	settings = obs_data_create();
	obs_data_set_obj(settings, "font", fixtureFont);
	obs_data_set_string(settings, "text", "Native scene control");
	auto *nativeA = nativeKind ? obs_source_create(nativeKind, "TextPPT Native A", settings, nullptr) : nullptr;
	auto *nativeB = nativeKind ? obs_source_create(nativeKind, "TextPPT Native B", settings, nullptr) : nullptr;
	obs_data_release(settings);
	auto *controlA = obs_scene_create("TextPPT Native Scene A");
	auto *controlB = obs_scene_create("TextPPT Native Scene B");
	if (check(nativeA && nativeB && controlA && controlB, "native scene-switch control fixtures are created")) {
		obs_scene_add(controlA, nativeA);
		obs_scene_add(controlB, nativeB);
		wait(250);
		const auto nativeBaseline = textppt_test_memory();
		auto nativePeak = nativeBaseline;
		for (int cycle = 0; cycle < 250; ++cycle) {
			obs_frontend_set_current_scene(obs_scene_get_source(cycle % 2 ? controlB : controlA));
			wait(25);
			nativePeak = std::max(nativePeak, textppt_test_memory());
		}
		check(nativePeak <= nativeBaseline + 128 * 1024 * 1024ull, "250 native scene switches stay within the memory growth limit");
		report << "Native scene-switch delta MiB: " << double(nativePeak - nativeBaseline) / 1048576.0 << '\n';
		peak = std::max(peak, nativePeak);
	}
	obs_frontend_set_current_scene(scene);
	wait(250);
	for (auto *control : {controlA, controlB}) {
		if (control) obs_source_remove(obs_scene_get_source(control));
		obs_scene_release(control);
	}
	for (auto *native : {nativeA, nativeB}) {
		obs_source_remove(native);
		obs_source_release(native);
	}
	wait(250);
	memoryReport("native scene-switch control");
	const bool scenesReady = a && b && other;
	check(scenesReady, "scene-switch stress fixtures are created");
	if (scenesReady) {
		obs_scene_add(a, source);
		obs_scene_add(b, other);
		for (int cycle = 0; cycle < 250; ++cycle) {
			obs_frontend_set_current_scene(obs_scene_get_source(cycle % 2 ? b : a));
			wait(25);
			peak = std::max(peak, textppt_test_memory());
		}
		wait(500);
		QComboBox *selector = nullptr;
		for (auto *widget : QApplication::allWidgets())
			if (widget->objectName() == "TextPPTSourceList") selector = qobject_cast<QComboBox *>(widget);
		check(selector && selector->currentData().toString() == QString::fromUtf8(obs_source_get_uuid(other)),
		      "250 scene switches select the final scene's source");
		memoryReport("following scene switches");
		QToolButton *sourceLock = nullptr;
		QToolButton *follow = nullptr;
		QWidget *dock = nullptr;
		for (auto *widget : QApplication::allWidgets()) {
			if (widget->objectName() == "TextPPTSourceLock") sourceLock = qobject_cast<QToolButton *>(widget);
			if (widget->objectName() == "TextPPTFollowScene") follow = qobject_cast<QToolButton *>(widget);
			if (widget->objectName() == "TextPPTDock") dock = widget;
		}
		if (check(selector && sourceLock && follow && dock, "lock stress controls are available")) {
			const QString otherUuid = QString::fromUtf8(obs_source_get_uuid(other));
			sourceLock->setChecked(true);
			bool retained = true;
			for (int cycle = 0; cycle < 500; ++cycle) {
				obs_frontend_set_current_scene(obs_scene_get_source(cycle % 2 ? b : a));
				if (cycle % 10 == 0) {
					const auto name = "TextPPT Locked " + std::to_string(cycle);
					obs_source_set_name(other, name.c_str());
				}
				if (cycle % 50 == 0) { dock->hide(); dock->show(); }
				wait(15);
				retained &= sourceLock->isChecked() && selector->currentData().toString() == otherUuid && !selector->isEnabled();
				peak = std::max(peak, textppt_test_memory());
			}
			check(retained && follow->isChecked(), "500 locked scene switches, 50 renames and 10 reopenings retain the source and eligibility");
			memoryReport("locked scene switches");
			obs_frontend_set_current_scene(obs_scene_get_source(a)); wait(200);
			sourceLock->setChecked(false);
			check(selector->isEnabled() && selector->currentData().toString() == QString::fromUtf8(obs_source_get_uuid(source)),
			      "unlocking after the scene burst immediately restores following");
			bool deleted = true;
			for (int cycle = 0; cycle < 50; ++cycle) {
				settings = obs_data_create();
				obs_data_set_string(settings, "custom_text", "Locked temporary\tContent");
				obs_data_set_obj(settings, "font", fixtureFont);
				auto *temporary = obs_source_create(kind, "TextPPT Lock Delete", settings, nullptr);
				obs_data_release(settings);
				if (!temporary) { deleted = false; break; }
				wait(50);
				const int row = selector->findData(QString::fromUtf8(obs_source_get_uuid(temporary)));
				if (row < 0) { deleted = false; obs_source_remove(temporary); obs_source_release(temporary); break; }
				selector->setCurrentIndex(row);
				sourceLock->setChecked(true);
				obs_source_remove(temporary); obs_source_release(temporary);
				wait(50);
				deleted &= !sourceLock->isChecked() && selector->isEnabled() &&
					selector->currentData().toString() == QString::fromUtf8(obs_source_get_uuid(source));
				peak = std::max(peak, textppt_test_memory());
			}
			check(deleted, "50 locked-source deletions release the lock without stale selection");
			memoryReport("locked-source deletions");
		}
	}
	obs_frontend_set_current_scene(scene); wait(200);
	if (previousTransition) obs_frontend_set_current_transition(previousTransition);
	obs_source_release(previousTransition);
	for (auto *fixture : {a, b}) {
		if (fixture) obs_source_remove(obs_scene_get_source(fixture));
		obs_scene_release(fixture);
	}
	obs_source_remove(other); obs_source_release(other);
	obs_sceneitem_remove(item); obs_source_remove(source);
	obs_source_release(source); obs_source_release(scene);
	wait(500);
	char *collection = obs_frontend_get_current_scene_collection();
	const std::string originalCollection = collection ? collection : "";
	bfree(collection);
	const auto temporaryCollection = QStringLiteral("TextPPT Lock %1").arg(QUuid::createUuid().toString(QUuid::Id128)).toUtf8();
	const auto clearCollectionFixture = [] {
		auto *currentScene = obs_frontend_get_current_scene();
		auto *current = currentScene ? obs_scene_from_source(currentScene) : nullptr;
		if (auto *item = current ? obs_scene_find_source(current, "TextPPT Collection Lock") : nullptr) {
			auto *previous = obs_source_get_ref(obs_sceneitem_get_source(item));
			obs_sceneitem_remove(item);
			obs_source_remove(previous);
			obs_source_release(previous);
		}
		obs_source_release(currentScene);
	};
	const bool addedCollection = !originalCollection.empty() && obs_frontend_add_scene_collection(temporaryCollection.constData());
	if (check(addedCollection, "collection-switch lock fixtures are created")) {
		bool reset = true;
		for (int cycle = 0; cycle < 10; ++cycle) {
			// Keep one fixture per collection.
			clearCollectionFixture();
			wait(100);
			settings = obs_data_create();
			obs_data_set_string(settings, "custom_text", "Collection lock\tContent");
			obs_data_set_obj(settings, "font", fixtureFont);
			auto *temporary = obs_source_create(kind, "TextPPT Collection Lock", settings, nullptr);
			obs_data_release(settings);
			auto *currentScene = obs_frontend_get_current_scene();
			auto *currentItem = temporary && currentScene ? obs_scene_add(obs_scene_from_source(currentScene), temporary) : nullptr;
			obs_source_release(currentScene);
			if (!currentItem) { reset = false; obs_source_release(temporary); break; }
			wait(100);
			QComboBox *selector = nullptr;
			QToolButton *sourceLock = nullptr;
			for (auto *widget : QApplication::allWidgets()) {
				if (widget->objectName() == "TextPPTSourceList") selector = qobject_cast<QComboBox *>(widget);
				if (widget->objectName() == "TextPPTSourceLock") sourceLock = qobject_cast<QToolButton *>(widget);
			}
			const QString uuid = QString::fromUtf8(obs_source_get_uuid(temporary));
			const int row = selector ? selector->findData(uuid) : -1;
			if (row >= 0 && sourceLock) { selector->setCurrentIndex(row); sourceLock->setChecked(true); }
			else reset = false;
			// OBS must own the last reference when clearing a scene collection.
			obs_source_release(temporary);
			obs_frontend_set_current_scene_collection(cycle % 2 ? temporaryCollection.constData() : originalCollection.c_str());
			wait(250);
			reset &= sourceLock && !sourceLock->isChecked() && selector && selector->currentData().toString() != uuid;
			peak = std::max(peak, textppt_test_memory());
			memoryReport("collection switch");
		}
		obs_frontend_set_current_scene_collection(originalCollection.c_str());
		wait(250);
		check(reset, "10 collection switches release source locks and discard old selections");
		clearCollectionFixture();
		wait(500);
	}
	memoryReport("scene and collection changes");
	check(peak <= baseline + 128 * 1024 * 1024ull, "peak memory growth stays below 128 MiB");
	obs_data_release(fixtureFont);
	report << "Baseline MiB: " << double(baseline) / 1048576.0 << '\n'
	       << "Peak MiB: " << double(peak) / 1048576.0 << '\n'
	       << "Create/delete delta MiB: " << (double(afterCycles) - double(beforeCycles)) / 1048576.0 << '\n';
	check(passed, "STRESS TEST COMPLETE");
}
