/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file incorporates work covered by the following license notice:
 *
 *   Licensed to the Apache Software Foundation (ASF) under one or more
 *   contributor license agreements. See the NOTICE file distributed
 *   with this work for additional information regarding copyright
 *   ownership. The ASF licenses this file to you under the Apache
 *   License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.apache.org/licenses/LICENSE-2.0 .
 */
#include <sfx2/sidebar/SidebarController.hxx>
#include <sfx2/sidebar/Deck.hxx>
#include <sfx2/sidebar/DeckDescriptor.hxx>
#include <sfx2/sidebar/DeckTitleBar.hxx>
#include <sfx2/sidebar/Panel.hxx>
#include <sfx2/sidebar/PanelDescriptor.hxx>
#include <sfx2/sidebar/PanelTitleBar.hxx>
#include <sfx2/sidebar/TabBar.hxx>
#include <sfx2/sidebar/Theme.hxx>
#include <sfx2/sidebar/SidebarChildWindow.hxx>
#include <sfx2/sidebar/Tools.hxx>
#include <sfx2/sidebar/SidebarDockingWindow.hxx>
#include <sfx2/sidebar/Context.hxx>
#include <sfx2/sidebar/ContextList.hxx>


#include <sfx2/sfxresid.hxx>
#include <sfx2/sfxsids.hrc>
#include <sfx2/titledockwin.hxx>
#include <sfx2/strings.hrc>
#include <framework/ContextChangeEventMultiplexerTunnel.hxx>
#include <vcl/floatwin.hxx>
#include <vcl/fixed.hxx>
#include <vcl/uitest/logger.hxx>
#include <vcl/uitest/eventdescription.hxx>
#include <vcl/svapp.hxx>
#include <splitwin.hxx>
#include <tools/diagnose_ex.h>
#include <tools/link.hxx>
#include <toolkit/helper/vclunohelper.hxx>
#include <comphelper/processfactory.hxx>
#include <comphelper/namedvaluecollection.hxx>
#include <sal/log.hxx>
#include <officecfg/Office/UI/Sidebar.hxx>

#include <com/sun/star/awt/XWindowPeer.hpp>
#include <com/sun/star/frame/XDispatch.hpp>
#include <com/sun/star/lang/XInitialization.hpp>
#include <com/sun/star/ui/ContextChangeEventMultiplexer.hpp>
#include <com/sun/star/ui/ContextChangeEventObject.hpp>
#include <com/sun/star/ui/theUIElementFactoryManager.hpp>
#include <com/sun/star/util/XURLTransformer.hpp>
#include <com/sun/star/util/URL.hpp>
#include <com/sun/star/rendering/XSpriteCanvas.hpp>


using namespace css;
using namespace css::uno;

namespace
{
    const static char gsReadOnlyCommandName[] = ".uno:EditDoc";
    const static sal_Int32 gnWidthCloseThreshold (70);
    const static sal_Int32 gnWidthOpenThreshold (40);
}

namespace sfx2 { namespace sidebar {

namespace {
    enum MenuId
    {
        MID_UNLOCK_TASK_PANEL = 1,
        MID_LOCK_TASK_PANEL,
        MID_HIDE_SIDEBAR,
        MID_CUSTOMIZATION,
        MID_RESTORE_DEFAULT,
        MID_FIRST_PANEL,
        MID_FIRST_HIDE = 1000
    };

    /** When in doubt, show this deck.
    */
    static const char gsDefaultDeckId[] = "PropertyDeck";
}

SidebarController::SidebarController (
    SidebarDockingWindow* pParentWindow,
    const css::uno::Reference<css::frame::XFrame>& rxFrame)
    : SidebarControllerInterfaceBase(m_aMutex),
      mpCurrentDeck(),
      mpParentWindow(pParentWindow),
      mpTabBar(VclPtr<TabBar>::Create(
              mpParentWindow,
              rxFrame,
              [this](const OUString& rsDeckId) { return this->OpenThenToggleDeck(rsDeckId); },
              [this](const tools::Rectangle& rButtonBox,const ::std::vector<TabBar::DeckMenuData>& rMenuData) { return this->ShowPopupMenu(rButtonBox,rMenuData); },
              this)),
      mxFrame(rxFrame),
      maCurrentContext(OUString(), OUString()),
      maRequestedContext(),
      mnRequestedForceFlags(SwitchFlag_NoForce),
      mnMaximumSidebarWidth(officecfg::Office::UI::Sidebar::General::MaximumWidth::get()),
      msCurrentDeckId(gsDefaultDeckId),
      maPropertyChangeForwarder([this](){ return this->BroadcastPropertyChange(); }),
      maContextChangeUpdate([this](){ return this->UpdateConfigurations(); }),
      maAsynchronousDeckSwitch(),
      mbIsDeckRequestedOpen(),
      mbIsDeckOpen(),
      mbFloatingDeckClosed(!pParentWindow->IsFloatingMode()),
      mnSavedSidebarWidth(pParentWindow->GetSizePixel().Width()),
      maFocusManager([this](const Panel& rPanel){ return this->ShowPanel(rPanel); },
                     [this](const sal_Int32 nIndex){ return this->IsDeckOpen(nIndex); }),
      mxReadOnlyModeDispatch(),
      mbIsDocumentReadOnly(false),
      mpSplitWindow(nullptr),
      mnWidthOnSplitterButtonDown(0),
      mpResourceManager()
{
    // Decks and panel collections for this sidebar
    mpResourceManager = std::make_unique<ResourceManager>();
}

rtl::Reference<SidebarController> SidebarController::create(
    SidebarDockingWindow* pParentWindow,
    const css::uno::Reference<css::frame::XFrame>& rxFrame)
{
    rtl::Reference<SidebarController> instance(
        new SidebarController(pParentWindow, rxFrame));

    registerSidebarForFrame(instance.get(), rxFrame->getController());
    rxFrame->addFrameActionListener(instance.get());
    // Listen for window events.
    instance->mpParentWindow->AddEventListener(LINK(instance.get(), SidebarController, WindowEventHandler));

    // Listen for theme property changes.
    Theme::GetPropertySet()->addPropertyChangeListener(
        "",
        static_cast<css::beans::XPropertyChangeListener*>(instance.get()));

    // Get the dispatch object as preparation to listen for changes of
    // the read-only state.
    const util::URL aURL (Tools::GetURL(gsReadOnlyCommandName));
    instance->mxReadOnlyModeDispatch = Tools::GetDispatch(rxFrame, aURL);
    if (instance->mxReadOnlyModeDispatch.is())
        instance->mxReadOnlyModeDispatch->addStatusListener(instance.get(), aURL);

    //first UpdateConfigurations call will SwitchToDeck

    return instance;
}

SidebarController::~SidebarController()
{
}

SidebarController* SidebarController::GetSidebarControllerForFrame (
    const css::uno::Reference<css::frame::XFrame>& rxFrame)
{
    uno::Reference<frame::XController> const xController(rxFrame->getController());
    if (!xController.is()) // this may happen during dispose of Draw controller but perhaps it's a bug
    {
        SAL_WARN("sfx.sidebar", "GetSidebarControllerForFrame: frame has no XController");
        return nullptr;
    }
    uno::Reference<ui::XContextChangeEventListener> const xListener(
        framework::GetFirstListenerWith(xController,
            [] (uno::Reference<uno::XInterface> const& xRef)
            { return nullptr != dynamic_cast<SidebarController*>(xRef.get()); }
        ));

    return dynamic_cast<SidebarController*>(xListener.get());
}

void SidebarController::registerSidebarForFrame(SidebarController* pController, const css::uno::Reference<css::frame::XController>& xController)
{
    // Listen for context change events.
    css::uno::Reference<css::ui::XContextChangeEventMultiplexer> xMultiplexer (
        css::ui::ContextChangeEventMultiplexer::get(
            ::comphelper::getProcessComponentContext()));
    xMultiplexer->addContextChangeEventListener(
        static_cast<css::ui::XContextChangeEventListener*>(pController),
        xController);
}

void SidebarController::unregisterSidebarForFrame(SidebarController* pController, const css::uno::Reference<css::frame::XController>& xController)
{
    pController->disposeDecks();
    css::uno::Reference<css::ui::XContextChangeEventMultiplexer> xMultiplexer (
        css::ui::ContextChangeEventMultiplexer::get(
            ::comphelper::getProcessComponentContext()));
    xMultiplexer->removeContextChangeEventListener(
        static_cast<css::ui::XContextChangeEventListener*>(pController),
        xController);
}

void SidebarController::disposeDecks()
{
    SolarMutexGuard aSolarMutexGuard;
    mpCurrentDeck.clear();
    maFocusManager.Clear();
    mpResourceManager->disposeDecks();
}

void SAL_CALL SidebarController::disposing()
{
    mpCloseIndicator.disposeAndClear();

    maFocusManager.Clear();
    mpTabBar.disposeAndClear();

    // save decks settings
    // Impress shutdown : context (frame) is disposed before sidebar disposing
    // calc writer : context (frame) is disposed after sidebar disposing
    // so need to test if GetCurrentContext is still valid regarding msApplication

    if (GetCurrentContext().msApplication != "none")
    {
        mpResourceManager->SaveDecksSettings(GetCurrentContext());
        mpResourceManager->SaveLastActiveDeck(GetCurrentContext(), msCurrentDeckId);
    }

    // clear decks
    ResourceManager::DeckContextDescriptorContainer aDecks;

    mpResourceManager->GetMatchingDecks (
            aDecks,
            GetCurrentContext(),
            IsDocumentReadOnly(),
            mxFrame->getController());

    for (const auto& rDeck : aDecks)
    {
        std::shared_ptr<DeckDescriptor> deckDesc = mpResourceManager->GetDeckDescriptor(rDeck.msId);

        VclPtr<Deck> aDeck = deckDesc->mpDeck;
        if (aDeck)
            aDeck.disposeAndClear();
    }

    uno::Reference<css::frame::XController> xController = mxFrame->getController();
    if (!xController.is())
        xController = mxCurrentController;

    mxFrame->removeFrameActionListener(this);
    unregisterSidebarForFrame(this, xController);

    if (mxReadOnlyModeDispatch.is())
        mxReadOnlyModeDispatch->removeStatusListener(this, Tools::GetURL(gsReadOnlyCommandName));
    if (mpSplitWindow != nullptr)
    {
        mpSplitWindow->RemoveEventListener(LINK(this, SidebarController, WindowEventHandler));
        mpSplitWindow = nullptr;
    }

    if (mpParentWindow != nullptr)
    {
        mpParentWindow->RemoveEventListener(LINK(this, SidebarController, WindowEventHandler));
        mpParentWindow = nullptr;
    }

    Theme::GetPropertySet()->removePropertyChangeListener(
        "",
        static_cast<css::beans::XPropertyChangeListener*>(this));

    maContextChangeUpdate.CancelRequest();
    maAsynchronousDeckSwitch.CancelRequest();

}

void SAL_CALL SidebarController::notifyContextChangeEvent (const css::ui::ContextChangeEventObject& rEvent)
{
    // Update to the requested new context asynchronously to avoid
    // subtle errors caused by SFX2 which in rare cases can not
    // properly handle a synchronous update.

    maRequestedContext = Context(
        rEvent.ApplicationName,
        rEvent.ContextName);

    if (maRequestedContext != maCurrentContext)
    {
        mxCurrentController.set(rEvent.Source, css::uno::UNO_QUERY);
        maAsynchronousDeckSwitch.CancelRequest();
        maContextChangeUpdate.RequestCall();
        // TODO: this call is redundant but mandatory for unit test to update context on document loading
        UpdateConfigurations();
    }
}

void SAL_CALL SidebarController::disposing (const css::lang::EventObject& )
{
    dispose();
}

void SAL_CALL SidebarController::propertyChange (const css::beans::PropertyChangeEvent& )
{
    maPropertyChangeForwarder.RequestCall();
}

void SAL_CALL SidebarController::statusChanged (const css::frame::FeatureStateEvent& rEvent)
{
    bool bIsReadWrite (true);
    if (rEvent.IsEnabled)
        rEvent.State >>= bIsReadWrite;

    if (mbIsDocumentReadOnly != !bIsReadWrite)
    {
        mbIsDocumentReadOnly = !bIsReadWrite;

        // Force the current deck to update its panel list.
        if ( ! mbIsDocumentReadOnly)
            SwitchToDefaultDeck();

        mnRequestedForceFlags |= SwitchFlag_ForceSwitch;
        maAsynchronousDeckSwitch.CancelRequest();
        maContextChangeUpdate.RequestCall();
    }
}

void SAL_CALL SidebarController::requestLayout()
{
    sal_Int32 nMinimalWidth = 0;
    if (mpCurrentDeck && !mpCurrentDeck->isDisposed())
    {
        mpCurrentDeck->RequestLayout();
        nMinimalWidth = mpCurrentDeck->GetMinimalWidth();
    }
    RestrictWidth(nMinimalWidth);
}

void SidebarController::BroadcastPropertyChange()
{
    mpParentWindow->Invalidate(InvalidateFlags::Children);
}

void SidebarController::NotifyResize()
{
    if (!mpTabBar)
    {
        OSL_ASSERT(mpTabBar!=nullptr);
        return;
    }

    vcl::Window* pParentWindow = mpTabBar->GetParent();
    sal_Int32 nTabBarDefaultWidth = TabBar::GetDefaultWidth() * mpTabBar->GetDPIScaleFactor();

    const sal_Int32 nWidth (pParentWindow->GetSizePixel().Width());
    const sal_Int32 nHeight (pParentWindow->GetSizePixel().Height());

    mbIsDeckOpen = (nWidth > nTabBarDefaultWidth);

    if (mnSavedSidebarWidth <= 0)
        mnSavedSidebarWidth = nWidth;

    bool bIsDeckVisible;
    const bool bIsOpening (nWidth > mnWidthOnSplitterButtonDown);
    if (bIsOpening)
        bIsDeckVisible = nWidth >= nTabBarDefaultWidth + gnWidthOpenThreshold;
    else
        bIsDeckVisible = nWidth >= nTabBarDefaultWidth + gnWidthCloseThreshold;
    mbIsDeckRequestedOpen = bIsDeckVisible;
    UpdateCloseIndicator(!bIsDeckVisible);

    if (mpCurrentDeck && !mpCurrentDeck->isDisposed())
    {
        SfxSplitWindow* pSplitWindow = GetSplitWindow();
        WindowAlign eAlign = pSplitWindow ? pSplitWindow->GetAlign() : WindowAlign::Right;
        long nDeckX, nTabX;
        if (eAlign == WindowAlign::Left)     // attach the Sidebar towards the left-side of screen
        {
            nDeckX = nTabBarDefaultWidth;
            nTabX = 0;
        }
        else   // attach the Sidebar towards the right-side of screen
        {
            nDeckX = 0;
            nTabX = nWidth-nTabBarDefaultWidth;
        }

        // Place the deck first.
        if (bIsDeckVisible)
        {
            mpCurrentDeck->setPosSizePixel(nDeckX, 0, nWidth - nTabBarDefaultWidth, nHeight);
            mpCurrentDeck->Show();
            mpCurrentDeck->RequestLayout();
        }
        else
            mpCurrentDeck->Hide();

        // Now place the tab bar.
        mpTabBar->setPosSizePixel(nTabX, 0, nTabBarDefaultWidth, nHeight);
        mpTabBar->Show();

    }

    // Determine if the closer of the deck can be shown.
    sal_Int32 nMinimalWidth = 0;
    if (mpCurrentDeck && !mpCurrentDeck->isDisposed())
    {
        VclPtr<DeckTitleBar> pTitleBar = mpCurrentDeck->GetTitleBar();
        if (pTitleBar && pTitleBar->IsVisible())
            pTitleBar->SetCloserVisible(CanModifyChildWindowWidth());
        nMinimalWidth = mpCurrentDeck->GetMinimalWidth();
    }

    RestrictWidth(nMinimalWidth);
}

void SidebarController::ProcessNewWidth (const sal_Int32 nNewWidth)
{
    if ( ! mbIsDeckRequestedOpen)
        return;

    if (mbIsDeckRequestedOpen.get())
     {
        // Deck became large enough to be shown.  Show it.
        mnSavedSidebarWidth = nNewWidth;
        RequestOpenDeck();
    }
    else
    {
        // Deck became too small.  Close it completely.
        // If window is wider than the tab bar then mark the deck as being visible, even when it is not.
        // This is to trigger an adjustment of the width to the width of the tab bar.
        mbIsDeckOpen = true;
        RequestCloseDeck();

        if (mnWidthOnSplitterButtonDown > TabBar::GetDefaultWidth() * mpTabBar->GetDPIScaleFactor())
            mnSavedSidebarWidth = mnWidthOnSplitterButtonDown;
    }
}

void SidebarController::UpdateConfigurations()
{
    if (maCurrentContext == maRequestedContext
        && mnRequestedForceFlags == SwitchFlag_NoForce)
        return;

    if ((maCurrentContext.msApplication != "none") &&
         !maCurrentContext.msApplication.isEmpty())
    {
        mpResourceManager->SaveDecksSettings(maCurrentContext);
        mpResourceManager->SetLastActiveDeck(maCurrentContext, msCurrentDeckId);
    }

    // get last active deck for this application on first update
    if (!maRequestedContext.msApplication.isEmpty() &&
         (maCurrentContext.msApplication != maRequestedContext.msApplication))
    {
       OUString sLastActiveDeck = mpResourceManager->GetLastActiveDeck( maRequestedContext );
       if (!sLastActiveDeck.isEmpty())
           msCurrentDeckId = sLastActiveDeck;
    }

    maCurrentContext = maRequestedContext;

    mpResourceManager->InitDeckContext(GetCurrentContext());

    // Find the set of decks that could be displayed for the new context.
    ResourceManager::DeckContextDescriptorContainer aDecks;

    css::uno::Reference<css::frame::XController> xController = mxCurrentController.is() ? mxCurrentController : mxFrame->getController();

    mpResourceManager->GetMatchingDecks (
        aDecks,
        maCurrentContext,
        mbIsDocumentReadOnly,
        xController);

    // Notify the tab bar about the updated set of decks.
    mpTabBar->SetDecks(aDecks);

    // Find the new deck.  By default that is the same as the old
    // one.  If that is not set or not enabled, then choose the
    // first enabled deck (which is PropertyDeck).
    OUString sNewDeckId;
    for (const auto& rDeck : aDecks)
    {
        if (rDeck.mbIsEnabled)
        {
            if (rDeck.msId == msCurrentDeckId)
            {
                sNewDeckId = msCurrentDeckId;
                break;
            }
            else if (sNewDeckId.getLength() == 0)
                sNewDeckId = rDeck.msId;
        }
    }

    if (sNewDeckId.getLength() == 0)
    {
        // We did not find a valid deck.
        RequestCloseDeck();
        return;
    }

    // Tell the tab bar to highlight the button associated
    // with the deck.
    mpTabBar->HighlightDeck(sNewDeckId);

    std::shared_ptr<DeckDescriptor> xDescriptor = mpResourceManager->GetDeckDescriptor(sNewDeckId);

    if (xDescriptor)
    {
        SwitchToDeck(*xDescriptor, maCurrentContext);
    }
}

namespace {

void collectUIInformation(const OUString& rDeckId)
{
    EventDescription aDescription;
    aDescription.aAction = "SIDEBAR";
    aDescription.aParent = "MainWindow";
    aDescription.aParameters = {{"PANEL", rDeckId}};
    aDescription.aKeyWord = "CurrentApp";

    UITestLogger::getInstance().logEvent(aDescription);
}

}

void SidebarController::OpenThenToggleDeck (
    const OUString& rsDeckId)
{
    SfxSplitWindow* pSplitWindow = GetSplitWindow();
    if ( pSplitWindow && !pSplitWindow->IsFadeIn() )
        // tdf#83546 Collapsed sidebar should expand first
        pSplitWindow->FadeIn();
    else if ( IsDeckVisible( rsDeckId ) )
    {
        if ( pSplitWindow )
        {
            // tdf#67627 Clicking a second time on a Deck icon will close the Deck
            RequestCloseDeck();
            return;
        }
        else if( !WasFloatingDeckClosed() )
        {
            // tdf#88241 Summoning an undocked sidebar a second time should close sidebar
            mpParentWindow->Close();
            return;
        }
    }
    RequestOpenDeck();
    SwitchToDeck(rsDeckId);

    // Make sure the sidebar is wide enough to fit the requested content
    sal_Int32 nRequestedWidth = (mpCurrentDeck->GetMinimalWidth() + TabBar::GetDefaultWidth())
                                * mpTabBar->GetDPIScaleFactor();
    if (mnSavedSidebarWidth < nRequestedWidth)
        SetChildWindowWidth(nRequestedWidth);

    mpTabBar->Invalidate();
    mpTabBar->HighlightDeck(rsDeckId);
    collectUIInformation(rsDeckId);
}

void SidebarController::OpenThenSwitchToDeck (
    const OUString& rsDeckId)
{
    SfxSplitWindow* pSplitWindow = GetSplitWindow();
    if ( pSplitWindow && !pSplitWindow->IsFadeIn() )
        // tdf#83546 Collapsed sidebar should expand first
        pSplitWindow->FadeIn();
    RequestOpenDeck();
    SwitchToDeck(rsDeckId);
    mpTabBar->Invalidate();
    mpTabBar->HighlightDeck(rsDeckId);
}

void SidebarController::SwitchToDefaultDeck()
{
    SwitchToDeck(gsDefaultDeckId);
}

void SidebarController::SwitchToDeck (
    const OUString& rsDeckId)
{
    if (  msCurrentDeckId != rsDeckId
        || ! mbIsDeckOpen
        || mnRequestedForceFlags!=SwitchFlag_NoForce)
    {
        std::shared_ptr<DeckDescriptor> xDeckDescriptor = mpResourceManager->GetDeckDescriptor(rsDeckId);

        if (xDeckDescriptor)
            SwitchToDeck(*xDeckDescriptor, maCurrentContext);
    }
}

void SidebarController::CreateDeck(const OUString& rDeckId) {
    CreateDeck(rDeckId, maCurrentContext);
}

void SidebarController::CreateDeck(const OUString& rDeckId, const Context& rContext, bool bForceCreate)
{
    std::shared_ptr<DeckDescriptor> xDeckDescriptor = mpResourceManager->GetDeckDescriptor(rDeckId);

    if (!xDeckDescriptor)
        return;

    VclPtr<Deck> aDeck = xDeckDescriptor->mpDeck;
    if (aDeck.get()==nullptr || bForceCreate)
    {
        if (aDeck.get()!=nullptr)
            aDeck.disposeAndClear();

        aDeck = VclPtr<Deck>::Create(
                        *xDeckDescriptor,
                        mpParentWindow,
                        [this]() { return this->RequestCloseDeck(); });
    }
    xDeckDescriptor->mpDeck = aDeck;
    CreatePanels(rDeckId, rContext);
}

void SidebarController::CreatePanels(const OUString& rDeckId, const Context& rContext)
{
    std::shared_ptr<DeckDescriptor> xDeckDescriptor = mpResourceManager->GetDeckDescriptor(rDeckId);

    // init panels bounded to that deck, do not wait them being displayed as may be accessed through API

    VclPtr<Deck> pDeck = xDeckDescriptor->mpDeck;

    ResourceManager::PanelContextDescriptorContainer aPanelContextDescriptors;

    css::uno::Reference<css::frame::XController> xController = mxCurrentController.is() ? mxCurrentController : mxFrame->getController();

    mpResourceManager->GetMatchingPanels(
                                        aPanelContextDescriptors,
                                        rContext,
                                        rDeckId,
                                        xController);

    // Update the panel list.
    const sal_Int32 nNewPanelCount (aPanelContextDescriptors.size());
    SharedPanelContainer aNewPanels;
    sal_Int32 nWriteIndex (0);

    aNewPanels.resize(nNewPanelCount);

    for (sal_Int32 nReadIndex=0; nReadIndex<nNewPanelCount; ++nReadIndex)
    {
        const ResourceManager::PanelContextDescriptor& rPanelContexDescriptor (
            aPanelContextDescriptors[nReadIndex]);

        // Determine if the panel can be displayed.
        const bool bIsPanelVisible (!mbIsDocumentReadOnly || rPanelContexDescriptor.mbShowForReadOnlyDocuments);
        if ( ! bIsPanelVisible)
            continue;

        Panel *const pPanel(pDeck->GetPanel(rPanelContexDescriptor.msId));
        if (pPanel != nullptr)
        {
            aNewPanels[nWriteIndex] = pPanel;
            pPanel->SetExpanded( rPanelContexDescriptor.mbIsInitiallyVisible );
            ++nWriteIndex;
        }
        else
        {
                VclPtr<Panel>  aPanel = CreatePanel(
                                            rPanelContexDescriptor.msId,
                                            pDeck->GetPanelParentWindow(),
                                            rPanelContexDescriptor.mbIsInitiallyVisible,
                                            rContext,
                                            pDeck);
                if (aPanel.get()!=nullptr )
                {
                    aNewPanels[nWriteIndex] = aPanel;

                    // Depending on the context we have to change the command
                    // for the "more options" dialog.
                    VclPtr<PanelTitleBar> pTitleBar = aNewPanels[nWriteIndex]->GetTitleBar();
                    if (pTitleBar)
                    {
                        pTitleBar->SetMoreOptionsCommand(
                            rPanelContexDescriptor.msMenuCommand,
                            mxFrame, xController);
                    }
                    ++nWriteIndex;
                }
        }
    }

    // mpCurrentPanels - may miss stuff (?)
    aNewPanels.resize(nWriteIndex);
    pDeck->ResetPanels(aNewPanels);
}

void SidebarController::SwitchToDeck (
    const DeckDescriptor& rDeckDescriptor,
    const Context& rContext)
{

    maFocusManager.Clear();

    const bool bForceNewDeck ((mnRequestedForceFlags&SwitchFlag_ForceNewDeck)!=0);
    const bool bForceNewPanels ((mnRequestedForceFlags&SwitchFlag_ForceNewPanels)!=0);
    mnRequestedForceFlags = SwitchFlag_NoForce;

    if (   msCurrentDeckId != rDeckDescriptor.msId
        || bForceNewDeck)
    {
        if (mpCurrentDeck)
            mpCurrentDeck->Hide();

        msCurrentDeckId = rDeckDescriptor.msId;
    }
    mpTabBar->HighlightDeck(msCurrentDeckId);

    // Determine the panels to display in the deck.
    ResourceManager::PanelContextDescriptorContainer aPanelContextDescriptors;

    css::uno::Reference<css::frame::XController> xController = mxCurrentController.is() ? mxCurrentController : mxFrame->getController();

    mpResourceManager->GetMatchingPanels(
        aPanelContextDescriptors,
        rContext,
        rDeckDescriptor.msId,
        xController);

    if (aPanelContextDescriptors.empty())
    {
        // There are no panels to be displayed in the current context.
        if (vcl::EnumContext::GetContextEnum(rContext.msContext) != vcl::EnumContext::Context::Empty)
        {
            // Switch to the "empty" context and try again.
            SwitchToDeck(
                rDeckDescriptor,
                Context(
                    rContext.msApplication,
                    vcl::EnumContext::GetContextName(vcl::EnumContext::Context::Empty)));
            return;
        }
        else
        {
            // This is already the "empty" context. Looks like we have
            // to live with an empty deck.
        }
    }

    // Provide a configuration and Deck object.

    CreateDeck(rDeckDescriptor.msId, rContext, bForceNewDeck);

    if (bForceNewPanels && !bForceNewDeck) // already forced if bForceNewDeck
        CreatePanels(rDeckDescriptor.msId, rContext);

    if (mpCurrentDeck && mpCurrentDeck != rDeckDescriptor.mpDeck)
        mpCurrentDeck->Hide();
    mpCurrentDeck.reset(rDeckDescriptor.mpDeck);

    if ( ! mpCurrentDeck)
        return;

#ifdef DEBUG
    // Show the context name in the deck title bar.
    VclPtr<DeckTitleBar> pDebugTitleBar = mpCurrentDeck->GetTitleBar();
    if (pDebugTitleBar)
        pDebugTitleBar->SetTitle(rDeckDescriptor.msTitle + " (" + maCurrentContext.msContext + ")");
#endif

    SfxSplitWindow* pSplitWindow = GetSplitWindow();
    sal_Int32 nTabBarDefaultWidth = TabBar::GetDefaultWidth() * mpTabBar->GetDPIScaleFactor();
    WindowAlign eAlign = pSplitWindow ? pSplitWindow->GetAlign() : WindowAlign::Right;
    long nDeckX;
    if (eAlign == WindowAlign::Left)     // attach the Sidebar towards the left-side of screen
    {
        nDeckX = nTabBarDefaultWidth;
    }
    else   // attach the Sidebar towards the right-side of screen
    {
        nDeckX = 0;
    }


    // Activate the deck and the new set of panels.
    mpCurrentDeck->setPosSizePixel(
        nDeckX,
        0,
        mpParentWindow->GetSizePixel().Width() - nTabBarDefaultWidth,
        mpParentWindow->GetSizePixel().Height());

    mpCurrentDeck->Show();

    mpParentWindow->SetText(rDeckDescriptor.msTitle);

    NotifyResize();

    // Tell the focus manager about the new panels and tab bar
    // buttons.
    maFocusManager.SetDeckTitle(mpCurrentDeck->GetTitleBar());
    maFocusManager.SetPanels(mpCurrentDeck->GetPanels());

    mpTabBar->UpdateFocusManager(maFocusManager);
    UpdateTitleBarIcons();
}

void SidebarController::notifyDeckTitle(const OUString& targetDeckId)
{
    if (msCurrentDeckId == targetDeckId)
    {
        maFocusManager.SetDeckTitle(mpCurrentDeck->GetTitleBar());
        mpTabBar->UpdateFocusManager(maFocusManager);
        UpdateTitleBarIcons();
    }
}

VclPtr<Panel> SidebarController::CreatePanel (
    const OUString& rsPanelId,
    vcl::Window* pParentWindow,
    const bool bIsInitiallyExpanded,
    const Context& rContext,
    const VclPtr<Deck>& pDeck)
{
    std::shared_ptr<PanelDescriptor> xPanelDescriptor = mpResourceManager->GetPanelDescriptor(rsPanelId);

    if (!xPanelDescriptor)
        return nullptr;

    // Create the panel which is the parent window of the UIElement.
    VclPtr<Panel> pPanel = VclPtr<Panel>::Create(
        *xPanelDescriptor,
        pParentWindow,
        bIsInitiallyExpanded,
        [pDeck]() { return pDeck.get()->RequestLayout(); },
        [this]() { return this->GetCurrentContext(); },
        mxFrame);

    // Create the XUIElement.
    Reference<ui::XUIElement> xUIElement (CreateUIElement(
            pPanel->GetComponentInterface(),
            xPanelDescriptor->msImplementationURL,
            xPanelDescriptor->mbWantsCanvas,
            rContext));
    if (xUIElement.is())
    {
        // Initialize the panel and add it to the active deck.
        pPanel->SetUIElement(xUIElement);
    }
    else
    {
        pPanel.disposeAndClear();
    }

    return pPanel;
}

Reference<ui::XUIElement> SidebarController::CreateUIElement (
    const Reference<awt::XWindowPeer>& rxWindow,
    const OUString& rsImplementationURL,
    const bool bWantsCanvas,
    const Context& rContext)
{
    try
    {
        const Reference<XComponentContext> xComponentContext (::comphelper::getProcessComponentContext() );
        const Reference<ui::XUIElementFactory> xUIElementFactory =
               ui::theUIElementFactoryManager::get( xComponentContext );

       // Create the XUIElement.
        ::comphelper::NamedValueCollection aCreationArguments;
        aCreationArguments.put("Frame", makeAny(mxFrame));
        aCreationArguments.put("ParentWindow", makeAny(rxWindow));
        SfxDockingWindow* pSfxDockingWindow = dynamic_cast<SfxDockingWindow*>(mpParentWindow.get());
        if (pSfxDockingWindow != nullptr)
            aCreationArguments.put("SfxBindings", makeAny(sal_uInt64(&pSfxDockingWindow->GetBindings())));
        aCreationArguments.put("Theme", Theme::GetPropertySet());
        aCreationArguments.put("Sidebar", makeAny(Reference<ui::XSidebar>(static_cast<ui::XSidebar*>(this))));
        if (bWantsCanvas)
        {
            Reference<rendering::XSpriteCanvas> xCanvas (VCLUnoHelper::GetWindow(rxWindow)->GetSpriteCanvas());
            aCreationArguments.put("Canvas", makeAny(xCanvas));
        }

        if (mxCurrentController.is())
        {
            OUString aModule = Tools::GetModuleName(mxCurrentController);
            if (!aModule.isEmpty())
            {
                aCreationArguments.put("Module", makeAny(aModule));
            }
            aCreationArguments.put("Controller", makeAny(mxCurrentController));
        }

        aCreationArguments.put("ApplicationName", makeAny(rContext.msApplication));
        aCreationArguments.put("ContextName", makeAny(rContext.msContext));

        Reference<ui::XUIElement> xUIElement(
            xUIElementFactory->createUIElement(
                rsImplementationURL,
                aCreationArguments.getPropertyValues()),
            UNO_SET_THROW);

        return xUIElement;
    }
    catch(const Exception&)
    {
        TOOLS_WARN_EXCEPTION("sfx.sidebar", "Cannot create panel " << rsImplementationURL);
        return nullptr;
    }
}

IMPL_LINK(SidebarController, WindowEventHandler, VclWindowEvent&, rEvent, void)
{
    if (rEvent.GetWindow() == mpParentWindow)
    {
        switch (rEvent.GetId())
        {
            case VclEventId::WindowShow:
            case VclEventId::WindowResize:
                NotifyResize();
                break;

            case VclEventId::WindowDataChanged:
                // Force an update of deck and tab bar to reflect
                // changes in theme (high contrast mode).
                Theme::HandleDataChange();
                UpdateTitleBarIcons();
                mpParentWindow->Invalidate();
                mnRequestedForceFlags |= SwitchFlag_ForceNewDeck | SwitchFlag_ForceNewPanels;
                maAsynchronousDeckSwitch.CancelRequest();
                maContextChangeUpdate.RequestCall();
                break;

            case VclEventId::ObjectDying:
                dispose();
                break;

            case VclEventId::WindowPaint:
                SAL_INFO("sfx.sidebar", "Paint");
                break;

            default:
                break;
        }
    }
    else if (rEvent.GetWindow()==mpSplitWindow && mpSplitWindow!=nullptr)
    {
        switch (rEvent.GetId())
        {
            case VclEventId::WindowMouseButtonDown:
                mnWidthOnSplitterButtonDown = mpParentWindow->GetSizePixel().Width();
                break;

            case VclEventId::WindowMouseButtonUp:
            {
                ProcessNewWidth(mpParentWindow->GetSizePixel().Width());
                mnWidthOnSplitterButtonDown = 0;
                break;
            }

            case VclEventId::ObjectDying:
                dispose();
                break;

            default: break;
         }
    }
}

void SidebarController::ShowPopupMenu (
    const tools::Rectangle& rButtonBox,
    const ::std::vector<TabBar::DeckMenuData>& rMenuData) const
{
    VclPtr<PopupMenu> pMenu = CreatePopupMenu(rMenuData);
    pMenu->SetSelectHdl(LINK(const_cast<SidebarController*>(this), SidebarController, OnMenuItemSelected));

    // pass toolbox button rect so the menu can stay open on button up
    tools::Rectangle aBox (rButtonBox);
    aBox.Move(mpTabBar->GetPosPixel().X(), 0);
    pMenu->Execute(mpParentWindow, aBox, PopupMenuFlags::ExecuteDown);
    pMenu.disposeAndClear();
}

VclPtr<PopupMenu> SidebarController::CreatePopupMenu (
    const ::std::vector<TabBar::DeckMenuData>& rMenuData) const
{
    // Create the top level popup menu.
    auto pMenu = VclPtr<PopupMenu>::Create();
    FloatingWindow* pMenuWindow = dynamic_cast<FloatingWindow*>(pMenu->GetWindow());
    if (pMenuWindow != nullptr)
    {
        pMenuWindow->SetPopupModeFlags(pMenuWindow->GetPopupModeFlags() | FloatWinPopupFlags::NoMouseUpClose);
    }

    // Create sub menu for customization (hiding of deck tabs.)
    VclPtr<PopupMenu> pCustomizationMenu = VclPtr<PopupMenu>::Create();

    // Add one entry for every tool panel element to individually make
    // them visible or hide them.
    sal_Int32 nIndex (0);
    for (const auto& rItem : rMenuData)
    {
        const sal_Int32 nMenuIndex (nIndex+MID_FIRST_PANEL);
        pMenu->InsertItem(nMenuIndex, rItem.msDisplayName, MenuItemBits::RADIOCHECK);
        pMenu->CheckItem(nMenuIndex, rItem.mbIsCurrentDeck);
        pMenu->EnableItem(nMenuIndex, rItem.mbIsEnabled && rItem.mbIsActive);

        const sal_Int32 nSubMenuIndex (nIndex+MID_FIRST_HIDE);
        if (rItem.mbIsCurrentDeck)
        {
            // Don't allow the currently visible deck to be disabled.
            pCustomizationMenu->InsertItem(nSubMenuIndex, rItem.msDisplayName, MenuItemBits::RADIOCHECK);
            pCustomizationMenu->CheckItem(nSubMenuIndex);
        }
        else
        {
            pCustomizationMenu->InsertItem(nSubMenuIndex, rItem.msDisplayName, MenuItemBits::CHECKABLE);
            pCustomizationMenu->CheckItem(nSubMenuIndex, rItem.mbIsEnabled && rItem.mbIsActive);
        }
        ++nIndex;
    }

    pMenu->InsertSeparator();

    // Add entry for docking or un-docking the tool panel.
    if (mpParentWindow->IsFloatingMode())
        pMenu->InsertItem(MID_LOCK_TASK_PANEL, SfxResId(STR_SFX_DOCK));
    else
        pMenu->InsertItem(MID_UNLOCK_TASK_PANEL, SfxResId(STR_SFX_UNDOCK));

    pMenu->InsertItem(MID_HIDE_SIDEBAR, SfxResId(SFX_STR_SIDEBAR_HIDE_SIDEBAR));
    pCustomizationMenu->InsertSeparator();
    pCustomizationMenu->InsertItem(MID_RESTORE_DEFAULT, SfxResId(SFX_STR_SIDEBAR_RESTORE));

    pMenu->InsertItem(MID_CUSTOMIZATION, SfxResId(SFX_STR_SIDEBAR_CUSTOMIZATION));
    pMenu->SetPopupMenu(MID_CUSTOMIZATION, pCustomizationMenu);

    pMenu->RemoveDisabledEntries(false);

    return pMenu;
}

IMPL_LINK(SidebarController, OnMenuItemSelected, Menu*, pMenu, bool)
{
    if (pMenu == nullptr)
    {
        OSL_ENSURE(pMenu!=nullptr, "sfx2::sidebar::SidebarController::OnMenuItemSelected: illegal menu!");
        return false;
    }

    pMenu->Deactivate();
    const sal_Int32 nIndex (pMenu->GetCurItemId());
    switch (nIndex)
    {
        case MID_UNLOCK_TASK_PANEL:
            mpParentWindow->SetFloatingMode(true);
            if (mpParentWindow->IsFloatingMode())
                mpParentWindow->ToTop(ToTopFlags::GrabFocusOnly);
            break;

        case MID_LOCK_TASK_PANEL:
            mpParentWindow->SetFloatingMode(false);
            break;

        case MID_RESTORE_DEFAULT:
            mpTabBar->RestoreHideFlags();
            break;

        case MID_HIDE_SIDEBAR:
        {
            const util::URL aURL (Tools::GetURL(".uno:Sidebar"));
            Reference<frame::XDispatch> xDispatch (Tools::GetDispatch(mxFrame, aURL));
            if (xDispatch.is())
                    xDispatch->dispatch(aURL, Sequence<beans::PropertyValue>());
            break;
        }
        default:
        {
            try
            {
                if (nIndex >= MID_FIRST_PANEL && nIndex<MID_FIRST_HIDE)
                {
                    RequestOpenDeck();
                    SwitchToDeck(mpTabBar->GetDeckIdForIndex(nIndex - MID_FIRST_PANEL));
                }
                else if (nIndex >=MID_FIRST_HIDE)
                    if (pMenu->GetItemBits(nIndex) == MenuItemBits::CHECKABLE)
                    {
                        mpTabBar->ToggleHideFlag(nIndex-MID_FIRST_HIDE);

                        // Find the set of decks that could be displayed for the new context.
                        ResourceManager::DeckContextDescriptorContainer aDecks;
                        mpResourceManager->GetMatchingDecks (
                                                    aDecks,
                                                    GetCurrentContext(),
                                                    IsDocumentReadOnly(),
                                                    mxFrame->getController());
                        // Notify the tab bar about the updated set of decks.
                        mpTabBar->SetDecks(aDecks);
                        mpTabBar->HighlightDeck(mpCurrentDeck->GetId());
                        mpTabBar->UpdateFocusManager(maFocusManager);
                    }
                mpParentWindow->GrabFocusToDocument();
            }
            catch (RuntimeException&)
            {
            }
        }
        break;
    }

    return true;
}

void SidebarController::RequestCloseDeck()
{
    mbIsDeckRequestedOpen = false;
    UpdateDeckOpenState();

    if (!mpCurrentDeck.get())
        mpTabBar->RemoveDeckHighlight();
}

void SidebarController::RequestOpenDeck()
{
    mbIsDeckRequestedOpen = true;
    UpdateDeckOpenState();
}

bool SidebarController::IsDeckOpen(const sal_Int32 nIndex)
{
    if (nIndex >= 0)
    {
        OUString asDeckId(mpTabBar->GetDeckIdForIndex(nIndex));
        return IsDeckVisible(asDeckId);
    }
    return mbIsDeckOpen && mbIsDeckOpen.get();
}

bool SidebarController::IsDeckVisible(const OUString& rsDeckId)
{
    return mbIsDeckOpen && mbIsDeckOpen.get() && msCurrentDeckId == rsDeckId;
}

void SidebarController::UpdateDeckOpenState()
{
    if ( ! mbIsDeckRequestedOpen)
        // No state requested.
        return;

    sal_Int32 nTabBarDefaultWidth = TabBar::GetDefaultWidth() * mpTabBar->GetDPIScaleFactor();

    // Update (change) the open state when it either has not yet been initialized
    // or when its value differs from the requested state.
    if ( mbIsDeckOpen && mbIsDeckOpen.get() == mbIsDeckRequestedOpen.get() )
        return;

    if (mbIsDeckRequestedOpen.get())
    {
        if (mnSavedSidebarWidth <= nTabBarDefaultWidth)
            SetChildWindowWidth(SidebarChildWindow::GetDefaultWidth(mpParentWindow));
        else
            SetChildWindowWidth(mnSavedSidebarWidth);
    }
    else
    {
        if ( ! mpParentWindow->IsFloatingMode())
            mnSavedSidebarWidth = SetChildWindowWidth(nTabBarDefaultWidth);
        if (mnWidthOnSplitterButtonDown > nTabBarDefaultWidth)
            mnSavedSidebarWidth = mnWidthOnSplitterButtonDown;
        mpParentWindow->SetStyle(mpParentWindow->GetStyle() & ~WB_SIZEABLE);
    }

    mbIsDeckOpen = mbIsDeckRequestedOpen.get();
    if (mbIsDeckOpen.get() && mpCurrentDeck)
        mpCurrentDeck->Show(mbIsDeckOpen.get());
    NotifyResize();
}

bool SidebarController::CanModifyChildWindowWidth()
{
    SfxSplitWindow* pSplitWindow = GetSplitWindow();
    if (pSplitWindow == nullptr)
        return false;

    sal_uInt16 nRow (0xffff);
    sal_uInt16 nColumn (0xffff);
    if (pSplitWindow->GetWindowPos(mpParentWindow, nColumn, nRow))
    {
        sal_uInt16 nRowCount (pSplitWindow->GetWindowCount(nColumn));
        return nRowCount==1;
    }
    else
        return false;
}

sal_Int32 SidebarController::SetChildWindowWidth (const sal_Int32 nNewWidth)
{
    SfxSplitWindow* pSplitWindow = GetSplitWindow();
    if (pSplitWindow == nullptr)
        return 0;

    sal_uInt16 nRow (0xffff);
    sal_uInt16 nColumn (0xffff);
    pSplitWindow->GetWindowPos(mpParentWindow, nColumn, nRow);
    const long nColumnWidth (pSplitWindow->GetLineSize(nColumn));

    vcl::Window* pWindow = mpParentWindow;
    const Size aWindowSize (pWindow->GetSizePixel());

    pSplitWindow->MoveWindow(
        mpParentWindow,
        Size(nNewWidth, aWindowSize.Height()),
        nColumn,
        nRow,
        false);
    static_cast<SplitWindow*>(pSplitWindow)->Split();

    return static_cast<sal_Int32>(nColumnWidth);
}

void SidebarController::RestrictWidth (sal_Int32 nWidth)
{
    SfxSplitWindow* pSplitWindow = GetSplitWindow();
    if (pSplitWindow != nullptr)
    {
        const sal_uInt16 nId (pSplitWindow->GetItemId(mpParentWindow.get()));
        const sal_uInt16 nSetId (pSplitWindow->GetSet(nId));
        const sal_Int32 nRequestedWidth
            = (TabBar::GetDefaultWidth() + nWidth) * mpTabBar->GetDPIScaleFactor();

        pSplitWindow->SetItemSizeRange(
            nSetId,
            Range(nRequestedWidth,
                  getMaximumWidth() * mpTabBar->GetDPIScaleFactor()));
    }
}

SfxSplitWindow* SidebarController::GetSplitWindow()
{
    if (mpParentWindow != nullptr)
    {
        SfxSplitWindow* pSplitWindow = dynamic_cast<SfxSplitWindow*>(mpParentWindow->GetParent());
        if (pSplitWindow != mpSplitWindow)
        {
            if (mpSplitWindow != nullptr)
                mpSplitWindow->RemoveEventListener(LINK(this, SidebarController, WindowEventHandler));

            mpSplitWindow = pSplitWindow;

            if (mpSplitWindow != nullptr)
                mpSplitWindow->AddEventListener(LINK(this, SidebarController, WindowEventHandler));
        }
        return mpSplitWindow;
    }
    else
        return nullptr;
}

void SidebarController::UpdateCloseIndicator (const bool bCloseAfterDrag)
{
    if (mpParentWindow == nullptr)
        return;

    if (bCloseAfterDrag)
    {
        // Make sure that the indicator exists.
        if ( ! mpCloseIndicator)
        {
            mpCloseIndicator.reset(VclPtr<FixedImage>::Create(mpParentWindow));
            FixedImage* pFixedImage = static_cast<FixedImage*>(mpCloseIndicator.get());
            const Image aImage (Theme::GetImage(Theme::Image_CloseIndicator));
            pFixedImage->SetImage(aImage);
            pFixedImage->SetSizePixel(aImage.GetSizePixel());
            pFixedImage->SetBackground(Theme::GetWallpaper(Theme::Paint_DeckBackground));
        }

        // Place and show the indicator.
        const Size aWindowSize (mpParentWindow->GetSizePixel());
        const Size aImageSize (mpCloseIndicator->GetSizePixel());
        mpCloseIndicator->SetPosPixel(
            Point(
                aWindowSize.Width() - TabBar::GetDefaultWidth() * mpTabBar->GetDPIScaleFactor() - aImageSize.Width(),
                (aWindowSize.Height() - aImageSize.Height())/2));
        mpCloseIndicator->Show();
    }
    else
    {
        // Hide but don't delete the indicator.
        if (mpCloseIndicator)
            mpCloseIndicator->Hide();
    }
}

void SidebarController::UpdateTitleBarIcons()
{
    if ( ! mpCurrentDeck)
        return;

    const bool bIsHighContrastModeActive (Theme::IsHighContrastMode());

    const ResourceManager& rResourceManager = *mpResourceManager;

    // Update the deck icon.
    std::shared_ptr<DeckDescriptor> xDeckDescriptor = rResourceManager.GetDeckDescriptor(mpCurrentDeck->GetId());
    if (xDeckDescriptor && mpCurrentDeck->GetTitleBar())
    {
        const OUString sIconURL(
            bIsHighContrastModeActive
                ? xDeckDescriptor->msHighContrastTitleBarIconURL
                : xDeckDescriptor->msTitleBarIconURL);
        mpCurrentDeck->GetTitleBar()->SetIcon(Tools::GetImage(sIconURL, mxFrame));
    }

    // Update the panel icons.
    const SharedPanelContainer& rPanels (mpCurrentDeck->GetPanels());
    for (const auto& rxPanel : rPanels)
    {
        if ( ! rxPanel)
            continue;
        if (!rxPanel->GetTitleBar())
            continue;
        std::shared_ptr<PanelDescriptor> xPanelDescriptor = rResourceManager.GetPanelDescriptor(rxPanel->GetId());
        if (!xPanelDescriptor)
            continue;
        const OUString sIconURL (
            bIsHighContrastModeActive
               ? xPanelDescriptor->msHighContrastTitleBarIconURL
               : xPanelDescriptor->msTitleBarIconURL);
        rxPanel->GetTitleBar()->SetIcon(Tools::GetImage(sIconURL, mxFrame));
    }
}

void SidebarController::ShowPanel (const Panel& rPanel)
{
    if (mpCurrentDeck)
    {
        if (!IsDeckOpen())
            RequestOpenDeck();
        mpCurrentDeck->ShowPanel(rPanel);
    }
}

ResourceManager::DeckContextDescriptorContainer SidebarController::GetMatchingDecks()
{
    ResourceManager::DeckContextDescriptorContainer aDecks;
    mpResourceManager->GetMatchingDecks (aDecks,
                                        GetCurrentContext(),
                                        IsDocumentReadOnly(),
                                        mxFrame->getController());
    return aDecks;
}

ResourceManager::PanelContextDescriptorContainer SidebarController::GetMatchingPanels(const OUString& rDeckId)
{
    ResourceManager::PanelContextDescriptorContainer aPanels;

    mpResourceManager->GetMatchingPanels(aPanels,
                                        GetCurrentContext(),
                                        rDeckId,
                                        mxFrame->getController());
    return aPanels;
}

void SidebarController::updateModel(const css::uno::Reference<css::frame::XModel>& xModel)
{
    mpResourceManager->UpdateModel(xModel);
}

void SidebarController::FadeOut()
{
    if (mpSplitWindow)
        mpSplitWindow->FadeOut();
}

void SidebarController::FadeIn()
{
    if (mpSplitWindow)
        mpSplitWindow->FadeIn();
}

tools::Rectangle SidebarController::GetDeckDragArea() const
{
    tools::Rectangle aRect;

    if(mpCurrentDeck)
    {
        VclPtr<DeckTitleBar> pTitleBar(mpCurrentDeck->GetTitleBar());

        if(pTitleBar)
        {
            aRect = DeckTitleBar::GetDragArea();
        }
    }

    return aRect;
}

void SidebarController::frameAction(const css::frame::FrameActionEvent& rEvent)
{
    if (rEvent.Frame == mxFrame)
    {
        if (rEvent.Action == css::frame::FrameAction_COMPONENT_DETACHING)
            unregisterSidebarForFrame(this, mxFrame->getController());
        else if (rEvent.Action == css::frame::FrameAction_COMPONENT_REATTACHED)
            registerSidebarForFrame(this, mxFrame->getController());
    }
}

} } // end of namespace sfx2::sidebar

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
