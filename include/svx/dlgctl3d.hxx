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

#ifndef INCLUDED_SVX_DLGCTL3D_HXX
#define INCLUDED_SVX_DLGCTL3D_HXX

#include <vcl/ctrl.hxx>
#include <vcl/scrbar.hxx>
#include <vcl/button.hxx>
#include <vcl/customweld.hxx>
#include <vcl/weld.hxx>
#include <svl/itemset.hxx>
#include <svx/svxdllapi.h>
#include <basegfx/vector/b3dvector.hxx>
#include <memory>

class FmFormModel;
class FmFormPage;
class E3dView;
class E3dObject;
class E3dScene;

enum class SvxPreviewObjectType { SPHERE, CUBE };

class SAL_WARN_UNUSED SVX_DLLPUBLIC Svx3DPreviewControl : public Control
{
protected:
    std::unique_ptr<FmFormModel> mpModel;
    FmFormPage*             mpFmPage;
    std::unique_ptr<E3dView> mp3DView;
    E3dScene*               mpScene;
    E3dObject*              mp3DObj;
    SvxPreviewObjectType    mnObjectType;

    void Construct();

public:
    Svx3DPreviewControl(vcl::Window* pParent, WinBits nStyle = 0);
    virtual ~Svx3DPreviewControl() override;
    virtual void dispose() override;

    virtual void Paint( vcl::RenderContext& rRenderContext, const tools::Rectangle& rRect ) override;
    virtual void MouseButtonDown( const MouseEvent& rMEvt ) override;
    virtual void Resize() override;
    virtual Size GetOptimalSize() const override;

    virtual void SetObjectType(SvxPreviewObjectType nType);
    SvxPreviewObjectType GetObjectType() const { return mnObjectType; }
    SfxItemSet const & Get3DAttributes() const;
    virtual void Set3DAttributes(const SfxItemSet& rAttr);
};

class SAL_WARN_UNUSED SVX_DLLPUBLIC PreviewControl3D : public weld::CustomWidgetController
{
protected:
    std::unique_ptr<FmFormModel> mpModel;
    FmFormPage*             mpFmPage;
    std::unique_ptr<E3dView> mp3DView;
    E3dScene*               mpScene;
    E3dObject*              mp3DObj;
    SvxPreviewObjectType    mnObjectType;

    void Construct();

public:
    PreviewControl3D();
    virtual void SetDrawingArea(weld::DrawingArea* pDrawingArea) override;
    virtual ~PreviewControl3D() override;

    virtual void Paint( vcl::RenderContext& rRenderContext, const tools::Rectangle& rRect ) override;
    virtual bool MouseButtonDown( const MouseEvent& rMEvt ) override;
    virtual void Resize() override;

    virtual void SetObjectType(SvxPreviewObjectType nType);
    SvxPreviewObjectType GetObjectType() const { return mnObjectType; }
    SfxItemSet const & Get3DAttributes() const;
    virtual void Set3DAttributes(const SfxItemSet& rAttr);
};


class SAL_WARN_UNUSED SVX_DLLPUBLIC Svx3DLightControl : public Svx3DPreviewControl
{
    // Callback for interactive changes
    Link<Svx3DLightControl*,void>  maChangeCallback;
    Link<Svx3DLightControl*,void>  maSelectionChangeCallback;

    // lights
    sal_uInt32                  maSelectedLight;

    // extra objects for light control
    E3dObject*                  mpExpansionObject;
    E3dObject*                  mpLampBottomObject;
    E3dObject*                  mpLampShaftObject;
    std::vector< E3dObject* >   maLightObjects;

    // 3d rotations of object
    double                      mfRotateX;
    double                      mfRotateY;
    double                      mfRotateZ;

    // interaction parameters
    Point                       maActionStartPoint;
    double                      mfSaveActionStartHor;
    double                      mfSaveActionStartVer;
    double                      mfSaveActionStartRotZ;

    bool                        mbMouseMoved : 1;
    bool                        mbGeometrySelected : 1;

    void Construct2();
    void ConstructLightObjects();
    void AdaptToSelectedLight();
    void TrySelection(Point aPosPixel);

public:
    Svx3DLightControl(vcl::Window* pParent, WinBits nStyle);

    virtual void Paint(vcl::RenderContext& rRenderContext, const tools::Rectangle& rRect) override;
    virtual void MouseButtonDown(const MouseEvent& rMEvt) override;
    virtual void Tracking( const TrackingEvent& rTEvt ) override;
    virtual void Resize() override;

    virtual void SetObjectType(SvxPreviewObjectType nType) override;

    // register user callback
    void SetChangeCallback(Link<Svx3DLightControl*,void> aNew) { maChangeCallback = aNew; }
    void SetSelectionChangeCallback(Link<Svx3DLightControl*,void> aNew) { maSelectionChangeCallback = aNew; }

    // selection checks
    bool IsSelectionValid();
    bool IsGeometrySelected() { return mbGeometrySelected; }

    // get/set position of selected lamp in polar coordinates, Hor:[0..360.0[ and Ver:[-90..90] degrees
    void GetPosition(double& rHor, double& rVer);
    void SetPosition(double fHor, double fVer);

    // get/set rotation of 3D object
    void SetRotation(double fRotX, double fRotY, double fRotZ);
    void GetRotation(double& rRotX, double& rRotY, double& rRotZ);

    void SelectLight(sal_uInt32 nLightNumber);
    virtual void Set3DAttributes(const SfxItemSet& rAttr) override;
    sal_uInt32 GetSelectedLight() { return maSelectedLight; }

    // light data access
    bool GetLightOnOff(sal_uInt32 nNum) const;
    Color GetLightColor(sal_uInt32 nNum) const;
    basegfx::B3DVector GetLightDirection(sal_uInt32 nNum) const;
};

class SAL_WARN_UNUSED SVX_DLLPUBLIC LightControl3D : public PreviewControl3D
{
    // Callback for interactive changes
    Link<LightControl3D*,void>  maChangeCallback;
    Link<LightControl3D*,void>  maSelectionChangeCallback;

    // lights
    sal_uInt32                  maSelectedLight;

    // extra objects for light control
    E3dObject*                  mpExpansionObject;
    E3dObject*                  mpLampBottomObject;
    E3dObject*                  mpLampShaftObject;
    std::vector< E3dObject* >   maLightObjects;

    // 3d rotations of object
    double                      mfRotateX;
    double                      mfRotateY;
    double                      mfRotateZ;

    // interaction parameters
    Point                       maActionStartPoint;
    double                      mfSaveActionStartHor;
    double                      mfSaveActionStartVer;
    double                      mfSaveActionStartRotZ;

    bool                        mbMouseMoved : 1;
    bool                        mbMouseCaptured : 1;
    bool                        mbGeometrySelected : 1;

    void Construct2();
    void ConstructLightObjects();
    void AdaptToSelectedLight();
    void TrySelection(Point aPosPixel);

public:
    LightControl3D();

    virtual void SetDrawingArea(weld::DrawingArea* pDrawingArea) override;
    virtual void Paint(vcl::RenderContext& rRenderContext, const tools::Rectangle& rRect) override;
    virtual tools::Rectangle GetFocusRect() override;
    virtual bool MouseButtonDown(const MouseEvent& rMEvt) override;
    virtual bool MouseMove( const MouseEvent& rMEvt ) override;
    virtual bool MouseButtonUp( const MouseEvent& rMEvt ) override;
    virtual void Resize() override;

    virtual void SetObjectType(SvxPreviewObjectType nType) override;

    // register user callback
    void SetChangeCallback(Link<LightControl3D*,void> aNew) { maChangeCallback = aNew; }
    void SetSelectionChangeCallback(Link<LightControl3D*,void> aNew) { maSelectionChangeCallback = aNew; }

    // selection checks
    bool IsSelectionValid();
    bool IsGeometrySelected() { return mbGeometrySelected; }

    // get/set position of selected lamp in polar coordinates, Hor:[0..360.0[ and Ver:[-90..90] degrees
    void GetPosition(double& rHor, double& rVer);
    void SetPosition(double fHor, double fVer);

    // get/set rotation of 3D object
    void SetRotation(double fRotX, double fRotY, double fRotZ);
    void GetRotation(double& rRotX, double& rRotY, double& rRotZ);

    void SelectLight(sal_uInt32 nLightNumber);
    virtual void Set3DAttributes(const SfxItemSet& rAttr) override;
    sal_uInt32 GetSelectedLight() { return maSelectedLight; }

    // light data access
    bool GetLightOnOff(sal_uInt32 nNum) const;
    Color GetLightColor(sal_uInt32 nNum) const;
    basegfx::B3DVector GetLightDirection(sal_uInt32 nNum) const;
};

class SAL_WARN_UNUSED SVX_DLLPUBLIC SvxLightCtl3D final : public Control
{
    // local controls
    VclPtr<Svx3DLightControl>  maLightControl;
    VclPtr<ScrollBar>          maHorScroller;
    VclPtr<ScrollBar>          maVerScroller;
    VclPtr<PushButton>         maSwitcher;

    // callback for interactive changes
    Link<SvxLightCtl3D*,void>  maUserSelectionChangeCallback;

public:
    SvxLightCtl3D(vcl::Window* pParent);
    virtual ~SvxLightCtl3D() override;
    virtual void dispose() override;

    // react to size changes
    virtual void Resize() override;
    void NewLayout();

    // check the selection for validity
    void CheckSelection();

    // bring further settings to the outside world
    Svx3DLightControl& GetSvx3DLightControl() { return *maLightControl; }

    // register user callback
    void SetUserSelectionChangeCallback(Link<SvxLightCtl3D*,void> aNew) { maUserSelectionChangeCallback = aNew; }

    virtual void KeyInput( const KeyEvent& rKEvt ) override;
    virtual void GetFocus() override;
    virtual void LoseFocus() override;

    virtual Size GetOptimalSize() const override;

private:

    DECL_LINK( InternalInteractiveChange, Svx3DLightControl*, void);
    DECL_LINK( InternalSelectionChange, Svx3DLightControl*, void);
    DECL_LINK( ScrollBarMove, ScrollBar*, void);
    DECL_LINK( ButtonPress, Button*, void);

    // initialize local parameters
    void Init();

    void move( double fDeltaHor, double fDeltaVer );
};

class SAL_WARN_UNUSED SVX_DLLPUBLIC LightCtl3D
{
    // local controls
    LightControl3D& mrLightControl;
    weld::Scale& mrHorScroller;
    weld::Scale& mrVerScroller;
    weld::Button& mrSwitcher;

    // callback for interactive changes
    Link<LightCtl3D*,void>  maUserInteractiveChangeCallback;
    Link<LightCtl3D*,void>  maUserSelectionChangeCallback;

public:
    LightCtl3D(LightControl3D& rLightControl, weld::Scale& rHori,
               weld::Scale& rVert, weld::Button& rButton);
    ~LightCtl3D();

    // check the selection for validity
    void CheckSelection();

    // bring further settings to the outside world
    LightControl3D& GetSvx3DLightControl() { return mrLightControl; }

    // register user callback
    void SetUserInteractiveChangeCallback(Link<LightCtl3D*,void> aNew) { maUserInteractiveChangeCallback = aNew; }
    void SetUserSelectionChangeCallback(Link<LightCtl3D*,void> aNew) { maUserSelectionChangeCallback = aNew; }

private:

    DECL_LINK(InternalInteractiveChange, LightControl3D*, void);
    DECL_LINK(InternalSelectionChange, LightControl3D*, void);
    DECL_LINK(ScrollBarMove, weld::Scale&, void);
    DECL_LINK(ButtonPress, weld::Button&, void);
    DECL_LINK(KeyInput, const KeyEvent&, bool);
    DECL_LINK(FocusIn, weld::Widget&, void);

    // initialize local parameters
    void Init();

    void move( double fDeltaHor, double fDeltaVer );
};


#endif // _SCH_DLGCTL3D_HXX

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
