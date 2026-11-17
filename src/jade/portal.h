// Jade's native layer on Linux: the desktop portal (xdg-desktop-portal), over D-Bus.
//
// What a desktop says of itself and shows on a program's behalf, the same way under KDE and
// GNOME: whether it is dark, its accent colour, and its file dialog. libdbus is opened when
// first needed (dlopen), so that ww runs where it is missing; there each answer is "cannot
// be told", and the dialog an error. Elsewhere than Linux, the same.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ww::jade::portal {

// org.freedesktop.appearance's color-scheme: 1 dark, 2 light, 0 no preference; -1 where it
// cannot be told (no D-Bus, no portal, or one too old to say).
int colorScheme();

// org.freedesktop.appearance's accent-color, as 0xRRGGBB; false where the desktop has none.
bool accentColor(uint32_t& rgb);

// Another of the portal's settings that is a string (`org.gnome.desktop.wm.preferences`,
// `button-layout`); false where there is none.
bool textSetting(const std::string& group, const std::string& key, std::string& value);

// Calls `changed` (on a thread of its own) each time the desktop says it has been made light or
// dark, or given another accent colour (the portal's SettingChanged); listened for from the first
// call on, for as long as the program runs.
void watchAppearance(std::function<void()> changed);

// The desktop's dialog for a file to open (kind 0), to save to (1), or a folder (2), as
// Platform::chooseFile asks for it, waited for on the calling thread. `parent` is the window
// it is in front of, as the portal names one ("wayland:<handle>"), or empty.
bool chooseFile(int kind, const std::string& title, const std::vector<std::pair<std::string, std::string>>& filters, const std::string& startDir,
                const std::string& name, const std::string& parent, std::string& path, std::string& error);

}  // namespace ww::jade::portal
