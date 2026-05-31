// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/automation/automation_session.hpp"
#include "services/automation/automation_engine.hpp"
#include "services/automation/automation_target_resolver.hpp"

#include <element/processor.hpp>

namespace element::automation {

namespace {

/** Local recursive node-by-uuid walk.  `findNodeByUuid` exists as a
 *  file-local static in a couple of TUs but isn't exported; duplicating
 *  the tiny traversal here keeps this helper decoupled. */
element::Node findNodeByUuidLocal (const element::Node& graph, const juce::Uuid& target)
{
    const int n = graph.getNumNodes();
    for (int i = 0; i < n; ++i)
    {
        element::Node child = graph.getNode (i);
        if (! child.isValid()) continue;
        if (child.getUuid() == target) return child;
        if (child.isGraph())
        {
            element::Node nested = findNodeByUuidLocal (child, target);
            if (nested.isValid()) return nested;
        }
    }
    return element::Node();
}

} // namespace

void rebindEngineTargets (AutomationEngine& engine, const element::Node& graphRoot)
{
    engine.forEachTrack ([&] (element::dsp::automation::AutomationTrack* track)
    {
        if (track == nullptr) return;
        const auto& key = track->targetKey;

        if (key.isMidi())
        {
            engine.bindMidiCc (track, key.midiCcChannel, key.midiCcNumber);
            return;
        }

        const int idx = decodeNodeParamId (key.paramId);
        if (idx < 0) return;

        const element::Node node = findNodeByUuidLocal (graphRoot, key.nodeId);
        if (! node.isValid()) return;

        auto* proc = node.getObject();
        if (proc == nullptr) return;

        const auto& params = proc->getParameters (true);
        if (idx >= params.size()) return;

        auto p = params.getUnchecked (idx);
        if (p == nullptr) return;

        engine.bindNodeParam (track, p);
    });
}

void restoreAutomationFromSession (AutomationEngine&      engine,
                                   const juce::ValueTree& sessionData,
                                   const element::Node&   graphRoot)
{
    engine.loadFromValueTree (sessionData);
    rebindEngineTargets (engine, graphRoot);
}

void saveAutomationToSession (const AutomationEngine& engine,
                              juce::ValueTree         sessionData)
{
    engine.saveToValueTree (sessionData);
}

} // namespace element::automation
