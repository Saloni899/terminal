// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "CTerminalHandoff.h"

using namespace Microsoft::WRL;

// The callback function when a connection is received
static NewHandoffFunction _pfnHandoff = nullptr;
// The registration ID of the class object for clean up later
static DWORD g_cTerminalHandoffRegistration = 0;
// Mutex so we only do start/stop/establish one at a time.
static std::shared_mutex _mtx;

// Routine Description:
// - Starts listening for TerminalHandoff requests by registering
//   our class and interface with COM.
// Arguments:
// - pfnHandoff - Function to callback when a handoff is received
// Return Value:
// - S_OK, E_NOT_VALID_STATE (start called when already started) or relevant COM registration error.
HRESULT CTerminalHandoff::s_StartListening(NewHandoffFunction pfnHandoff)
try
{
    std::unique_lock lock{ _mtx };

    RETURN_HR_IF(E_NOT_VALID_STATE, _pfnHandoff != nullptr);

    const auto classFactory = Make<SimpleClassFactory<CTerminalHandoff>>();

    RETURN_IF_NULL_ALLOC(classFactory);

    ComPtr<IUnknown> unk;
    RETURN_IF_FAILED(classFactory.As(&unk));

    RETURN_IF_FAILED(CoRegisterClassObject(__uuidof(CTerminalHandoff), unk.Get(), CLSCTX_LOCAL_SERVER, REGCLS_SINGLEUSE, &g_cTerminalHandoffRegistration));

    _pfnHandoff = pfnHandoff;

    return S_OK;
}
CATCH_RETURN()

// Routine Description:
// - Stops listening for TerminalHandoff requests by revoking the registration
//   our class and interface with COM
// Arguments:
// - <none>
// Return Value:
// - S_OK, E_NOT_VALID_STATE (stop called when not started), or relevant COM class revoke error
HRESULT CTerminalHandoff::s_StopListening()
{
    std::unique_lock lock{ _mtx };
    return s_StopListeningLocked();
}

// See s_StopListening()
HRESULT CTerminalHandoff::s_StopListeningLocked() noexcept
{
    RETURN_HR_IF_NULL(E_NOT_VALID_STATE, _pfnHandoff);

    _pfnHandoff = nullptr;

    if (g_cTerminalHandoffRegistration)
    {
        RETURN_IF_FAILED(CoRevokeClassObject(g_cTerminalHandoffRegistration));
        g_cTerminalHandoffRegistration = 0;
    }

    return S_OK;
}

// Routine Description:
// - Receives the terminal handoff via COM from the other process,
//   duplicates handles as COM will free those given on the way out,
//   then fires off an event notifying the rest of the terminal that
//   a connection is on its way in.
// Arguments:
// - pipes:
//   0: in - PTY input handle that we will read from
//   1: out - PTY output handle that we will write to
//   2: signal - PTY signal handle for out of band messaging
// - processes:
//   0: server - PTY process handle to track for lifetime/cleanup
//   1: client - Process handle to client so we can track its lifetime and exit appropriately
// Return Value:
// - E_NOT_VALID_STATE if a event handler is not registered before calling. `::DuplicateHandle`
//   error codes if we cannot manage to make our own copy of handles to retain. Or S_OK/error
//   from the registered handler event function.
HRESULT CTerminalHandoff::EstablishPtyHandoff(const HANDLE* pipes, const HANDLE* processes, TERMINAL_STARTUP_INFO startupInfo, PTY_HANDOFF_RESPONSE* response) noexcept
try
{
    std::unique_lock lock{ _mtx };

    const auto stopListeningOnExit = wil::scope_exit([]() {
        // Because we are REGCLS_SINGLEUSE... we need to `CoRevokeClassObject` after we handle this ONE call.
        // COM does not automatically clean that up for us. We must do it.
        LOG_IF_FAILED(s_StopListeningLocked());
    });

    // Report an error if no one registered a handoff function before calling this.
    THROW_HR_IF_NULL(E_NOT_VALID_STATE, _pfnHandoff);

    // If the client didn't specify a response object, that's fine. It'll work anyways and
    // they just won't get any information from us. To avoid forcing our handoff callback
    // target to check the pointer, we'll just use a temporary throw-away object here.
    PTY_HANDOFF_RESPONSE tempResponse;
    if (!response)
    {
        response = &tempResponse;
    }

    // Call registered handler from when we started listening.
    THROW_IF_FAILED(_pfnHandoff(pipes, processes, startupInfo, *response));

#pragma warning(suppress : 26477)
    TraceLoggingWrite(
        g_hTerminalConnectionProvider,
        "ReceiveTerminalHandoff_Success",
        TraceLoggingDescription("successfully received a terminal handoff"),
        TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
        TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

    return S_OK;
}
catch (...)
{
    const auto hr = wil::ResultFromCaughtException();

#pragma warning(suppress : 26477)
    TraceLoggingWrite(
        g_hTerminalConnectionProvider,
        "ReceiveTerminalHandoff_Failed",
        TraceLoggingDescription("failed while receiving a terminal handoff"),
        TraceLoggingHResult(hr),
        TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
        TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

    return hr;
}
