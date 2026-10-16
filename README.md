# OBS-TextPPT

Lightweight text slideshows for OBS Studio.
Windows x64 and macOS Apple Silicon. US English, Simplified Chinese, Traditional Chinese.
Author: [haliluya.net](https://haliluya.net).

Use custom text, UTF-8 files, existing text sources, or groups. Blank lines separate pages. Tabs provide approximate columns; use left alignment. Text styles use native GDI+ on Windows and FreeType 2 on Mac.

Control pages from **Docks > TextPPT** or **Settings > Hotkeys**. **Lines** expands each page by line; **Points** expands TAB cells and lines. Choose one mode or leave both off. The Dock keeps page summaries and highlights projected text in yellow with progress. Both choices default to off and are saved per source. The chain button toggles **Follow scene**; the lock keeps the current source selected. Right-click to edit text or reload files.

In Studio Mode, click a page to preview it. If its source is elsewhere, the Dock switches preview to a scene where it is visible; shared sources offer a scene menu. Hidden or unattached sources show a short hint. Double-click to use the current OBS transition once the scene and page are ready. Rapid repeats are ignored. Changing pages, sources or scenes cancels a pending request. Enable **Duplicate Scene** and **Duplicate Sources** to copy the current page and reveal progress to program. A new copy retains that position on its first activation; later activations follow its visibility setting.

Referenced text sources, groups and their media remain shared. Editing linked content or advancing a shared slideshow affects every view using it.

The editor opens at 16:9 and fits the screen. Use its size slider, Ctrl + wheel or +/- to zoom, and Ctrl + 0 to reset. Use Command on Mac. TAB columns scale with the reading font. This changes only the editor view. Scrollbars, page numbers and progress use the OBS theme.

Group pages with visible image slideshows show separate image controls. Multiple slideshows offer a target menu. Set the image slideshow to manual mode for manual control.

## Install

Download and extract a ZIP from [Releases](https://github.com/iesv/OBS-TextPPT/releases). Close OBS first.

- **Windows:** Run `install_obs-textppt.bat` as administrator.
- **macOS arm64:** Run `bash ./install_obs-textppt.sh` without sudo. The installer clears the plugin's quarantine attribute.

Installers replace old plugin files and preserve OBS settings. Restart OBS and add a **TextPPT** source.

## License

GPL-2.0-or-later. Derived from [Joshua Wong's obs-text-slideshow](https://github.com/jbwong05/obs-text-slideshow); original copyright retained. Build support: [OBS plugin template](https://github.com/obsproject/obs-plugintemplate).
