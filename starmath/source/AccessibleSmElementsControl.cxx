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

#include <AccessibleSmElementsControl.hxx>
#include <AccessibleSmElement.hxx>
#include <ElementsDockingWindow.hxx>
#include <smmod.hxx>

#include <comphelper/accessiblewrapper.hxx>
#include <comphelper/processfactory.hxx>
#include <comphelper/types.hxx>
#include <com/sun/star/accessibility/AccessibleEventId.hpp>
#include <com/sun/star/accessibility/AccessibleRole.hpp>
#include <com/sun/star/accessibility/AccessibleStateType.hpp>
#include <com/sun/star/lang/IndexOutOfBoundsException.hpp>
#include <cppuhelper/supportsservice.hxx>
#include <cppuhelper/typeprovider.hxx>
#include <toolkit/helper/convert.hxx>
#include <unotools/accessiblerelationsethelper.hxx>
#include <unotools/accessiblestatesethelper.hxx>
#include <vcl/svapp.hxx>
#include <vcl/vclevent.hxx>

using namespace ::com::sun::star;
using namespace ::com::sun::star::accessibility;
using OContextEntryGuard = ::comphelper::OContextEntryGuard;
using OExternalLockGuard = ::comphelper::OExternalLockGuard;

// AccessibleSmElementsControl

AccessibleSmElementsControl::AccessibleSmElementsControl(SmElementsControl& rControl)
    : m_pControl(&rControl)
{
}

AccessibleSmElementsControl::~AccessibleSmElementsControl() {}

void AccessibleSmElementsControl::UpdateFocus(sal_uInt16 nPos)
{
    const bool bSetFocus = (nPos == SAL_MAX_UINT16);

    // submit events only if the widget has the focus to avoid sending events due to mouse move
    if (!m_pControl || (bSetFocus && !m_pControl->HasFocus()))
        return;

    if (bSetFocus)
        nPos = m_pControl->itemHighlighted() - m_pControl->itemOffset();

    if (nPos < m_aAccessibleChildren.size())
    {
        const auto& rxChild = m_aAccessibleChildren[nPos];
        if (rxChild.is())
            rxChild->SetFocus(bSetFocus);
    }
}

void AccessibleSmElementsControl::ReleaseAllItems()
{
    if (m_aAccessibleChildren.empty())
        return;

    m_aAccessibleChildren.clear();

    // The original toolbox accessibility code uses individual NAME_CHANGED
    // events in a loop. We can't do this, because on each remove event the
    // list of known children is rebuild. But since we rely on the child
    // count of the SmElementsControl, we'll always have no or all items.
    // In the latter case this would automatically recreate all items!
    assert(m_pControl && m_pControl->itemCount() == 0);
    NotifyAccessibleEvent(AccessibleEventId::INVALIDATE_ALL_CHILDREN, uno::Any(), uno::Any());
}

void AccessibleSmElementsControl::AddAllItems()
{
    assert(m_pControl);
    if (!m_pControl)
        return;

    uno::Any aNewName(getAccessibleName());
    NotifyAccessibleEvent(AccessibleEventId::NAME_CHANGED, uno::Any(), aNewName);

    // register the new items
    sal_uInt16 nCount = getAccessibleChildCount();
    for (sal_uInt16 i = 0; i < nCount; ++i)
    {
        uno::Any aNewValue;
        aNewValue <<= getAccessibleChild(static_cast<sal_Int32>(i));
        NotifyAccessibleEvent(AccessibleEventId::CHILD, uno::Any(), aNewValue);
    }
}

IMPLEMENT_FORWARD_XINTERFACE2(AccessibleSmElementsControl, comphelper::OAccessibleComponentHelper,
                              AccessibleSmElementsControl_BASE)

IMPLEMENT_FORWARD_XTYPEPROVIDER2(AccessibleSmElementsControl,
                                 comphelper::OAccessibleComponentHelper,
                                 AccessibleSmElementsControl_BASE)

// XAccessible
uno::Reference<XAccessibleContext> AccessibleSmElementsControl::getAccessibleContext()
{
    return this;
}

// XComponent
void AccessibleSmElementsControl::disposing()
{
    comphelper::OAccessibleComponentHelper::disposing();
    m_aAccessibleChildren.clear();
}

void AccessibleSmElementsControl::grabFocus()
{
    SolarMutexGuard aGuard;
    if (!m_pControl)
        throw uno::RuntimeException();

    m_pControl->GrabFocus();
}

sal_Int32 AccessibleSmElementsControl::getForeground()
{
    SolarMutexGuard aGuard;

    if (!m_pControl)
        throw uno::RuntimeException();
    return static_cast<sal_Int32>(m_pControl->GetTextColor());
}

sal_Int32 AccessibleSmElementsControl::getBackground()
{
    SolarMutexGuard aGuard;

    if (!m_pControl)
        throw uno::RuntimeException();
    Color nCol = m_pControl->GetControlBackground();
    return static_cast<sal_Int32>(nCol);
}

// XServiceInfo
OUString AccessibleSmElementsControl::getImplementationName()
{
    return OUString("com.sun.star.comp.toolkit.AccessibleSmElementsControl");
}

sal_Bool AccessibleSmElementsControl::supportsService(const OUString& rServiceName)
{
    return cppu::supportsService(this, rServiceName);
}

uno::Sequence<OUString> AccessibleSmElementsControl::getSupportedServiceNames()
{
    return { "com.sun.star.accessibility.AccessibleContext",
             "com.sun.star.accessibility.AccessibleComponent",
             "com.sun.star.accessibility.AccessibleSelection",
             "com.sun.star.accessibility.AccessibleSmElementsControl" };
}

// XAccessibleContext
sal_Int32 AccessibleSmElementsControl::getAccessibleChildCount()
{
    comphelper::OExternalLockGuard aGuard(this);
    sal_Int32 nCount = 0;
    if (m_pControl)
    {
        nCount = m_pControl->itemCount();
        if (m_aAccessibleChildren.size() != sal_uInt16(nCount))
            m_aAccessibleChildren.resize(nCount);
        if (m_pControl->scrollbarAccessible().is())
            nCount++;
    }
    return nCount;
}

uno::Reference<XAccessible> AccessibleSmElementsControl::getAccessibleChild(sal_Int32 i)
{
    comphelper::OExternalLockGuard aGuard(this);

    if (i < 0 || i >= getAccessibleChildCount())
        throw lang::IndexOutOfBoundsException();

    // first child may be the scrollbar
    sal_uInt16 c(i);
    uno::Reference<XAccessible> xScrollbar = m_pControl->scrollbarAccessible();
    if (xScrollbar.is())
    {
        if (c == 0)
            return xScrollbar;
        c--;
    }

    assert(c < m_aAccessibleChildren.size());
    rtl::Reference<AccessibleSmElement> xChild = m_aAccessibleChildren[c];
    const sal_uInt16 nItemId = m_pControl->itemOffset() + c;
    if (xChild.is() && xChild->itemId() != nItemId)
        xChild.clear();
    if (!xChild.is())
    {
        sal_uInt16 nHighlightItemId = m_pControl->itemHighlighted();
        AccessibleSmElement* pChild = new AccessibleSmElement(m_pControl, nItemId, i);
        if (pChild->itemId() == nHighlightItemId)
            pChild->SetFocus(true);
        m_aAccessibleChildren[c] = pChild;
        xChild = pChild;
    }
    return xChild.get();
}

uno::Reference<XAccessible> AccessibleSmElementsControl::getAccessibleParent()
{
    SolarMutexGuard aGuard;
    if (!m_pControl)
        throw uno::RuntimeException();

    vcl::Window* pAccParent = m_pControl->GetAccessibleParentWindow();
    assert(pAccParent);
    return pAccParent ? pAccParent->GetAccessible() : uno::Reference<XAccessible>();
}

uno::Reference<XAccessible>
AccessibleSmElementsControl::getAccessibleAtPoint(const awt::Point& rPoint)
{
    comphelper::OExternalLockGuard aGuard(this);

    uno::Reference<XAccessible> xAccessible;
    if (m_pControl)
    {
        sal_uInt16 nPos = m_pControl->itemAtPos(VCLPoint(rPoint));
        nPos -= m_pControl->itemOffset();
        if (nPos <= m_aAccessibleChildren.size())
            xAccessible = getAccessibleChild(nPos);
    }
    return xAccessible;
}

sal_Int16 AccessibleSmElementsControl::getAccessibleRole() { return AccessibleRole::SCROLL_PANE; }

OUString AccessibleSmElementsControl::getAccessibleDescription() { return OUString(); }

OUString AccessibleSmElementsControl::getAccessibleName()
{
    SolarMutexGuard aGuard;
    OUString aName;
    if (m_pControl)
        aName = SmResId(m_pControl->elementSetId().getStr());
    return aName;
}

// XAccessibleSelection
void AccessibleSmElementsControl::selectAccessibleChild(sal_Int32 nChildIndex)
{
    OExternalLockGuard aGuard(this);

    if ((!m_pControl) || nChildIndex < 0
        || static_cast<size_t>(nChildIndex) >= m_aAccessibleChildren.size())
        throw lang::IndexOutOfBoundsException();

    m_pControl->setItemHighlighted(nChildIndex);
}

sal_Bool AccessibleSmElementsControl::isAccessibleChildSelected(sal_Int32 nChildIndex)
{
    OExternalLockGuard aGuard(this);
    if ((!m_pControl) || nChildIndex < 0
        || static_cast<size_t>(nChildIndex) >= m_aAccessibleChildren.size())
        throw lang::IndexOutOfBoundsException();

    return (m_pControl->itemHighlighted() == nChildIndex);
}

void AccessibleSmElementsControl::clearAccessibleSelection()
{
    OExternalLockGuard aGuard(this);
    if (m_pControl)
        m_pControl->setItemHighlighted(SAL_MAX_UINT16);
}

void AccessibleSmElementsControl::selectAllAccessibleChildren()
{
    // intentionally empty
}

sal_Int32 AccessibleSmElementsControl::getSelectedAccessibleChildCount()
{
    OExternalLockGuard aGuard(this);

    sal_Int32 nRet = 0;
    if (m_pControl
        && (m_pControl->itemHighlighted() - m_pControl->itemOffset()) < getAccessibleChildCount())
        nRet = 1;
    return nRet;
}

uno::Reference<XAccessible>
AccessibleSmElementsControl::getSelectedAccessibleChild(sal_Int32 nSelectedChildIndex)
{
    OExternalLockGuard aGuard(this);
    if (nSelectedChildIndex != 0 || !m_pControl)
        throw lang::IndexOutOfBoundsException();
    return getAccessibleChild(m_pControl->itemHighlighted() - m_pControl->itemOffset());
}

void AccessibleSmElementsControl::deselectAccessibleChild(sal_Int32 nChildIndex)
{
    OExternalLockGuard aGuard(this);
    if (nChildIndex != 0 || nChildIndex >= getAccessibleChildCount())
        throw lang::IndexOutOfBoundsException();
    clearAccessibleSelection(); // there can be just one selected child
}

// XAccessibleComponent
static awt::Point lcl_GetLocationOnScreen(vcl::Window const* m_pControl)
{
    awt::Point aPos;
    if (m_pControl)
    {
        tools::Rectangle aRect = m_pControl->GetWindowExtentsRelative(nullptr);
        aPos.X = aRect.Left();
        aPos.Y = aRect.Top();
    }
    return aPos;
}

static awt::Rectangle lcl_GetBounds(vcl::Window const* m_pControl)
{
    // !! see VCLXAccessibleComponent::implGetBounds()

    //! the coordinates returned are relative to the parent window !
    //! Thus the top-left point may be different from (0, 0) !

    awt::Rectangle aBounds;
    if (m_pControl)
    {
        tools::Rectangle aRect = m_pControl->GetWindowExtentsRelative(nullptr);
        aBounds.X = aRect.Left();
        aBounds.Y = aRect.Top();
        aBounds.Width = aRect.GetWidth();
        aBounds.Height = aRect.GetHeight();

        vcl::Window* pParent = m_pControl->GetAccessibleParentWindow();
        if (pParent)
        {
            tools::Rectangle aParentRect = pParent->GetWindowExtentsRelative(nullptr);
            awt::Point aParentScreenLoc(aParentRect.Left(), aParentRect.Top());
            aBounds.X -= aParentScreenLoc.X;
            aBounds.Y -= aParentScreenLoc.Y;
        }
    }
    return aBounds;
}

void AccessibleSmElementsControl::TestControl()
{
    if (!m_pControl)
        throw uno::RuntimeException();
    assert(m_pControl->GetParent()->GetAccessible() == getAccessibleParent());
}

awt::Rectangle AccessibleSmElementsControl::implGetBounds()
{
    SolarMutexGuard aGuard;
    TestControl();
    return lcl_GetBounds(m_pControl);
}

awt::Rectangle AccessibleSmElementsControl::getBounds() { return implGetBounds(); }

sal_Bool AccessibleSmElementsControl::containsPoint(const awt::Point& aPoint)
{
    SolarMutexGuard aGuard;
    TestControl();
    Size aSz(m_pControl->GetSizePixel());
    return aPoint.X >= 0 && aPoint.Y >= 0 && aPoint.X < aSz.Width() && aPoint.Y < aSz.Height();
}

awt::Point AccessibleSmElementsControl::getLocation()
{
    SolarMutexGuard aGuard;
    TestControl();
    awt::Rectangle aRect(lcl_GetBounds(m_pControl));
    return awt::Point(aRect.X, aRect.Y);
}

awt::Point AccessibleSmElementsControl::getLocationOnScreen()
{
    SolarMutexGuard aGuard;
    TestControl();
    return lcl_GetLocationOnScreen(m_pControl);
}

awt::Size AccessibleSmElementsControl::getSize()
{
    SolarMutexGuard aGuard;
    TestControl();

    Size aSz(m_pControl->GetSizePixel());
#if OSL_DEBUG_LEVEL > 0 && !defined NDEBUG
    awt::Rectangle aRect(lcl_GetBounds(m_pControl));
    Size aSz2(aRect.Width, aRect.Height);
    assert(aSz == aSz2 && "mismatch in width");
#endif
    return awt::Size(aSz.Width(), aSz.Height());
}

uno::Reference<XAccessibleRelationSet> AccessibleSmElementsControl::getAccessibleRelationSet()
{
    uno::Reference<XAccessibleRelationSet> xRelSet = new utl::AccessibleRelationSetHelper();
    return xRelSet; // empty relation set
}

uno::Reference<XAccessibleStateSet> AccessibleSmElementsControl::getAccessibleStateSet()
{
    SolarMutexGuard aGuard;
    ::utl::AccessibleStateSetHelper* pStateSet = new ::utl::AccessibleStateSetHelper;

    uno::Reference<XAccessibleStateSet> xStateSet(pStateSet);

    if (!m_pControl)
        pStateSet->AddState(AccessibleStateType::DEFUNC);
    else
    {
        pStateSet->AddState(AccessibleStateType::ENABLED);
        pStateSet->AddState(AccessibleStateType::FOCUSABLE);
        if (m_pControl->HasFocus())
            pStateSet->AddState(AccessibleStateType::FOCUSED);
        if (m_pControl->IsActive())
            pStateSet->AddState(AccessibleStateType::ACTIVE);
        if (m_pControl->IsVisible())
            pStateSet->AddState(AccessibleStateType::SHOWING);
        if (m_pControl->IsReallyVisible())
            pStateSet->AddState(AccessibleStateType::VISIBLE);
        if (COL_TRANSPARENT != m_pControl->GetBackground().GetColor())
            pStateSet->AddState(AccessibleStateType::OPAQUE);
    }

    return xStateSet;
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
