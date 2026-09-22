#pragma once

#include "game/ui/MenuScreenBase.h"
#include "game/ui/screens/VitaGuideCopy.h"
#include "game/ui/screens/VitaGuideLayout.h"
#include <algorithm>
#include <functional>
#include <cmath>

namespace OpenYAMM::Game
{
inline bool vitaGuideRequested = false;

class VitaGuideScreen : public MenuScreenBase
{
  public:
    VitaGuideScreen(const Engine::AssetFileSystem &fs, AppMode returnMode, std::function<void()> close,
                    std::string backLabel = "Back")
        : MenuScreenBase(fs), m_mode(returnMode), m_close(std::move(close)), m_backLabel(std::move(backLabel))
    {
        preloadFont("Create");
        preloadFont("Book");
        preloadTexture("vita_guide_paper");
        preloadTexture("vita_guide_journey");
    }
    AppMode mode() const override
    {
        return m_mode;
    }

  private:
    static constexpr uint32_t Ink = 0xff29333bu;
    static constexpr uint32_t RedInk = 0xff293a78u;
    struct Sheet
    {
        int topic = 0;
        std::vector<std::string> lines, hint;
        std::string number, chapter;
    };
    struct Illustration
    {
        const char *asset;
        const char *caption;
        const char *hint;
    };
    inline static constexpr Illustration illustrations[] = {
        {"vita_guide_journey", "A shared adventure", "Explore at your own pace."},
        {"book1", "Your field reference", "Use touch or pointer + X."},
        {"2hsword1", "Time to think", "Select changes combat mode."},
        {"2hsword1", "Choose your battles", "L inspects. Square attacks."},
        {"book4", "The spellbook", "Circle casts your quick spell."},
        {"amulet1", "Equip your party", "R opens your inventory."},
        {"bottle23", "A little alchemy", "Reagent + empty bottle."},
        {"bottle24", "Use with care", "Read an item's effects first."},
        {"vita_guide_journey", "Better together", "Read the offer before hiring."},
        {"book3", "Knowledge is power", "Levels and mastery differ."},
        {"bottle23", "Keep everyone ready", "Check health and conditions."},
        {"item203", "Leave yourself a clue", "Read your journal often."},
        {"item206", "Spend wisely", "Keep some gold in reserve."},
        {"book5", "Look carefully", "Some chests are trapped."},
        {"vita_guide_journey", "Beyond the horizon", "Keep more than one save."},
        {"book1", "A game within a game", "Read each card before playing."}};

    Rect rect(float x, float y, float w, float h) const
    {
        return {m_x + x * m_scale, m_y + y * m_scale, w * m_scale, h * m_scale};
    }
    // Native bitmap sizes at 960x544: no 1.25/1.5x glyph resampling.
    float measure(const std::string &value, float scale)
    {
        return measureTextWidth(scale >= 1.3f && scale < 2 ? "Book" : "Create", value, scale >= 2 ? 2.0f : 1.0f);
    }
    void text(const std::string &value, float x, float y, uint32_t color = Ink, float scale = 1.5f)
    {
        drawText(scale >= 1.3f && scale < 2 ? "Book" : "Create", value, std::round(m_x + x * m_scale),
                 std::round(m_y + y * m_scale), color, (scale >= 2 ? 2.0f : 1.0f) * m_scale, false);
    }
    void centered(const std::string &value, float x, float y, float width, uint32_t color = Ink, float scale = 1.5f)
    {
        text(value, x + (width - measure(value, scale)) * 0.5f, y, color, scale);
    }
    void rule(float x, float y, float w, float h = 1)
    {
        static const std::vector<uint8_t> ink{82, 101, 122, 255};
        drawPixelsBgra("__guide_rule", 1, 1, ink, rect(x, y, w, h));
    }
    bool button(const std::string &label, float x, float y, float width, bool enabled = true)
    {
        const Rect box = rect(x, y, width, 44);
        const bool hover = enabled && hitTest(box);
        static const std::vector<uint8_t> normal{190, 212, 226, 255}, over{134, 174, 199, 255};
        drawPixelsBgra(hover ? "__guide_button_over" : "__guide_button_paper", 1, 1, hover ? over : normal, box);
        rule(x, y, width);
        rule(x, y + 43, width);
        centered(label, x, y + 9, width, enabled ? Ink : 0xff888888u);
        return enabled && m_inputArmed && hover && leftMouseJustReleased();
    }
    void preparePages()
    {
        if (!m_sheets.empty())
            return;
        // Logical-space wrapping makes the same pages available at every render resolution.
        const int linesPerSheet = std::max(1, int(302.0f / std::max(30.0f, fontHeight("Book") + 5.0f)));
        for (int topic = 0; topic < 16; ++topic)
        {
            m_topicFirst[topic] = int(m_sheets.size());
            m_topicLabels[topic] = std::to_string(topic + 1) + ". " + m_copy.entries[topic + 1].title;
            const auto lines = wrapVitaGuideText(m_copy.entries[topic + 1].body, 550.0f,
                                                 [this](const std::string &value) { return measure(value, 1.5f); });
            size_t begin = 0;
            while (begin < lines.size())
            {
                while (begin < lines.size() && lines[begin].empty())
                    ++begin;
                if (begin == lines.size())
                    break;
                // Balance continuation pages: never strand a word or two on an otherwise empty sheet.
                const size_t remaining = lines.size() - begin;
                const size_t sheetsLeft = (remaining + linesPerSheet - 1) / linesPerSheet;
                const size_t count = (remaining + sheetsLeft - 1) / sheetsLeft;
                const size_t end = begin + count;
                Sheet sheet;
                sheet.topic = topic;
                sheet.lines.assign(lines.begin() + begin, lines.begin() + end);
                sheet.hint = wrapVitaGuideText(illustrations[topic].hint, 242.0f,
                                               [this](const std::string &value) { return measure(value, 1.25f); });
                sheet.chapter = "CHAPTER " + std::to_string(topic + 1) + " / 16";
                m_sheets.push_back(std::move(sheet));
                begin = end;
            }
        }
        for (size_t i = 0; i < m_sheets.size(); ++i)
            m_sheets[i].number = "Page " + std::to_string(i + 1) + " / " + std::to_string(m_sheets.size());
    }
    void changePage(int page)
    {
        m_page = page;
        m_inputArmed = false;
    }
    void drawScreen(float) override
    {
        const bool escape = isScancodeHeld(SDL_SCANCODE_ESCAPE);
        const bool back = m_inputArmed && escape && !m_escapeHeld;
        m_escapeHeld = escape;
        if (!leftMouseDown() && !leftMouseJustReleased() && !escape)
            m_inputArmed = true;
        m_scale = std::min(frameWidth() / 960.0f, frameHeight() / 544.0f);
        m_x = (frameWidth() - 960 * m_scale) * 0.5f;
        m_y = (frameHeight() - 544 * m_scale) * 0.5f;
        preparePages();
        drawTexture("vita_guide_paper", rect(0, 0, 960, 544));
        text("MIGHT AND MAGIC", 82, 40, RedInk, 1.25f);
        text("THE ADVENTURER'S GUIDE", 348, 40, RedInk, 1.25f);
        if (button(m_backLabel, 700, 24, 200))
        {
            m_close();
            return;
        }
        rule(60, 74, 840);
        if (back)
        {
            m_close();
            return;
        }
        if (m_page < 0)
        {
            text("Contents", 60, 86, Ink, 2.0f);
            text("Read in order, or choose a chapter.", 350, 93, Ink, 1.5f);
            for (int i = 0; i < 16; ++i)
            {
                const float x = 60 + (i / 8) * 426, y = 140 + (i % 8) * 36;
                const Rect box = rect(x, y, 410, 34);
                const bool hover = hitTest(box);
                if (hover)
                    rule(x, y + 32, 410, 2);
                const std::string &label = m_topicLabels[i];
                float scale = 1.3f;
                if (measure(label, scale) > 408)
                    scale = 1.0f;
                text(label, x + 4, y + 5, hover ? RedInk : Ink, scale);
                if (m_inputArmed && hover && leftMouseJustReleased())
                {
                    changePage(m_topicFirst[i]);
                    return;
                }
            }
            if (button("First page", 60, 462, 190))
            {
                changePage(0);
                return;
            }
            if (button("Close guide", 710, 462, 190))
            {
                m_close();
                return;
            }
            return;
        }
        const Sheet &sheet = m_sheets[m_page];
        const auto &entry = m_copy.entries[sheet.topic + 1];
        const auto &illustration = illustrations[sheet.topic];
        text(sheet.chapter, 60, 92, RedInk, 1.25f);
        float titleScale = 2.0f;
        const float titleWidth = measure(entry.title, titleScale);
        if (titleWidth > 550)
            titleScale = 1.5f;
        text(entry.title, 350, 89, Ink, titleScale);
        rule(326, 92, 1, 338);
        const auto size = textureSize(illustration.asset);
        if (size && size->width > 0 && size->height > 0)
        {
            const float scale = std::min({210.0f / size->width, 218.0f / size->height, 2.0f});
            const float w = size->width * scale, h = size->height * scale;
            drawTexture(illustration.asset, rect(66 + (238 - w) * 0.5f, 128 + (218 - h) * 0.5f, w, h));
        }
        centered(illustration.caption, 60, 361, 248, RedInk, 1.25f);
        // These brief captions are intentionally separate from the editable instructions.
        for (size_t i = 0; i < sheet.hint.size(); ++i)
            centered(sheet.hint[i], 60, 393 + i * 23, 248, Ink, 1.25f);
        const float lineHeight = std::max(30.0f, fontHeight("Book") + 5.0f);
        for (size_t i = 0; i < sheet.lines.size(); ++i)
            text(sheet.lines[i], 350, 139 + i * lineHeight);
        rule(60, 447, 840);
        if (button("Previous", 60, 462, 170, m_page > 0))
        {
            changePage(m_page - 1);
            return;
        }
        if (button("Contents", 247, 462, 170))
        {
            changePage(-1);
            return;
        }
        centered(sheet.number, 431, 475, 260, Ink, 1.25f);
        if (button(m_page + 1 < int(m_sheets.size()) ? "Next page" : "Close guide", 710, 462, 190))
        {
            if (m_page + 1 < int(m_sheets.size()))
                changePage(m_page + 1);
            else
            {
                m_close();
                return;
            }
        }
    }
    AppMode m_mode;
    std::function<void()> m_close;
    std::string m_backLabel;
    VitaGuideCopy m_copy;
    std::vector<Sheet> m_sheets;
    std::array<int, 16> m_topicFirst{};
    std::array<std::string, 16> m_topicLabels;
    int m_page = 0;
    float m_scale = 1, m_x = 0, m_y = 0;
    bool m_inputArmed = false, m_escapeHeld = false;
};
} // namespace OpenYAMM::Game
