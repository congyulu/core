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

#include "scripthandler.hxx"

#include <com/sun/star/frame/DispatchResultEvent.hpp>
#include <com/sun/star/frame/DispatchResultState.hpp>
#include <com/sun/star/frame/XController.hpp>
#include <com/sun/star/frame/XModel.hpp>

#include <com/sun/star/document/XEmbeddedScripts.hpp>
#include <com/sun/star/document/XScriptInvocationContext.hpp>

#include <com/sun/star/lang/WrappedTargetRuntimeException.hpp>
#include <com/sun/star/lang/XSingleServiceFactory.hpp>
#include <com/sun/star/script/provider/ScriptFrameworkErrorException.hpp>
#include <com/sun/star/script/provider/XScriptProviderSupplier.hpp>
#include <com/sun/star/script/provider/theMasterScriptProviderFactory.hpp>
#include <com/sun/star/script/provider/ScriptFrameworkErrorType.hpp>

#include <sfx2/objsh.hxx>
#include <sfx2/frame.hxx>
#include <sfx2/sfxdlg.hxx>
#include <vcl/abstdlg.hxx>
#include <tools/diagnose_ex.h>
#include <sal/log.hxx>

#include <comphelper/processfactory.hxx>
#include <cppuhelper/factory.hxx>
#include <cppuhelper/exc_hlp.hxx>
#include <cppuhelper/supportsservice.hxx>
#include <framework/documentundoguard.hxx>

#include <com/sun/star/uno/XComponentContext.hpp>
#include <com/sun/star/uri/XUriReference.hpp>
#include <com/sun/star/uri/UriReferenceFactory.hpp>

#include <memory>

using namespace ::com::sun::star;
using namespace ::com::sun::star::uno;
using namespace ::com::sun::star::frame;
using namespace ::com::sun::star::util;
using namespace ::com::sun::star::beans;
using namespace ::com::sun::star::lang;
using namespace ::com::sun::star::script;
using namespace ::com::sun::star::script::provider;
using namespace ::com::sun::star::document;

namespace scripting_protocolhandler
{

void SAL_CALL ScriptProtocolHandler::initialize(
    const css::uno::Sequence < css::uno::Any >& aArguments )
{
    if ( m_bInitialised )
    {
        return ;
    }

    // first argument contains a reference to the frame (may be empty or the desktop,
    // but usually it's a "real" frame)
    if ( aArguments.hasElements() && !( aArguments[ 0 ] >>= m_xFrame ) )
    {
        OUString temp = "ScriptProtocolHandler::initialize: could not extract reference to the frame";
        throw RuntimeException( temp );
    }

    ENSURE_OR_THROW( m_xContext.is(), "ScriptProtocolHandler::initialize: No Service Manager available" );
    m_bInitialised = true;
}

Reference< XDispatch > SAL_CALL ScriptProtocolHandler::queryDispatch(
    const URL& aURL, const OUString&, sal_Int32 )
{
    Reference< XDispatch > xDispatcher;
    // get scheme of url

    Reference< uri::XUriReferenceFactory > xFac = uri::UriReferenceFactory::create( m_xContext );
    Reference<  uri::XUriReference > uriRef(
        xFac->parse( aURL.Complete ), UNO_QUERY );
    if ( uriRef.is() )
    {
        if ( uriRef->getScheme() == "vnd.sun.star.script" )
        {
            xDispatcher = this;
        }
    }

    return xDispatcher;
}

Sequence< Reference< XDispatch > > SAL_CALL
ScriptProtocolHandler::queryDispatches(
const Sequence < DispatchDescriptor >& seqDescriptor )
{
    sal_Int32 nCount = seqDescriptor.getLength();
    Sequence< Reference< XDispatch > > lDispatcher( nCount );
    for ( sal_Int32 i = 0; i < nCount; ++i )
    {
        lDispatcher[ i ] = queryDispatch( seqDescriptor[ i ].FeatureURL,
                                                seqDescriptor[ i ].FrameName,
                                                seqDescriptor[ i ].SearchFlags );
    }
    return lDispatcher;
}

void SAL_CALL ScriptProtocolHandler::dispatchWithNotification(
    const URL& aURL, const Sequence < PropertyValue >& lArgs,
    const Reference< XDispatchResultListener >& xListener )
{

    bool bSuccess = false;
    Any invokeResult;
    bool bCaughtException = false;
    Any aException;

    if ( m_bInitialised )
    {
        try
        {
            bool bIsDocumentScript = ( aURL.Complete.indexOf( "document" ) !=-1 );
                // TODO: isn't this somewhat strange? This should be a test for a location=document parameter, shouldn't it?

            if ( bIsDocumentScript )
            {
                // obtain the component for our security check
                Reference< XEmbeddedScripts > xDocumentScripts;
                if ( getScriptInvocation() )
                    xDocumentScripts.set( m_xScriptInvocation->getScriptContainer(), UNO_SET_THROW );

                OSL_ENSURE( xDocumentScripts.is(), "ScriptProtocolHandler::dispatchWithNotification: can't do the security check!" );
                if ( !xDocumentScripts.is() || !xDocumentScripts->getAllowMacroExecution() )
                {
                    if ( xListener.is() )
                    {
                        css::frame::DispatchResultEvent aEvent(
                                static_cast< ::cppu::OWeakObject* >( this ),
                                css::frame::DispatchResultState::FAILURE,
                                invokeResult );
                        try
                        {
                            xListener->dispatchFinished( aEvent ) ;
                        }
                        catch(const RuntimeException &)
                        {
                            TOOLS_WARN_EXCEPTION("scripting",
                                "ScriptProtocolHandler::dispatchWithNotification: caught RuntimeException"
                                "while dispatchFinished with failure of the execution");
                        }
                    }
                    return;
                }
            }

            // Creates a ScriptProvider ( if one is not created already )
            createScriptProvider();

            Reference< provider::XScript > xFunc =
                m_xScriptProvider->getScript( aURL.Complete );
            ENSURE_OR_THROW( xFunc.is(),
                "ScriptProtocolHandler::dispatchWithNotification: validate xFunc - unable to obtain XScript interface" );


            Sequence< Any > inArgs( 0 );
            Sequence< Any > outArgs( 0 );
            Sequence< sal_Int16 > outIndex;

            if ( lArgs.hasElements() )
            {
               int argCount = 0;
               for ( int index = 0; index < lArgs.getLength(); index++ )
               {
                   // Sometimes we get a propertyval with name = "Referer" or "SynchronMode". These
                   // are not actual arguments to be passed to script, but flags describing the
                   // call, so ignore. Who thought that passing such "meta-arguments" mixed in with
                   // real arguments was a good idea?
                   if ( (lArgs[ index ].Name != "Referer" &&
                         lArgs[ index ].Name != "SynchronMode") ||
                        lArgs[ index ].Name.isEmpty() ) //TODO:???
                   {
                       inArgs.realloc( ++argCount );
                       inArgs[ argCount - 1 ] = lArgs[ index ].Value;
                   }
               }
            }

            // attempt to protect the document against the script tampering with its Undo Context
            std::unique_ptr< ::framework::DocumentUndoGuard > pUndoGuard;
            if ( bIsDocumentScript )
                pUndoGuard.reset( new ::framework::DocumentUndoGuard( m_xScriptInvocation ) );

            bSuccess = false;
            while ( !bSuccess )
            {
                std::exception_ptr aFirstCaughtException;
                try
                {
                    invokeResult = xFunc->invoke( inArgs, outIndex, outArgs );
                    bSuccess = true;
                }
                catch( const provider::ScriptFrameworkErrorException& se )
                {
                    if  (!aFirstCaughtException)
                        aFirstCaughtException = std::current_exception();

                    if ( se.errorType != provider::ScriptFrameworkErrorType::NO_SUCH_SCRIPT )
                        // the only condition which allows us to retry is if there is no method with the
                        // given name/signature
                        std::rethrow_exception(aFirstCaughtException);

                    if ( !inArgs.hasElements() )
                        // no chance to retry if we can't strip more in-args
                        std::rethrow_exception(aFirstCaughtException);

                    // strip one argument, then retry
                    inArgs.realloc( inArgs.getLength() - 1 );
                }
            }
        }
        // Office doesn't handle exceptions rethrown here very well, it cores,
        // all we can is log them and then set fail for the dispatch event!
        // (if there is a listener of course)
        catch ( const Exception & e )
        {
            aException = ::cppu::getCaughtException();

            OUString reason = "ScriptProtocolHandler::dispatch: caught ";

            invokeResult <<= reason.concat( aException.getValueTypeName() ).concat( ": " ).concat( e.Message );

            bCaughtException = true;
        }
    }
    else
    {
        invokeResult <<= OUString(
            "ScriptProtocolHandler::dispatchWithNotification failed, ScriptProtocolHandler not initialised"
        );
    }

    if ( bCaughtException )
    {
        SfxAbstractDialogFactory* pFact = SfxAbstractDialogFactory::Create();
        ScopedVclPtr<VclAbstractDialog> pDlg(
                pFact->CreateScriptErrorDialog( aException ));
        pDlg->Execute();
    }

    if ( xListener.is() )
    {
        // always call dispatchFinished(), because we didn't load a document but
        // executed a macro instead!
        css::frame::DispatchResultEvent aEvent;

        aEvent.Source = static_cast< ::cppu::OWeakObject* >( this );
        aEvent.Result = invokeResult;
        if ( bSuccess )
        {
            aEvent.State = css::frame::DispatchResultState::SUCCESS;
        }
        else
        {
            aEvent.State = css::frame::DispatchResultState::FAILURE;
        }

        try
        {
            xListener->dispatchFinished( aEvent ) ;
        }
        catch(const RuntimeException &)
        {
            TOOLS_WARN_EXCEPTION("scripting",
                "ScriptProtocolHandler::dispatchWithNotification: caught RuntimeException"
                "while dispatchFinished" );
        }
    }
}

void SAL_CALL ScriptProtocolHandler::dispatch(
const URL& aURL, const Sequence< PropertyValue >& lArgs )
{
    dispatchWithNotification( aURL, lArgs, Reference< XDispatchResultListener >() );
}

void SAL_CALL ScriptProtocolHandler::addStatusListener(
const Reference< XStatusListener >&, const URL& )
{
    // implement if status is supported
}

void SAL_CALL ScriptProtocolHandler::removeStatusListener(
const Reference< XStatusListener >&, const URL& )
{}

bool
ScriptProtocolHandler::getScriptInvocation()
{
    if ( !m_xScriptInvocation.is() && m_xFrame.is() )
    {
        Reference< XController > xController = m_xFrame->getController();
        if ( xController .is() )
        {
            // try to obtain an XScriptInvocationContext interface, preferred from the
            // mode, then from the controller
            if ( !m_xScriptInvocation.set( xController->getModel(), UNO_QUERY ) )
                m_xScriptInvocation.set( xController, UNO_QUERY );
        }
        else
        {
            Reference< XFrame > xFrame( m_xFrame.get(), UNO_QUERY );
            if ( xFrame.is() )
            {
                SfxFrame* pFrame = nullptr;
                for ( pFrame = SfxFrame::GetFirst(); pFrame; pFrame = SfxFrame::GetNext( *pFrame ) )
                {
                    if ( pFrame->GetFrameInterface() == xFrame )
                        break;
                }
                SfxObjectShell* pDocShell = pFrame ? pFrame->GetCurrentDocument() : SfxObjectShell::Current();
                if ( pDocShell )
                {
                    Reference< XModel > xModel( pDocShell->GetModel() );
                    m_xScriptInvocation.set( xModel, UNO_QUERY );
                }
            }
        }
    }
    return m_xScriptInvocation.is();
}

void ScriptProtocolHandler::createScriptProvider()
{
    if ( m_xScriptProvider.is() )
        return;

    try
    {
        // first, ask the component supporting the XScriptInvocationContext interface
        // (if there is one) for a script provider
        if ( getScriptInvocation() )
        {
            Reference< XScriptProviderSupplier > xSPS( m_xScriptInvocation, UNO_QUERY );
            if ( xSPS.is() )
                m_xScriptProvider = xSPS->getScriptProvider();
        }

        // second, ask the model in our frame
        if ( !m_xScriptProvider.is() && m_xFrame.is() )
        {
            Reference< XController > xController = m_xFrame->getController();
            if ( xController .is() )
            {
                Reference< XScriptProviderSupplier > xSPS( xController->getModel(), UNO_QUERY );
                if ( xSPS.is() )
                    m_xScriptProvider = xSPS->getScriptProvider();
            }
        }


        // as a fallback, ask the controller
        if ( !m_xScriptProvider.is() && m_xFrame.is() )
        {
            Reference< XScriptProviderSupplier > xSPS( m_xFrame->getController(), UNO_QUERY );
            if ( xSPS.is() )
                m_xScriptProvider = xSPS->getScriptProvider();
        }

        // if nothing of this is successful, use the master script provider
        if ( !m_xScriptProvider.is() )
        {
            Reference< provider::XScriptProviderFactory > xFac =
                provider::theMasterScriptProviderFactory::get( m_xContext );

            Any aContext;
            if ( getScriptInvocation() )
                aContext <<= m_xScriptInvocation;
            m_xScriptProvider.set( xFac->createScriptProvider( aContext ), UNO_SET_THROW );
        }
    }
    catch ( const Exception & e )
    {
        css::uno::Any anyEx = cppu::getCaughtException();
        throw css::lang::WrappedTargetRuntimeException(
            "ScriptProtocolHandler::createScriptProvider: " + e.Message,
            nullptr, anyEx );
    }
}

ScriptProtocolHandler::ScriptProtocolHandler( const Reference< css::uno::XComponentContext > & xContext )
  : m_bInitialised( false ), m_xContext( xContext )
{
}

ScriptProtocolHandler::~ScriptProtocolHandler()
{
}

/* XServiceInfo */
OUString SAL_CALL ScriptProtocolHandler::getImplementationName( )
{
    return impl_getStaticImplementationName();
}

/* XServiceInfo */
sal_Bool SAL_CALL ScriptProtocolHandler::supportsService(const OUString& sServiceName )
{
    return cppu::supportsService(this, sServiceName);
}

/* XServiceInfo */
Sequence< OUString > SAL_CALL ScriptProtocolHandler::getSupportedServiceNames()
{
    return impl_getStaticSupportedServiceNames();
}

/* Helper for XServiceInfo */
Sequence< OUString > ScriptProtocolHandler::impl_getStaticSupportedServiceNames()
{
    return {"com.sun.star.frame.ProtocolHandler"};
}

/* Helper for XServiceInfo */
OUString ScriptProtocolHandler::impl_getStaticImplementationName()
{
    return OUString("com.sun.star.comp.ScriptProtocolHandler");
}

/* Helper for registry */
Reference< XInterface > SAL_CALL ScriptProtocolHandler::impl_createInstance(
const Reference< css::lang::XMultiServiceFactory >& xServiceManager )
{
    return Reference< XInterface > ( *new ScriptProtocolHandler( comphelper::getComponentContext(xServiceManager) ) );
}

/* Factory for registration */
Reference< XSingleServiceFactory > ScriptProtocolHandler::impl_createFactory(
const Reference< XMultiServiceFactory >& xServiceManager )
{
    Reference< XSingleServiceFactory > xReturn (
        cppu::createSingleFactory( xServiceManager,
            ScriptProtocolHandler::impl_getStaticImplementationName(),
            ScriptProtocolHandler::impl_createInstance,
            ScriptProtocolHandler::impl_getStaticSupportedServiceNames() )
    );
    return xReturn;
}

} // namespace scripting_protocolhandler

extern "C"
{
    SAL_DLLPUBLIC_EXPORT void* protocolhandler_component_getFactory( const sal_Char * pImplementationName ,
                                         void * pServiceManager ,
                                         void * )
    {
        // Set default return value for this operation - if it failed.
        void * pReturn = nullptr ;

        if (
            ( pImplementationName != nullptr ) &&
            ( pServiceManager != nullptr )
        )
        {
            // Define variables which are used in following macros.
            css::uno::Reference< css::lang::XSingleServiceFactory > xFactory;
            css::uno::Reference< css::lang::XMultiServiceFactory > xServiceManager(
                static_cast< css::lang::XMultiServiceFactory* >( pServiceManager ) ) ;

            if ( ::scripting_protocolhandler::ScriptProtocolHandler::impl_getStaticImplementationName().equalsAscii(
                    pImplementationName ) )
            {
                xFactory = ::scripting_protocolhandler::ScriptProtocolHandler::impl_createFactory( xServiceManager );
            }

            // Factory is valid - service was found.
            if ( xFactory.is() )
            {
                xFactory->acquire();
                pReturn = xFactory.get();
            }
        }

        // Return with result of this operation.
        return pReturn ;
    }
} // extern "C"


/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
