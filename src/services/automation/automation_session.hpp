// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/node.hpp>
#include <element/juce/data_structures.hpp>

namespace element::automation {

class AutomationEngine;

/** Session-level (view-INDEPENDENT) population + persistence of timeline
 *  automation.  Timeline automation is song-owned: it must play + persist
 *  regardless of which view is active, so these run from the engine/graph
 *  lifecycle (EngineService), not from any ContentView.  See memory
 *  project_automation_timeline_vs_clip_semantics. */

/** Rebind every track in `engine` to a live destination resolved from its
 *  persistent target key against the graph rooted at `graphRoot`.
 *
 *  - MIDI-CC tracks bind unconditionally (no graph lookup needed).
 *  - node / plugin-param tracks resolve targetKey.nodeId -> node ->
 *    param-index (the same input ParameterArray MappingEngine resolves
 *    against).
 *
 *  Tracks whose node or param can't be resolved are left unbound -- the
 *  engine treats them as inactive until a later rebind (e.g. once an
 *  async-loaded plugin's params materialise).  Safe to call repeatedly. */
void rebindEngineTargets (AutomationEngine& engine, const element::Node& graphRoot);

/** Populate `engine` from sessionData's tags::automationTracks child, then
 *  rebind all targets against graphRoot.  loadFromValueTree clears any
 *  existing tracks first (replace semantics). */
void restoreAutomationFromSession (AutomationEngine&      engine,
                                   const juce::ValueTree& sessionData,
                                   const element::Node&   graphRoot);

/** Flush `engine`'s tracks into sessionData under tags::automationTracks.
 *  Caller is expected to skip this when the engine has zero tracks (the
 *  engine's saveToValueTree removes the existing child before writing, so
 *  an empty-engine flush would erase previously-saved automation -- the
 *  call site guards against that). */
/* sessionData taken BY VALUE: juce::ValueTree is a shared handle, and
 * Model::data() returns by value -- a copy still mutates the same
 * underlying session tree. */
void saveAutomationToSession (const AutomationEngine& engine,
                              juce::ValueTree         sessionData);

} // namespace element::automation
