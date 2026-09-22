#pragma once
#include "game/ui/screens/VitaGuideScreen.h"
#include <memory>
#include "game/ui/MenuScreenBase.h"
#include "game/render/VitaPerformanceConfig.h"
#include "engine/VitaCaptureSettings.h"
#include "packaging/vita/vita-controls.h"
#include <algorithm>
#include <functional>
#include <map>
#include <sstream>
#include <fstream>
#include <filesystem>

namespace OpenYAMM::Game
{
inline bool vitaSettingsRequested = false; // Game-thread only.
// The active copy is distinct from edits waiting for restart. Unknown INI keys survive.
namespace VitaSettings
{
inline std::map<std::string, std::string> active, pending;
inline std::string engineSettings;
inline bool initialized = false;
inline bool restartHousePresentation = false;
inline std::string trim(std::string v)
{
    const auto first = v.find_first_not_of(" \t\r");
    if (first == std::string::npos)
        return {};
    return v.substr(first, v.find_last_not_of(" \t\r") - first + 1);
}
inline void publish()
{
    std::string text = "# OpenYAMM 01.06 active settings; pending restart edits excluded\n";
    for (const auto &[k, v] : active)
        text += k + "=" + v + "\n";
    text += "\n# Engine settings at startup\n" + engineSettings;
    Engine::VitaCaptureSettings::publish(text.c_str());
}
inline void initialize()
{
    if (initialized)
        return;
    initialized = true;
    std::ifstream file("/data/vita-performance.ini");
    std::string line;
    while (std::getline(file, line))
    {
        const auto p = line.find('=');
        if (p == std::string::npos || trim(line).starts_with('#'))
            continue;
        active[trim(line.substr(0, p))] = trim(line.substr(p + 1));
    }
    // Values parsed by the engine are authoritative for live fields/defaults.
    const auto &s = vitaPerformanceSettings();
    active["indoor_diagnostics"] = std::to_string(s.indoorDiagnostics);
    active["movement_diagnostics"] = std::to_string(s.movementDiagnostics);
    active["house_diagnostics"] = std::to_string(s.houseDiagnostics);
    active["house_pause_presentation"] = std::to_string(s.housePausePresentation);
    active["fast_opaque_shader"] = std::to_string(s.fastOpaqueShader);
    active["adaptive_world_resolution"] = std::to_string(s.adaptiveWorldResolution);
    active["world_resolution"] = std::to_string(s.worldResolution);
    active["resolution_indicator"] = std::to_string(s.resolutionIndicator);
    active["movement_interpolation"] = std::to_string(s.movementInterpolation);
    active["frame_limit"] = std::to_string(s.frameLimit);
    active["path_progressive"] = std::to_string(s.pathProgressive);
    active["sprite_half_resolution"] = std::to_string(s.spriteHalfResolution);
    active["indoor_vertex_lighting"] = std::to_string(s.indoorVertexLighting);
    if (!active.contains("lock_diagnostics"))
        active["lock_diagnostics"] = "0";
    if (!active.contains("allocator_priority"))
        active["allocator_priority"] = "1";
    OyVitaControls controls; oy_vita_controls_init(&controls);
    for (const auto &d : oy_vita_binding_definitions)
    {
        const auto found=active.find(d.key);
        if (found!=active.end()) oy_vita_control_value(&controls,d.key,found->second.c_str());
    }
    if (active.contains("l_modifier")) oy_vita_control_value(&controls,"l_modifier",active["l_modifier"].c_str());
    for (int i=0;i<OY_VITA_BINDING_COUNT;++i)
        active[oy_vita_binding_definitions[i].key]=std::to_string(controls.bindings[i]);
    active["l_modifier"]=std::to_string(controls.modifierMode);
    pending = active;
    std::ifstream settings("/data/settings.ini");
    engineSettings.assign(std::istreambuf_iterator<char>(settings), {});
    publish();
}
inline bool save()
{
    const std::filesystem::path path("/data/vita-performance.ini"), tmp("/data/vita-performance.ini.tmp"),
        backup("/data/vita-performance.ini.backup");
    std::ofstream stream(tmp, std::ios::binary | std::ios::trunc);
    stream << "# OpenYAMM 01.06 Vita settings. Restart options apply after closing the app.\n";
    for (const auto &[k, v] : pending)
        stream << k << '=' << v << '\n';
    stream.flush();
    if (!stream)
        return false;
    stream.close();
    if (stream.fail())
        return false;
    std::error_code ec;
    std::filesystem::remove(backup, ec);
    ec.clear();
    const bool hadOriginal = std::filesystem::exists(path);
    if (hadOriginal)
    {
        std::filesystem::rename(path, backup, ec);
        if (ec)
            return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        if (hadOriginal)
        {
            std::error_code restore;
            std::filesystem::rename(backup, path, restore);
        }
        return false;
    }
    return true;
}
} // namespace VitaSettings

class VitaSettingsScreen : public MenuScreenBase
{
  public:
    VitaSettingsScreen(const Engine::AssetFileSystem &fs, AppMode returnMode, std::function<void()> close)
        : MenuScreenBase(fs), m_mode(returnMode), m_close(std::move(close))
    {
        VitaSettings::initialize();
        preloadFont("Create");
        preloadFont("Book");
        preloadTexture("vita_guide_paper");
        rebuildLabels();
    }
    AppMode mode() const override
    {
        return m_mode;
    }
    void renderFrame(int width, int height, const GameplayInputFrame &input, float deltaSeconds) override
    {
        if (m_guide)
        {
            m_guide->renderFrame(width, height, input, deltaSeconds);
            if (m_closeGuide)
            {
                m_guide.reset();
                m_closeGuide = false;
                m_waitForRelease = true;
            }
            return;
        }
        MenuScreenBase::renderFrame(width, height, input, deltaSeconds);
    }

  private:
    enum Category
    {
        Controls,
        Graphics,
        Advanced,
        Capture
    };
    struct Option
    {
        const char *key, *label, *values;
        bool live;
        Category category;
        unsigned controlPage;
    };
#define OY_CONTROL_OPTION(key,label,action,group) {key,label,OY_VITA_ACTION_VALUES,false,Controls,group},
    inline static const Option options[] = {
        {"l_modifier", "L shortcut layer", "0|1|2", false, Controls, 2},
        OY_VITA_BINDINGS(OY_CONTROL_OPTION)
        {"capture", "Record diagnostics", "0|1", true, Capture, 0},
        {"movement_diagnostics", "NPC movement breakdown", "0|1", true, Capture, 0},
        {"house_pause_presentation", "Pause house animation", "0|1", true, Capture, 0},
        {"fast_opaque_shader", "Fast opaque shader", "0|1", false, Graphics, 0},
        {"frame_limit", "Gameplay FPS cap", "0|30|20|60", true, Graphics, 0},
        {"sprite_half_resolution", "Half-resolution sprites", "0|1", false, Graphics, 0},
        {"indoor_vertex_lighting", "Simple indoor lighting", "0|1", false, Graphics, 0},
        {"adaptive_world_resolution", "Adaptive world resolution (test)", "0|1", false, Graphics, 0},
        {"world_resolution", "Lower world resolution", "0|1", false, Graphics, 0},
        {"resolution_indicator", "Show world resolution", "0|1", false, Graphics, 0},
        {"movement_interpolation", "Smooth indoor camera (test)", "0|1", false, Controls, 0},
        {"path_progressive", "Responsive NPC paths", "0|1", false, Advanced, 0},
        {"vanilla_distance", "Original draw distance", "0|1", false, Graphics, 0},
        {"geometry_highlight", "Door highlight", "0|1", false, Graphics, 0},
        {"low_texture_filtering", "Fast texture filtering", "0|1", false, Graphics, 0},
        {"opaque_surfaces", "Opaque surface split", "0|1", false, Advanced, 0},
        {"indoor_frame_movement", "Indoor frame movement", "0|1", false, Advanced, 0},
        {"simulation_hz", "Simulation Hz", "64|32|0", false, Advanced, 0},
        {"simulation_max_steps", "Maximum simulation steps", "6|4|3|0", false, Advanced, 0},
        {"ai_lod_period", "AI detail period", "4|0", false, Advanced, 0},
        {"actor_fact_workers", "Actor fact workers (test)", "0|2|3", false, Advanced, 0},
        {"path_workers", "Path workers", "1|2", false, Advanced, 0},
        {"audio_async", "Background sound decoding", "1|0", false, Advanced, 0},
        {"actor_render_lod", "Sprite memory LOD", "0|1", false, Graphics, 0},
        {"terrain_resident_frames", "Resident terrain animation", "1|0", false, Advanced, 0},
        {"terrain_fast_shader", "Fast opaque terrain", "1|0", false, Advanced, 0},
        {"terrain_frustum", "Terrain view rejection", "1|0", false, Advanced, 0},
        {"extra_frustum", "Additional view rejection", "1|0", false, Advanced, 0},
        {"lighting_coalesce", "Lighting reuse (isolation)", "0|1", false, Advanced, 0},
        {"transient_reuse", "Transient buffer reuse", "1|0", false, Advanced, 0},
        {"explicit_buffer_sync", "GPU wait (isolation)", "0|1", false, Advanced, 0},
        {"allocator_priority", "Priority heap handoff", "1|0", false, Advanced, 0},
        {"lock_diagnostics", "Hitch lock-owner tracking", "0|1", false, Capture, 0},
        {"camera_multiplier", "Camera speed", "1|1.5|2|3", false, Controls, 0},
        {"cursor_multiplier", "Cursor speed", "1|1.5|2", false, Controls, 0},
    };
#undef OY_CONTROL_OPTION
    static constexpr uint32_t Ink = 0xff29333bu, RedInk = 0xff293a78u;
    Rect rect(float x, float y, float width, float height) const
    {
        return {m_x + x * m_scale, m_y + y * m_scale, width * m_scale, height * m_scale};
    }
    void text(const std::string &value, float x, float y, bool small = false, uint32_t color = Ink)
    {
        drawText(small ? "Create" : "Book", value, std::round(m_x + x * m_scale), std::round(m_y + y * m_scale), color,
                 m_scale, false);
    }
    void rule(float x, float y, float width)
    {
        static const std::vector<uint8_t> ink{82, 101, 122, 255};
        drawPixelsBgra("__guide_rule", 1, 1, ink, rect(x, y, width, 1));
    }
    bool button(const std::string &label, float x, float y, float width, float height = 44, bool selected = false)
    {
        const Rect box = rect(x, y, width, height);
        const bool hover = hitTest(box);
        static const std::vector<uint8_t> normal{190, 212, 226, 255}, over{134, 174, 199, 255};
        drawPixelsBgra(hover || selected ? "__guide_button_over" : "__guide_button_paper", 1, 1,
                       hover || selected ? over : normal, box);
        rule(x, y, width);
        rule(x, y + height - 1, width);
        text(label, x + (width - measureTextWidth("Book", label, 1)) * 0.5f, y + (height - fontHeight("Book")) * 0.5f);
        return !m_waitForRelease && hover && leftMouseJustReleased();
    }
    void rebuildLabels()
    {
        // Rebuild on entry/edit only, not 60 times a second while reading settings.
        for (size_t i = 0; i < std::size(options); ++i)
        {
            const auto &o = options[i];
            const std::string key = std::string(o.key) == "capture" ? "indoor_diagnostics" : o.key;
            auto found = VitaSettings::pending.find(key);
            std::string value = found == VitaSettings::pending.end() ? "Default" : found->second;
            if (key.starts_with("bind_"))
            {
                const int action = std::atoi(value.c_str());
                value = oy_vita_action_names[std::clamp(action,0,int(OY_ACTION_COUNT)-1)];
            }
            else if (key == "l_modifier")
                value = value == "2" ? "Always" : value == "1" ? "Real-time" : "Inspect only";
            else if (std::string(o.values) == "0|1" || std::string(o.values) == "1|0")
                value = value == "1" ? "On" : value == "0" ? "Off" : value;
            else if (key == "camera_multiplier" || key == "cursor_multiplier")
                value += "x";
            else if (value == "0")
                value = "Off";
            m_values[i] = std::move(value);
            const auto active = VitaSettings::active.find(key);
            m_pending[i] = found != VitaSettings::pending.end() && active != VitaSettings::active.end() &&
                           active->second != found->second;
        }
    }
    void change(const Option &o)
    {
        std::string old = std::string(o.key) == "capture" ? VitaSettings::pending["indoor_diagnostics"]
                                                          : VitaSettings::pending[o.key];
        std::vector<std::string> values;
        std::stringstream list(o.values);
        std::string v;
        while (std::getline(list, v, '|'))
            values.push_back(v);
        auto i = std::find(values.begin(), values.end(), old);
        const std::string next = i == values.end() || ++i == values.end() ? values.front() : *i;
        auto saved = VitaSettings::pending;
        if (std::string(o.key) == "capture")
        {
            VitaSettings::pending["indoor_diagnostics"] = next;
            VitaSettings::pending["house_diagnostics"] = next;
        }
        else
            VitaSettings::pending[o.key] = next;
        if (!VitaSettings::save())
        {
            VitaSettings::pending = std::move(saved);
            m_status = "Could not save settings. No changes applied.";
            return;
        }
        if (o.live)
        {
            // The decoder keeps a diagnostic pointer for the lifetime of a clip.
            // Reopen that presentation on leaving settings; never swap its pointer
            // while the decoder is running.
            if (std::string(o.key) != "frame_limit") VitaSettings::restartHousePresentation = true;
            auto &s = vitaPerformanceSettings();
            if (std::string(o.key) == "frame_limit")
            {
                s.frameLimit = unsigned(std::atoi(next.c_str()));
                VitaSettings::active[o.key] = next;
            }
            else if (std::string(o.key) == "capture")
            {
                s.indoorDiagnostics = s.houseDiagnostics = unsigned(next == "1");
                VitaSettings::active["indoor_diagnostics"] = next;
                VitaSettings::active["house_diagnostics"] = next;
            }
            else if (std::string(o.key) == "movement_diagnostics")
            {
                s.movementDiagnostics = unsigned(next == "1");
                VitaSettings::active[o.key] = next;
            }
            else if (std::string(o.key) == "house_pause_presentation")
            {
                s.housePausePresentation = unsigned(next == "1");
                VitaSettings::active[o.key] = next;
            }
            else
            {
                m_status = "Saved. Close and reopen the app to apply.";
                rebuildLabels();
                return;
            }
            VitaSettings::publish();
            m_status = "Saved and applied.";
        }
        else
            m_status = "Saved. Close and reopen the app to apply.";
        rebuildLabels();
    }
    void drawScreen(float) override
    {
        const bool escape = isScancodeHeld(SDL_SCANCODE_ESCAPE);
        if (!m_waitForRelease && escape && !m_escapeHeld)
        {
            m_close();
            return;
        }
        m_escapeHeld = escape;
        if (m_waitForRelease && !leftMouseDown() && !leftMouseJustReleased() && !escape)
            m_waitForRelease = false;
        m_scale = std::min(frameWidth() / 960.0f, frameHeight() / 544.0f);
        m_x = (frameWidth() - 960 * m_scale) * 0.5f;
        m_y = (frameHeight() - 544 * m_scale) * 0.5f;
        drawTexture("vita_guide_paper", rect(0, 0, 960, 544));
        text("VITA SETTINGS", 60, 36, false, RedInk);
        text("01.06  |  Tap a value to change it", 460, 40, true);
        rule(60, 74, 840);
        static const char *tabs[] = {"Controls", "Graphics", "Advanced", "Capture"};
        for (unsigned i = 0; i < 4; ++i)
            if (button(tabs[i], 60 + i * 214, 88, 198, 42, m_category == i))
            {
                m_category = static_cast<Category>(i);
                m_page = 0;
                m_waitForRelease = true;
                return;
            }
        if (m_category == Controls)
        {
            static const char *pages[] = {"Look / pointer", "D-pad", "L shortcuts", "Reference"};
            for (unsigned i=0;i<4;++i)
                if (button(pages[i],60+i*214,144,198,36,m_controlPage==i))
                { m_controlPage=i; m_page=0; m_waitForRelease=true; return; }
        }
        const unsigned rowsPerPage = 6;
        unsigned count = 0, row = 0;
        for (size_t i = 0; i < std::size(options); ++i)
        {
            const auto &o = options[i];
            if (o.category != m_category || (m_category == Controls && o.controlPage != m_controlPage))
                continue;
            const unsigned index = count++;
            if (index < m_page * rowsPerPage || index >= (m_page + 1) * rowsPerPage)
                continue;
            const bool controls = m_category == Controls;
            const float y = (controls ? 198 : 148) + row++ * (controls ? 36 : 44);
            text(o.label, 72, y + (controls ? 3 : 7));
            text(m_pending[i] ? "Restart pending" : o.live ? "Applies now" : "Restart",
                 controls ? 514 : 570, y + (controls ? 7 : 11), true, RedInk);
            if (button(m_values[i], controls ? 666 : 752, y, controls ? 226 : 140, controls ? 32 : 39))
            {
                change(o);
                m_waitForRelease = true;
                return;
            }
        }
        if (m_category == Controls && m_controlPage == 3)
        {
            text("Left stick: move   |   Right stick: look   |   Triangle: jump",72,202,true);
            text("X: interact   |   Square: attack   |   Circle: quick spell",72,231,true);
            text("L alone: inspect   |   R: inventory   |   Select: combat mode",72,260,true);
            text("Start: menu   |   L + Select: diagnostic marker",72,289,true);
            text("Menus: left stick / D-pad moves pointer; X clicks; Circle inspects.",72,318,true);
            text("Spellbook: X selects a learned spell; Triangle sets / clears it.",72,347,true);
            text("Set it for each character. Circle casts it during gameplay.",72,376,true);
        }
        else if (m_category == Controls && m_controlPage == 0)
        {
            text("D-pad and L shortcuts have their own editable pages.",72,330,true);
            text("L alone still inspects. Hold L before pressing a shortcut button.",72,360,true);
            text("Real-time = shortcuts outside turn-based mode; Always = both modes.",72,390,true);
        }
        else if (m_category == Controls && m_controlPage == 1)
        {
            text("Gameplay only; menu navigation is unchanged.",72,365,true);
            text("Fly up / down / land still require an active Fly spell.",72,395,true);
        }
        text(m_status, 60, 428, true, RedInk);
        rule(60, 452, 840);
        if (button("Guide", 60, 466, 160))
        {
            m_guide = std::make_unique<VitaGuideScreen>(
                assetFileSystem(), m_mode, [this]() { m_closeGuide = true; }, "Back to settings");
            return;
        }
        if (m_page > 0 && button("Previous", 242, 466, 150))
        {
            --m_page;
            m_waitForRelease = true;
            return;
        }
        if ((m_page + 1) * rowsPerPage < count && button("Next", 414, 466, 140))
        {
            ++m_page;
            m_waitForRelease = true;
            return;
        }
        if (count > rowsPerPage)
            text(std::to_string(m_page + 1) + " / " + std::to_string((count + rowsPerPage - 1) / rowsPerPage),
                 580, 480, true);
        if (button("Back", 710, 466, 190))
        {
            m_close();
            return;
        }
    }
    std::unique_ptr<VitaGuideScreen> m_guide;
    bool m_closeGuide = false, m_waitForRelease = true;
    AppMode m_mode;
    std::function<void()> m_close;
    unsigned m_page = 0, m_controlPage = 0;
    Category m_category = Controls;
    bool m_escapeHeld = false;
    float m_scale = 1, m_x = 0, m_y = 0;
    std::array<std::string, std::size(options)> m_values;
    std::array<bool, std::size(options)> m_pending{};
    std::string m_status = "Changes are saved automatically. Restart options need an app restart.";
};
} // namespace OpenYAMM::Game
