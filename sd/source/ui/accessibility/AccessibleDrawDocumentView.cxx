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

#include <AccessibleDrawDocumentView.hxx>
#include <com/sun/star/drawing/ShapeCollection.hpp>
#include <com/sun/star/drawing/XDrawView.hpp>
#include <com/sun/star/drawing/XDrawPagesSupplier.hpp>
#include <com/sun/star/drawing/XShapes.hpp>
#include <com/sun/star/beans/XPropertySet.hpp>
#include <com/sun/star/accessibility/AccessibleEventId.hpp>
#include <com/sun/star/accessibility/AccessibleStateType.hpp>
#include <com/sun/star/accessibility/AccessibleRole.hpp>
#include <com/sun/star/lang/IndexOutOfBoundsException.hpp>
#include <com/sun/star/lang/XMultiServiceFactory.hpp>
#include <com/sun/star/view/XSelectionSupplier.hpp>
#include <cppuhelper/queryinterface.hxx>
#include <comphelper/processfactory.hxx>
#include <sal/log.hxx>
#include <tools/debug.hxx>

#include <svx/AccessibleShape.hxx>
#include <svx/ChildrenManager.hxx>
#include <svx/svdobj.hxx>
#include <svx/unoapi.hxx>
#include <vcl/svapp.hxx>

#include <ViewShell.hxx>
#include <View.hxx>
#include <DrawDocShell.hxx>
#include <drawdoc.hxx>
#include <algorithm>
#include <slideshow.hxx>
#include <anminfo.hxx>
#include <AccessiblePageShape.hxx>

#include <strings.hrc>
#include <sdresid.hxx>
#include <osl/mutex.hxx>

using namespace ::com::sun::star;
using namespace ::com::sun::star::uno;
using namespace ::com::sun::star::accessibility;

namespace accessibility {

struct XShapePosCompareHelper
{
    bool operator() ( const uno::Reference<drawing::XShape>& xshape1,
        const uno::Reference<drawing::XShape>& xshape2 ) const
    {
        // modify the compare method to return the Z-Order, not layout order
        SdrObject* pObj1 = GetSdrObjectFromXShape(xshape1);
        SdrObject* pObj2 = GetSdrObjectFromXShape(xshape2);
        if(pObj1 && pObj2)
            return pObj1->GetOrdNum() < pObj2->GetOrdNum();
        else
            return false;
    }
};
//=====  internal  ============================================================

AccessibleDrawDocumentView::AccessibleDrawDocumentView (
    ::sd::Window* pSdWindow,
    ::sd::ViewShell* pViewShell,
    const uno::Reference<frame::XController>& rxController,
    const uno::Reference<XAccessible>& rxParent)
    : AccessibleDocumentViewBase (pSdWindow, pViewShell, rxController, rxParent),
      mpSdViewSh( pViewShell )
{
    UpdateAccessibleName();
}

AccessibleDrawDocumentView::~AccessibleDrawDocumentView()
{
    DBG_ASSERT (rBHelper.bDisposed || rBHelper.bInDispose,
        "~AccessibleDrawDocumentView: object has not been disposed");
}

void AccessibleDrawDocumentView::Init()
{
    AccessibleDocumentViewBase::Init ();

    // Determine the list of shapes on the current page.
    uno::Reference<drawing::XShapes> xShapeList;
    uno::Reference<drawing::XDrawView> xView (mxController, uno::UNO_QUERY);
    if (xView.is())
        xShapeList.set( xView->getCurrentPage(), uno::UNO_QUERY);

    // Create the children manager.
    mpChildrenManager.reset(new ChildrenManager(this, xShapeList, maShapeTreeInfo, *this));

    rtl::Reference<AccessiblePageShape> xPage(CreateDrawPageShape());
    if (xPage.is())
    {
        xPage->Init();
        mpChildrenManager->AddAccessibleShape (xPage.get());
        mpChildrenManager->Update ();
    }

    mpChildrenManager->UpdateSelection ();
}

void AccessibleDrawDocumentView::ViewForwarderChanged()
{
    AccessibleDocumentViewBase::ViewForwarderChanged();
    if (mpChildrenManager != nullptr)
        mpChildrenManager->ViewForwarderChanged();
}

/**  The page shape is created on every call at the moment (provided that
     every thing goes well).
*/
rtl::Reference<AccessiblePageShape> AccessibleDrawDocumentView::CreateDrawPageShape()
{
    rtl::Reference<AccessiblePageShape> xShape;

    // Create a shape that represents the actual draw page.
    uno::Reference<drawing::XDrawView> xView (mxController, uno::UNO_QUERY);
    if (xView.is())
    {
        uno::Reference<beans::XPropertySet> xSet (
            uno::Reference<beans::XPropertySet> (xView->getCurrentPage(), uno::UNO_QUERY));
        if (xSet.is())
        {
            // Create a rectangle shape that will represent the draw page.
            uno::Reference<lang::XMultiServiceFactory> xFactory (mxModel, uno::UNO_QUERY);
            uno::Reference<drawing::XShape> xRectangle;
            if (xFactory.is())
                xRectangle.set(xFactory->createInstance ("com.sun.star.drawing.RectangleShape"),
                    uno::UNO_QUERY);

            // Set the shape's size and position.
            if (xRectangle.is())
            {
                uno::Any aValue;
                awt::Point aPosition;
                awt::Size aSize;

                // Set size and position of the shape to those of the draw
                // page.
                aValue = xSet->getPropertyValue ("BorderLeft");
                aValue >>= aPosition.X;
                aValue = xSet->getPropertyValue ("BorderTop");
                aValue >>= aPosition.Y;
                xRectangle->setPosition (aPosition);

                aValue = xSet->getPropertyValue ("Width");
                aValue >>= aSize.Width;
                aValue = xSet->getPropertyValue ("Height");
                aValue >>= aSize.Height;
                xRectangle->setSize (aSize);

                // Create the accessible object for the shape and
                // initialize it.
                xShape = new AccessiblePageShape (
                    xView->getCurrentPage(), this, maShapeTreeInfo);
            }
        }
    }
    return xShape;
}

//=====  XAccessibleContext  ==================================================

sal_Int32 SAL_CALL
    AccessibleDrawDocumentView::getAccessibleChildCount()
{
    ThrowIfDisposed ();

    long nChildCount = AccessibleDocumentViewBase::getAccessibleChildCount();

    // Forward request to children manager.
    if (mpChildrenManager != nullptr)
        nChildCount += mpChildrenManager->GetChildCount();

    return nChildCount;
}

uno::Reference<XAccessible> SAL_CALL
    AccessibleDrawDocumentView::getAccessibleChild (sal_Int32 nIndex)
{
    ThrowIfDisposed ();

    ::osl::ClearableMutexGuard aGuard (maMutex);

    // Take care of children of the base class.
    sal_Int32 nCount = AccessibleDocumentViewBase::getAccessibleChildCount();
    if (nCount > 0)
    {
        if (nIndex < nCount)
            return AccessibleDocumentViewBase::getAccessibleChild(nIndex);
        else
            nIndex -= nCount;
    }

    // Create a copy of the pointer to the children manager and release the
    // mutex before calling any of its methods.
    ChildrenManager* pChildrenManager = mpChildrenManager.get();
    aGuard.clear();

    // Forward request to children manager.
    if (pChildrenManager == nullptr)
        throw lang::IndexOutOfBoundsException (
            "no accessible child with index " + OUString::number(nIndex),
            static_cast<uno::XWeak*>(this));

    return pChildrenManager->GetChild (nIndex);
}

OUString SAL_CALL
    AccessibleDrawDocumentView::getAccessibleName()
{
    SolarMutexGuard g;

    OUString sName = SdResId(SID_SD_A11Y_D_PRESENTATION);
    ::sd::View* pSdView = static_cast< ::sd::View* >( maShapeTreeInfo.GetSdrView() );
    if ( pSdView )
    {
        SdDrawDocument& rDoc = pSdView->GetDoc();
        OUString sFileName = rDoc.getDocAccTitle();
        if ( !sFileName.getLength() )
        {
            ::sd::DrawDocShell* pDocSh = pSdView->GetDocSh();
            if ( pDocSh )
            {
                sFileName = pDocSh->GetTitle( SFX_TITLE_APINAME );
            }
        }

        OUString sReadOnly;
        if(rDoc.getDocReadOnly())
        {
            sReadOnly = SdResId(SID_SD_A11Y_D_PRESENTATION_READONLY);
        }

        if ( sFileName.getLength() )
        {
            sName = sFileName + sReadOnly + " - " + sName;
        }
    }

    return sName;
}

//=====  XEventListener  ======================================================

void SAL_CALL
    AccessibleDrawDocumentView::disposing (const lang::EventObject& rEventObject)
{
    ThrowIfDisposed ();

    AccessibleDocumentViewBase::disposing (rEventObject);
    if (rEventObject.Source == mxModel)
    {
        ::osl::Guard< ::osl::Mutex> aGuard (::osl::Mutex::getGlobalMutex());
        // maShapeTreeInfo has been modified in base class.
        if (mpChildrenManager != nullptr)
            mpChildrenManager->SetInfo (maShapeTreeInfo);
    }
}

//=====  XPropertyChangeListener  =============================================

void SAL_CALL
    AccessibleDrawDocumentView::propertyChange (const beans::PropertyChangeEvent& rEventObject)
{
    ThrowIfDisposed ();

    AccessibleDocumentViewBase::propertyChange (rEventObject);

    // add page switch event for slide show mode
    if (rEventObject.PropertyName == "CurrentPage" ||
        rEventObject.PropertyName == "PageChange")
    {
        // Update the accessible name to reflect the current slide.
        UpdateAccessibleName();

        // The current page changed.  Update the children manager accordingly.
        uno::Reference<drawing::XDrawView> xView (mxController, uno::UNO_QUERY);
        if (xView.is() && mpChildrenManager!=nullptr)
        {
            // Inform the children manager to forget all children and give
            // him the new ones.
            mpChildrenManager->ClearAccessibleShapeList ();
            mpChildrenManager->SetShapeList (uno::Reference<drawing::XShapes> (
                xView->getCurrentPage(), uno::UNO_QUERY));

            rtl::Reference<AccessiblePageShape> xPage(CreateDrawPageShape ());
            if (xPage.is())
            {
                xPage->Init();
                mpChildrenManager->AddAccessibleShape (xPage.get());
                mpChildrenManager->Update (false);
            }
        }
        else
            SAL_WARN("sd", "View invalid");
        CommitChange(AccessibleEventId::PAGE_CHANGED,rEventObject.NewValue,rEventObject.OldValue);
    }
    else if ( rEventObject.PropertyName == "VisibleArea" )
    {
        if (mpChildrenManager != nullptr)
            mpChildrenManager->ViewForwarderChanged();
    }
    else if (rEventObject.PropertyName == "ActiveLayer")
    {
        CommitChange(AccessibleEventId::PAGE_CHANGED,rEventObject.NewValue,rEventObject.OldValue);
    }
    else if (rEventObject.PropertyName == "UpdateAcc")
    {
        // The current page changed.  Update the children manager accordingly.
        uno::Reference<drawing::XDrawView> xView (mxController, uno::UNO_QUERY);
        if (xView.is() && mpChildrenManager!=nullptr)
        {
            // Inform the children manager to forget all children and give
            // him the new ones.
            mpChildrenManager->ClearAccessibleShapeList ();
            // update the slide show page's accessible info
            //mpChildrenManager->SetShapeList (uno::Reference<drawing::XShapes> (
            //    xView->getCurrentPage(), uno::UNO_QUERY));
            rtl::Reference< sd::SlideShow > xSlideshow( sd::SlideShow::GetSlideShow( mpSdViewSh->GetViewShellBase() ) );
            if( xSlideshow.is() && xSlideshow->isRunning() && xSlideshow->isFullScreen() )
            {
                css::uno::Reference< drawing::XDrawPage > xSlide;
                // MT IA2: Not used...
                // sal_Int32 currentPageIndex = xSlideshow->getCurrentPageIndex();
                css::uno::Reference< css::presentation::XSlideShowController > xSlideController = xSlideshow->getController();
                if( xSlideController.is() )
                {
                    xSlide = xSlideController->getCurrentSlide();
                    if (xSlide.is())
                    {
                        mpChildrenManager->SetShapeList (uno::Reference<drawing::XShapes> (
                                    xSlide, uno::UNO_QUERY));
                    }
                }
            }
            rtl::Reference<AccessiblePageShape> xPage(CreateDrawPageShape ());
            if (xPage.is())
            {
                xPage->Init();
                mpChildrenManager->AddAccessibleShape (xPage.get());
                mpChildrenManager->Update (false);
            }
        }
    }
    else
    {
        SAL_INFO("sd", "unhandled");
    }
}

// XServiceInfo

OUString SAL_CALL
    AccessibleDrawDocumentView::getImplementationName()
{
    return OUString("AccessibleDrawDocumentView");
}

css::uno::Sequence< OUString> SAL_CALL
    AccessibleDrawDocumentView::getSupportedServiceNames()
{
    ThrowIfDisposed();
    // Get list of supported service names from base class...
    uno::Sequence<OUString> aServiceNames =
        AccessibleDocumentViewBase::getSupportedServiceNames();
    sal_Int32 nCount (aServiceNames.getLength());

    // ...and add additional names.
    aServiceNames.realloc (nCount + 1);
    aServiceNames[nCount] = "com.sun.star.drawing.AccessibleDrawDocumentView";

    return aServiceNames;
}

//=====  XInterface  ==========================================================

uno::Any SAL_CALL
    AccessibleDrawDocumentView::queryInterface (const uno::Type & rType)
{
    uno::Any aReturn = AccessibleDocumentViewBase::queryInterface (rType);
    if ( ! aReturn.hasValue())
        aReturn = ::cppu::queryInterface (rType,
            static_cast<XAccessibleGroupPosition*>(this)
            );
    return aReturn;
}

void SAL_CALL
    AccessibleDrawDocumentView::acquire()
    throw ()
{
    AccessibleDocumentViewBase::acquire ();
}
void SAL_CALL
    AccessibleDrawDocumentView::release()
    throw ()
{
    AccessibleDocumentViewBase::release ();
}
//=====  XAccessibleGroupPosition  =========================================
uno::Sequence< sal_Int32 > SAL_CALL
    AccessibleDrawDocumentView::getGroupPosition( const uno::Any& rAny )
{
    SolarMutexGuard g;

    // we will return the:
    // [0] group level(always be 0 now)
    // [1] similar items counts in the group
    // [2] the position of the object in the group
    uno::Sequence< sal_Int32 > aRet( 3 );
    //get the xShape of the current selected drawing object
    uno::Reference<XAccessibleContext> xAccContent;
    rAny >>= xAccContent;
    if ( !xAccContent.is() )
    {
        return aRet;
    }
    AccessibleShape* pAcc = comphelper::getUnoTunnelImplementation<AccessibleShape>( xAccContent );
    if ( !pAcc )
    {
        return aRet;
    }
    uno::Reference< drawing::XShape > xCurShape = pAcc->GetXShape();
    if ( !xCurShape.is() )
    {
        return aRet;
    }
    //find all the child in the page, insert them into a vector and sort
    if ( mpChildrenManager == nullptr )
    {
        return aRet;
    }
    std::vector< uno::Reference<drawing::XShape> > vXShapes;
    sal_Int32 nCount = mpChildrenManager->GetChildCount();
    //get pointer of SdView & SdrPageView for further use.
    SdrPageView* pPV = nullptr;
    ::sd::View* pSdView = nullptr;
    if ( mpSdViewSh )
    {
        pSdView = mpSdViewSh->GetView();
        pPV = pSdView->GetSdrPageView();
    }
    for ( sal_Int32 i = 0; i < nCount; i++ )
    {
        uno::Reference< drawing::XShape > xShape = mpChildrenManager->GetChildShape(i);
        if ( xShape.is() )
        {
            //if the object is visible in the page, we add it into the group list.
            SdrObject* pObj = GetSdrObjectFromXShape(xShape);
            if ( pObj && pPV && pSdView && pSdView->IsObjMarkable( pObj, pPV ) )
            {
                vXShapes.push_back( xShape );
            }
        }
    }
    std::sort( vXShapes.begin(), vXShapes.end(), XShapePosCompareHelper() );
    //get the index of the selected object in the group
    auto aIter = std::find_if(vXShapes.begin(), vXShapes.end(),
        [&xCurShape](const uno::Reference<drawing::XShape>& rxShape) { return rxShape.get() == xCurShape.get(); });
    if (aIter != vXShapes.end())
    {
        sal_Int32* pArray = aRet.getArray();
        pArray[0] = 1; //it should be 1 based, not 0 based.
        pArray[1] = vXShapes.size();
        pArray[2] = static_cast<sal_Int32>(std::distance(vXShapes.begin(), aIter)) + 1; //we start counting position from 1
    }
    return aRet;
}

OUString AccessibleDrawDocumentView::getObjectLink( const uno::Any& rAny )
{
    SolarMutexGuard g;

    OUString aRet;
    //get the xShape of the current selected drawing object
    uno::Reference<XAccessibleContext> xAccContent;
    rAny >>= xAccContent;
    if ( !xAccContent.is() )
    {
        return aRet;
    }
    AccessibleShape* pAcc = comphelper::getUnoTunnelImplementation<AccessibleShape>( xAccContent );
    if ( !pAcc )
    {
        return aRet;
    }
    uno::Reference< drawing::XShape > xCurShape = pAcc->GetXShape();
    if ( !xCurShape.is() )
    {
        return aRet;
    }
    SdrObject* pObj = GetSdrObjectFromXShape(xCurShape);
    if (pObj)
    {
        SdAnimationInfo* pInfo = SdDrawDocument::GetShapeUserData(*pObj);
        if( pInfo && (pInfo->meClickAction == presentation::ClickAction_DOCUMENT) )
            aRet = pInfo->GetBookmark();
    }
    return aRet;
}

/// Create a name for this view.
OUString AccessibleDrawDocumentView::CreateAccessibleName()
{
    OUString sName;

    uno::Reference<lang::XServiceInfo> xInfo (mxController, uno::UNO_QUERY);
    if (xInfo.is())
    {
        uno::Sequence< OUString > aServices( xInfo->getSupportedServiceNames() );
        OUString sFirstService = aServices[0];
        if ( sFirstService == "com.sun.star.drawing.DrawingDocumentDrawView" )
        {
            if( aServices.getLength() >= 2 && aServices[1] == "com.sun.star.presentation.PresentationView")
            {
                SolarMutexGuard aGuard;

                sName = SdResId(SID_SD_A11Y_I_DRAWVIEW_N);
            }
            else
            {
                SolarMutexGuard aGuard;

                sName = SdResId(SID_SD_A11Y_D_DRAWVIEW_N);
            }
        }
        else if ( sFirstService == "com.sun.star.presentation.NotesView" )
        {
            SolarMutexGuard aGuard;

            sName = SdResId(SID_SD_A11Y_I_NOTESVIEW_N);
        }
        else if ( sFirstService == "com.sun.star.presentation.HandoutView" )
        {
            SolarMutexGuard aGuard;

            sName = SdResId(SID_SD_A11Y_I_HANDOUTVIEW_N);
        }
        else
        {
            sName = sFirstService;
        }
    }
    else
    {
        sName = "AccessibleDrawDocumentView";
    }
    return sName;
}

/** Return selection state of specified child
*/
bool
    AccessibleDrawDocumentView::implIsSelected( sal_Int32 nAccessibleChildIndex )
{
    const SolarMutexGuard aSolarGuard;
    uno::Reference< view::XSelectionSupplier >  xSel( mxController, uno::UNO_QUERY );
    bool                                    bRet = false;

    OSL_ENSURE( 0 <= nAccessibleChildIndex, "AccessibleDrawDocumentView::implIsSelected: invalid index!" );

    if( xSel.is() && ( 0 <= nAccessibleChildIndex ) )
    {
        uno::Any                            aAny( xSel->getSelection() );
        uno::Reference< drawing::XShapes >  xShapes;

        aAny >>= xShapes;

        if( xShapes.is() )
        {
            AccessibleShape* pAcc = comphelper::getUnoTunnelImplementation<AccessibleShape>( getAccessibleChild( nAccessibleChildIndex ) );

            if( pAcc )
            {
                uno::Reference< drawing::XShape > xShape( pAcc->GetXShape() );

                if( xShape.is() )
                {
                    for( sal_Int32 i = 0, nCount = xShapes->getCount(); ( i < nCount ) && !bRet; ++i )
                        if( xShapes->getByIndex( i ) == xShape )
                            bRet = true;
                }
            }
        }
    }

    return bRet;
}

/** Select or deselect the specified shapes.  The corresponding accessible
    shapes are notified over the selection change listeners registered with
    the XSelectionSupplier of the controller.
*/
void
    AccessibleDrawDocumentView::implSelect( sal_Int32 nAccessibleChildIndex, bool bSelect )
{
    const SolarMutexGuard aSolarGuard;
    uno::Reference< view::XSelectionSupplier >  xSel( mxController, uno::UNO_QUERY );

    if( !xSel.is() )
        return;

    uno::Any aAny;

    if( ACCESSIBLE_SELECTION_CHILD_ALL == nAccessibleChildIndex )
    {
        // Select or deselect all children.

        if( !bSelect )
            xSel->select( aAny );
        else
        {
            uno::Reference< drawing::XShapes > xShapes = drawing::ShapeCollection::create(
                    comphelper::getProcessComponentContext());

            for(sal_Int32 i = 0, nCount = getAccessibleChildCount(); i < nCount; ++i )
            {
                AccessibleShape* pAcc = comphelper::getUnoTunnelImplementation<AccessibleShape>( getAccessibleChild( i ) );

                if( pAcc && pAcc->GetXShape().is() )
                    xShapes->add( pAcc->GetXShape() );
            }

            if( xShapes->getCount() )
            {
                xSel->select( Any(xShapes) );
            }
        }
    }
    else if( nAccessibleChildIndex >= 0 )
    {
        // Select or deselect only the child with index
        // nAccessibleChildIndex.

        AccessibleShape* pAcc = comphelper::getUnoTunnelImplementation<AccessibleShape>(
            getAccessibleChild( nAccessibleChildIndex ));

        // Add or remove the shape that is made accessible from the
        // selection of the controller.
        if( pAcc )
        {
            uno::Reference< drawing::XShape > xShape( pAcc->GetXShape() );

            if( xShape.is() )
            {
                uno::Reference< drawing::XShapes >  xShapes;
                bool                            bFound = false;

                aAny = xSel->getSelection();
                aAny >>= xShapes;

                // Search shape to be selected in current selection.
                if (xShapes.is())
                {
                    sal_Int32 nCount = xShapes->getCount();
                    for (sal_Int32 i=0; ( i < nCount ) && !bFound; ++i )
                        if( xShapes->getByIndex( i ) == xShape )
                            bFound = true;
                }
                else
                    // Create an empty selection to add the shape to.
                    xShapes = drawing::ShapeCollection::create(
                            comphelper::getProcessComponentContext());

                // Update the selection.
                if( !bFound && bSelect )
                    xShapes->add( xShape );
                else if( bFound && !bSelect )
                    xShapes->remove( xShape );

                xSel->select( Any(xShapes) );
            }
        }
    }
}

void AccessibleDrawDocumentView::Activated()
{
    if (mpChildrenManager == nullptr)
        return;

    bool bChange = false;
    // When none of the children has the focus then claim it for the
    // view.
    if ( ! mpChildrenManager->HasFocus())
    {
        SetState (AccessibleStateType::FOCUSED);
        bChange = true;
    }
    else
        ResetState (AccessibleStateType::FOCUSED);
    mpChildrenManager->UpdateSelection();
    // if the child gets focus in UpdateSelection(), needs to reset the focus on document.
    if (mpChildrenManager->HasFocus() && bChange)
        ResetState (AccessibleStateType::FOCUSED);
}

void AccessibleDrawDocumentView::Deactivated()
{
    if (mpChildrenManager != nullptr)
        mpChildrenManager->RemoveFocus();
    ResetState (AccessibleStateType::FOCUSED);
}

void AccessibleDrawDocumentView::impl_dispose()
{
    mpChildrenManager.reset();
    AccessibleDocumentViewBase::impl_dispose();
}

/** This method is called from the component helper base class while
    disposing.
*/
void SAL_CALL AccessibleDrawDocumentView::disposing()
{
    // Release resources.
    mpChildrenManager.reset();

    // Forward call to base classes.
    AccessibleDocumentViewBase::disposing ();
}

css::uno::Sequence< css::uno::Any >
        SAL_CALL AccessibleDrawDocumentView::getAccFlowTo(const css::uno::Any& rAny, sal_Int32 nType)
{
    SolarMutexGuard g;

    const sal_Int32 SPELLCHECKFLOWTO = 1;
    const sal_Int32 FINDREPLACEFLOWTO = 2;
    if ( nType == SPELLCHECKFLOWTO )
    {
        uno::Reference< css::drawing::XShape > xShape;
        rAny >>= xShape;
        if ( mpChildrenManager && xShape.is() )
        {
            uno::Reference < XAccessible > xAcc = mpChildrenManager->GetChild(xShape);
            uno::Reference < XAccessibleSelection > xAccSelection( xAcc, uno::UNO_QUERY );
            if ( xAccSelection.is() )
            {
                if ( xAccSelection->getSelectedAccessibleChildCount() )
                {
                    uno::Reference < XAccessible > xSel = xAccSelection->getSelectedAccessibleChild( 0 );
                    if ( xSel.is() )
                    {
                        uno::Reference < XAccessibleContext > xSelContext( xSel->getAccessibleContext() );
                        if ( xSelContext.is() )
                        {
                            //if in sw we find the selected paragraph here
                            if ( xSelContext->getAccessibleRole() == AccessibleRole::PARAGRAPH )
                            {
                                uno::Sequence<uno::Any> aRet( 1 );
                                aRet[0] <<= xSel;
                                return aRet;
                            }
                        }
                    }
                }
            }
            uno::Reference<XAccessible> xPara = GetSelAccContextInTable();
            if ( xPara.is() )
            {
                uno::Sequence<uno::Any> aRet( 1 );
                aRet[0] <<= xPara;
                return aRet;
            }
        }
        else
        {
            goto Rt;
        }
    }
    else if ( nType == FINDREPLACEFLOWTO )
    {
        sal_Int32 nChildCount = getSelectedAccessibleChildCount();
        if ( nChildCount )
        {
            uno::Reference < XAccessible > xSel = getSelectedAccessibleChild( 0 );
            if ( xSel.is() )
            {
                uno::Reference < XAccessibleSelection > xAccChildSelection( xSel, uno::UNO_QUERY );
                if ( xAccChildSelection.is() )
                {
                    if ( xAccChildSelection->getSelectedAccessibleChildCount() )
                    {
                        uno::Reference < XAccessible > xChildSel = xAccChildSelection->getSelectedAccessibleChild( 0 );
                        if ( xChildSel.is() )
                        {
                            uno::Reference < XAccessibleContext > xChildSelContext( xChildSel->getAccessibleContext() );
                            if ( xChildSelContext.is() &&
                                xChildSelContext->getAccessibleRole() == AccessibleRole::PARAGRAPH )
                            {
                                uno::Sequence<uno::Any> aRet( 1 );
                                aRet[0] <<= xChildSel;
                                return aRet;
                            }
                        }
                    }
                }
            }
        }
        else
        {
            uno::Reference<XAccessible> xPara = GetSelAccContextInTable();
            if ( xPara.is() )
            {
                uno::Sequence<uno::Any> aRet( 1 );
                aRet[0] <<= xPara;
                return aRet;
            }
        }
    }

Rt:
    css::uno::Sequence< uno::Any> aRet;
    return aRet;
}
uno::Reference<XAccessible> AccessibleDrawDocumentView::GetSelAccContextInTable()
{
    uno::Reference<XAccessible> xRet;
    sal_Int32 nCount = mpChildrenManager ? mpChildrenManager->GetChildCount() : 0;
    if ( nCount )
    {
        for ( sal_Int32 i = 0; i < nCount; i++ )
        {
            try
            {
                uno::Reference<XAccessible> xObj = mpChildrenManager->GetChild(i);
                if ( xObj.is() )
                {
                    uno::Reference<XAccessibleContext> xObjContext( xObj, uno::UNO_QUERY );
                    if ( xObjContext.is() && xObjContext->getAccessibleRole() == AccessibleRole::TABLE )
                    {
                        uno::Reference<XAccessibleSelection> xObjSelection( xObj, uno::UNO_QUERY );
                        if ( xObjSelection.is() && xObjSelection->getSelectedAccessibleChildCount() )
                        {
                            uno::Reference<XAccessible> xCell = xObjSelection->getSelectedAccessibleChild(0);
                            if ( xCell.is() )
                            {
                                uno::Reference<XAccessibleSelection> xCellSel( xCell, uno::UNO_QUERY );
                                if ( xCellSel.is() && xCellSel->getSelectedAccessibleChildCount() )
                                {
                                    uno::Reference<XAccessible> xPara = xCellSel->getSelectedAccessibleChild( 0 );
                                    if ( xPara.is() )
                                    {
                                        uno::Reference<XAccessibleContext> xParaContext( xPara, uno::UNO_QUERY );
                                        if ( xParaContext.is() &&
                                            xParaContext->getAccessibleRole() == AccessibleRole::PARAGRAPH )
                                        {
                                            xRet = xPara;
                                            return xRet;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            catch (const lang::IndexOutOfBoundsException&)
            {
                uno::Reference<XAccessible> xEmpty;
                return xEmpty;
            }
            catch (const uno::RuntimeException&)
            {
                uno::Reference<XAccessible> xEmpty;
                return xEmpty;
            }
        }
    }

    return xRet;
}

void AccessibleDrawDocumentView::UpdateAccessibleName()
{
    OUString sNewName (CreateAccessibleName());
    sNewName += ": ";

    // Add the number of the current slide.
    uno::Reference<drawing::XDrawView> xView (mxController, uno::UNO_QUERY);
    if (xView.is())
    {
        uno::Reference<beans::XPropertySet> xProperties (xView->getCurrentPage(), UNO_QUERY);
        if (xProperties.is())
            try
            {
                sal_Int16 nPageNumber (0);
                if (xProperties->getPropertyValue("Number") >>= nPageNumber)
                {
                    sNewName += OUString::number(nPageNumber);
                }
            }
            catch (const beans::UnknownPropertyException&)
            {
            }
    }

    // Add the number of pages/slides.
    Reference<drawing::XDrawPagesSupplier> xPagesSupplier (mxModel, UNO_QUERY);
    if (xPagesSupplier.is())
    {
        Reference<container::XIndexAccess> xPages (xPagesSupplier->getDrawPages(), UNO_QUERY);
        if (xPages.is())
        {
            sNewName += " / ";
            sNewName += OUString::number(xPages->getCount());
        }
    }

    SetAccessibleName (sNewName, AutomaticallyCreated);
}

} // end of namespace accessibility

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
