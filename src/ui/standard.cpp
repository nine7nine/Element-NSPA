// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <element/context.hpp>
#include <element/devices.hpp>
#include <element/node.hpp>
#include <element/plugins.hpp>
#include <element/services.hpp>
#include <element/settings.hpp>
#include <element/ui/commands.hpp>
#include <element/ui/mainwindow.hpp>
#include <element/ui/meterbridge.hpp>
#include <element/ui/navigation.hpp>
#include <element/ui/standard.hpp>
#include <element/ui/style.hpp>

#include "services/mappingservice.hpp"
#include "services/sessionservice.hpp"
#include "ui/audioiopanelview.hpp"
#include "ui/connectiongrid.hpp"
#include "ui/arrangementview.hpp"
#include "ui/sessionview.hpp"
#include "ui/controllersview.hpp"
#include "ui/trackersidedock.hpp"
#include "ui/pianoroll/pianoroll_view.hpp"
#include "ui/trackerhostview.hpp"
#include "ui/diskopview.hpp"
#include "ui/datapathbrowser.hpp"
#include "ui/emptyview.hpp"
#include "ui/grapheditorview.hpp"
#include "ui/graphmixerview.hpp"
#include "ui/keymapeditorview.hpp"
#include "ui/luaconsoleview.hpp"
#include "ui/mainmenu.hpp"
#include "ui/navigationview.hpp"
#include "ui/navigationsidebar.hpp"
#include "ui/nodechannelstripview.hpp"
#include "ui/pluginspanelview.hpp"
#include "ui/sessiontreepanel.hpp"
#include "ui/viewhelpers.hpp"

#include "ui/audioiopanelview.hpp"
#include "ui/graphsettingsview.hpp"
#include "ui/nodeeditorview.hpp"
#include "ui/nodepropertiesview.hpp"
#include "ui/pluginmanagercomponent.hpp"
#include "ui/pluginspanelview.hpp"
#include "ui/sessionsettingsview.hpp"
#include "ui/sessiontreepanel.hpp"
#include "ui/virtualkeyboardview.hpp"

#define EL_NAV_MIN_WIDTH 170
#define EL_NAV_MAX_WIDTH 510

namespace element {

//=============================================================================
class SmartLayoutManager : public StretchableLayoutManager
{
public:
    SmartLayoutManager() {}
    virtual ~SmartLayoutManager() {}

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SmartLayoutManager)
};

class SmartLayoutResizeBar : public StretchableLayoutResizerBar
{
public:
    Signal<void()> mousePressed, mouseReleased;
    SmartLayoutResizeBar (StretchableLayoutManager* layoutToUse,
                          int itemIndexInLayout,
                          bool isBarVertical)
        : StretchableLayoutResizerBar (layoutToUse, itemIndexInLayout, isBarVertical)
    {
        mousePressed.disconnect_all_slots();
        mouseReleased.disconnect_all_slots();
    }

    ~SmartLayoutResizeBar()
    {
    }

    void mouseDown (const MouseEvent& ev) override
    {
        StretchableLayoutResizerBar::mouseDown (ev);
        mousePressed();
    }

    void mouseUp (const MouseEvent& ev) override
    {
        StretchableLayoutResizerBar::mouseUp (ev);
        mouseReleased();
    }
};

// MARK: Content container

class ContentContainer : public Component
{
public:
    ContentContainer (StandardContent& cc, Services& app)
        : owner (cc)
    {
        /* No default-placeholder ContentView -- primary + secondary
         * start null and the first setMainView / setSecondaryView
         * attaches a cached instance owned by StandardContent.
         * resized() and getMainViewName() both already guard the
         * null case. */
        bar.reset (new SmartLayoutResizeBar (&layout, 1, false));
        addAndMakeVisible (bar.get());
        bar->mousePressed.connect (
            std::bind (&ContentContainer::updateLayout, this));
        bar->mouseReleased.connect (
            std::bind (&ContentContainer::lockLayout, this));

        bottom.reset (new Bottom (cc));
        addAndMakeVisible (bottom.get());
        bottom->setVisible (false);

        updateLayout();
        resized();
    }

    virtual ~ContentContainer() {}

    void resized() override
    {
        if (primary == nullptr)
            return;

        auto r = getLocalBounds();
        if (bottom->requiredHeight() > 0)
        {
            bottom->setVisible (true);
            bottom->setBounds (r.removeFromBottom (bottom->requiredHeight()));
            r.removeFromBottom (1);
        }
        else
        {
            bottom->setVisible (false);
        }

        if (showAccessoryView && secondary != nullptr)
        {
            Component* comps[] = {
                primary,
                bar.get(),
                secondary
            };

            layout.layOutComponents (comps, 3, 0, 0, r.getWidth(), r.getHeight(), true, true);
            lastSecondaryHeight = secondary->getHeight();
        }
        else
        {
            primary->setBounds (r);
        }
    }

    void setNode (const Node& node)
    {
        if (auto* gdv = dynamic_cast<GraphDisplayView*> (primary))
            gdv->setNode (node);
        else if (auto* grid = dynamic_cast<ConnectionGrid*> (primary))
            grid->setNode (node);
        else if (auto* ed = dynamic_cast<GraphEditorView*> (primary))
            ed->setNode (node);
        else if (nullptr != primary)
            primary->stabilizeContent();
    }

    void setMainView (ContentView* view)
    {
        /* Non-owning attach.  The view's lifetime is managed by
         * StandardContent::viewCache_; this method just routes
         * willBeRemoved / willBecomeActive / didBecomeActive +
         * Component parent membership.  Passing nullptr detaches
         * the current view.
         *
         * initializeView is NOT called here -- it runs exactly once
         * per cached view instance at cache-population time inside
         * StandardContent::lookupOrCreateMainView.  Re-invoking it
         * on every attach was destroying / recreating child trees
         * for views with initializeView impls that allocate (e.g.
         * GraphMixerView's content.reset(new Content)). */

        if (primary != nullptr && primary != view)
        {
            primary->willBeRemoved();
            removeChildComponent (primary);
        }

        primary = view;

        if (primary)
        {
            primary->willBecomeActive();
            addAndMakeVisible (primary);
        }

        resized();

        if (primary)
        {
            primary->didBecomeActive();
            primary->stabilizeContent();
        }
    }

    void setSecondaryView (ContentView* view)
    {
        /* Non-owning attach -- mirror of setMainView.  initializeView
         * is handled at cache-population time, not per-attach (see
         * setMainView for rationale). */

        if (secondary != nullptr && secondary != view)
        {
            secondary->willBeRemoved();
            removeChildComponent (secondary);
        }

        secondary = view;

        if (secondary)
        {
            secondary->willBecomeActive();
            addAndMakeVisible (secondary);
        }

        setShowAccessoryView (secondary != nullptr, true);

        if (secondary)
        {
            secondary->didBecomeActive();
            secondary->stabilizeContent();
        }
    }

    void setShowAccessoryView (const bool show, bool force = false)
    {
        if (showAccessoryView == show && ! force)
            return;
        showAccessoryView = show;
        if (showAccessoryView)
        {
            lastSecondaryHeight = jmax (50, lastSecondaryHeight);
            layout.setItemLayout (0, 48, -1.0, primary->getHeight() - 4 - lastSecondaryHeight);
            layout.setItemLayout (1, barSize, barSize, barSize);
            layout.setItemLayout (2, 48, -1.0, lastSecondaryHeight);
        }

        resized();
    }

    void saveState (PropertiesFile* props)
    {
        props->setValue ("ContentContainer_lastSecondaryHeight", lastSecondaryHeight);
        props->setValue ("ContentContainer_showAccessoryView", showAccessoryView);
        props->setValue ("ContentContainer_lastSecondaryView", owner.getAccessoryViewName());
    }

    void restoreState (PropertiesFile* props)
    {
        lastSecondaryHeight = props->getIntValue ("ContentContainer_lastSecondaryHeight", lastSecondaryHeight);
        lastSecondaryHeight = jmax (50, lastSecondaryHeight);
        showAccessoryView = props->getIntValue ("ContentContainer_showAccessoryView", showAccessoryView);
        auto lastSecondaryName = props->getValue ("ContentContainer_lastSecondaryView");
        if (showAccessoryView)
        {
            owner.setSecondaryView (lastSecondaryName.trim());
        }
        else
        {
            setShowAccessoryView (false, true);
        }
    }

private:
    friend class StandardContent;
    StandardContent& owner;

    StretchableLayoutManager layout;
    /* Non-owning -- views live in StandardContent::viewCache_.  See
     * the note in standard.hpp + the cache rationale above the
     * viewCache_ member. */
    ContentView* primary { nullptr };
    std::unique_ptr<SmartLayoutResizeBar> bar;
    ContentView* secondary { nullptr };

    class Bottom : public juce::Component
    {
    public:
        StandardContent& standard;
        Bottom (StandardContent& s) : standard (s)
        {
            keyboard = std::make_unique<VirtualKeyboardView>();
            keyboard->willBecomeActive();
            addAndMakeVisible (keyboard.get());
            keyboard->initializeView (standard.services());
            keyboard->didBecomeActive();

            bridge = std::make_unique<MeterBridgeView>();
            bridge->willBecomeActive();
            addAndMakeVisible (bridge.get());
            bridge->initializeView (standard.services());
            bridge->didBecomeActive();
        }

        ~Bottom() {}

        void paint (Graphics& g) override
        {
            g.fillAll (Colours::black);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            if (keyboard->isVisible() && bridge->isVisible())
            {
                keyboard->setBounds (r.removeFromBottom (80));
                r.removeFromBottom (1);
                bridge->setBounds (r.removeFromBottom (80));
            }
            else if (keyboard->isVisible())
                keyboard->setBounds (r.removeFromBottom (80));
            else if (bridge->isVisible())
                bridge->setBounds (r.removeFromBottom (80));
        }

        int requiredHeight()
        {
            int h = keyboard->isVisible() ? 80 : 0;
            h += (bridge->isVisible() ? 80 : 0);
            if (keyboard->isVisible() && bridge->isVisible())
                h += 1;
            return h;
        }

        std::unique_ptr<VirtualKeyboardView> keyboard;
        std::unique_ptr<MeterBridgeView> bridge;
    };

    std::unique_ptr<Bottom> bottom;

    bool showAccessoryView = false;
    int barSize = 2;
    int lastSecondaryHeight = 172;
    int capturedAccessoryHeight = -1;
    bool locked = true;

    void lockLayout()
    {
        locked = true;

        const int primaryMin = 48;
        if (showAccessoryView)
        {
            layout.setItemLayout (0, primaryMin, -1.0, primary->getHeight());
            layout.setItemLayout (1, barSize, barSize, barSize);
            layout.setItemLayout (2, secondary->getHeight(), secondary->getHeight(), secondary->getHeight());
        }

        resized();

        if (showAccessoryView)
        {
            capturedAccessoryHeight = secondary->getHeight();
        }
    }

    void updateLayout()
    {
        locked = false;

        if (showAccessoryView)
        {
            layout.setItemLayout (0, 48, -1.0, primary->getHeight());
            layout.setItemLayout (1, barSize, barSize, barSize);
            layout.setItemLayout (2, 48, -1.0, secondary->getHeight());
        }

        resized();

        if (showAccessoryView)
        {
            capturedAccessoryHeight = secondary->getHeight();
        }
    }
};

class StandardContent::Resizer : public StretchableLayoutResizerBar
{
public:
    Resizer (StandardContent& StandardContent, StretchableLayoutManager* layoutToUse, int itemIndexInLayout, bool isBarVertical)
        : StretchableLayoutResizerBar (layoutToUse, itemIndexInLayout, isBarVertical),
          owner (StandardContent)
    {
    }

    void mouseDown (const MouseEvent& ev) override
    {
        StretchableLayoutResizerBar::mouseDown (ev);
        owner.resizerMouseDown();
    }

    void mouseUp (const MouseEvent& ev) override
    {
        StretchableLayoutResizerBar::mouseUp (ev);
        owner.resizerMouseUp();
    }

private:
    StandardContent& owner;
};

//==============================================================================
#if 0
static ContentView* createLastContentView (Settings& settings)
{
    auto* props = settings.getUserSettings();
    const String lastContentView = props->getValue ("lastContentView");
    std::unique_ptr<ContentView> view;
    typedef GraphEditorView DefaultView;

    if (lastContentView.isEmpty())
        view = std::make_unique<DefaultView>();
    else if (lastContentView == "PatchBay")
        view = std::make_unique<ConnectionGrid>();
    else if (lastContentView == EL_VIEW_GRAPH_EDITOR)
        view.reset (new GraphEditorView());
    else if (lastContentView == EL_VIEW_CONTROLLERS)
        view = std::make_unique<ControllersView>();
    else
        view = std::make_unique<DefaultView>();

    return view ? view.release() : nullptr;
}

static String stringProperty (Settings& settings, const String& property, const String defaultValue = String())
{
    auto* props = settings.getUserSettings();
    return props == nullptr ? String() : props->getValue (property, defaultValue);
}

static bool booleanProperty (Settings& settings, const String& property, const bool defaultValue)
{
    auto* props = settings.getUserSettings();
    return props == nullptr ? false : props->getBoolValue (property, defaultValue);
}

static void windowSizeProperty (Settings& settings, const String& property, int& w, int& h, const int defaultW, const int defaultH)
{
    auto* props = settings.getUserSettings();
    StringArray tokens;
    tokens.addTokens (props->getValue (property).trim(), false);
    tokens.removeEmptyStrings();
    tokens.trim();

    w = defaultW;
    h = defaultH;

    const bool fs = tokens[0].startsWithIgnoreCase ("fs");
    const int firstCoord = fs ? 1 : 0;

    if (tokens.size() != firstCoord + 4)
        return;

    Rectangle<int> newPos (tokens[firstCoord].getIntValue(),
                           tokens[firstCoord + 1].getIntValue(),
                           tokens[firstCoord + 2].getIntValue(),
                           tokens[firstCoord + 3].getIntValue());

    if (! newPos.isEmpty())
    {
        w = newPos.getWidth();
        h = newPos.getHeight();
    }
}
#endif

//==============================================================================
StandardContent::StandardContent (Context& ctl_)
    : Content (ctl_)
{
    setOpaque (true);

    container = std::make_unique<ContentContainer> (*this, services());
    addAndMakeVisible (container.get());
    bar1 = std::make_unique<Resizer> (*this, &layout, 1, true);
    addAndMakeVisible (bar1.get());
    nav = std::make_unique<NavigationSidebar> (ctl_);
    addAndMakeVisible (nav.get());
    nav->updateContent();
    nav->onLayoutInvalidated = [this]() {
        /* Sidebar's collapse state changed (header chevron, icon
         * click, or Ctrl+B).  Re-derive the StretchableLayoutManager
         * column rules from the sidebar's desired width so both the
         * resizer min/max + the laid-out width follow.  Without this
         * the layout manager stays locked at the previous state's
         * column width and either:
         *   (a) collapsed -> icon strip appears in a 304px column
         *       with the panel content hidden, or
         *   (b) expanded -> concertina is squeezed into the 52px
         *       collapsed column. */
        if (! nav) return;
        if (nav->isCollapsed())
        {
            const int w = NavigationSidebar::kCollapsedW;
            layout.setItemLayout (0, w, w, w);
            nav->setSize (w, nav->getHeight());
        }
        else
        {
            const int w = nav->getExpandedWidth();
            layout.setItemLayout (0, w, w, w);
            nav->setSize (w, nav->getHeight());
        }
        resized();
    };

    toolBarVisible = true;
    /* Taller toolbar -- absorbs the vertical strip the menu bar
     * currently uses (~24 px) PLUS extra room so the LCD-style sub-
     * labels (FILE / PLUG / PREF / PLAY / STOP / REC / REW / GRAPH /
     * ARR / TRK / SESS / PATCH) sit comfortably under each icon
     * inside the cluster bezels.  Menu removal is a follow-up. */
    toolBarSize = 96;
    statusBarVisible = true;
    statusBarSize = 22;

    setSize (1230, 730);

    setMainView (EL_VIEW_GRAPH_EDITOR);

    nav->setSize (304, getHeight());
    nav->setExpandedWidth (304);
    resizerMouseUp();
    /* Don't call expandPanelFully / setPanelSize on the concertina
     * here -- in the new VS-Code-style model the concertina is just
     * a panel registry, not a rendered accordion.  Its internal
     * layout machinery operates on panels we've reparented away,
     * which crashes.  Section selection is driven by
     * setActiveSection() instead. */

    resized();

    setVirtualKeyboardVisible (false);
    setNodeChannelStripVisible (false);
    setMeterBridgeVisible (false);

    auto& srv = *ctl_.services().find<SessionService>();
    sessionLoadedConn = srv.sigSessionLoaded.connect ([this]() {
        /* New session loaded: drop the view cache so views rebuild
         * against the new session's state.  Without this,
         * ArrangementView's lanesLoadedFromSession_ stays true and
         * lane state goes stale across session loads; SessionView /
         * others have similar one-shot init.  Captures the current
         * primary + secondary view names BEFORE clearing so we can
         * restore the user's selection against fresh instances. */
        const juce::String prevMain = getMainViewName();
        const juce::String prevAcc  = getAccessoryViewName();

        if (container)
        {
            container->setMainView (nullptr);
            container->setSecondaryView (nullptr);
        }
        viewCache_.clear();

        /* Clear the global UndoManager too -- any actions queued
         * against the old session's views/state become dangling
         * SafePointers once the cache cleared, and even those that
         * survive (graph-side actions) would replay against a
         * different session's data tree, corrupting state. */
        if (auto* gui = services().find<GuiService>())
            gui->getUndoManager().clearUndoHistory();

        if (prevMain.isNotEmpty())  setMainView (prevMain);
        if (prevAcc.isNotEmpty())   setSecondaryView (prevAcc);

        setCurrentNode (session()->getActiveGraph());

        /* TrackerSideDock holds a ValueTree::Listener pointing at
         * the previous session's active-graph nodes container.  After
         * the session swap that VT is stale, so the listener wouldn't
         * fire on the new session's add/remove.  refreshFromGraph()
         * detaches + re-attaches as part of its normal flow. */
        if (trackerDock != nullptr)
            trackerDock->refreshFromGraph();
    });
}

StandardContent::~StandardContent() noexcept
{
    sessionLoadedConn.disconnect();
    /* Detach the container's raw pointers BEFORE viewCache_ destroys
     * the underlying ContentView instances, otherwise the container's
     * implicit destruction would dereference dangling pointers. */
    if (container)
    {
        container->setMainView (nullptr);
        container->setSecondaryView (nullptr);
    }
    viewCache_.clear();
}

String StandardContent::getMainViewName() const
{
    String name;
    if (auto c1 = container->primary)
        name = c1->getName();
    return name;
}

Component* StandardContent::getMainViewComponent() const
{
    if (auto c1 = container->primary)
        return c1;
    return nullptr;
}

String StandardContent::getAccessoryViewName() const
{
    String name;
    if (auto c2 = container->secondary)
        name = c2->getName();
    return name;
}

int StandardContent::getNavSize()
{
    return nav != nullptr ? nav->getWidth() : 220;
}

NavigationConcertinaPanel* StandardContent::getNavigationConcertinaPanel() const
{
    return nav != nullptr ? nav->concertina() : nullptr;
}

void StandardContent::setNavSidebarCollapsed (bool c)
{
    if (nav == nullptr) return;
    /* setCollapsed fires onLayoutInvalidated which centralises the
     * layout-rule update + parent resize -- nothing else to do here. */
    nav->setCollapsed (c);
}

bool StandardContent::isNavSidebarCollapsed() const
{
    return nav != nullptr && nav->isCollapsed();
}

ContentView* StandardContent::lookupOrCreateMainView (const String& name)
{
    auto it = viewCache_.find (name);
    if (it != viewCache_.end())
        return it->second.get();

    /* Cache miss -- factory.  createContentView is the subclass hook
     * (returns nullptr by default); fall through to the named built-ins
     * if it doesn't supply one. */
    std::unique_ptr<ContentView> v;
    if (auto* custom = createContentView (name))
        v.reset (custom);
    else if (name == "PatchBay")                            v = std::make_unique<ConnectionGrid>();
    else if (name == EL_VIEW_GRAPH_EDITOR)                  v = std::make_unique<GraphEditorView>();
    else if (name == EL_VIEW_PLUGIN_MANAGER)                v = std::make_unique<PluginManagerContentView>();
    else if (name == EL_VIEW_SESSION_SETTINGS
             || name == "SessionProperties")                v = std::make_unique<SessionContentView>();
    else if (name == "GraphSettings")                       v = std::make_unique<GraphSettingsView>();
    else if (name == EL_VIEW_KEYMAP_EDITOR)                 v = std::make_unique<KeymapEditorView>();
    else if (name == EL_VIEW_CONTROLLERS)                   v = std::make_unique<ControllersView>();
    else if (name == EL_VIEW_ARRANGEMENT)                   v = std::make_unique<ArrangementView>();
    else if (name == EL_VIEW_TRACKER_HOST)                  v = std::make_unique<TrackerHostView>();
    else if (name == EL_VIEW_SESSION_VIEW)                  v = std::make_unique<SessionView>();
    else if (name == EL_VIEW_DISK_OP)                       v = std::make_unique<DiskOpContentView>();
    else
    {
        /* Fallback: GraphEditorView if any graph exists in the session,
         * else EmptyContentView. */
        if (auto s = context().session())
            v = s->getNumGraphs() > 0
                  ? std::unique_ptr<ContentView> (new GraphEditorView())
                  : std::unique_ptr<ContentView> (new EmptyContentView());
        else
            v = std::make_unique<EmptyContentView>();
    }

    if (! v) return nullptr;
    v->setName (name);
    /* One-shot initializeView: runs exactly once per cached instance,
     * here at cache population.  Re-invoking on every attach would
     * trigger destruction / recreation in views that allocate inside
     * initializeView (e.g. GraphMixerView::initializeView does
     * content.reset (new Content), which was wiping the inner tree
     * every hide/show cycle once the cache landed). */
    v->initializeView (services());
    ContentView* raw = v.get();
    viewCache_[name] = std::move (v);
    return raw;
}

ContentView* StandardContent::lookupOrCreateSecondaryView (const String& name)
{
    auto it = viewCache_.find (name);
    if (it != viewCache_.end())
        return it->second.get();

    std::unique_ptr<ContentView> v;
    if (auto* custom = createContentView (name))
        v.reset (custom);
    else if (name == EL_VIEW_GRAPH_MIXER) v = std::make_unique<GraphMixerView>();
    else if (name == EL_VIEW_CONSOLE)     v = std::make_unique<LuaConsoleView>();

    if (! v) return nullptr;
    v->setName (name);
    v->initializeView (services());
    ContentView* raw = v.get();
    viewCache_[name] = std::move (v);
    return raw;
}

void StandardContent::setMainView (const String& name)
{
    /* Pre-switch: capture state from outgoing view for cross-view
     * handoffs (e.g. PatchBay reads the GraphEditor's current graph;
     * GraphEditor reads the PatchBay's). */
    Node handoffGraph;
    if (name == "PatchBay")
    {
        if (auto* gev = dynamic_cast<GraphEditorView*> (getMainViewComponent()))
            handoffGraph = gev->getGraph();
    }
    else if (name == EL_VIEW_GRAPH_EDITOR)
    {
        if (auto* grid = dynamic_cast<ConnectionGrid*> (getMainViewComponent()))
            handoffGraph = grid->getGraph();
    }

    auto* view = lookupOrCreateMainView (name);
    if (view == nullptr) return;

    lastMainView = getMainViewName();
    container->setMainView (view);

    if (handoffGraph.isValid())
    {
        if (auto* grid = dynamic_cast<ConnectionGrid*> (view))
            grid->setNode (handoffGraph);
        else if (auto* gev = dynamic_cast<GraphEditorView*> (view))
            gev->setNode (handoffGraph);
    }
}

void StandardContent::backMainView()
{
    setMainView (lastMainView.isEmpty() ? EL_VIEW_GRAPH_EDITOR : lastMainView);
}

void StandardContent::nextMainView()
{
    // only have two rotatable views as of now
    if (getMainViewName() == "EmptyView")
        return;
    const String nextName = getMainViewName() == EL_VIEW_GRAPH_EDITOR ? "PatchBay" : EL_VIEW_GRAPH_EDITOR;
    setMainView (nextName);
}

void StandardContent::setContentView (ContentView* view, const bool accessory)
{
    /* Cache-routing attach: callers may pass either a freshly-new'd
     * ContentView (which we take ownership of via the cache) or
     * nullptr to detach.  If a cached instance with the same name
     * already exists, the incoming view is discarded and the cached
     * one is reused -- mirrors the lookupOrCreateMainView path so
     * stray `setContentView(new X())` call sites still work.
     *
     * For nullptr detach, route straight to the container. */
    if (view == nullptr)
    {
        if (accessory)
        {
            container->setSecondaryView (nullptr);
        }
        else
        {
            lastMainView = getMainViewName();
            container->setMainView (nullptr);
        }
        return;
    }

    std::unique_ptr<ContentView> incoming (view);
    const juce::String name = view->getName();

    ContentView* toAttach = view;
    bool freshlyCached = false;
    if (name.isNotEmpty())
    {
        auto it = viewCache_.find (name);
        if (it != viewCache_.end())
        {
            /* Cache hit -- drop the freshly-constructed view and
             * reuse the existing instance.  `incoming` falls out of
             * scope after this branch, deleting the duplicate. */
            toAttach = it->second.get();
        }
        else
        {
            toAttach = incoming.get();
            viewCache_[name] = std::move (incoming);
            freshlyCached = true;
        }
    }
    else
    {
        /* Unnamed views: assign a synthetic key so the cache can own
         * the lifetime.  Synthetic-keyed views aren't reused (they're
         * one-off wrappers from callers like presentView), but we
         * still need the cache to hold them so the container's
         * non-owning pointer stays valid. */
        static int synthCounter = 0;
        const auto synthKey = juce::String ("__synth_") + juce::String (++synthCounter);
        view->setName (synthKey);
        toAttach = incoming.get();
        viewCache_[synthKey] = std::move (incoming);
        freshlyCached = true;
    }

    /* One-shot initializeView -- mirrors the lookupOrCreate* paths. */
    if (freshlyCached)
        toAttach->initializeView (services());

    if (accessory)
    {
        container->setSecondaryView (toAttach);
    }
    else
    {
        lastMainView = getMainViewName();
        container->setMainView (toAttach);
    }
}

void StandardContent::setSecondaryView (const String& name)
{
    auto* view = lookupOrCreateSecondaryView (name);
    if (view == nullptr) return;
    container->setSecondaryView (view);
}

void StandardContent::resizeContent (const Rectangle<int>& area)
{
    Rectangle<int> r (area);

    if (_extra && _extra->isVisible())
    {
        _extra->setBounds (r.removeFromBottom (44));
        r.removeFromBottom (1);
    }

    /* Piano-roll dock sits on the BOTTOM edge (full window width, ABOVE
     * _extra if visible).  Lives outside ContentContainer so the
     * container's primary/secondary split doesn't squeeze it.  Height
     * is user-resizable via the dock's TOP-edge drag handle; the
     * dragHandle callback updates pianoRollHeight_ + retriggers this
     * layout.  Removed BEFORE the tracker side dock so the tracker
     * dock spans the FULL HEIGHT remaining above _extra; this matches
     * the documented orientation in the class comment of
     * TrackerSideDock and PianoRollView. */
    if (pianoRollVisible_ && pianoRoll != nullptr)
    {
        const int h = juce::jlimit (kPianoRollMinH, kPianoRollMaxH,
                                      pianoRollHeight_);
        pianoRoll->setBounds (r.removeFromBottom (h));
        r.removeFromBottom (1);
    }

    /* Tracker side dock sits on the RIGHT edge (full height, above
     * _extra if visible).  Lives outside ContentContainer so it
     * doesn't get squeezed by the container's primary/secondary
     * split.  Width is user-resizable via the dock's left-edge drag
     * handle; the dragHandle callback updates trackerDockWidth_ +
     * retriggers this layout. */
    if (trackerDockVisible_ && trackerDock != nullptr)
    {
        const int w = juce::jlimit (kTrackerDockMinW, kTrackerDockMaxW,
                                      trackerDockWidth_);
        trackerDock->setBounds (r.removeFromRight (w));
        r.removeFromRight (1);
    }

    if (nodeStrip && nodeStrip->isVisible())
        nodeStrip->setBounds (r.removeFromRight (nodeStripSize));

    Component* comps[3] = { nav.get(), bar1.get(), container.get() };
    layout.layOutComponents (comps, 3, r.getX(), r.getY(), r.getWidth(), r.getHeight(), false, true);
}

bool StandardContent::isInterestedInDragSource (const SourceDetails& dragSourceDetails)
{
    const auto& desc (dragSourceDetails.description);
    return desc.toString() == "ccNavConcertinaPanel" || (desc.isArray() && desc.size() >= 2 && desc[0] == "plugin");
}

void StandardContent::itemDropped (const SourceDetails& dragSourceDetails)
{
    const auto& desc (dragSourceDetails.description);
    if (desc.toString() == "ccNavConcertinaPanel")
    {
        if (auto* panel = nav->findPanel<DataPathTreeComponent>())
            filesDropped (StringArray ({ panel->getSelectedFile().getFullPathName() }),
                          dragSourceDetails.localPosition.getX(),
                          dragSourceDetails.localPosition.getY());
    }
    else if (desc.isArray() && desc.size() >= 2 && desc[0] == "plugin")
    {
        auto& list (context().plugins().getKnownPlugins());
        if (auto plugin = list.getTypeForIdentifierString (desc[1].toString()))
            this->post (new LoadPluginMessage (*plugin, true));
        else
            AlertWindow::showMessageBoxAsync (AlertWindow::InfoIcon, "Could not load plugin", "The plugin you dropped could not be loaded for an unknown reason.");
    }
}

bool StandardContent::isInterestedInFileDrag (const StringArray& files)
{
    for (const auto& path : files)
    {
        const File file (path);
        if (file.hasFileExtension ("elc;elg;els;dll;vst3;vst;elpreset;eln"))
            return true;
    }
    return false;
}

void StandardContent::filesDropped (const StringArray& files, int x, int y)
{
    for (const auto& path : files)
    {
        const File file (path);
        if (file.hasFileExtension ("els"))
        {
            this->post (new OpenSessionMessage (file));
        }
        else if (file.hasFileExtension ("elg"))
        {
            if (auto* sess = services().find<SessionService>())
                sess->importGraph (file);
        }
        else if (file.hasFileExtension ("elpreset;eln"))
        {
            const auto data = Node::parse (file);
            if (data.hasType (types::Node))
            {
                const Node node (data, false);
                this->post (new AddNodeMessage (node));
            }
            else
            {
                AlertWindow::showMessageBox (AlertWindow::InfoIcon,
                                             TRANS ("Node"),
                                             TRANS ("Error adding node from file"));
            }
        }
        else if ((file.hasFileExtension ("dll") || file.hasFileExtension ("vst") || file.hasFileExtension ("vst3")) && (getMainViewName() == EL_VIEW_GRAPH_EDITOR || getMainViewName() == "PatchBay" || getMainViewName() == EL_VIEW_PLUGIN_MANAGER))
        {
            auto s = session();
            auto graph = s->getActiveGraph();
            PluginDescription desc;
            desc.pluginFormatName = file.hasFileExtension ("vst3") ? "VST3" : "VST";
            desc.fileOrIdentifier = file.getFullPathName();

            auto message = std::make_unique<AddPluginMessage> (graph, desc, false);
            auto& builder (message->builder);

            if (ModifierKeys::getCurrentModifiersRealtime().isAltDown())
            {
                const auto audioInputNode = graph.getIONode (PortType::Audio, true);
                const auto midiInputNode = graph.getIONode (PortType::Midi, true);
                builder.addChannel (audioInputNode, PortType::Audio, 0, 0, false);
                builder.addChannel (audioInputNode, PortType::Audio, 1, 1, false);
                builder.addChannel (midiInputNode, PortType::Midi, 0, 0, false);
            }

            if (ModifierKeys::getCurrentModifiersRealtime().isCommandDown())
            {
                const auto audioOutputNode = graph.getIONode (PortType::Audio, false);
                const auto midiOutNode = graph.getIONode (PortType::Midi, false);
                builder.addChannel (audioOutputNode, PortType::Audio, 0, 0, true);
                builder.addChannel (audioOutputNode, PortType::Audio, 1, 1, true);
                builder.addChannel (midiOutNode, PortType::Midi, 0, 0, true);
            }

            this->post (message.release());
        }
    }
}

void StandardContent::stabilize (const bool refreshDataPathTrees)
{
    auto session = context().session();
    if (session->getNumGraphs() <= 0)
        setContentView (new EmptyContentView());

    if (auto* ss = nav->findPanel<SessionTreePanel>())
        ss->setSession (session);
    if (auto* mcv = nav->findPanel<NodePropertiesView>())
        mcv->stabilizeContent();
    if (auto* ncv = nav->findPanel<NodeEditorView>())
        ncv->stabilizeContent();
    if (auto* gcv = nav->findPanel<GraphSettingsView>())
        gcv->stabilizeContent();

    stabilizeViews();

    if (auto* main = findParentComponentOfClass<MainWindow>())
        main->refreshMenu();

    if (refreshDataPathTrees)
        if (auto* data = nav->findPanel<DataPathTreeComponent>())
            data->refresh();

    refreshToolbar();
    refreshStatusBar();
}

void StandardContent::stabilizeViews()
{
    if (container->primary)
        container->primary->stabilizeContent();
    if (auto c2 = container->secondary)
        c2->stabilizeContent();
    if (nodeStrip)
        nodeStrip->stabilizeContent();
}

void StandardContent::saveState (PropertiesFile* props)
{
    jassert (props);
    if (nav)
        nav->saveState (props);
    if (container)
        container->saveState (props);
    if (auto* const vk = getVirtualKeyboardView())
    {
        vk->saveState (props);
        props->setValue ("virtualKeyboard", isVirtualKeyboardVisible());
    }

    props->setValue ("standardNavSize", getNavSize());

    props->setValue ("channelStrip", isNodeChannelStripVisible());

    auto& mo = container->bottom->bridge->meterBridge();
    props->setValue ("meterBridge", isMeterBridgeVisible());
    props->setValue ("trackerDock",      trackerDockVisible_);
    props->setValue ("trackerDockWidth", trackerDockWidth_);
    props->setValue ("pianoRoll",        pianoRollVisible_);
    props->setValue ("pianoRollHeight",  pianoRollHeight_);
    props->setValue ("meterBridgeSize", mo.meterSize());
    props->setValue ("meterBridgeVisibility", (int) mo.visibility());
}

void StandardContent::restoreState (PropertiesFile* props)
{
    jassert (props);
    if (nav)
        nav->restoreState (props);
    if (container)
        container->restoreState (props);
    if (auto* const vk = getVirtualKeyboardView())
    {
        vk->restoreState (props);
        setVirtualKeyboardVisible (props->getBoolValue ("virtualKeyboard", isVirtualKeyboardVisible()));
    }

    setNodeChannelStripVisible (props->getBoolValue ("channelStrip", isNodeChannelStripVisible()));

    auto& bo = container->bottom->bridge->meterBridge();
    bo.setMeterSize (props->getIntValue ("meterBridgeSize", bo.meterSize()));
    bo.setVisibility ((uint32) props->getIntValue ("meterBridgeVisibility", bo.visibility()));
    setMeterBridgeVisible (props->getBoolValue ("meterBridge", isMeterBridgeVisible()));
    /* Honor legacy `trackerStripHeight` / `trackerStrip` keys ONCE
     * if the new keys aren't present, so old sessions don't lose
     * their toggle state.  Treat the legacy height as a hint only --
     * the new dock is width-based, so default to kTrackerDockMinW
     * range. */
    if (props->containsKey ("trackerDockWidth"))
        trackerDockWidth_ = props->getIntValue ("trackerDockWidth", trackerDockWidth_);
    setTrackerDockVisible (props->getBoolValue ("trackerDock",
                                                 props->getBoolValue ("trackerStrip", false)));

    if (props->containsKey ("pianoRollHeight"))
        pianoRollHeight_ = props->getIntValue ("pianoRollHeight", pianoRollHeight_);
    setPianoRollVisible (props->getBoolValue ("pianoRoll", false));

    {
        /* Prefer the new wrapper-managed keys (navExpandedWidth /
         * navCollapsed restored in NavigationSidebar::restoreState)
         * over the legacy standardNavSize.  If the user came from an
         * older session the wrapper falls back to its default 304;
         * we then honour the legacy key so they don't lose their
         * tuned width. */
        if (nav)
        {
            if (! props->containsKey ("navExpandedWidth")
                && props->containsKey ("standardNavSize"))
            {
                nav->setExpandedWidth (props->getIntValue ("standardNavSize",
                                                            nav->getExpandedWidth()));
            }
            const int w = nav->isCollapsed()
                            ? NavigationSidebar::kCollapsedW
                            : nav->getExpandedWidth();
            nav->setSize (w, nav->getHeight());
        }
        resizerMouseUp();
    }

    resized();
    container->bottom->resized();
    container->resized();
}

void StandardContent::setCurrentNode (const Node& node)
{
    // clang-format off
    if ((nullptr != dynamic_cast<EmptyContentView*> (container->primary) || 
        getMainViewName() == EL_VIEW_SESSION_SETTINGS || 
        getMainViewName() == EL_VIEW_PLUGIN_MANAGER || 
        getMainViewName() == EL_VIEW_CONTROLLERS) && 
        session()->getNumGraphs() > 0)
    {
        setMainView (EL_VIEW_GRAPH_EDITOR);
    }
    // clang-format on

    container->setNode (node);
}

void StandardContent::updateLayout()
{
    if (nav && nav->isCollapsed())
    {
        /* Collapsed: lock the column to a fixed icon-strip width.
         * Equal min/max prevents the resizer from dragging it.
         * Resizer itself becomes a visual no-op (still painted, but
         * any drag is clamped to the fixed width). */
        const int w = NavigationSidebar::kCollapsedW;
        layout.setItemLayout (0, w, w, w);
    }
    else
    {
        layout.setItemLayout (0, EL_NAV_MIN_WIDTH, EL_NAV_MAX_WIDTH,
                                nav ? nav->getWidth() : 304);
    }
    layout.setItemLayout (1, 2, 2, 2);
    layout.setItemLayout (2, 100, -1, 400);
}

void StandardContent::resizerMouseDown()
{
    updateLayout();
}

void StandardContent::resizerMouseUp()
{
    if (nav && nav->isCollapsed())
    {
        const int w = NavigationSidebar::kCollapsedW;
        layout.setItemLayout (0, w, w, w);
    }
    else
    {
        const int w = nav ? nav->getWidth() : 304;
        layout.setItemLayout (0, w, w, w);
        if (nav) nav->setExpandedWidth (w);
    }
    layout.setItemLayout (1, 2, 2, 2);
    layout.setItemLayout (2, 100, -1, 400);
    resized();
}

void StandardContent::setNodeChannelStripVisible (const bool isVisible)
{
    if (! nodeStrip)
    {
        nodeStrip = std::make_unique<NodeChannelStripView>();
        nodeStrip->initializeView (services());
    }

    if (isVisible == nodeStrip->isVisible())
        return;

    if (isVisible)
    {
        nodeStrip->willBecomeActive();
        addAndMakeVisible (nodeStrip.get());
        nodeStrip->didBecomeActive();
        nodeStrip->stabilizeContent();
        if (nodeStrip->isShowing() || nodeStrip->isOnDesktop())
            nodeStrip->grabKeyboardFocus();
    }
    else
    {
        nodeStrip->setVisible (false);
    }

    resized();
    container->bottom->resized();
    container->resized();
}
bool StandardContent::isNodeChannelStripVisible() const { return nodeStrip && nodeStrip->isVisible(); }

//==============================================================================
bool StandardContent::isVirtualKeyboardVisible() const
{
    if (auto vc = getVirtualKeyboardView())
        return vc->isVisible();
    return false;
}

void StandardContent::setVirtualKeyboardVisible (const bool vis)
{
    auto keyboard = getVirtualKeyboardView();
    if (keyboard->isVisible() == vis)
        return;

    keyboard->setVisible (vis);
    if (isVirtualKeyboardVisible())
    {
        if (keyboard->isShowing() || keyboard->isOnDesktop())
            keyboard->grabKeyboardFocus();
    }
    else
    {
        keyboard->setVisible (false);
    }

    container->resized();
}

void StandardContent::toggleVirtualKeyboard()
{
    setVirtualKeyboardVisible (! isVirtualKeyboardVisible());
}

VirtualKeyboardView* StandardContent::getVirtualKeyboardView() const
{
    return container->bottom->keyboard.get();
}

//==============================================================================
ApplicationCommandTarget* StandardContent::getNextCommandTarget()
{
    return nullptr;
}

void StandardContent::getAllCommands (Array<CommandID>& commands)
{
    // clang-format off
    commands.addArray ({
        Commands::showControllers,
        Commands::showKeymapEditor,
        Commands::showPluginManager,
        Commands::showSessionConfig,
        Commands::showGraphConfig,
        Commands::showPatchBay,
        Commands::showGraphEditor,
        Commands::showGraphMixer,
        Commands::showConsole,
        Commands::showArrangement,
        Commands::showTrackerHost,
        Commands::showSessionView,
        Commands::showDiskOp,
        Commands::toggleVirtualKeyboard,
        Commands::toggleMeterBridge,
        Commands::toggleChannelStrip,
        Commands::toggleTrackerStrip,
        Commands::togglePianoRoll,
        Commands::toggleNavSidebar,
        Commands::showLastContentView,
        Commands::rotateContentView,
        Commands::selectAll
    });
    // clang-format on
}

void StandardContent::getCommandInfo (CommandID commandID, ApplicationCommandInfo& result)
{
    using Info = ApplicationCommandInfo;

    switch (commandID)
    {
        case Commands::showControllers: {
            int flags = 0;
            if (getMainViewName() == EL_VIEW_CONTROLLERS)
                flags |= Info::isTicked;
            result.setInfo ("Controllers", "Show the session's controllers", "UI", flags);
            result.addDefaultKeypress ('m', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            break;
        }
        case Commands::showKeymapEditor: {
            int flags = 0;
            if (getMainViewName() == EL_VIEW_KEYMAP_EDITOR)
                flags |= Info::isTicked;
            result.setInfo ("Keymappings", "Show the session's controllers", "UI", flags);
            break;
        }
        case Commands::showPluginManager: {
            int flags = 0;
            if (getMainViewName() == EL_VIEW_PLUGIN_MANAGER)
                flags |= Info::isTicked;
            result.setInfo ("Plugin Manager", "Show the session's controllers", "UI", flags);
            break;
        }
        //=====
        case Commands::showSessionConfig: {
            int flags = 0;
            if (getMainViewName() == EL_VIEW_SESSION_SETTINGS)
                flags |= Info::isTicked;
            result.setInfo ("Session Settings", "Session Settings", "Session", flags);
        }
        //=====
        case Commands::showGraphConfig: {
            int flags = 0;
            if (getMainViewName() == "GraphSettings")
                flags |= Info::isTicked;
            result.setInfo ("Graph Settings", "Graph Settings", "Graph", flags);
            break;
        }
        //===
        case Commands::showPatchBay: {
            int flags = 0;
            if (getMainViewName() == "PatchBay")
                flags |= Info::isTicked;
            result.addDefaultKeypress (KeyPress::F1Key, 0);
            result.setInfo ("Patch Bay", "Show the patch bay", "UI", flags);
            break;
        }

        case Commands::showGraphEditor: {
            int flags = 0;
            if (getMainViewName() == EL_VIEW_GRAPH_EDITOR)
                flags |= Info::isTicked;
            result.addDefaultKeypress (KeyPress::F2Key, 0);
            result.setInfo ("Graph Editor", "Show the graph editor", "UI", flags);
        }
        break;
            //===
        case Commands::showGraphMixer: {
            int flags = (showAccessoryView() && getAccessoryViewName() == EL_VIEW_GRAPH_MIXER)
                            ? Info::isTicked
                            : 0;
            result.setInfo ("Graph Mixer", "Show/hide the graph mixer", "UI", flags);
            break;
        }
        //======================================================================
        case Commands::showConsole: {
            int flags = (showAccessoryView() && getAccessoryViewName() == EL_VIEW_CONSOLE)
                            ? Info::isTicked
                            : 0;
            result.setInfo ("Console", "Show the scripting console", "UI", flags);
            break;
        }
        case Commands::showArrangement: {
            int flags = 0;
            if (getMainViewName() == EL_VIEW_ARRANGEMENT)
                flags |= Info::isTicked;
            result.addDefaultKeypress (KeyPress::F3Key, 0);
            result.setInfo ("Arrangement", "Show the arrangement view", "UI", flags);
            break;
        }
        case Commands::showTrackerHost: {
            int flags = 0;
            if (getMainViewName() == EL_VIEW_TRACKER_HOST)
                flags |= Info::isTicked;
            result.addDefaultKeypress (KeyPress::F4Key, 0);
            result.setInfo ("Trackers", "Show the tabbed tracker editors", "UI", flags);
            break;
        }
        case Commands::showSessionView: {
            int flags = 0;
            if (getMainViewName() == EL_VIEW_SESSION_VIEW)
                flags |= Info::isTicked;
            result.addDefaultKeypress (KeyPress::F6Key, 0);
            result.setInfo ("Session", "Show the clip-launcher session view", "UI", flags);
            break;
        }
        case Commands::showDiskOp: {
            int flags = 0;
            if (getMainViewName() == EL_VIEW_DISK_OP)
                flags |= Info::isTicked;
            result.addDefaultKeypress (KeyPress::F5Key, 0);
            result.setInfo ("Disk Op", "Show the embedded Disk Op file page", "UI", flags);
            break;
        }
        //======================================================================
        case Commands::toggleVirtualKeyboard: {
            result.addDefaultKeypress ('k', ModifierKeys::altModifier);
            int flags = 0;
            if (isVirtualKeyboardVisible())
                flags |= Info::isTicked;
            result.setInfo ("Virtual Keyboard", "Toggle the virtual keyboard", "UI", flags);
            break;
        }
        case Commands::toggleMeterBridge: {
            result.addDefaultKeypress ('m', ModifierKeys::altModifier);
            int flags = 0;
            if (isMeterBridgeVisible())
                flags |= Info::isTicked;
            result.setInfo ("MeterBridge", "Toggle the Meter Bridge", "UI", flags);
            break;
        }
        case Commands::toggleChannelStrip: {
            int flags = 0;
            if (isNodeChannelStripVisible())
                flags |= Info::isTicked;
            result.setInfo ("Channel Strip", "Toggles the global channel strip", "UI", flags);
            break;
        }
        case Commands::toggleTrackerStrip: {
            int flags = 0;
            if (isTrackerDockVisible()) flags |= Info::isTicked;
            result.setInfo ("Tracker Dock",
                            "Show / hide the tracker editor on the right side of the window",
                            "UI", flags);
            result.addDefaultKeypress ('t', ModifierKeys::commandModifier);
            break;
        }
        case Commands::togglePianoRoll: {
            int flags = 0;
            if (isPianoRollVisible()) flags |= Info::isTicked;
            result.setInfo ("Piano Roll",
                            "Show / hide the piano-roll editor at the bottom of the window",
                            "UI", flags);
            result.addDefaultKeypress ('p', ModifierKeys::commandModifier);
            break;
        }
        case Commands::toggleNavSidebar: {
            int flags = 0;
            /* Tick when the sidebar is EXPANDED -- the menu item then
             * reads as "Sidebar is on", matching the way Channel Strip
             * / Meter Bridge ticks when their feature is visible. */
            if (! isNavSidebarCollapsed()) flags |= Info::isTicked;
            result.setInfo ("Sidebar",
                            "Collapse / expand the left navigation sidebar",
                            "UI", flags);
            result.addDefaultKeypress ('b', ModifierKeys::commandModifier);
            break;
        }
        case Commands::showLastContentView: {
            result.setInfo ("Last View", "Shows the last shown View", "UI", 0);
            break;
        }
        case Commands::rotateContentView: {
            result.setInfo ("Next View", "Shows the next View", "UI", 0);
            break;
        }
        case Commands::selectAll: {
            int flags = 0;
            result.setInfo ("Select all", "Select all nodes", "UI", flags);
            result.addDefaultKeypress ('a', ModifierKeys::commandModifier);
            break;
        }
    }
}

bool StandardContent::perform (const InvocationInfo& info)
{
    bool result = true;
    switch (info.commandID)
    {
        case Commands::showControllers: {
            setMainView (EL_VIEW_CONTROLLERS);
            break;
        }
        case Commands::showKeymapEditor:
            setMainView (EL_VIEW_KEYMAP_EDITOR);
            break;
        case Commands::showPluginManager:
            setMainView (EL_VIEW_PLUGIN_MANAGER);
            break;
        case Commands::showDiskOp:
            setMainView (EL_VIEW_DISK_OP);
            break;
            //===
        case Commands::showSessionConfig:
            setMainView (EL_VIEW_SESSION_SETTINGS);
            break;
        case Commands::showGraphConfig:
            setMainView ("GraphSettings");
            break;
            //===

        case Commands::showPatchBay:
            setMainView ("PatchBay");
            break;
        case Commands::showGraphEditor:
            setMainView (EL_VIEW_GRAPH_EDITOR);
            break;
        //======================================================================
        case Commands::showGraphMixer: {
            if (showAccessoryView() && getAccessoryViewName() == EL_VIEW_GRAPH_MIXER)
            {
                container->setSecondaryView (nullptr);
            }
            else
            {
                setSecondaryView (EL_VIEW_GRAPH_MIXER);
            }
            break;
        }
        case Commands::showConsole: {
            if (showAccessoryView() && getAccessoryViewName() == EL_VIEW_CONSOLE)
            {
                container->setSecondaryView (nullptr);
            }
            else
            {
                setSecondaryView (EL_VIEW_CONSOLE);
            }
            break;
        }
        case Commands::showArrangement:
            setMainView (EL_VIEW_ARRANGEMENT);
            break;
        case Commands::showTrackerHost:
            setMainView (EL_VIEW_TRACKER_HOST);
            break;
        case Commands::showSessionView:
            setMainView (EL_VIEW_SESSION_VIEW);
            break;

        case Commands::toggleVirtualKeyboard:
            toggleVirtualKeyboard();
            break;
        case Commands::toggleMeterBridge:
            setMeterBridgeVisible (! isMeterBridgeVisible());
            break;
        case Commands::toggleChannelStrip:
            setNodeChannelStripVisible (! isNodeChannelStripVisible());
            break;
        case Commands::toggleTrackerStrip:
            setTrackerDockVisible (! isTrackerDockVisible());
            break;
        case Commands::togglePianoRoll:
            setPianoRollVisible (! isPianoRollVisible());
            break;
        case Commands::toggleNavSidebar:
            setNavSidebarCollapsed (! isNavSidebarCollapsed());
            break;
        case Commands::showLastContentView:
            backMainView();
            break;
        case Commands::rotateContentView:
            nextMainView();
            break;
        case Commands::selectAll: {
            if (auto view = dynamic_cast<GraphEditorView*> (container->primary))
                view->selectAllNodes();
            break;
        }
        default:
            result = false;
            break;
    }

    if (result)
    {
        services().find<UI>()->refreshMainMenu();
    }
    return result;
}

void StandardContent::setShowAccessoryView (const bool show)
{
    if (container)
        container->setShowAccessoryView (show);
}

bool StandardContent::showAccessoryView() const
{
    return (container) ? container->showAccessoryView : false;
}

void StandardContent::getSessionState (String& state)
{
    ValueTree data ("state");

    if (auto* const ned = nav->findPanel<NodeEditorView>())
    {
        String nedState;
        ned->getState (nedState);
        if (nedState.isNotEmpty())
        {
            data.setProperty ("NodeEditorView", nedState, nullptr);
        }
    }

    if (auto* const npv = nav->findPanel<NodePropertiesView>())
    {
        String npvState;
        npv->getState (npvState);
        if (npvState.isNotEmpty())
        {
            data.setProperty ("NodePropertiesView", npvState, nullptr);
        }
    }

    MemoryOutputStream mo;
    {
        GZIPCompressorOutputStream gzip (mo, 9);
        data.writeToStream (gzip);
    }

    state = mo.getMemoryBlock().toBase64Encoding();
}

void StandardContent::applySessionState (const String& state)
{
    MemoryBlock mb;
    mb.fromBase64Encoding (state);
    const ValueTree data = (mb.getSize() > 0)
                               ? ValueTree::readFromGZIPData (mb.getData(), mb.getSize())
                               : ValueTree();
    if (! data.isValid())
        return;

    if (auto* const ned = nav->findPanel<NodeEditorView>())
    {
        String nedState = data.getProperty ("NodeEditorView").toString();
        ned->setState (nedState);
    }

    if (auto* const npv = nav->findPanel<NodePropertiesView>())
    {
        String npvState = data.getProperty ("NodePropertiesView").toString();
        npv->setState (npvState);
    }
}

void StandardContent::presentView (const juce::String& view)
{
    setMainView (view);
}

void StandardContent::presentView (std::unique_ptr<View> view)
{
    if (view == nullptr)
        return;

    class ViewWrapper : public ContentView
    {
    public:
        ViewWrapper()
        {
            setSize (1, 1);
            setInterceptsMouseClicks (false, true);
        }

        ~ViewWrapper() { view.reset(); }

        void init()
        {
            addAndMakeVisible (view.get());
        }

        void paint (juce::Graphics& g) override
        {
            if (isOpaque())
                g.fillAll (Colours::black);
        }

        void resized() override
        {
            if (view)
                view->setBounds (getLocalBounds());
        }

        std::unique_ptr<View> view;
    };

    if (auto c = dynamic_cast<ContentView*> (view.get()))
    {
        view.release();
        setContentView (c, false);
    }
    else
    {
        auto wrap = new ViewWrapper();
        wrap->view = std::move (view);
        wrap->setOpaque (wrap->view->isOpaque());
        wrap->addAndMakeVisible (wrap->view.get());
        wrap->resized();
        setContentView (wrap, false);
    }
}

void StandardContent::setMainView (ContentView* view)
{
    jassert (view != nullptr);
    setContentView (view, false);
}

void StandardContent::setExtraView (Component* extra)
{
    _extra.reset (extra);
    if (_extra)
    {
        addAndMakeVisible (_extra.get());
    }
    resized();
}

void StandardContent::setMeterBridgeVisible (bool vis)
{
    if (isMeterBridgeVisible() == vis)
        return;
    container->bottom->bridge->setVisible (vis);
    container->bottom->resized();
    container->resized();
}

//============================================================================
// Tracker right-side dock

bool StandardContent::isTrackerDockVisible() const { return trackerDockVisible_; }
int  StandardContent::getTrackerDockWidth() const  { return trackerDockWidth_; }

void StandardContent::setTrackerDockWidth (int w)
{
    w = juce::jlimit (kTrackerDockMinW, kTrackerDockMaxW, w);
    if (w == trackerDockWidth_) return;
    trackerDockWidth_ = w;
    resized();
}

void StandardContent::setTrackerDockVisible (bool v)
{
    if (v == trackerDockVisible_) return;
    trackerDockVisible_ = v;

    if (v && trackerDock == nullptr)
    {
        trackerDock = std::make_unique<TrackerSideDock> (services());
        addChildComponent (trackerDock.get());
        /* Drag handle delta: positive = user dragged RIGHT (dock
         * shrinks); negative = user dragged LEFT (dock widens).
         * Subtract delta from the width to invert into the
         * shrink/grow convention. */
        trackerDock->onResizeDrag = [this] (int delta) {
            setTrackerDockWidth (trackerDockWidth_ - delta);
        };
        trackerDock->onResizeDragEnd = [this]() {
            /* Persist final width on drag end -- writing on every
             * mouseDrag frame would thrash session XML. */
            if (auto* props = services().context().settings().getUserSettings())
                props->setValue ("trackerDockWidth", trackerDockWidth_);
        };
        trackerDock->onCloseClicked = [this]() {
            setTrackerDockVisible (false);
        };
    }

    if (trackerDock != nullptr)
        trackerDock->setVisible (v);

    resized();
}

void StandardContent::showTrackerDockForNode (const juce::Uuid& trackerNodeId,
                                                int sequenceIdx)
{
    /* Implicit show + bind.  Used by ArrangementView's tracker-clip
     * click affordance and by anything else that wants to surface a
     * specific tracker without forcing the user to toggle the dock
     * first.  sequenceIdx >= 0 navigates the editor to that pattern
     * so the dock opens on whichever clip the user actually clicked
     * (not pattern 0). */
    if (! trackerDockVisible_)
        setTrackerDockVisible (true);
    if (trackerDock != nullptr)
        trackerDock->setTrackerAndPattern (trackerNodeId, sequenceIdx);
}

//============================================================================
// Bottom-attached piano-roll dock

bool StandardContent::isPianoRollVisible() const { return pianoRollVisible_; }
int  StandardContent::getPianoRollHeight() const { return pianoRollHeight_; }

void StandardContent::setPianoRollHeight (int h)
{
    h = juce::jlimit (kPianoRollMinH, kPianoRollMaxH, h);
    if (h == pianoRollHeight_) return;
    pianoRollHeight_ = h;
    resized();
}

void StandardContent::setPianoRollVisible (bool v)
{
    if (v == pianoRollVisible_) return;
    pianoRollVisible_ = v;

    if (v && pianoRoll == nullptr)
    {
        pianoRoll = std::make_unique<PianoRollView> (services());
        addChildComponent (pianoRoll.get());

        /* Drag handle delta: positive = user dragged DOWN (dock
         * shrinks); negative = user dragged UP (dock grows).
         * Subtract delta from the height to invert into the
         * shrink/grow convention. */
        pianoRoll->onResizeDrag = [this] (int delta) {
            setPianoRollHeight (pianoRollHeight_ - delta);
        };
        pianoRoll->onResizeDragEnd = [this]() {
            /* Persist final height on drag end -- writing on every
             * mouseDrag frame would thrash session XML. */
            if (auto* props = services().context().settings().getUserSettings())
                props->setValue ("pianoRollHeight", pianoRollHeight_);
        };
        pianoRoll->onCloseClicked = [this]() {
            setPianoRollVisible (false);
        };
    }

    if (pianoRoll != nullptr)
        pianoRoll->setVisible (v);

    resized();
}

void StandardContent::showPianoRollForRegion (const juce::Uuid& regionId)
{
    /* Implicit show + bind.  Used by ArrangementView's MIDI-region
     * double-click affordance and by anything else that wants to
     * surface a specific region without forcing the user to toggle
     * the dock first. */
    if (! pianoRollVisible_)
        setPianoRollVisible (true);
    if (pianoRoll == nullptr) return;

    /* Install (or refresh) the resolver lambda that walks the
     * cached ArrangementView and queries findMidiRegion(uuid).  The
     * lambda captures `this` (StandardContent) so the resolver always
     * targets the current viewCache_ entry -- the ArrangementView is
     * built once on first request and reused across view switches.
     * If the user has not yet visited the arrangement (no entry in
     * viewCache_), the lambda returns nullptr and the grid paints
     * its empty-state hint -- harmless. */
    pianoRoll->setRegionResolver (
        [this] (const juce::Uuid& uuid) -> MidiNoteRegion* {
            /* Resolver probes both surfaces that own MIDI regions:
             *  1. ArrangementView lanes (long-form MIDI regions).
             *  2. SessionView clips (per-clip MidiNoteRegion held by
             *     the host MidiPlayerNode).
             * First non-null wins. */
            auto itArr = viewCache_.find (EL_VIEW_ARRANGEMENT);
            if (itArr != viewCache_.end())
            {
                if (auto* arr = dynamic_cast<ArrangementView*> (itArr->second.get()))
                    if (auto* r = arr->findMidiRegion (uuid))
                        return r;
            }
            auto itSess = viewCache_.find (EL_VIEW_SESSION_VIEW);
            if (itSess != viewCache_.end())
            {
                if (auto* sv = dynamic_cast<SessionView*> (itSess->second.get()))
                    if (auto* r = sv->findMidiClipRegion (uuid))
                        return r;
            }
            return nullptr;
        });

    /* Region-edited callback -- piano-roll edits fire this on commit
     * + on Delete-key + on Erase-tool hit + on undo/redo of any of the
     * above.  Two responsibilities:
     *
     *   1. Flush lanes_ to the session ValueTree.  Piano-roll edits
     *      mutate MidiNoteRegion via the resolver but don't reach the
     *      session XML on their own; without this, a Save+Reload loses
     *      every note authored in the piano roll while the parent
     *      MIDI clip box itself persists.
     *   2. Repaint the arrangement strip so the lane's note-count
     *      badge tracks live.  Cheap: ArrangementView's paint reads
     *      noteCount() per region via an atomic load.
     *
     *  flushLanesToSession deliberately skips the ArrangementSnapshot
     *  undo push -- the MidiNoteDiffCommand already owns this gesture's
     *  Ctrl+Z slot on the global UndoManager. */
    pianoRoll->onRegionEdited = [this]() {
        /* Two persistence sinks depending on which surface owns the
         * currently-bound region:
         *  - ArrangementView lanes: flush the lanes ValueTree.
         *  - SessionView clips: the underlying MidiPlayerNode owns the
         *    region; its getState/setState round-trip happens via the
         *    graph node state path, so just nudging Session's
         *    dirty-mark is enough.  StandardContent doesn't know which
         *    owns the region without re-probing the resolver -- cheap
         *    enough to fire both. */
        auto itArr = viewCache_.find (EL_VIEW_ARRANGEMENT);
        if (itArr != viewCache_.end())
        {
            if (auto* arr = dynamic_cast<ArrangementView*> (itArr->second.get()))
            {
                arr->flushLanesToSession();
                /* A piano-roll edit may have added / removed / retargeted a
                 * clip-local param-automation lane -- refresh the owning
                 * MidiPlayerNode's param-binding table so the new target
                 * resolves (#11 Phase 2c).  Cheap: no-op unless the bound
                 * region lives in an arrangement lane. */
                arr->republishMidiBindingsForRegion (pianoRoll->getBoundRegionId());
                arr->repaint();
            }
        }
        auto itSess = viewCache_.find (EL_VIEW_SESSION_VIEW);
        if (itSess != viewCache_.end())
        {
            if (auto* sv = dynamic_cast<SessionView*> (itSess->second.get()))
                sv->repaint();
        }
    };

    pianoRoll->setRegion (regionId);
}

bool StandardContent::isMeterBridgeVisible() const
{
    return container->bottom->bridge->isVisible();
}

} // namespace element
