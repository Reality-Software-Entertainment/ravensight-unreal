// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The one place the subsystem creates and starts an HTTP request. Internal to
// the module: it is not part of the plugin's public API.
//
// By default it is exactly FHttpModule::Get().CreateRequest() and
// Request->ProcessRequest(). The automation specs install a sender that
// records each request and starts nothing, so they can count requests, read
// their verb, URL, headers and body, and answer them with a fake response,
// without touching the network.

#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "Templates/Function.h"

namespace RavensightTransport
{
	/** Returns true when the request was started, like IHttpRequest::ProcessRequest. */
	using FSender = TFunction<bool(const FHttpRequestRef&)>;

	/** A fresh request from the engine's HTTP module. */
	FHttpRequestRef CreateRequest();

	/** Starts the request through the installed sender, or ProcessRequest when none is installed. */
	bool Send(const FHttpRequestRef& Request);

	/** For the automation specs. An empty sender restores the real one. Game thread only. */
	void SetSenderForTests(FSender InSender);
}
