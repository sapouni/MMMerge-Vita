#pragma once

#include <yaml-cpp/yaml.h>
#include "game/maps/VitaYamlEventCache.h"
#include "game/render/VitaPerformanceConfig.h"
#include "engine/VitaLoadProfile.h"
#include <yaml-cpp/eventhandler.h>

#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace OpenYAMM::Game
{
// Keep only one record from the large outdoor sequences alive at a time. The
// remaining document is returned to the existing schema validator unchanged.
// Packaged scene files use scalar keys and no YAML anchors; reject unsupported
// constructs explicitly instead of silently changing their meaning.
class StreamingSceneYaml final : public YAML::EventHandler
{
public:
    using RecordConsumer = std::function<void(const std::string &, const YAML::Node &)>;

    explicit StreamingSceneYaml(RecordConsumer consumer,
        std::unordered_set<std::string> paths = {"/bmodel_faces/interactive_faces", "/entities", "/spawns"})
        : m_consumer(std::move(consumer)), m_paths(std::move(paths)) {}

    YAML::Node load(const std::string &text)
    {
#if defined(OPENYAMM_VITA_LOADER)
        if (vitaPerformanceSettings().yamlEventCache)
        {
            OY_LOAD(Engine::VitaLoad::Scope replayTime(Engine::VitaLoad::YamlReplay));
            if (VitaYamlEvents::replay(text, *this))
            { OY_LOAD(if (Engine::VitaLoad::active) ++Engine::VitaLoad::yamlHits); return m_root; }
            OY_LOAD(if (Engine::VitaLoad::active) ++Engine::VitaLoad::yamlMisses);
        }
#endif
        OY_LOAD(Engine::VitaLoad::Scope parseTime(Engine::VitaLoad::YamlParse));
        std::istringstream input(text);
        YAML::Parser parser(input);
        parser.HandleNextDocument(*this);
        return m_root;
    }

    void OnDocumentStart(const YAML::Mark &) override {}
    void OnDocumentEnd() override {}

    void OnNull(const YAML::Mark &, YAML::anchor_t anchor) override
    {
        checkAnchor(anchor);
        complete(YAML::Node(YAML::NodeType::Null));
    }

    void OnAlias(const YAML::Mark &, YAML::anchor_t) override
    {
        throw std::runtime_error("Vita scene streaming does not support YAML aliases");
    }

    void OnScalar(const YAML::Mark &, const std::string &tag, YAML::anchor_t anchor,
                  const std::string &value) override
    {
        checkAnchor(anchor);
        YAML::Node node(value);
        node.SetTag(tag);
        complete(node);
    }

    void OnSequenceStart(const YAML::Mark &, const std::string &tag, YAML::anchor_t anchor,
                         YAML::EmitterStyle::value style) override
    {
        begin(YAML::NodeType::Sequence, tag, anchor, style);
    }
    void OnSequenceEnd() override { end(); }

    void OnMapStart(const YAML::Mark &, const std::string &tag, YAML::anchor_t anchor,
                    YAML::EmitterStyle::value style) override
    {
        begin(YAML::NodeType::Map, tag, anchor, style);
    }
    void OnMapEnd() override { end(); }

private:
    struct Frame
    {
        YAML::Node node;
        std::string path;
        std::string key;
        YAML::Node keyNode;
        bool expectingKey = true;
        bool streaming = false;
    };

    static void checkAnchor(YAML::anchor_t anchor)
    {
        if (anchor != 0)
        {
            throw std::runtime_error("Vita scene streaming does not support YAML anchors");
        }
    }

    void begin(YAML::NodeType::value type, const std::string &tag, YAML::anchor_t anchor,
               YAML::EmitterStyle::value style)
    {
        checkAnchor(anchor);
        std::string path;
        if (!m_frames.empty())
        {
            const Frame &parent = m_frames.back();
            if (parent.node.IsMap() && parent.expectingKey)
            {
                throw std::runtime_error("scene mapping keys must be scalars");
            }
            path = parent.path + (parent.node.IsMap() ? "/" + parent.key : "/[]");
        }
        Frame frame{YAML::Node(type), path};
        frame.node.SetTag(tag);
        frame.node.SetStyle(style);
        frame.streaming = type == YAML::NodeType::Sequence && m_paths.count(path) != 0;
        if (frame.streaming && !m_streamedPaths.insert(path).second)
        {
            throw std::runtime_error("duplicate streamed scene sequence: " + path);
        }
        m_frames.push_back(std::move(frame));
    }

    void end()
    {
        YAML::Node node = m_frames.back().node;
        m_frames.pop_back();
        complete(node);
    }

    void complete(const YAML::Node &node)
    {
        if (m_frames.empty())
        {
            m_root.reset(node);
            return;
        }
        Frame &parent = m_frames.back();
        if (parent.streaming)
        {
            m_consumer(parent.path, node);
        }
        else if (parent.node.IsSequence())
        {
            parent.node.push_back(node);
        }
        else if (parent.expectingKey)
        {
            if (!node.IsScalar())
            {
                throw std::runtime_error("scene mapping keys must be scalars");
            }
            parent.key = node.Scalar();
            parent.keyNode.reset(node);
            parent.expectingKey = false;
        }
        else
        {
            // force_insert retains duplicate-key behavior of YAML::Load.
            parent.node.force_insert(parent.keyNode, node);
            parent.keyNode.reset();
            parent.expectingKey = true;
        }
    }

    RecordConsumer m_consumer;
    std::unordered_set<std::string> m_paths;
    std::vector<Frame> m_frames;
    std::unordered_set<std::string> m_streamedPaths;
    YAML::Node m_root;
};
}
