#pragma once
#include <sstream>
#include <string>
#include <vector>

namespace OpenYAMM::Game
{
// Called on page/viewport changes, never for each glyph of every rendered frame.
template <class Measure>
std::vector<std::string> wrapVitaGuideText(const std::string &text, float width, Measure measure)
{
    std::vector<std::string> lines;
    std::istringstream paragraphs(text);
    std::string paragraph;
    while (std::getline(paragraphs, paragraph))
    {
        if (paragraph.empty())
        {
            lines.emplace_back();
            continue;
        }
        std::istringstream words(paragraph);
        std::string word, line;
        while (words >> word)
        {
            if (!line.empty() && measure(line + " " + word) > width)
            {
                lines.push_back(std::move(line));
                line.clear();
            }
            while (measure(word) > width && word.size() > 1)
            {
                size_t count = 1;
                while (count < word.size() && measure(word.substr(0, count + 1)) <= width)
                    ++count;
                lines.push_back(word.substr(0, count));
                word.erase(0, count);
            }
            if (!line.empty())
                line += ' ';
            line += word;
        }
        if (!line.empty())
            lines.push_back(std::move(line));
    }
    while (!lines.empty() && lines.back().empty())
        lines.pop_back();
    return lines;
}
} // namespace OpenYAMM::Game
