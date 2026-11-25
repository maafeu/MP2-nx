#include <algorithm>
#include <iterator>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "audio/output.h"
#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"

namespace game {

namespace {

// port 0 has the touch controls, the keyboard and mouse; the game sees no input while the panel is open
void Pad(int port, input::Pad& pad) {
    input::SeenPad(pad);
    // Minus + R3 opens and closes the panel (the game's own menu has no entry for it): input::PanelChords
    // (core/panel_chords.h) in the hosts' Poll, before the pad is emptied while the panel is open (checked here, on the
    // emptied pad, it never closed it)
    if (port == 0) pad.connected = true;
    if (app::MenuOpen()) pad = input::Pad{.connected = pad.connected};
    else if (port == 0) input::Keyboard(pad);
}

// no vibration while the keyboard and mouse are in use
void Rumble(int port, uint16_t& left, uint16_t& right) {
    if (port != 0 || !input::KeyboardActive()) return;
    left = 0;
    right = 0;
}

// what the options list cannot hold
void Load(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);
    Options& o = Opt();
    o.mouse_sensitivity = float(t["mouse_sensitivity"].value_or(double(o.mouse_sensitivity)));
    // the touch screen followed the mouse's before it had its own: a player's choice carries over
    o.touch_sensitivity = float(t["touch_sensitivity"].value_or(double(o.mouse_sensitivity)));
    o.left_sensitivity = float(t["left_stick_sensitivity"].value_or(double(o.left_sensitivity)));
    o.right_sensitivity = float(t["right_stick_sensitivity"].value_or(double(o.right_sensitivity)));
    std::string language = t["language"].value_or(o.language);
    bool known = std::find(std::begin(kLanguages), std::end(kLanguages), language) != std::end(kLanguages);
    if (known) o.language = language;
    if (const toml::table* keys = t["keys"].as_table())
        for (auto&& [id, value] : *keys)
            if (auto v = value.value<std::string>()) o.keys[std::string(id.str())] = *v;
    // a test aid, never saved (options.h)
    o.test_level = uint32_t(t["test_level"].value_or(int64_t(0)));
    o.test_level_command = t["test_level_command"].value_or(o.test_level_command);
    o.test_level_after = uint32_t(t["test_level_after"].value_or(int64_t(0)));
    o.test_levels = t["test_levels"].value_or(o.test_levels);
    o.test_level_every = uint32_t(t["test_level_every"].value_or(int64_t(o.test_level_every)));
    o.test_gpu_clocks = t["test_gpu_clocks"].value_or(o.test_gpu_clocks);
    o.test_script = t["test_script"].value_or(o.test_script);
    o.test_native = t["test_native"].value_or(o.test_native);
    o.test_shadow = t["test_shadow"].value_or(o.test_shadow);
}

// after the settings load, before boot
void Init() {
    BootLanguage();  // latched before the options can change the setting
    input::LoadBindings();
    audio::SetOutputMode(Opt().audio_output);
}

void Save(void* table) {
    toml::table& t = *static_cast<toml::table*>(table);
    const Options& o = Opt();
    t.insert("mouse_sensitivity", double(o.mouse_sensitivity));
    t.insert("touch_sensitivity", double(o.touch_sensitivity));
    t.insert("left_stick_sensitivity", double(o.left_sensitivity));
    t.insert("right_stick_sensitivity", double(o.right_sensitivity));
    t.insert("language", o.language);
    toml::table keys;
    for (const auto& [id, value] : o.keys) keys.insert(id, value);
    t.insert("keys", keys);
}

}  // namespace

Options& Opt() {
    static Options o;
    return o;
}

const std::string& BootLanguage() {
    static const std::string language = Opt().language;
    return language;
}

}  // namespace game

const app::GameInfo app::kGame = {
    .title = L"Max Payne 2",
    .short_title = L"Max Payne 2",
    .name = L"MaxPayne2",
    .log = "mp2",
    .settings = "mp2-nx.toml",
    .splash = nullptr,
    .missing = L"Put the game in the mp2-nx folder: its disc image (.iso), or default.xbe and the disc folders beside it.",
    .images = {{"default.xbe", "MaxPayne2"}},
    .options = {{"fps_counter", &game::Opt().fps_counter, nullptr},
                {"screen_filter", nullptr, &game::Opt().screen_filter},
                {"brightness", nullptr, &game::Opt().brightness},
                {"render_wide", nullptr, &game::Opt().render_wide},
                {"oled_screen", nullptr, &game::Opt().oled},
                {"left_stick_deadzone", nullptr, &game::Opt().left_deadzone},
                {"right_stick_deadzone", nullptr, &game::Opt().right_deadzone},
                {"audio_output", nullptr, &game::Opt().audio_output}},
    .load = game::Load,
    .save = game::Save,
    .init = game::Init,
    .loaded = nullptr,
    .window = nullptr,
    .message = nullptr,
    .ui = app::DrawUi,
    .pad = game::Pad,
    .rumble = game::Rumble,
};
