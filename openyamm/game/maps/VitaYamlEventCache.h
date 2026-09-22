#pragma once
#include <yaml-cpp/eventhandler.h>
#include <yaml-cpp/parser.h>
#include <yaml-cpp/mark.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace OpenYAMM::Game::VitaYamlEvents
{
// Derived parser events, never authoritative game data. The original text must match byte for byte.
// Every event is validated before publishing anything to the existing schema consumer.
inline uint64_t hash(const void *bytes, size_t size)
{
    uint64_t value = 14695981039346656037ull;
    const auto *p = static_cast<const uint8_t *>(bytes);
    for (size_t i = 0; i < size; ++i) { value = (value ^ p[i]) * 1099511628211ull; }
    return value;
}
inline void integer(std::vector<uint8_t> &out, uint64_t value, unsigned count)
{ for (unsigned i = 0; i < count; ++i) { out.push_back(uint8_t(value >> (i * 8))); } }
inline void string(std::vector<uint8_t> &out, const std::string &value)
{
    integer(out, value.size(), 4); out.insert(out.end(), value.begin(), value.end());
}
struct Reader
{
    const uint8_t *p; size_t left;
    uint64_t integer(unsigned count)
    {
        if (count > left) { throw std::runtime_error("short event cache"); }
        uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i) { value |= uint64_t(*p++) << (i * 8); }
        left -= count; return value;
    }
    std::string string()
    {
        const size_t count = integer(4);
        if (count > left) { throw std::runtime_error("short cache string"); }
        std::string value(reinterpret_cast<const char *>(p), count); p += count; left -= count; return value;
    }
};
class Recorder final : public YAML::EventHandler
{
public:
    std::vector<uint8_t> events;
    void OnDocumentStart(const YAML::Mark &) override { events.push_back(1); }
    void OnDocumentEnd() override { events.push_back(2); }
    void OnNull(const YAML::Mark &, YAML::anchor_t a) override { anchor(a); events.push_back(3); }
    void OnAlias(const YAML::Mark &, YAML::anchor_t) override { throw std::runtime_error("uncached alias"); }
    void OnScalar(const YAML::Mark &, const std::string &tag, YAML::anchor_t a, const std::string &value) override
    { anchor(a); events.push_back(4); string(events, tag); string(events, value); }
    void OnSequenceStart(const YAML::Mark &, const std::string &tag, YAML::anchor_t a, YAML::EmitterStyle::value style) override
    { start(5, tag, a, style); }
    void OnSequenceEnd() override { events.push_back(6); }
    void OnMapStart(const YAML::Mark &, const std::string &tag, YAML::anchor_t a, YAML::EmitterStyle::value style) override
    { start(7, tag, a, style); }
    void OnMapEnd() override { events.push_back(8); }
private:
    void anchor(YAML::anchor_t a) { if (a) { throw std::runtime_error("uncached anchor"); } }
    void start(uint8_t type, const std::string &tag, YAML::anchor_t a, YAML::EmitterStyle::value style)
    { anchor(a); events.push_back(type); events.push_back(uint8_t(style)); string(events, tag); }
};
inline void walk(const uint8_t *data, size_t bytes, YAML::EventHandler *handler)
{
    Reader input{data, bytes}; std::vector<uint8_t> nesting; unsigned document = 0, roots = 0;
    YAML::Mark mark{};
    while (input.left)
    {
        const unsigned type = unsigned(input.integer(1));
        if (type == 1)
        {
            if (document) { throw std::runtime_error("extra cache document"); }
            document = 1; if (handler) { handler->OnDocumentStart(mark); } continue;
        }
        if (type == 2)
        {
            if (document != 1 || !nesting.empty() || roots != 1 || input.left) { throw std::runtime_error("invalid cache end"); }
            document = 2; if (handler) { handler->OnDocumentEnd(); } continue;
        }
        if (document != 1) { throw std::runtime_error("event outside document"); }
        if (type == 6 || type == 8)
        {
            if (nesting.empty() || nesting.back() + 1 != type) { throw std::runtime_error("unbalanced cache"); }
            nesting.pop_back();
            if (handler) { if (type == 6) handler->OnSequenceEnd(); else handler->OnMapEnd(); }
            continue;
        }
        if (nesting.empty()) { ++roots; }
        if (type == 3) { if (handler) handler->OnNull(mark, 0); }
        else if (type == 4)
        {
            const std::string tag = input.string(), value = input.string();
            if (handler) { handler->OnScalar(mark, tag, 0, value); }
        }
        else if (type == 5 || type == 7)
        {
            const unsigned style = unsigned(input.integer(1)); const std::string tag = input.string();
            if (style > 2 || nesting.size() >= 256) { throw std::runtime_error("invalid cache style/depth"); }
            nesting.push_back(uint8_t(type));
            if (handler)
            {
                if (type == 5) handler->OnSequenceStart(mark, tag, 0, YAML::EmitterStyle::value(style));
                else handler->OnMapStart(mark, tag, 0, YAML::EmitterStyle::value(style));
            }
        }
        else { throw std::runtime_error("unknown cache event"); }
    }
    if (document != 2) { throw std::runtime_error("incomplete cache document"); }
}
struct Entry { uint64_t key, offset; uint32_t bytes; };
inline bool replay(const std::string &text, YAML::EventHandler &handler,
    const char *path = "/data/yaml-events-v1.pack")
{
    std::vector<uint8_t> payload;
    // Do not catch consumer exceptions after publication: schema errors remain genuine load errors.
    try
    {
        std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path, "rb"), std::fclose);
        if (!file) { return false; }
        uint8_t header[12];
        if (std::fread(header, 1, 12, file.get()) != 12 || std::memcmp(header, "OYYEV001", 8)) { return false; }
        Reader countReader{header + 8, 4}; const size_t count = countReader.integer(4);
        if (count > 8192) { return false; }
        const uint64_t key = hash(text.data(), text.size());
        uint64_t offset = 0; uint32_t bytes = 0;
        std::vector<uint8_t> directory(count * 20);
        if (std::fread(directory.data(), 1, directory.size(), file.get()) != directory.size()) { return false; }
        for (size_t i = 0; i < count; ++i)
        {
            Reader row{directory.data() + i * 20,20}; const uint64_t candidate = row.integer(8), position = row.integer(8);
            const uint32_t length = uint32_t(row.integer(4));
            if (candidate == key) { offset = position; bytes = length; break; }
        }
        if (!offset || offset > 1024 * 1024 * 1024 || bytes < 12 || bytes > 16 * 1024 * 1024
            || offset < 12 + count * 20 || std::fseek(file.get(), long(offset), SEEK_SET)) { return false; }
        payload.resize(bytes);
        if (std::fread(payload.data(), 1, bytes, file.get()) != bytes) { return false; }
        Reader reader{payload.data(), payload.size()};
        if (reader.string() != text || reader.left < 8) { return false; }
        const uint64_t checksum = reader.integer(8);
        if (hash(reader.p, reader.left) != checksum) { return false; }
        walk(reader.p, reader.left, nullptr); // Validate completely before modifying the scene.
    }
    catch (const std::exception &) { return false; }
    Reader reader{payload.data(),payload.size()}; reader.string(); reader.integer(8);
    walk(reader.p, reader.left, &handler); return true;
}
}
