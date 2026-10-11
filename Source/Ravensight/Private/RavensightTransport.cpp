// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "RavensightTransport.h"

#include "HttpModule.h"

namespace
{
	RavensightTransport::FSender& InstalledSender()
	{
		static RavensightTransport::FSender Sender;
		return Sender;
	}
}

FHttpRequestRef RavensightTransport::CreateRequest()
{
	return FHttpModule::Get().CreateRequest();
}

bool RavensightTransport::Send(const FHttpRequestRef& Request)
{
	const FSender& Sender = InstalledSender();
	return Sender ? Sender(Request) : Request->ProcessRequest();
}

#if WITH_DEV_AUTOMATION_TESTS
void RavensightTransport::SetSenderForTests(FSender InSender)
{
	InstalledSender() = MoveTemp(InSender);
}
#endif
