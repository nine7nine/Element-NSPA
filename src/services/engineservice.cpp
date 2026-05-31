// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <element/context.hpp>
#include <element/devices.hpp>
#include <element/graph.hpp>
#include <element/node.hpp>
#include <element/plugins.hpp>
#include <element/services.hpp>
#include <element/settings.hpp>

#include "engine/graphmanager.hpp"
#include "nodes/mididevice.hpp"
#if ELEMENT_USE_JACK
#include "nodes/jackmidiinputnode.hpp"
#include "nodes/jackmidioutputnode.hpp"
#endif
#include "engine/rootgraph.hpp"
#include "services/automation/automation_engine.hpp"
#include "services/automation/automation_session.hpp"
#include <element/engine.hpp>
#include <element/ui.hpp>

#define ELEMENT_TRACE_SESSION_LOAD 0

using namespace juce;

namespace element {

namespace detail {
static void initializeRootGraphPorts (RootGraph* root, const Node& model)
{
    PortArray ins, outs;
    model.getPorts (ins, outs, PortType::Audio);
    root->setNumPorts (PortType::Audio, ins.size(), true, false);
    root->setNumPorts (PortType::Audio, outs.size(), false, false);

    ins.clearQuick();
    outs.clearQuick();
    model.getPorts (ins, outs, PortType::Midi);
    root->setNumPorts (PortType::Midi, ins.size(), true, false);
    root->setNumPorts (PortType::Midi, outs.size(), false, false);
}
} // namespace detail

struct RootGraphHolder
{
    RootGraphHolder (const Node& n, Context& world)
        : plugins (world.plugins()),
          devices (world.devices()),
          model (n)
    {
    }

    ~RootGraphHolder()
    {
        jassert (! attached());
        controller = nullptr;
        model.data().removeProperty (tags::object, 0);
        node = nullptr;
        model = Node();
    }

    bool attached() const { return node && controller; }

    /** This will create a root graph processor/controller and load it if not
        done already. Properties are set from the model, so make sure they are
        correct before calling this 
     */
    bool attach (AudioEnginePtr engine)
    {
        jassert (engine);
        if (! engine)
        {
            DBG ("[element] root graph attach: engine is nil");
            return false;
        }

        if (attached())
        {
            DBG ("[element] root graph attach: already attached");
            return true;
        }

        node = new RootGraph (engine->context());

        if (auto* root = getRootGraph())
        {
            const auto modeStr = model.getProperty (tags::renderMode, "single").toString().trim().toLowerCase();
            const auto mode = modeStr == "single" ? RootGraph::SingleGraph : RootGraph::Parallel;
            const auto channels = model.getMidiChannels();
            const auto program = (int) model.getProperty ("midiProgram", -1);

            // TODO: Uniform method for saving/restoring nodes with custom ports.
            detail::initializeRootGraphPorts (root, model);

            // root->setPlayConfigFor (devices);
            root->setRenderMode (mode);
            root->setMidiChannels (channels);
            root->setMidiProgram (program);

            if (engine->addGraph (root))
            {
                controller = std::make_unique<RootGraphManager> (*root, plugins);
                model.setProperty (tags::object, node.get());

                controller->setNodeModel (model);
            }
            else
            {
                std::clog << "[element] failed to set root graph\n";
            }
        }

        return attached();
    }

    bool detach (AudioEnginePtr engine)
    {
        if (! engine)
            return false;

        if (! attached())
            return true;

        bool wasRemoved = false;
        if (auto* g = getRootGraph())
            wasRemoved = engine->removeGraph (g);

        if (wasRemoved)
        {
            controller = nullptr;
            node = nullptr;
        }

        return wasRemoved;
    }

    RootGraphManager* getController() const { return controller.get(); }
    RootGraph* getRootGraph() const { return dynamic_cast<RootGraph*> (node ? node.get() : nullptr); }

    bool hasController() const { return nullptr != controller; }

private:
    friend class EngineService;
    friend class EngineService::RootGraphs;
    PluginManager& plugins;
    DeviceManager& devices;
    std::unique_ptr<RootGraphManager> controller;
    Node model;
    ProcessorPtr node;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RootGraphHolder);
};

class EngineService::RootGraphs
{
public:
    RootGraphs (EngineService& e) : owner (e) {}
    ~RootGraphs() {}

    RootGraphHolder* add (RootGraphHolder* item)
    {
        jassert (! graphs.contains (item));
        return graphs.add (item);
    }

    void clear()
    {
        detachAll();
        graphs.clear();
    }

    RootGraphHolder* findByEngineIndex (const int index) const
    {
        if (index >= 0)
            for (auto* const n : graphs)
                if (auto* p = n->getRootGraph())
                    if (p->getEngineIndex() == index)
                        return n;
        return 0;
    }

    RootGraphHolder* findFor (const Node& node) const
    {
        for (auto* const n : graphs)
            if (n->model == node)
                return n;
        return nullptr;
    }

    /** Returns the active graph according to the engine */
    RootGraphHolder* findActiveInEngine() const
    {
        auto engine = owner.context().audio();
        if (! engine)
            return 0;
        const int currentIndex = engine->getActiveGraph();
        if (currentIndex >= 0)
            for (auto* h : graphs)
                if (auto* root = h->getRootGraph())
                    if (currentIndex == root->getEngineIndex())
                        return h;
        return 0;
    }

    /** Returns the active graph according to the session */
    RootGraphHolder* findActive() const
    {
        if (auto session = owner.context().session())
            if (auto* h = findFor (session->getActiveGraph()))
                return h;
        return 0;
    }

    /** This returns a GraphManager for the provided node. The
        passed in node is expected to have type="graph" 
        
        NOTE: this is a recursive operation
     */
    GraphManager* findGraphManagerFor (const Node& graph)
    {
        for (const auto* h : graphs)
        {
            if (auto* m1 = h->controller.get())
                if (auto* m2 = m1->findGraphManagerForGraph (graph))
                    return m2;
        }

        return nullptr;
    }

    RootGraphManager* findActiveRootGraphManager() const
    {
        if (auto* h = findActive())
            return h->controller.get();
        return 0;
    }

    void attachAll()
    {
        engine = owner.context().audio();
        for (auto* g : graphs)
            g->attach (engine);
    }

    void detachAll()
    {
        engine = owner.context().audio();
        for (auto* g : graphs)
            g->detach (engine);
    }

    // remove the holder, this will also delete it!
    void remove (RootGraphHolder* g)
    {
        graphs.removeObject (g, true);
    }

    const OwnedArray<RootGraphHolder>& getGraphs() const { return graphs; }

private:
    EngineService& owner;
    SessionPtr session;
    AudioEnginePtr engine;
    OwnedArray<RootGraphHolder> graphs;
};

EngineService::EngineService()
    : Service()
{
    graphs = std::make_unique<RootGraphs> (*this);
}

EngineService::~EngineService()
{
    graphs = nullptr;
}

void EngineService::addConnection (const uint32 s, const uint32 sp, const uint32 d, const uint32 dp)
{
    if (auto session = context().session())
        if (auto* h = graphs->findFor (session->getCurrentGraph()))
            if (auto* c = h->getController())
                c->addConnection (s, sp, d, dp);
}

void EngineService::addConnection (const uint32 s, const uint32 sp, const uint32 d, const uint32 dp, const Node& graph)
{
    if (auto* controller = graphs->findGraphManagerFor (graph))
        controller->addConnection (s, sp, d, dp);
}

void EngineService::addGraph()
{
    auto& world = context();
    auto engine = world.audio();
    auto session = world.session();

    /* Element: prefer the device's registered port count.  See the
     * matching reasoning in SessionService::loadNewSessionData. */
    auto* device = world.devices().getCurrentAudioDevice();
    int numIn  = device != nullptr ? device->getInputChannelNames().size()  : 2;
    int numOut = device != nullptr ? device->getOutputChannelNames().size() : 2;
    if (numIn  <= 0) numIn  = engine ? engine->getNumChannels (true)  : 2;
    if (numOut <= 0) numOut = engine ? engine->getNumChannels (false) : 2;
    if (numIn  <= 0) numIn  = 2;
    if (numOut <= 0) numOut = 2;

    /* See SessionService::loadNewSessionData for the JACK rationale —
     * no abstract MIDI ports on new graphs because JACK MIDI bypasses
     * the Graph I/O pseudo-nodes entirely. */
#if ELEMENT_USE_JACK
    constexpr bool wantGraphMidiIn  = false;
    constexpr bool wantGraphMidiOut = false;
#else
    constexpr bool wantGraphMidiIn  = true;
    constexpr bool wantGraphMidiOut = true;
#endif
    Node node (Graph::create ("Graph " + String (session->getNumGraphs() + 1),
                              numIn,
                              numOut,
                              wantGraphMidiIn,
                              wantGraphMidiOut));

    addGraph (node, false);
}

void EngineService::addGraph (const Node& newGraph, bool makeActive)
{
    jassert (newGraph.isGraph());

    Node ret;
    Node node = newGraph.data().getParent().isValid() ? newGraph
                                                      : Node (newGraph.data().createCopy(), false);
    auto engine = context().audio();
    auto session = context().session();
    String err = node.isGraph() ? String() : "Not a graph";

    if (err.isNotEmpty())
    {
        AlertWindow::showMessageBoxAsync (AlertWindow::InfoIcon, "Audio Engine", err);
        return;
    }

    if (auto* holder = graphs->add (new RootGraphHolder (node, context())))
    {
        if (holder->attach (engine))
        {
            session->addGraph (node, makeActive);
            DBG ("[element] graph added: active: " << (int) makeActive);
            if (makeActive)
                setRootNode (node);
        }
        else
        {
            err = "Could not attach new graph to engine.";
        }
    }
    else
    {
        err = "Could not create new graph.";
    }

    if (err.isNotEmpty())
    {
        AlertWindow::showMessageBoxAsync (AlertWindow::InfoIcon, "Audio Engine", err);
    }
}

void EngineService::duplicateGraph (const Node& graph)
{
    Node duplicate (graph.data().createCopy());
    duplicate.savePluginState(); // need objects present to update processor states
    Node::sanitizeRuntimeProperties (duplicate.data());
    // reset UUIDs to avoid compilcations with undoable actions
    duplicate.forEach ([] (const ValueTree& tree) {
        if (! tree.hasType (types::Node))
            return;
        auto nodeRef = tree;
        nodeRef.setProperty (tags::uuid, Uuid().toString(), nullptr);
    });

    duplicate.setProperty (tags::name, duplicate.getName().replace ("(copy)", "").trim() + String (" (copy)"));
    addGraph (duplicate, true);
}

void EngineService::duplicateGraph()
{
    auto& world = context();
    auto engine = world.audio();
    auto session = world.session();
    const Node current (session->getCurrentGraph());
    duplicateGraph (current);
}

void EngineService::removeGraph (int index)
{
    auto& world = context();
    auto engine = world.audio();
    auto session = world.session();

    if (index < 0)
        index = session->getActiveGraphIndex();

    const auto toRemove = session->getGraph (index);
    const auto active = session->getActiveGraph();
    const bool removedIsActive = toRemove == active;

    if (! toRemove.isValid())
    {
        DBG ("[element] cannot remove invalid graph");
        return;
    }

    if (auto* holder = graphs->findByEngineIndex (index))
    {
        bool removeIt = false;
        if (holder->detach (engine))
        {
            ValueTree sgraphs = session->data().getChildWithName (tags::graphs);
            sgraphs.removeChild (holder->model.data(), nullptr);
            removeIt = true;
        }
        else
        {
            DBG ("[element] could not detach root graph");
        }

        if (removeIt)
        {
            graphs->remove (holder);
            DBG ("[element] graph removed: index: " << index);
            ValueTree sgraphs = session->data().getChildWithName (tags::graphs);
            if (removedIsActive)
            {
                if (index < 0 || index >= session->getNumGraphs())
                    index = session->getNumGraphs() - 1;

                sgraphs.setProperty (tags::active, index, 0);
                const Node nextGraph = session->getCurrentGraph();

                if (nextGraph.isRootGraph())
                {
                    DBG ("[element] setting new graph: " << nextGraph.getName());
                    setRootNode (nextGraph);
                }
                else if (session->getNumGraphs() > 0)
                {
                    DBG ("[element] failed to find appropriate index.");
                    sgraphs.setProperty (tags::active, 0, nullptr);
                    setRootNode (session->getActiveGraph());
                }
            }
            else
            {
                index = std::max (0, sgraphs.indexOf (active.data()));
                sgraphs.setProperty (tags::active, index, nullptr);
                setRootNode (active);
            }
        }
    }
    else
    {
        DBG ("[element] could not find root graph index: " << index);
    }

    if (toRemove.isValid())
        sigNodeRemoved (toRemove);
    // FIXME: dont notify the UI top-down
    sibling<UI>()->stabilizeContent();
}

void EngineService::connectChannels (const Node& graph, const Node& src, const int sc, const Node& dst, const int dc)
{
    connectChannels (graph, src.getNodeId(), sc, dst.getNodeId(), dc);
}

void EngineService::connectChannels (const Node& graph, const uint32 s, const int sc, const uint32 d, const int dc)
{
    if (auto* root = graphs->findGraphManagerFor (graph))
    {
        auto src = root->getNodeForId (s);
        auto dst = root->getNodeForId (d);
        if (! src || ! dst)
            return;
        // clang-format off
        addConnection (src->nodeId, 
            src->getPortForChannel (PortType::Audio, sc, false), 
            dst->nodeId, 
            dst->getPortForChannel (PortType::Audio, dc, true));
        // clang-format on
    }
}

void EngineService::connectChannels (const uint32 s, const int sc, const uint32 d, const int dc)
{
    connectChannels (context().session()->getActiveGraph(), s, sc, d, dc);
}

void EngineService::connect (PortType type, const Node& src, int sc, const Node& dst, int dc, int nc)
{
    if (nc < 1)
        nc = 1;
    if (auto manager = graphs->findGraphManagerFor (src.getParentGraph()))
    {
        auto s = src.getObject();
        auto d = dst.getObject();
        while (s && d && --nc >= 0)
        {
            auto sp = s->getPortForChannel (type, sc, false);
            auto dp = d->getPortForChannel (type, dc, true);
            if (! manager->addConnection (src.getNodeId(), sp, dst.getNodeId(), dp))
            {
                String msg = "[element] connection failed: \n";

                msg << "Source: " << src.getName() << juce::newLine
                    << " ch. " << (int) sc << juce::newLine
                    << " port " << (int) sp << juce::newLine
                    << "Target: " << dst.getName() << juce::newLine
                    << " ch. " << (int) dc << juce::newLine
                    << " port " << (int) dp << juce::newLine;
                DBG (msg);
                break;
            };

            ++sc;
            ++dc;
        }
        manager->removeIllegalConnections();
        manager->syncArcsModel();
    }
}

void EngineService::removeConnection (const uint32 s, const uint32 sp, const uint32 d, const uint32 dp)
{
    if (auto* root = graphs->findActiveRootGraphManager())
        root->removeConnection (s, sp, d, dp);
}

void EngineService::removeConnection (const uint32 s, const uint32 sp, const uint32 d, const uint32 dp, const Node& target)
{
    if (auto* controller = graphs->findGraphManagerFor (target))
        controller->removeConnection (s, sp, d, dp);
}

Node EngineService::addNode (const Node& node, const Node& target, const ConnectionBuilder& builder)
{
    auto ref = node;
    if (EL_NODE_VERSION > node.version())
    {
        String error;
        const auto data = Node::migrate (node.data(), error);
        if (error.isEmpty() && data.isValid())
            ref = Node (data, true);
    }

    if (auto* controller = graphs->findGraphManagerFor (target))
    {
        const uint32 nodeId = controller->addNode (ref);
        ref = controller->getNodeModelForId (nodeId);
        if (ref.isValid())
        {
            builder.addConnections (*controller, nodeId);
            return ref;
        }
    }

    return Node();
}

Node EngineService::addNode (const String& ID, const String& format)
{
    juce::PluginDescription desc;
    desc.fileOrIdentifier = ID;
    desc.pluginFormatName = format;
    return addPlugin (desc, true, .5f, .5f, true);
}

void EngineService::addNode (const Node& _node)
{
    auto node = _node;
    if (EL_NODE_VERSION != node.version())
    {
        String error;
        auto data = Node::migrate (_node.data(), error);
        if (data.isValid() && error.isEmpty())
        {
            node = Node (data, false);
        }
        else
        {
            AlertWindow::showMessageBoxAsync (AlertWindow::WarningIcon, "Error adding node", error);
            return;
        }
    }

    auto* root = graphs->findActiveRootGraphManager();
    const uint32 nodeId = (root != nullptr) ? root->addNode (node) : EL_INVALID_NODE;
    if (EL_INVALID_NODE != nodeId)
    {
        const Node actual (root->getNodeModelForId (nodeId));
        if (context().settings().showPluginWindowsWhenAdded())
            sibling<GuiService>()->presentPluginWindow (actual);
    }
    else
    {
        AlertWindow::showMessageBox (AlertWindow::InfoIcon,
                                     "Audio Engine",
                                     String ("Could not add node: ") + node.getName());
    }
}

void EngineService::addPluginAsync (const PluginDescription& desc,
                                    bool verified,
                                    float rx,
                                    float ry,
                                    bool dontShowUI,
                                    std::function<void (const Node&)> userCallback)
{
    /* Element: async equivalent of addPlugin.  The cheap parts
     * (verify / scan / fallback resolution) run synchronously on
     * the message thread; the heavy createPluginInstance step is
     * dispatched to a worker via GraphManager::addNodeAsync.  Once
     * the worker callback lands back on the message thread we look
     * up the resulting Node, present the plugin window (matching
     * the sync addPlugin behaviour), and invoke the user callback. */
    auto* root = graphs->findActiveRootGraphManager();
    if (! root)
    {
        if (userCallback) userCallback ({});
        return;
    }

    auto descCopy = std::make_shared<PluginDescription> (desc);

    if (! verified)
    {
        auto* format = context().plugins().getAudioPluginFormat (descCopy->pluginFormatName);
        jassert (format != nullptr);
        auto& list (context().plugins().getKnownPlugins());
        list.removeFromBlacklist (descCopy->fileOrIdentifier);
        list.removeType (*descCopy);
        OwnedArray<PluginDescription> plugs;
        if (list.scanAndAddFile (descCopy->fileOrIdentifier, false, plugs, *format))
        {
            context().plugins().saveUserPlugins (context().settings());
        }
        if (plugs.size() == 0)
        {
            AlertWindow::showMessageBoxAsync (AlertWindow::NoIcon, "Add Plugin",
                String ("Could not add ") + descCopy->name + " for an unknown reason");
            if (userCallback) userCallback ({});
            return;
        }
        descCopy = std::make_shared<PluginDescription> (*plugs.getFirst());
    }

    /* Capture the GuiService raw — Services live as long as Context,
     * so by the time the async callback fires they're still valid.
     * RootGraphManager itself is guarded by addNodeAsync's
     * WeakReference; if the manager dies we get EL_INVALID_NODE back
     * and short-circuit cleanly. */
    auto* gui = sibling<GuiService>();
    bool showWindow = ! dontShowUI && context().settings().showPluginWindowsWhenAdded();

    root->addNodeAsync (descCopy.get(), rx, ry, 0,
        [this, root, descCopy, showWindow, gui, cb = std::move (userCallback)] (uint32 nodeId)
        {
            Node node;
            if (nodeId != EL_INVALID_NODE)
            {
                node = root->getNodeModelForId (nodeId);
                if (showWindow && gui != nullptr && node.isValid())
                    gui->presentPluginWindow (node);
            }
            if (cb) cb (node);
        });
}

Node EngineService::addPlugin (const PluginDescription& desc, const bool verified, const float rx, const float ry, bool dontShowUI)
{
    auto* root = graphs->findActiveRootGraphManager();
    if (! root)
    {
        return {};
    }

    OwnedArray<PluginDescription> plugs;
    if (! verified)
    {
        auto* format = context().plugins().getAudioPluginFormat (desc.pluginFormatName);
        jassert (format != nullptr);
        auto& list (context().plugins().getKnownPlugins());
        list.removeFromBlacklist (desc.fileOrIdentifier);
        list.removeType (desc);
        if (list.scanAndAddFile (desc.fileOrIdentifier, false, plugs, *format))
        {
            context().plugins().saveUserPlugins (context().settings());
        }
    }
    else
    {
        plugs.add (new PluginDescription (desc));
    }

    Node node;
    if (plugs.size() > 0)
    {
        const auto nodeId = root->addNode (plugs.getFirst(), rx, ry);
        if (EL_INVALID_NODE != nodeId)
        {
            node = root->getNodeModelForId (nodeId);
            if (! dontShowUI && context().settings().showPluginWindowsWhenAdded())
                sibling<GuiService>()->presentPluginWindow (node);
        }
    }
    else
    {
        AlertWindow::showMessageBoxAsync (AlertWindow::NoIcon, "Add Plugin", String ("Could not add ") + desc.name + " for an unknown reason");
    }
    return node;
}

void EngineService::removeNode (const Node& node)
{
    const Node graph (node.getParentGraph());
    if (! graph.isGraph())
        return;

    auto* const gui = sibling<GuiService>();
    if (auto* manager = graphs->findGraphManagerFor (graph))
    {
        jassert (manager->contains (node.getNodeId()));
        gui->closePluginWindowsFor (node, true);
        if (gui->getSelectedNode() == node)
            gui->selectNode (Node());
        manager->removeNode (node.getNodeId());
        sigNodeRemoved (node);
    }

    /* Removing an IONode block must NOT zero the parent graph's
     * port count.  The configured count (set via the Graph
     * properties Audio Ins/Outs slider, the Preferences "Audio in
     * ports" / "Audio out ports" spinners, or the session model)
     * is the source of truth — JACK exposes that many ports, and
     * re-adding the IONode should pick the same count back up via
     * IONode::refreshPorts() → graph->getNumPorts().  The old
     * `setNumPorts(0)` here was a workaround for an obsolete
     * GraphNode::removeNode that cleared parent ports; current
     * GraphNode::removeNode (engine/graphnode.cpp) doesn't touch
     * port counts, so this branch is dead weight that breaks
     * remove-then-readd by forcing the new IONode to fall back to
     * IONode::setParentGraph's default (2 audio / 1 midi). */
    juce::ignoreUnused (graph);
}

void EngineService::removeNode (const Uuid& uuid)
{
    auto session = context().session();
    const auto node = session->findNodeById (uuid);
    if (node.isValid())
    {
        removeNode (node);
    }
    else
    {
        DBG ("[element] node not found: " << uuid.toString());
    }
}

void EngineService::removeNode (const uint32 nodeId)
{
    auto* root = graphs->findActiveRootGraphManager();
    if (! root)
        return;
    if (auto* gui = sibling<GuiService>())
        gui->closePluginWindowsFor (nodeId, true);
    root->removeNode (nodeId);
}

void EngineService::disconnectNode (const Node& node, const bool inputs, const bool outputs, const bool audio, const bool midi)
{
    const auto graph (node.getParentGraph());
    if (auto* controller = graphs->findGraphManagerFor (graph))
        controller->disconnectNode (node.getNodeId(), inputs, outputs, audio, midi);
}

void EngineService::activate()
{
    Service::activate();

    auto& globals (context());
    auto engine (globals.audio());
    auto session (globals.session());
    engine->setSession (session);
    engine->activate();

    sessionReloaded();
}

void EngineService::deactivate()
{
    Service::deactivate();
    auto& globals (context());
    auto engine (globals.audio());
    auto session (globals.session());

    if (auto* const gui = sibling<UI>())
    {
        // UI might not deactivate before the engine, so
        // close the windows here
        gui->closeAllPluginWindows();
    }

    /* Flush the active graph's timeline automation into the session tree
     * before it serializes (safety net for the close/save path; the
     * arrangement view also flushes on edit).  Guard on a NON-empty
     * engine: saveToValueTree removes the existing automationTracks child
     * before writing, so an empty-engine flush would erase previously-
     * saved automation. */
    if (auto* holder = graphs->findActive())
        if (auto* rg = holder->getRootGraph())
            if (auto* ae = rg->automationEngine())
                if (ae->numTracks() > 0)
                    automation::saveAutomationToSession (*ae, session->data());

    session->saveGraphState();
    graphs->clear();

    engine->deactivate();
    engine->setSession (nullptr);
}

void EngineService::clear()
{
    graphs->clear();
}

void EngineService::setRootNode (const Node& newRootNode)
{
    if (! newRootNode.isRootGraph())
    {
        jassertfalse; // needs to be a graph
        return;
    }

    auto* holder = graphs->findFor (newRootNode);
    if (! holder)
    {
        jassertfalse; // you should have a root graph registered before calling this.
        holder = graphs->add (new RootGraphHolder (newRootNode, context()));
    }

    if (! holder)
    {
        DBG ("[element] failed to find root graph for node: " << newRootNode.getName());
        return;
    }

    auto engine = context().audio();
    auto session = context().session();
    auto& devices = context().devices();

    if (! holder->attached())
        holder->attach (engine);
    const int index = holder->getRootGraph()->getEngineIndex();

#if 0
    // saving this for reference. graphs will need to be
    // explicitly de-activated by users to unload them
    // moving forward - MRF
    
    /* Unload the active graph if necessary */
    auto* active = graphs->findActiveInEngine();
    if (active && active != holder)
    {
        if (auto* gui = sibling<GuiService>())
            gui->closeAllPluginWindows();
        
        if (! (bool) active->model.getProperty(tags::persistent) && active->attached())
        {
            active->controller->savePluginStates();
            active->controller->unloadGraph();
            DBG("[element] graph unloaded: " << active->model.getName());
        }
    }
#endif

    if (auto* proc = holder->getRootGraph())
    {
        proc->setMidiChannels (newRootNode.getMidiChannels().get());
        proc->setVelocityCurveMode ((VelocityCurve::Mode) (int) newRootNode.getProperty (
            tags::velocityCurveMode, (int) VelocityCurve::Linear));

        // TODO: Uniform method for saving/restoring nodes with custom ports.
        detail::initializeRootGraphPorts (proc, newRootNode);
    }
    else
    {
        DBG ("[element] couldn't find graph processor for node.");
    }

    if (auto* const r = holder->getController())
    {
        if (! r->isLoaded())
        {
            r->getRootGraph().setPlayConfigFor (devices);
            r->setNodeModel (newRootNode);
        }

        engine->setCurrentGraph (index);
    }
    else
    {
        DBG ("[element] no graph controller for node: " << newRootNode.getName());
    }

    /* Timeline automation is song-owned (see memory
     * project_automation_timeline_vs_clip_semantics): populate the active
     * graph's AutomationEngine from the session + rebind targets now that
     * the graph's nodes are instantiated.  View-INDEPENDENT -- runs
     * whether or not the arrangement view is ever opened.  Guard on an
     * empty engine so re-activating a graph doesn't clobber runtime
     * edits (single active graph; multi-graph automation is future). */
    if (auto* rg = holder->getRootGraph())
        if (auto* ae = rg->automationEngine())
            if (ae->numTracks() == 0)
                if (auto sess = context().session())
                    automation::restoreAutomationFromSession (*ae, sess->data(), newRootNode);

    engine->refreshSession();
}

void EngineService::syncModels()
{
    for (auto* holder : graphs->getGraphs())
    {
        Node graph (holder->model);
        for (int i = 0; i < graph.getNumNodes(); ++i)
        {
            Node node (graph.getNode (i));
            if (! node.isIONode())
                continue;
            node.resetPorts();
        }

        if (auto* controller = holder->getController())
        {
            controller->syncArcsModel();
        }
    }
}

Node EngineService::addPlugin (const Node& graph, const PluginDescription& desc)
{
    if (! graph.isGraph())
        return {};

    Node node;
    if (auto* controller = graphs->findGraphManagerFor (graph))
        node = addPlugin (*controller, desc);

    return node;
}

Node EngineService::addPlugin (const Node& graph, const PluginDescription& desc,
                                float rx, float ry)
{
    if (! graph.isGraph())
        return {};

    auto* controller = graphs->findGraphManagerFor (graph);
    if (controller == nullptr)
        return {};

    /* Spawn via the rx/ry-aware GraphManager::addNode path.  The
     * ValueTree gets tags::relativeX / relativeY set BEFORE the
     * BlockComponent reads them at first updatePosition, so the
     * placement is visible on initial paint -- no post-spawn fixup
     * needed.  Plain addPlugin (graph, desc) hardcodes 0.5/0.5;
     * this overload threads through caller-chosen coords. */
    auto& plugins (context().plugins());
    const auto nodeId = controller->addNode (&desc, rx, ry, 0);

    if (EL_INVALID_NODE == nodeId)
        return {};

    plugins.addToKnownPlugins (desc);

    const Node node (controller->getNodeModelForId (nodeId));
    if (! node.isValid())
    {
        jassertfalse;   /* fatal but non-crashing */
        return {};
    }

    if (node.getUuid().isNull())
    {
        ValueTree nodeData = node.data();
        nodeData.setProperty (tags::uuid, Uuid().toString(), 0);
    }

    return node;
}

void EngineService::addPluginAsync (const Node& graph,
                                    const PluginDescription& desc,
                                    const ConnectionBuilder& builder,
                                    bool verified,
                                    std::function<void (const Node&)> userCallback)
{
    /* Element: async variant of addPlugin(graph, desc, builder,
     * verified).  See PluginManager::createAudioPluginAsync for the
     * worker-thread mechanics.  Verify/scan prework happens
     * synchronously on the message thread, the heavy load + post-
     * load setup runs through GraphManager::addNodeAsync, then the
     * builder applies any auto-connections back on the message
     * thread inside our completion callback. */
    if (! graph.isGraph())
    {
        if (userCallback) userCallback ({});
        return;
    }

    auto* controller = graphs->findGraphManagerFor (graph);
    if (controller == nullptr)
    {
        if (userCallback) userCallback ({});
        return;
    }

    auto& list (context().plugins().getKnownPlugins());
    PluginDescription descToLoad = desc;

    if (! verified)
    {
        if (desc.pluginFormatName == "LV2")
        {
            if (auto lv2 = context().plugins().getProvider ("LV2"))
            {
                juce::ignoreUnused (lv2);
                list.removeFromBlacklist (desc.fileOrIdentifier);
                list.addType (desc);
            }
        }
        else
        {
            auto* format = context().plugins().getAudioPluginFormat (desc.pluginFormatName);
            jassert (format != nullptr);
            list.removeFromBlacklist (desc.fileOrIdentifier);
            OwnedArray<PluginDescription> plugs;
            if (list.scanAndAddFile (desc.fileOrIdentifier, false, plugs, *format))
            {
                context().plugins().saveUserPlugins (context().settings());
            }
            if (plugs.size() > 0)
                descToLoad = *plugs.getFirst();
        }
    }

    /* Capture by value where possible — descToLoad is a struct, the
     * builder doesn't survive easily through an async hop so we copy
     * it.  controller pointer is fine since the GraphManager outlives
     * the async hop (and addNodeAsync itself uses a WeakReference). */
    auto builderCopy = std::make_shared<ConnectionBuilder> (builder);
    controller->addNodeAsync (&descToLoad, 0.5, 0.5, 0,
        [controller, builderCopy, cb = std::move (userCallback)] (uint32 nodeId) mutable
        {
            Node node;
            if (nodeId != EL_INVALID_NODE)
            {
                node = controller->getNodeModelForId (nodeId);
                if (node.isValid())
                {
                    builderCopy->addConnections (*controller, nodeId);
                    jassert (! node.getUuid().isNull());
                }
            }
            if (cb) cb (node);
        });
}

Node EngineService::addPlugin (const Node& graph, const PluginDescription& desc, const ConnectionBuilder& builder, const bool verified)
{
    if (! graph.isGraph())
        return Node();

    auto& list (context().plugins().getKnownPlugins());
    OwnedArray<PluginDescription> plugs;

    if (! verified)
    {
        if (desc.pluginFormatName == "LV2")
        {
            if (auto lv2 = context().plugins().getProvider ("LV2"))
            {
                juce::ignoreUnused (lv2);
                list.removeFromBlacklist (desc.fileOrIdentifier);
                list.addType (desc);
            }
        }
        else
        {
            auto* format = context().plugins().getAudioPluginFormat (desc.pluginFormatName);
            jassert (format != nullptr);

            list.removeFromBlacklist (desc.fileOrIdentifier);

            if (list.scanAndAddFile (desc.fileOrIdentifier, false, plugs, *format))
            {
                context().plugins().saveUserPlugins (context().settings());
            }
        }
    }
    else
    {
        plugs.add (new PluginDescription (desc));
    }

    const PluginDescription descToLoad = (plugs.size() > 0) ? *plugs.getFirst() : desc;

    if (auto* controller = graphs->findGraphManagerFor (graph))
    {
        const Node node (addPlugin (*controller, descToLoad));
        if (node.isValid())
        {
            builder.addConnections (*controller, node.getNodeId());
            jassert (! node.getUuid().isNull());
        }
        return node;
    }

    return Node();
}

void EngineService::sessionReloaded()
{
    graphs->clear();

    auto session = context().session();
    auto engine = context().audio();

    if (session->getNumGraphs() > 0)
    {
        for (int i = 0; i < session->getNumGraphs(); ++i)
        {
            Node rootGraph (session->getGraph (i));
            if (auto* holder = graphs->add (new RootGraphHolder (rootGraph, context())))
            {
                if (! holder->attach (engine))
                {
                    std::clog << "[element] failed attaching root grapn: " << holder->model.getName() << std::endl;
                }

                if (auto* const controller = holder->getController())
                {
                    // noop: saving this logical block
                }
            }
        }

        const auto ag = session->getActiveGraph();
        setRootNode (ag);
    }

    if (session->getNumGraphs() != graphs->getGraphs().size())
    {
        Logger::writeToLog ("[element] model and engine graph counts do not match");
    }
#if ELEMENT_TRACE_SESSION_LOAD
    else
    {
        String msg ("[element] session reloaded");
        if (session->getName().isNotEmpty())
            msg << ": " << session->getName();
        DBG (msg);
    }
#endif
}

Node EngineService::addPlugin (GraphManager& c, const PluginDescription& desc)
{
    auto& plugins (context().plugins());
    const auto nodeId = c.addNode (&desc, 0.5f, 0.5f, 0);

    if (EL_INVALID_NODE != nodeId)
    {
        plugins.addToKnownPlugins (desc);

        const Node node (c.getNodeModelForId (nodeId));
        if (context().settings().showPluginWindowsWhenAdded())
            sibling<GuiService>()->presentPluginWindow (node);
        if (! node.isValid())
        {
            jassertfalse; // fatal, but continue
        }

        if (node.getUuid().isNull())
        {
            jassertfalse;
            ValueTree nodeData = node.data();
            nodeData.setProperty (tags::uuid, Uuid().toString(), 0);
        }
        return node;
    }

    return Node();
}

#if ELEMENT_USE_JACK
void EngineService::dispatchProgramChangeFromNode (juce::AudioProcessor* source, int program)
{
    if (source == nullptr || program < 0 || program > 127)
        return;
    auto* const rootMgr = graphs->findActiveRootGraphManager();
    if (rootMgr == nullptr)
        return;
    auto& graph = rootMgr->getRootGraph();

    /* Locate the Processor wrapping `source` in the active root
     * graph.  We only support nodes that live at the root level for
     * MVP — sub-graphs are not walked. */
    uint32 sourceNodeId = 0;
    for (int i = 0; i < graph.getNumNodes(); ++i)
    {
        if (auto* p = graph.getNode (i))
        {
            if (p->getAudioProcessor() == source)
            {
                sourceNodeId = p->nodeId;
                break;
            }
        }
    }
    if (sourceNodeId == 0)
        return;

    /* BFS through MIDI connections downstream from the source node.
     * Follows only connections whose sourcePort matches each visited
     * node's MIDI output port, so audio / control connections aren't
     * traversed.  Visited set bounds the walk against cycles. */
    juce::SortedSet<uint32> visited;
    juce::Array<uint32> frontier;
    visited.add (sourceNodeId);
    frontier.add (sourceNodeId);

    while (! frontier.isEmpty())
    {
        const uint32 current = (uint32) frontier.getFirst();
        frontier.remove (0);

        auto* const currentProc = graph.getNodeForId (current);
        if (currentProc == nullptr)
            continue;
        const uint32 midiOut = currentProc->getMidiOutputPort();

        for (int i = 0; i < graph.getNumConnections(); ++i)
        {
            const auto* const conn = graph.getConnection (i);
            if (conn == nullptr) continue;
            if (conn->sourceNode != current) continue;
            if (conn->sourcePort != midiOut) continue;
            if (visited.contains (conn->destNode)) continue;
            visited.add (conn->destNode);
            frontier.add (conn->destNode);
        }
    }

    /* Apply setCurrentProgram to every visited node except the
     * source.  Internal Element nodes typically expose one program
     * and treat the call as a no-op; real VST/VST3/AU plugins switch
     * via the VST setProgram dispatcher.  Bounds-checked against
     * each plugin's actual program count — out-of-range PCs are
     * dropped silently per VST convention. */
    for (int i = 0; i < graph.getNumNodes(); ++i)
    {
        auto* const p = graph.getNode (i);
        if (p == nullptr) continue;
        if (p->nodeId == sourceNodeId) continue;
        if (! visited.contains (p->nodeId)) continue;

        auto* const ap = p->getAudioProcessor();
        if (ap == nullptr) continue;
        const int nprog = ap->getNumPrograms();
        if (nprog <= 1) continue;          /* nothing to switch */
        if (program >= nprog) continue;    /* out of range */

        ap->setCurrentProgram (program);
    }
}

Node EngineService::addJackMidiOutputNode (int portIndex)
{
    ProcessorPtr ptr;
    Node graph;
    if (auto s = context().session())
        graph = s->getActiveGraph();
    if (auto* const root = graphs->findActiveRootGraphManager())
    {
        PluginDescription desc;
        desc.pluginFormatName = EL_NODE_FORMAT_NAME;
        desc.fileOrIdentifier = EL_NODE_ID_JACK_MIDI_OUTPUT;
        ptr = root->getNodeForId (root->addNode (&desc, 0.5, 0.5));
    }

    auto* const proc = (ptr == nullptr) ? nullptr
                                        : dynamic_cast<JackMidiOutputNode*> (ptr->getAudioProcessor());

    Node deviceNode;
    if (proc != nullptr)
    {
        proc->setPortIndex (portIndex);

        for (int i = 0; i < graph.getNumNodes(); ++i)
        {
            auto node (graph.getNode (i));
            if (node.getObject() == ptr.get())
            {
                node.setProperty (tags::name, proc->getName());
                node.resetPorts();
                deviceNode = node;
                break;
            }
        }
    }
    return deviceNode;
}

Node EngineService::addJackMidiInputNode (int portIndex)
{
    ProcessorPtr ptr;
    Node graph;
    if (auto s = context().session())
        graph = s->getActiveGraph();
    if (auto* const root = graphs->findActiveRootGraphManager())
    {
        PluginDescription desc;
        desc.pluginFormatName = EL_NODE_FORMAT_NAME;
        desc.fileOrIdentifier = EL_NODE_ID_JACK_MIDI_INPUT;
        ptr = root->getNodeForId (root->addNode (&desc, 0.5, 0.5));
    }

    auto* const proc = (ptr == nullptr) ? nullptr
                                        : dynamic_cast<JackMidiInputNode*> (ptr->getAudioProcessor());

    Node deviceNode;
    if (proc != nullptr)
    {
        proc->setPortIndex (portIndex);

        for (int i = 0; i < graph.getNumNodes(); ++i)
        {
            auto node (graph.getNode (i));
            if (node.getObject() == ptr.get())
            {
                node.setProperty (tags::name, proc->getName());
                node.resetPorts();
                deviceNode = node;
                break;
            }
        }
    }
    return deviceNode;
}
#endif

Node EngineService::addMidiDeviceNode (const MidiDeviceInfo& device, const bool isInput)
{
    ProcessorPtr ptr;
    Node graph;
    if (auto s = context().session())
        graph = s->getActiveGraph();
    if (auto* const root = graphs->findActiveRootGraphManager())
    {
        PluginDescription desc;
        desc.pluginFormatName = EL_NODE_FORMAT_NAME;
        desc.fileOrIdentifier = isInput ? EL_NODE_ID_MIDI_INPUT_DEVICE
                                        : EL_NODE_ID_MIDI_OUTPUT_DEVICE;
        ptr = root->getNodeForId (root->addNode (&desc, 0.5, 0.5));
    }

    MidiDeviceProcessor* const proc = (ptr == nullptr) ? nullptr
                                                       : dynamic_cast<MidiDeviceProcessor*> (ptr->getAudioProcessor());

    Node deviceNode;
    if (proc != nullptr)
    {
        proc->setDevice (device);

        for (int i = 0; i < graph.getNumNodes(); ++i)
        {
            auto node (graph.getNode (i));
            if (node.getObject() == ptr.get())
            {
                node.setProperty (tags::name, proc->getDeviceName());
                node.resetPorts();
                deviceNode = node;
                break;
            }
        }
    }
    else
    {
        // noop
    }

    return deviceNode;
}

void EngineService::changeBusesLayout (const Node& n, const AudioProcessor::BusesLayout& layout)
{
    Node node = n;
    Node graph = node.getParentGraph();
    ProcessorPtr ptr = node.getObject();
    auto* controller = graphs->findGraphManagerFor (graph);
    if (! controller)
        return;

    if (AudioProcessor* proc = ptr ? ptr->getAudioProcessor() : nullptr)
    {
        ProcessorPtr ptr2 = graph.getObject();
        if (auto* gp = dynamic_cast<GraphNode*> (ptr2.get()))
        {
            if (proc->checkBusesLayoutSupported (layout))
            {
                while (! gp->isSuspended())
                    gp->suspendProcessing (true);
                gp->releaseResources();

                const bool wasNotSuspended = ! proc->isSuspended();
                proc->suspendProcessing (true);
                proc->releaseResources();
                proc->setBusesLayoutWithoutEnabling (layout);
                node.resetPorts();
                if (wasNotSuspended)
                    proc->suspendProcessing (false);

                gp->prepareToRender (gp->getSampleRate(), gp->getBlockSize());

                while (gp->isSuspended())
                    gp->suspendProcessing (false);

                controller->removeIllegalConnections();
                controller->syncArcsModel();

                sibling<GuiService>()->stabilizeViews();
            }
        }
    }
}

void EngineService::replace (const Node& node, const PluginDescription& desc)
{
    const auto graph (node.getParentGraph());
    if (! graph.isGraph())
        return;

    if (auto* ctl = graphs->findGraphManagerFor (graph))
    {
        double x = 0.0, y = 0.0;
        node.getPosition (x, y);
        const auto oldNodeId = node.getNodeId();
        const auto wasWindowOpen = (bool) node.getProperty ("windowVisible");
        const auto nodeId = ctl->addNode (&desc, x, y);
        if (nodeId != EL_INVALID_NODE)
        {
            ProcessorPtr newptr = ctl->getNodeForId (nodeId);
            const ProcessorPtr oldptr = node.getObject();
            jassert (newptr && oldptr);
            // attempt to retain connections from the replaced node
            for (int i = ctl->getNumConnections(); --i >= 0;)
            {
                const auto* arc = ctl->getConnection (i);
                if (oldNodeId == arc->sourceNode)
                {
                    ctl->addConnection (
                        nodeId,
                        newptr->getPortForChannel (
                            oldptr->getPortType (arc->sourcePort),
                            oldptr->getChannelPort (arc->sourcePort),
                            oldptr->isPortInput (arc->sourcePort)),
                        arc->destNode,
                        arc->destPort);
                }
                else if (oldNodeId == arc->destNode)
                {
                    ctl->addConnection (
                        arc->sourceNode,
                        arc->sourcePort,
                        nodeId,
                        newptr->getPortForChannel (
                            oldptr->getPortType (arc->destPort),
                            oldptr->getChannelPort (arc->destPort),
                            oldptr->isPortInput (arc->destPort)));
                }
            }

            auto newNode (ctl->getNodeModelForId (nodeId));
            newNode.setPosition (x, y); // TODO: GraphManager should handle these
            newNode.setProperty ("windowX", (int) node.getProperty ("windowX"))
                .setProperty ("windowY", (int) node.getProperty ("windowY"));

            removeNode (node);
            if (wasWindowOpen)
                sibling<GuiService>()->presentPluginWindow (newNode);
        }
    }

    sibling<GuiService>()->stabilizeViews();
}

} // namespace element
