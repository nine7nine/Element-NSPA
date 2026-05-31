// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dsp/automation/automation_track.hpp"   // AutomationTargetKey

#include <element/parameter.hpp>
#include <element/juce/core.hpp>

#include <vector>

namespace element::automation {

/** UI-agnostic helpers for turning a node's parameters into automation
 *  targets, and back.  These are the shared primitives behind every
 *  automation editing surface (arrangement lanes, piano-roll CC lanes),
 *  so they live apart from any one view.
 *
 *  paramId convention (mirrors AutomationTargetKey's documented rule +
 *  MappingEngine's index-based resolution): a node/plugin parameter is
 *  identified by its INDEX into the node's input ParameterArray,
 *  stringified.  The index is stable across reload for both internal
 *  nodes (fixed param order by definition) and hosted plugins (JUCE
 *  guarantees stable parameter order per instance).  MIDI-CC targets
 *  don't use paramId -- they carry (channel, ccNumber) on the key.
 *
 *  Binding the resolved key to a live destination (find the node in the
 *  graph, fetch the live Parameter, call engine.bindNodeParam / etc.)
 *  needs graph access and stays in the view layer; these helpers only
 *  cover the pure, testable encode / enumerate / key-build steps. */

/** One automatable parameter exposed for the target picker. */
struct AutomatableParam
{
    int          index { -1 };   /**< Index into the node's input ParameterArray. */
    juce::String paramId;        /**< Stable id stored on the AutomationTargetKey. */
    juce::String name;           /**< Display name. */
    juce::String label;          /**< Unit label, e.g. "Hz" / "%" (may be empty). */
};

/** Encode a parameter index as the stable paramId token. */
inline juce::String encodeNodeParamId (int paramIndex)
{
    return juce::String (paramIndex);
}

/** Decode a paramId token back to a parameter index.  Returns -1 when
 *  the token is empty or not a non-negative integer (e.g. a MIDI-CC key
 *  whose paramId was never populated). */
inline int decodeNodeParamId (const juce::String& paramId)
{
    if (paramId.isEmpty() || ! paramId.containsOnly ("0123456789"))
        return -1;
    return paramId.getIntValue();
}

/** Build the list of automatable parameters from a node's INPUT
 *  parameter array (the same array MappingEngine resolves against).
 *  Non-automatable params are skipped, but the stored index is the real
 *  array index so resolution stays correct across the gaps. */
inline std::vector<AutomatableParam> enumerateAutomatableParams (const element::ParameterArray& params)
{
    std::vector<AutomatableParam> out;
    out.reserve ((size_t) params.size());
    for (int i = 0; i < params.size(); ++i)
    {
        auto p = params.getUnchecked (i);   // ParameterPtr (ref-counted)
        if (p == nullptr || ! p->isAutomatable())
            continue;

        AutomatableParam ap;
        ap.index   = i;
        ap.paramId = encodeNodeParamId (i);
        ap.name    = p->getName (64);
        ap.label   = p->getLabel();
        out.push_back (std::move (ap));
    }
    return out;
}

/** Build a target key for a node / plugin parameter. */
inline element::dsp::automation::AutomationTargetKey
makeNodeParamKey (const juce::Uuid& nodeId, int paramIndex)
{
    element::dsp::automation::AutomationTargetKey key;
    key.nodeId  = nodeId;
    key.paramId = encodeNodeParamId (paramIndex);
    return key;
}

/** Build a target key for a MIDI-CC destination.  channel in [1, 16],
 *  ccNumber in [0, 127] (JUCE conventions). */
inline element::dsp::automation::AutomationTargetKey
makeMidiCcKey (int channel, int ccNumber)
{
    element::dsp::automation::AutomationTargetKey key;
    key.midiCcChannel = channel;
    key.midiCcNumber  = ccNumber;
    return key;
}

} // namespace element::automation
