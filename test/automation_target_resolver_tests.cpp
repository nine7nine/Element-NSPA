// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <boost/test/unit_test.hpp>

#include "services/automation/automation_target_resolver.hpp"

#include <element/parameter.hpp>

using element::ParameterArray;
using element::automation::AutomatableParam;
using element::automation::decodeNodeParamId;
using element::automation::encodeNodeParamId;
using element::automation::enumerateAutomatableParams;
using element::automation::makeMidiCcKey;
using element::automation::makeNodeParamKey;

namespace {

/** Minimal element::Parameter fake with configurable name + automatable
 *  flag, for exercising the resolver helpers without a live graph. */
class FakeParam : public element::Parameter
{
public:
    FakeParam (juce::String n, bool autom, juce::String lbl = {})
        : name_ (std::move (n)), label_ (std::move (lbl)), autom_ (autom) {}

    int   getPortIndex() const noexcept override               { return 0; }
    int   getParameterIndex() const noexcept override          { return 0; }
    float getValue() const override                            { return 0.f; }
    void  setValue (float) override                            {}
    float getDefaultValue() const override                     { return 0.5f; }
    float getValueForText (const juce::String&) const override { return 0.f; }
    juce::String getName (int) const override                  { return name_; }
    juce::String getLabel() const override                     { return label_; }
    bool  isAutomatable() const override                       { return autom_; }

private:
    juce::String name_, label_;
    bool         autom_;
};

ParameterArray makeParams()
{
    ParameterArray a;
    a.add (new FakeParam ("Cutoff",   true,  "Hz"));   // index 0
    a.add (new FakeParam ("Meter",    false));         // index 1 (read-only)
    a.add (new FakeParam ("Resonance", true, "%"));    // index 2
    return a;
}

} // namespace

BOOST_AUTO_TEST_SUITE (AutomationTargetResolverTests)

BOOST_AUTO_TEST_CASE (param_id_encode_decode_round_trip)
{
    for (int i : { 0, 1, 7, 42, 1000 })
        BOOST_CHECK_EQUAL (decodeNodeParamId (encodeNodeParamId (i)), i);
}

BOOST_AUTO_TEST_CASE (decode_rejects_non_index_tokens)
{
    BOOST_CHECK_EQUAL (decodeNodeParamId (juce::String()), -1);  // empty
    BOOST_CHECK_EQUAL (decodeNodeParamId ("volume"),       -1);  // symbolic
    BOOST_CHECK_EQUAL (decodeNodeParamId ("-3"),           -1);  // negative / has '-'
    BOOST_CHECK_EQUAL (decodeNodeParamId ("3.5"),          -1);  // has '.'
}

BOOST_AUTO_TEST_CASE (enumerate_skips_non_automatable_and_keeps_real_indices)
{
    const auto params = makeParams();
    const auto autoParams = enumerateAutomatableParams (params);

    /* Two of three params are automatable; the read-only meter (index 1)
     * is dropped, but the surviving entries keep their REAL array index
     * so later resolution against getParameters()[index] stays correct. */
    BOOST_REQUIRE_EQUAL (autoParams.size(), (size_t) 2);

    BOOST_CHECK_EQUAL (autoParams[0].index, 0);
    BOOST_CHECK_EQUAL (autoParams[0].name.toStdString(), "Cutoff");
    BOOST_CHECK_EQUAL (autoParams[0].label.toStdString(), "Hz");
    BOOST_CHECK_EQUAL (autoParams[0].paramId.toStdString(), "0");

    BOOST_CHECK_EQUAL (autoParams[1].index, 2);             // NOT 1 -- gap skipped
    BOOST_CHECK_EQUAL (autoParams[1].name.toStdString(), "Resonance");
    BOOST_CHECK_EQUAL (autoParams[1].paramId.toStdString(), "2");
}

BOOST_AUTO_TEST_CASE (enumerate_empty_array_yields_empty)
{
    ParameterArray empty;
    BOOST_CHECK (enumerateAutomatableParams (empty).empty());
}

BOOST_AUTO_TEST_CASE (make_node_param_key_populates_node_and_param)
{
    const juce::Uuid node;
    const auto key = makeNodeParamKey (node, 2);

    BOOST_CHECK (key.nodeId == node);
    BOOST_CHECK_EQUAL (key.paramId.toStdString(), "2");
    BOOST_CHECK (! key.isMidi());
    BOOST_CHECK_EQUAL (decodeNodeParamId (key.paramId), 2);
}

BOOST_AUTO_TEST_CASE (make_midi_cc_key_is_midi_and_carries_channel_cc)
{
    const auto key = makeMidiCcKey (5, 74);

    BOOST_CHECK (key.isMidi());
    BOOST_CHECK_EQUAL (key.midiCcChannel, 5);
    BOOST_CHECK_EQUAL (key.midiCcNumber,  74);
}

BOOST_AUTO_TEST_CASE (node_key_and_midi_key_are_distinct_kinds)
{
    /* A node-param key (no CC populated) must NOT read as a MIDI key,
     * and a MIDI key must read as MIDI -- this is the discriminator the
     * engine + resolver branch on. */
    BOOST_CHECK (! makeNodeParamKey (juce::Uuid(), 0).isMidi());
    BOOST_CHECK (  makeMidiCcKey (1, 11).isMidi());
}

BOOST_AUTO_TEST_SUITE_END()
