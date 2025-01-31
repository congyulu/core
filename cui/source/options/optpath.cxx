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

#include <svx/svxdlg.hxx>
#include <sfx2/filedlghelper.hxx>
#include <sfx2/app.hxx>
#include <svl/aeitem.hxx>
#include <vcl/svtabbx.hxx>
#include <vcl/treelistentry.hxx>
#include <vcl/fixed.hxx>
#include <tools/urlobj.hxx>
#include <vcl/svapp.hxx>
#include <unotools/defaultoptions.hxx>
#include <unotools/localfilehelper.hxx>
#include <unotools/pathoptions.hxx>
#include <unotools/moduleoptions.hxx>
#include <unotools/viewoptions.hxx>

#include <optpath.hxx>
#include <dialmgr.hxx>
#include <strings.hrc>
#include <comphelper/configuration.hxx>
#include <comphelper/processfactory.hxx>
#include <comphelper/string.hxx>
#include <com/sun/star/uno/Exception.hpp>
#include <com/sun/star/beans/XPropertySet.hpp>
#include <com/sun/star/beans/PropertyAttribute.hpp>
#include <com/sun/star/lang/XMultiServiceFactory.hpp>
#include <com/sun/star/ui/dialogs/ExecutableDialogResults.hpp>
#include <com/sun/star/ui/dialogs/XAsynchronousExecutableDialog.hpp>
#include <com/sun/star/ui/dialogs/FolderPicker.hpp>
#include <com/sun/star/ui/dialogs/FilePicker.hpp>
#include <com/sun/star/ui/dialogs/TemplateDescription.hpp>
#include <com/sun/star/util/thePathSettings.hpp>
#include <officecfg/Office/Common.hxx>
#include "optHeaderTabListbox.hxx"
#include <vcl/help.hxx>
#include <tools/diagnose_ex.h>
#include <sal/log.hxx>

using namespace css;
using namespace css::beans;
using namespace css::lang;
using namespace css::ui::dialogs;
using namespace css::uno;
using namespace svx;

// define ----------------------------------------------------------------

#define TAB_WIDTH_MIN   10
#define ITEMID_TYPE       1
#define ITEMID_USER_PATHS 2
#define ITEMID_INTERNAL_PATHS 3

#define POSTFIX_INTERNAL    "_internal"
#define POSTFIX_USER        "_user"
#define POSTFIX_WRITABLE    "_writable"
#define VAR_ONE             "%1"
#define IODLG_CONFIGNAME    "FilePicker_Save"

// struct OptPath_Impl ---------------------------------------------------

struct OptPath_Impl
{
    SvtDefaultOptions           m_aDefOpt;
    Image                       m_aLockImage;
    OUString                    m_sMultiPathDlg;
    Reference< css::util::XPathSettings >   m_xPathSettings;

    OptPath_Impl(const Image& rLockImage, const OUString& rMultiPathDlg)
        : m_aLockImage(rLockImage)
        , m_sMultiPathDlg(rMultiPathDlg)
    {
    }
};

// struct PathUserData_Impl ----------------------------------------------

struct PathUserData_Impl
{
    sal_uInt16      nRealId;
    SfxItemState    eState;
    OUString        sUserPath;
    OUString        sInternalPath;
    OUString        sWritablePath;

    explicit PathUserData_Impl( sal_uInt16 nId ) :
        nRealId( nId ), eState( SfxItemState::UNKNOWN ) {}
};

struct Handle2CfgNameMapping_Impl
{
    sal_uInt16      m_nHandle;
    const char* m_pCfgName;
};

static Handle2CfgNameMapping_Impl const Hdl2CfgMap_Impl[] =
{
    { SvtPathOptions::PATH_AUTOCORRECT, "AutoCorrect" },
    { SvtPathOptions::PATH_AUTOTEXT,    "AutoText" },
    { SvtPathOptions::PATH_BACKUP,      "Backup" },
    { SvtPathOptions::PATH_GALLERY,     "Gallery" },
    { SvtPathOptions::PATH_GRAPHIC,     "Graphic" },
    { SvtPathOptions::PATH_TEMP,        "Temp" },
    { SvtPathOptions::PATH_TEMPLATE,    "Template" },
    { SvtPathOptions::PATH_WORK,        "Work" },
    { SvtPathOptions::PATH_DICTIONARY,        "Dictionary" },
    { SvtPathOptions::PATH_CLASSIFICATION, "Classification" },
#if OSL_DEBUG_LEVEL > 1
    { SvtPathOptions::PATH_LINGUISTIC,        "Linguistic" },
#endif
    { USHRT_MAX, nullptr }
};

static OUString getCfgName_Impl( sal_uInt16 _nHandle )
{
    OUString sCfgName;
    sal_uInt16 nIndex = 0;
    while ( Hdl2CfgMap_Impl[ nIndex ].m_nHandle != USHRT_MAX )
    {
        if ( Hdl2CfgMap_Impl[ nIndex ].m_nHandle == _nHandle )
        {
            // config name found
            sCfgName = OUString::createFromAscii( Hdl2CfgMap_Impl[ nIndex ].m_pCfgName );
            break;
        }
        ++nIndex;
    }

    return sCfgName;
}

#define MULTIPATH_DELIMITER     ';'

static OUString Convert_Impl( const OUString& rValue )
{
    if (rValue.isEmpty())
        return OUString();

    sal_Int32 nPos = 0;
    OUStringBuffer aReturn;
    for (;;)
    {
        OUString aValue = rValue.getToken( 0, MULTIPATH_DELIMITER, nPos );
        INetURLObject aObj( aValue );
        if ( aObj.GetProtocol() == INetProtocol::File )
            aReturn.append(aObj.PathToFileName());
        if ( nPos < 0 )
            break;
        aReturn.append(MULTIPATH_DELIMITER);
    }

    return aReturn.makeStringAndClear();
}

// functions -------------------------------------------------------------

static bool IsMultiPath_Impl( const sal_uInt16 nIndex )
{
#if OSL_DEBUG_LEVEL > 1
    return ( SvtPathOptions::PATH_AUTOCORRECT == nIndex ||
             SvtPathOptions::PATH_AUTOTEXT == nIndex ||
             SvtPathOptions::PATH_BASIC == nIndex ||
             SvtPathOptions::PATH_GALLERY == nIndex ||
             SvtPathOptions::PATH_TEMPLATE == nIndex );
#else
    return ( SvtPathOptions::PATH_AUTOCORRECT == nIndex ||
             SvtPathOptions::PATH_AUTOTEXT == nIndex ||
             SvtPathOptions::PATH_BASIC == nIndex ||
             SvtPathOptions::PATH_GALLERY == nIndex ||
             SvtPathOptions::PATH_TEMPLATE == nIndex ||
             SvtPathOptions::PATH_LINGUISTIC == nIndex ||
             SvtPathOptions::PATH_DICTIONARY == nIndex  );
#endif
}

// class SvxPathTabPage --------------------------------------------------

SvxPathTabPage::SvxPathTabPage(vcl::Window* pParent, const SfxItemSet& rSet)
    : SfxTabPage( pParent, "OptPathsPage", "cui/ui/optpathspage.ui", &rSet)
    , pImpl( new OptPath_Impl(get<FixedImage>("lock")->GetImage(),
        get<FixedText>("editpaths")->GetText()) )
    , xDialogListener ( new ::svt::DialogClosedListener() )
{
    get(m_pStandardBtn, "default");
    get(m_pPathBtn, "edit");
    get(m_pPathCtrl, "paths");

    m_pStandardBtn->SetClickHdl(LINK(this, SvxPathTabPage, StandardHdl_Impl));
    m_pPathBtn->SetClickHdl( LINK( this, SvxPathTabPage, PathHdl_Impl ) );

    Size aControlSize(236 , 147);
    aControlSize = LogicToPixel(aControlSize, MapMode(MapUnit::MapAppFont));
    m_pPathCtrl->set_width_request(aControlSize.Width());
    m_pPathCtrl->set_height_request(aControlSize.Height());
    WinBits nBits = WB_SORT | WB_HSCROLL | WB_CLIPCHILDREN | WB_TABSTOP;
    pPathBox = VclPtr<svx::OptHeaderTabListBox>::Create( *m_pPathCtrl, nBits );

    HeaderBar &rBar = pPathBox->GetTheHeaderBar();
    rBar.SetSelectHdl( LINK( this, SvxPathTabPage, HeaderSelect_Impl ) );
    rBar.SetEndDragHdl( LINK( this, SvxPathTabPage, HeaderEndDrag_Impl ) );

    rBar.InsertItem( ITEMID_TYPE, get<FixedText>("type")->GetText(),
                            0,
                            HeaderBarItemBits::LEFT | HeaderBarItemBits::CLICKABLE | HeaderBarItemBits::UPARROW );
    rBar.InsertItem( ITEMID_USER_PATHS, get<FixedText>("user_paths")->GetText(),
                            0,
                            HeaderBarItemBits::LEFT );
    rBar.InsertItem( ITEMID_INTERNAL_PATHS, get<FixedText>("internal_paths")->GetText(),
                            0,
                            HeaderBarItemBits::LEFT );

    long nWidth1 = rBar.GetTextWidth(rBar.GetItemText(ITEMID_TYPE));
    long nWidth2 = rBar.GetTextWidth(rBar.GetItemText(ITEMID_USER_PATHS));
    long nWidth3 = rBar.GetTextWidth(rBar.GetItemText(ITEMID_INTERNAL_PATHS));

    long aTabs[] = {0, 0, 0, 0};
    aTabs[1] = nWidth1 + 12;
    aTabs[2] = aTabs[1] + nWidth2 + 12;
    aTabs[3] = aTabs[2] + nWidth3 + 12;
    pPathBox->SetTabs(SAL_N_ELEMENTS(aTabs), aTabs, MapUnit::MapPixel);

    pPathBox->SetDoubleClickHdl( LINK( this, SvxPathTabPage, DoubleClickPathHdl_Impl ) );
    pPathBox->SetSelectHdl( LINK( this, SvxPathTabPage, PathSelect_Impl ) );
    pPathBox->SetSelectionMode( SelectionMode::Multiple );
    pPathBox->SetHighlightRange();

    xDialogListener->SetDialogClosedLink( LINK( this, SvxPathTabPage, DialogClosedHdl ) );
}


SvxPathTabPage::~SvxPathTabPage()
{
    disposeOnce();
}

void SvxPathTabPage::dispose()
{
    if ( pPathBox )
    {
        for ( sal_uLong i = 0; i < pPathBox->GetEntryCount(); ++i )
            delete static_cast<PathUserData_Impl*>(pPathBox->GetEntry(i)->GetUserData());
        pPathBox.disposeAndClear();
    }
    pImpl.reset();
    m_pPathCtrl.clear();
    m_pStandardBtn.clear();
    m_pPathBtn.clear();
    SfxTabPage::dispose();
}

VclPtr<SfxTabPage> SvxPathTabPage::Create( TabPageParent pParent,
                                           const SfxItemSet* rAttrSet )
{
    return VclPtr<SvxPathTabPage>::Create( pParent.pParent, *rAttrSet );
}

bool SvxPathTabPage::FillItemSet( SfxItemSet* )
{
    for ( sal_uLong i = 0; i < pPathBox->GetEntryCount(); ++i )
    {
        PathUserData_Impl* pPathImpl = static_cast<PathUserData_Impl*>(pPathBox->GetEntry(i)->GetUserData());
        sal_uInt16 nRealId = pPathImpl->nRealId;
        if ( pPathImpl->eState == SfxItemState::SET )
            SetPathList( nRealId, pPathImpl->sUserPath, pPathImpl->sWritablePath );
    }
    return true;
}

void SvxPathTabPage::Reset( const SfxItemSet* )
{
    pPathBox->Clear();

    HeaderBar &rBar = pPathBox->GetTheHeaderBar();
    long nWidth1 = rBar.GetTextWidth(rBar.GetItemText(1));
    long nWidth2 = rBar.GetTextWidth(rBar.GetItemText(2));
    long nWidth3 = rBar.GetTextWidth(rBar.GetItemText(3));

    for( sal_uInt16 i = 0; i <= sal_uInt16(SvtPathOptions::PATH_CLASSIFICATION); ++i )
    {
        // only writer uses autotext
        if ( i == SvtPathOptions::PATH_AUTOTEXT
            && !SvtModuleOptions().IsModuleInstalled( SvtModuleOptions::EModule::WRITER ) )
            continue;

        const char* pId = nullptr;

        switch (i)
        {
            case SvtPathOptions::PATH_AUTOCORRECT:
                pId = RID_SVXSTR_KEY_AUTOCORRECT_DIR;
                break;
            case SvtPathOptions::PATH_AUTOTEXT:
                pId = RID_SVXSTR_KEY_GLOSSARY_PATH;
                break;
            case SvtPathOptions::PATH_BACKUP:
                pId = RID_SVXSTR_KEY_BACKUP_PATH;
                break;
            case SvtPathOptions::PATH_GALLERY:
                pId = RID_SVXSTR_KEY_GALLERY_DIR;
                break;
            case SvtPathOptions::PATH_GRAPHIC:
                pId = RID_SVXSTR_KEY_GRAPHICS_PATH;
                break;
            case SvtPathOptions::PATH_TEMP:
                pId = RID_SVXSTR_KEY_TEMP_PATH;
                break;
            case SvtPathOptions::PATH_TEMPLATE:
                pId = RID_SVXSTR_KEY_TEMPLATE_PATH;
                break;
            case SvtPathOptions::PATH_DICTIONARY:
                pId = RID_SVXSTR_KEY_DICTIONARY_PATH;
                break;
            case SvtPathOptions::PATH_CLASSIFICATION:
                pId = RID_SVXSTR_KEY_CLASSIFICATION_PATH;
                break;
#if OSL_DEBUG_LEVEL > 1
            case SvtPathOptions::PATH_LINGUISTIC:
                pId = RID_SVXSTR_KEY_LINGUISTIC_DIR;
                break;
#endif
            case SvtPathOptions::PATH_WORK:
                pId = RID_SVXSTR_KEY_WORK_PATH;
                break;
        }

        if (pId)
        {
            OUString aStr(CuiResId(pId));

            nWidth1 = std::max(nWidth1, pPathBox->GetTextWidth(aStr));
            aStr += "\t";
            OUString sInternal, sUser, sWritable;
            bool bReadOnly = false;
            GetPathList( i, sInternal, sUser, sWritable, bReadOnly );
            OUString sTmpPath = sUser;
            if ( !sTmpPath.isEmpty() && !sWritable.isEmpty() )
                sTmpPath += OUStringLiteral1(MULTIPATH_DELIMITER);
            sTmpPath += sWritable;
            const OUString aValue = Convert_Impl( sTmpPath );
            nWidth2 = std::max(nWidth2, pPathBox->GetTextWidth(aValue));
            aStr += aValue;
            aStr += "\t";
            const OUString aValueInternal = Convert_Impl( sInternal );
            nWidth3 = std::max(nWidth3, pPathBox->GetTextWidth(aValueInternal));
            aStr += aValueInternal;
            SvTreeListEntry* pEntry = pPathBox->InsertEntry( aStr );
            if ( bReadOnly )
            {
                pPathBox->SetCollapsedEntryBmp( pEntry, pImpl->m_aLockImage );
            }
            PathUserData_Impl* pPathImpl = new PathUserData_Impl(i);
            pPathImpl->sUserPath = sUser;
            pPathImpl->sInternalPath = sInternal;
            pPathImpl->sWritablePath = sWritable;
            pEntry->SetUserData( pPathImpl );
        }
    }

    long aTabs[] = {0, 0, 0, 0};
    aTabs[1] = nWidth1 + 12;
    aTabs[2] = aTabs[1] + nWidth2 + 12;
    aTabs[3] = aTabs[2] + nWidth3 + 12;
    pPathBox->SetTabs(SAL_N_ELEMENTS(aTabs), aTabs, MapUnit::MapPixel);

    PathSelect_Impl( nullptr );
}

void SvxPathTabPage::FillUserData()
{
    HeaderBar &rBar = pPathBox->GetTheHeaderBar();

    OUString aUserData = OUString::number( rBar.GetItemSize( ITEMID_TYPE ) ) + ";";
    HeaderBarItemBits nBits = rBar.GetItemBits( ITEMID_TYPE );
    bool bUp = ( ( nBits & HeaderBarItemBits::UPARROW ) == HeaderBarItemBits::UPARROW );
    aUserData += bUp ? OUString("1") : OUString("0");
    SetUserData( aUserData );
}

IMPL_LINK_NOARG(SvxPathTabPage, PathSelect_Impl, SvTreeListBox*, void)
{
    sal_uInt16 nSelCount = 0;
    SvTreeListEntry* pEntry = pPathBox->FirstSelected();

    //the entry image indicates whether the path is write protected
    Image aEntryImage;
    if(pEntry)
        aEntryImage = SvTreeListBox::GetCollapsedEntryBmp( pEntry );
    bool bEnable = !aEntryImage;
    while ( pEntry && ( nSelCount < 2 ) )
    {
        nSelCount++;
        pEntry = pPathBox->NextSelected( pEntry );
    }

    m_pPathBtn->Enable( 1 == nSelCount && bEnable);
    m_pStandardBtn->Enable( nSelCount > 0 && bEnable);
}

IMPL_LINK_NOARG(SvxPathTabPage, StandardHdl_Impl, Button*, void)
{
    SvTreeListEntry* pEntry = pPathBox->FirstSelected();
    while ( pEntry )
    {
        PathUserData_Impl* pPathImpl = static_cast<PathUserData_Impl*>(pEntry->GetUserData());
        OUString aOldPath = pImpl->m_aDefOpt.GetDefaultPath( pPathImpl->nRealId );

        if ( !aOldPath.isEmpty() )
        {
            OUString sInternal, sUser, sWritable, sTemp;
            bool bReadOnly = false;
            GetPathList( pPathImpl->nRealId, sInternal, sUser, sWritable, bReadOnly );

            sal_Int32 nOldPos = 0;
            do
            {
                bool bFound = false;
                const OUString sOnePath = aOldPath.getToken( 0, MULTIPATH_DELIMITER, nOldPos );
                if ( !sInternal.isEmpty() )
                {
                    sal_Int32 nInternalPos = 0;
                    do
                    {
                        if ( sInternal.getToken( 0, MULTIPATH_DELIMITER, nInternalPos ) == sOnePath )
                            bFound = true;
                    }
                    while ( !bFound && nInternalPos >= 0 );
                }
                if ( !bFound )
                {
                    if ( !sTemp.isEmpty() )
                        sTemp += OUStringLiteral1(MULTIPATH_DELIMITER);
                    sTemp += sOnePath;
                }
            }
            while ( nOldPos >= 0 );

            OUString sWritablePath;
            OUStringBuffer sUserPath;
            if ( !sTemp.isEmpty() )
            {
                sal_Int32 nNextPos = 0;
                for (;;)
                {
                    const OUString sToken = sTemp.getToken( 0, MULTIPATH_DELIMITER, nNextPos );
                    if ( nNextPos<0 )
                    {
                        // Last token need a different handling
                        sWritablePath = sToken;
                        break;
                    }
                    if ( !sUserPath.isEmpty() )
                        sUserPath.append(MULTIPATH_DELIMITER);
                    sUserPath.append(sToken);
                }
            }
            pPathBox->SetEntryText( Convert_Impl( sTemp ), pEntry, 1 );
            pPathImpl->eState = SfxItemState::SET;
            pPathImpl->sUserPath = sUserPath.makeStringAndClear();
            pPathImpl->sWritablePath = sWritablePath;
        }
        pEntry = pPathBox->NextSelected( pEntry );
    }
}


void SvxPathTabPage::ChangeCurrentEntry( const OUString& _rFolder )
{
    SvTreeListEntry* pEntry = pPathBox->GetCurEntry();
    if ( !pEntry )
    {
        SAL_WARN( "cui.options", "SvxPathTabPage::ChangeCurrentEntry(): no entry" );
        return;
    }

    OUString sInternal, sUser, sWritable;
    PathUserData_Impl* pPathImpl = static_cast<PathUserData_Impl*>(pEntry->GetUserData());
    bool bReadOnly = false;
    GetPathList( pPathImpl->nRealId, sInternal, sUser, sWritable, bReadOnly );
    sUser = pPathImpl->sUserPath;
    sWritable = pPathImpl->sWritablePath;

    // old path is a URL?
    INetURLObject aObj( sWritable );
    bool bURL = ( aObj.GetProtocol() != INetProtocol::NotValid );
    INetURLObject aNewObj( _rFolder );
    aNewObj.removeFinalSlash();

    // then the new path also a URL else system path
    OUString sNewPathStr = bURL ? _rFolder : aNewObj.getFSysPath( FSysStyle::Detect );

    bool bChanged =
#ifdef UNX
// Unix is case sensitive
        ( sNewPathStr != sWritable );
#else
        !sNewPathStr.equalsIgnoreAsciiCase( sWritable );
#endif

    if ( bChanged )
    {
        pPathBox->SetEntryText( Convert_Impl( sNewPathStr ), pEntry, 1 );
        sal_uInt16 nPos = static_cast<sal_uInt16>(pPathBox->GetModel()->GetAbsPos( pEntry ));
        pPathImpl = static_cast<PathUserData_Impl*>(pPathBox->GetEntry(nPos)->GetUserData());
        pPathImpl->eState = SfxItemState::SET;
        pPathImpl->sWritablePath = sNewPathStr;
        if ( SvtPathOptions::PATH_WORK == pPathImpl->nRealId )
        {
            // Remove view options entry so the new work path
            // will be used for the next open dialog.
            SvtViewOptions aDlgOpt( EViewType::Dialog, IODLG_CONFIGNAME );
            aDlgOpt.Delete();
            // Reset also last used dir in the sfx application instance
            SfxApplication *pSfxApp = SfxGetpApp();
            pSfxApp->ResetLastDir();

            // Set configuration flag to notify file picker that it's necessary
            // to take over the path provided.
            std::shared_ptr< comphelper::ConfigurationChanges > batch(
                comphelper::ConfigurationChanges::create());
            officecfg::Office::Common::Path::Info::WorkPathChanged::set(
                true, batch);
            batch->commit();
        }
    }
}


IMPL_LINK_NOARG(SvxPathTabPage, DoubleClickPathHdl_Impl, SvTreeListBox*, bool)
{
    PathHdl_Impl(nullptr);
    return false;
}

IMPL_LINK_NOARG(SvxPathTabPage, PathHdl_Impl, Button*, void)
{
    SvTreeListEntry* pEntry = pPathBox->GetCurEntry();
    sal_uInt16 nPos = ( pEntry != nullptr ) ? static_cast<PathUserData_Impl*>(pEntry->GetUserData())->nRealId : 0;
    OUString sInternal, sUser, sWritable;
    bool bPickFile = false;
    if ( pEntry )
    {
        PathUserData_Impl* pPathImpl = static_cast<PathUserData_Impl*>(pEntry->GetUserData());
        bool bReadOnly = false;
        GetPathList( pPathImpl->nRealId, sInternal, sUser, sWritable, bReadOnly );
        sUser = pPathImpl->sUserPath;
        sWritable = pPathImpl->sWritablePath;
        bPickFile = pPathImpl->nRealId == SvtPathOptions::PATH_CLASSIFICATION;
    }

    if(pEntry && !(!SvTreeListBox::GetCollapsedEntryBmp(pEntry)))
        return;

    if ( IsMultiPath_Impl( nPos ) )
    {
        SvxAbstractDialogFactory* pFact = SvxAbstractDialogFactory::Create();
        ScopedVclPtr<AbstractSvxMultiPathDialog> pMultiDlg(
            pFact->CreateSvxMultiPathDialog(GetDialogFrameWeld()));

        OUString sPath( sUser );
        if ( !sPath.isEmpty() )
            sPath += OUStringLiteral1(MULTIPATH_DELIMITER);
        sPath += sWritable;
        pMultiDlg->SetPath( sPath );

        const OUString sPathName = SvTabListBox::GetEntryText( pEntry, 0 );
        const OUString sNewTitle = pImpl->m_sMultiPathDlg.replaceFirst( VAR_ONE, sPathName );
        pMultiDlg->SetTitle( sNewTitle );

        if ( pMultiDlg->Execute() == RET_OK && pEntry )
        {
            sUser.clear();
            sWritable.clear();
            OUString sFullPath;
            OUString sNewPath = pMultiDlg->GetPath();
            if ( !sNewPath.isEmpty() )
            {
                sal_Int32 nNextPos = 0;
                for (;;)
                {
                    const OUString sToken(sNewPath.getToken( 0, MULTIPATH_DELIMITER, nNextPos ));
                    if ( nNextPos<0 )
                    {
                        // Last token need a different handling
                        sWritable = sToken;
                        break;
                    }
                    if ( !sUser.isEmpty() )
                        sUser += OUStringLiteral1(MULTIPATH_DELIMITER);
                    sUser += sToken;
                }
                sFullPath = sUser;
                if ( !sFullPath.isEmpty() )
                    sFullPath += OUStringLiteral1(MULTIPATH_DELIMITER);
                sFullPath += sWritable;
            }

            pPathBox->SetEntryText( Convert_Impl( sFullPath ), pEntry, 1 );
            // save modified flag
            PathUserData_Impl* pPathImpl = static_cast<PathUserData_Impl*>(pEntry->GetUserData());
            pPathImpl->eState = SfxItemState::SET;
            pPathImpl->sUserPath = sUser;
            pPathImpl->sWritablePath = sWritable;
        }
    }
    else if (pEntry && !bPickFile)
    {
        try
        {
            Reference < XComponentContext > xContext( ::comphelper::getProcessComponentContext() );
            xFolderPicker = FolderPicker::create(xContext);

            INetURLObject aURL( sWritable, INetProtocol::File );
            xFolderPicker->setDisplayDirectory( aURL.GetMainURL( INetURLObject::DecodeMechanism::NONE ) );

            Reference< XAsynchronousExecutableDialog > xAsyncDlg( xFolderPicker, UNO_QUERY );
            if ( xAsyncDlg.is() )
                xAsyncDlg->startExecuteModal( xDialogListener.get() );
            else
            {
                short nRet = xFolderPicker->execute();
                if (ExecutableDialogResults::OK != nRet)
                    return;

                OUString sFolder(xFolderPicker->getDirectory());
                ChangeCurrentEntry(sFolder);
            }
        }
        catch( Exception const & )
        {
            css::uno::Any ex( cppu::getCaughtException() );
            SAL_WARN( "cui.options", "SvxPathTabPage::PathHdl_Impl: exception from folder picker " << exceptionToString(ex) );
        }
    }
    else if (pEntry)
    {
        try
        {
            uno::Reference<uno::XComponentContext> xComponentContext(comphelper::getProcessComponentContext());
            uno::Reference<ui::dialogs::XFilePicker3> xFilePicker = ui::dialogs::FilePicker::createWithMode(xComponentContext, ui::dialogs::TemplateDescription::FILEOPEN_SIMPLE);
            xFilePicker->appendFilter(OUString(), "*.xml");
            if (xFilePicker->execute() == ui::dialogs::ExecutableDialogResults::OK)
            {
                uno::Sequence<OUString> aPathSeq(xFilePicker->getSelectedFiles());
                ChangeCurrentEntry(aPathSeq[0]);
            }
        }
        catch (const uno::Exception&)
        {
            DBG_UNHANDLED_EXCEPTION("cui.options", "exception from file picker");
        }
    }
}


IMPL_LINK( SvxPathTabPage, HeaderSelect_Impl, HeaderBar*, pBar, void )
{
    if (!pBar || pBar->GetCurItemId() != ITEMID_TYPE)
        return;

    HeaderBarItemBits nBits = pBar->GetItemBits(ITEMID_TYPE);
    bool bUp = ( ( nBits & HeaderBarItemBits::UPARROW ) == HeaderBarItemBits::UPARROW );
    SvSortMode eMode = SortAscending;

    if ( bUp )
    {
        nBits &= ~HeaderBarItemBits::UPARROW;
        nBits |= HeaderBarItemBits::DOWNARROW;
        eMode = SortDescending;
    }
    else
    {
        nBits &= ~HeaderBarItemBits::DOWNARROW;
        nBits |= HeaderBarItemBits::UPARROW;
    }
    pBar->SetItemBits( ITEMID_TYPE, nBits );
    SvTreeList* pModel = pPathBox->GetModel();
    pModel->SetSortMode( eMode );
    pModel->Resort();
}


IMPL_LINK( SvxPathTabPage, HeaderEndDrag_Impl, HeaderBar*, pBar, void )
{
    if (!pBar || !pBar->GetCurItemId())
        return;

    if ( !pBar->IsItemMode() )
    {
        Size aSz;
        sal_uInt16 nTabs = pBar->GetItemCount();
        long nTmpSz = 0;
        long nWidth = pBar->GetItemSize(ITEMID_TYPE);
        long nBarWidth = pBar->GetSizePixel().Width();

        if(nWidth < TAB_WIDTH_MIN)
            pBar->SetItemSize( ITEMID_TYPE, TAB_WIDTH_MIN);
        else if ( ( nBarWidth - nWidth ) < TAB_WIDTH_MIN )
            pBar->SetItemSize( ITEMID_TYPE, nBarWidth - TAB_WIDTH_MIN );

        for ( sal_uInt16 i = 1; i <= nTabs; ++i )
        {
            long _nWidth = pBar->GetItemSize(i);
            aSz.setWidth(  _nWidth + nTmpSz );
            nTmpSz += _nWidth;
            pPathBox->SetTab( i, PixelToLogic( aSz, MapMode(MapUnit::MapAppFont) ).Width() );
        }
    }
}

IMPL_LINK( SvxPathTabPage, DialogClosedHdl, DialogClosedEvent*, pEvt, void )
{
    if (RET_OK == pEvt->DialogResult)
    {
        assert(xFolderPicker.is() && "SvxPathTabPage::DialogClosedHdl(): no folder picker");
        OUString sURL = xFolderPicker->getDirectory();
        ChangeCurrentEntry( sURL );
    }
}

void SvxPathTabPage::GetPathList(
    sal_uInt16 _nPathHandle, OUString& _rInternalPath,
    OUString& _rUserPath, OUString& _rWritablePath, bool& _rReadOnly )
{
    OUString sCfgName = getCfgName_Impl( _nPathHandle );

    try
    {
        // load PathSettings service if necessary
        if ( !pImpl->m_xPathSettings.is() )
        {
            Reference< XComponentContext > xContext = comphelper::getProcessComponentContext();
            pImpl->m_xPathSettings = css::util::thePathSettings::get( xContext );
        }

        // load internal paths
        Any aAny = pImpl->m_xPathSettings->getPropertyValue(
            sCfgName + POSTFIX_INTERNAL);
        Sequence< OUString > aPathSeq;
        if ( aAny >>= aPathSeq )
        {
            long i, nCount = aPathSeq.getLength();
            const OUString* pPaths = aPathSeq.getConstArray();

            for ( i = 0; i < nCount; ++i )
            {
                if ( !_rInternalPath.isEmpty() )
                    _rInternalPath += ";";
                _rInternalPath += pPaths[i];
            }
        }
        // load user paths
        aAny = pImpl->m_xPathSettings->getPropertyValue(
            sCfgName + POSTFIX_USER);
        if ( aAny >>= aPathSeq )
        {
            long i, nCount = aPathSeq.getLength();
            const OUString* pPaths = aPathSeq.getConstArray();

            for ( i = 0; i < nCount; ++i )
            {
                if ( !_rUserPath.isEmpty() )
                    _rUserPath += ";";
                _rUserPath += pPaths[i];
            }
        }
        // then the writable path
        aAny = pImpl->m_xPathSettings->getPropertyValue(
            sCfgName + POSTFIX_WRITABLE);
        OUString sWritablePath;
        if ( aAny >>= sWritablePath )
            _rWritablePath = sWritablePath;

        // and the readonly flag
        Reference< XPropertySetInfo > xInfo = pImpl->m_xPathSettings->getPropertySetInfo();
        Property aProp = xInfo->getPropertyByName(sCfgName);
        _rReadOnly = ( ( aProp.Attributes & PropertyAttribute::READONLY ) == PropertyAttribute::READONLY );
    }
    catch( const Exception& )
    {
        OSL_FAIL( "SvxPathTabPage::GetPathList(): caught an exception!" );
    }
}


void SvxPathTabPage::SetPathList(
    sal_uInt16 _nPathHandle, const OUString& _rUserPath, const OUString& _rWritablePath )
{
    OUString sCfgName = getCfgName_Impl( _nPathHandle );

    try
    {
        // load PathSettings service if necessary
        if ( !pImpl->m_xPathSettings.is() )
        {
            Reference< XComponentContext > xContext = comphelper::getProcessComponentContext();
            pImpl->m_xPathSettings = css::util::thePathSettings::get( xContext );
        }

        // save user paths
        const sal_Int32 nCount = comphelper::string::getTokenCount(_rUserPath, MULTIPATH_DELIMITER);
        Sequence< OUString > aPathSeq( nCount );
        OUString* pArray = aPathSeq.getArray();
        sal_Int32 nPos = 0;
        for ( sal_Int32 i = 0; i < nCount; ++i )
            pArray[i] = _rUserPath.getToken( 0, MULTIPATH_DELIMITER, nPos );
        Any aValue( aPathSeq );
        pImpl->m_xPathSettings->setPropertyValue(
            sCfgName + POSTFIX_USER, aValue);

        // then the writable path
        aValue <<= _rWritablePath;
        pImpl->m_xPathSettings->setPropertyValue(
            sCfgName + POSTFIX_WRITABLE, aValue);
    }
    catch( const Exception& )
    {
        TOOLS_WARN_EXCEPTION("cui.options", "");
    }
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
