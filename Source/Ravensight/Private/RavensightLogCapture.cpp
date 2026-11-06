// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "RavensightLogCapture.h"

#include "CoreGlobals.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTLS.h"
#include "HAL/PlatformTime.h"
#include "Misc/CString.h"
#include "Misc/CoreDelegates.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"
#include "RavensightSubsystem.h"
#include "UObject/Script.h"
#include "UObject/Stack.h"

// FBlueprintExceptionInfo moved out of Script.h into its own header during 5.x.
#if __has_include("Blueprint/BlueprintExceptionInfo.h")
#include "Blueprint/BlueprintExceptionInfo.h"
#endif

#include <type_traits>

namespace
{
	// FCoreDelegates::OnEnsureFailed carries the expression, the message and
	// the stack. Engines without it fall back to the ensure's own log line.
	// Detected rather than version guessed: the template parameter makes the
	// member lookup a substitution failure instead of a compile error.

	template <typename TDelegates, typename = void>
	struct THasEnsureFailed : std::false_type {};

	template <typename TDelegates>
	struct THasEnsureFailed<TDelegates, std::void_t<decltype(TDelegates::OnEnsureFailed)>> : std::true_type {};

	// One binding per capture object, so two game instances (several PIE
	// clients) each get every ensure once.
	template <typename TDelegates>
	FDelegateHandle BindEnsure(std::true_type, FRavensightLogCapture* Capture)
	{
		return TDelegates::OnEnsureFailed.AddLambda(
			[Capture](const ANSICHAR* Expr, const ANSICHAR* /*File*/, int32 /*Line*/, const TCHAR* Msg, const TCHAR* Combined)
			{
				Capture->HandleEnsure(Expr ? FString(ANSI_TO_TCHAR(Expr)) : FString(), Msg, Combined);
			});
	}

	template <typename TDelegates>
	FDelegateHandle BindEnsure(std::false_type, FRavensightLogCapture* /*Capture*/)
	{
		return FDelegateHandle();
	}

	template <typename TDelegates>
	void UnbindEnsure(FDelegateHandle Handle, std::true_type)
	{
		TDelegates::OnEnsureFailed.Remove(Handle);
	}

	template <typename TDelegates>
	void UnbindEnsure(FDelegateHandle, std::false_type)
	{
	}

#if !NO_LOGGING
	bool StartsWith(const TCHAR* Text, const TCHAR* Prefix)
	{
		return FCString::Strncmp(Text, Prefix, FCString::Strlen(Prefix)) == 0;
	}

	ERavensightLogLevel ToLevel(ELogVerbosity::Type Verbosity, bool& bKnown)
	{
		bKnown = true;
		switch (Verbosity & ELogVerbosity::VerbosityMask)
		{
		case ELogVerbosity::Fatal: return ERavensightLogLevel::Fatal;
		case ELogVerbosity::Error: return ERavensightLogLevel::Error;
		case ELogVerbosity::Warning: return ERavensightLogLevel::Warning;
		case ELogVerbosity::Display: return ERavensightLogLevel::Display;
		case ELogVerbosity::Log: return ERavensightLogLevel::Log;
		case ELogVerbosity::Verbose: return ERavensightLogLevel::Verbose;
		case ELogVerbosity::VeryVerbose: return ERavensightLogLevel::VeryVerbose;
		default:
			bKnown = false;
			return ERavensightLogLevel::VeryVerbose;
		}
	}

#endif

	const TCHAR* ExceptionTypeName(int32 Type)
	{
		switch (Type)
		{
		case EBlueprintExceptionType::AccessViolation: return TEXT("Blueprint.AccessViolation");
		case EBlueprintExceptionType::InfiniteLoop: return TEXT("Blueprint.InfiniteLoop");
		case EBlueprintExceptionType::NonFatalError: return TEXT("Blueprint.NonFatalError");
		case EBlueprintExceptionType::FatalError: return TEXT("Blueprint.FatalError");
		case EBlueprintExceptionType::AbortExecution: return TEXT("Blueprint.AbortExecution");
		default: return TEXT("Blueprint.Exception");
		}
	}
}

FRavensightLogCapture::FRavensightLogCapture(const FString& InOwnHost)
	: OwnHost(InOwnHost)
{
}

FRavensightLogCapture::~FRavensightLogCapture()
{
	Uninstall();
}

bool FRavensightLogCapture::HasEnsureHook()
{
	return THasEnsureFailed<FCoreDelegates>::value;
}

void FRavensightLogCapture::Install()
{
	if (bInstalled || !GLog)
	{
		return;
	}
	bInstalled = true;

	EnsureHandle = BindEnsure<FCoreDelegates>(THasEnsureFailed<FCoreDelegates>{}, this);
	bEnsureHooked = EnsureHandle.IsValid();

	bScriptWasUnbound = !FBlueprintCoreDelegates::OnScriptException.IsBound();
	ScriptHandle = FBlueprintCoreDelegates::OnScriptException.AddRaw(this, &FRavensightLogCapture::HandleScriptException);

	// AddOutputDevice may replay the engine's backlog into a new device;
	// those lines are not this session's errors.
#if !NO_LOGGING
	bInstalling.store(true);
	GLog->AddOutputDevice(this);
	bInstalling.store(false);
#endif
}

void FRavensightLogCapture::Uninstall()
{
	if (!bInstalled)
	{
		return;
	}
	bInstalled = false;

	if (ScriptHandle.IsValid())
	{
		FBlueprintCoreDelegates::OnScriptException.Remove(ScriptHandle);
		ScriptHandle.Reset();
	}
	if (EnsureHandle.IsValid())
	{
		UnbindEnsure<FCoreDelegates>(EnsureHandle, THasEnsureFailed<FCoreDelegates>{});
		EnsureHandle.Reset();
	}
	bEnsureHooked = false;

	// The redirector takes its write lock to remove a device, so no Serialize
	// is still running in this object once this returns.
	if (GLog)
	{
		GLog->RemoveOutputDevice(this);
	}
}

void FRavensightLogCapture::Drain(TArray<FRavensightRawCapture>& Out)
{
	FScopeLock ScopeLock(&Lock);
	Out = MoveTemp(Pending);
	Pending.Reset();
	PendingCaptures.store(0);
	PendingLines.store(0);
}

bool FRavensightLogCapture::HasRoom(bool bCapture) const
{
	return bCapture
		? (bAcceptCaptures.load() && PendingCaptures.load() < MaxPendingCaptures)
		: PendingLines.load() < MaxPendingLines;
}

void FRavensightLogCapture::Push(FRavensightRawCapture&& Record)
{
	const bool bCapture = !Record.bLine || Record.Level <= ERavensightLogLevel::Error;
	FScopeLock ScopeLock(&Lock);
	std::atomic<int32>& Count = bCapture ? PendingCaptures : PendingLines;
	if (Count.load() < (bCapture ? MaxPendingCaptures : MaxPendingLines))
	{
		Count.store(Count.load() + 1);
		Pending.Add(MoveTemp(Record));
	}
}

void FRavensightLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
#if NO_LOGGING
	// A build without logging never registers this device; the log
	// categories it compares against do not exist there.
	(void)V;
	(void)Verbosity;
	(void)Category;
#else
	if (!V || bInstalling.load())
	{
		return;
	}

	// Never the plugin's own lines: no feedback loop.
	if (Category == LogRavensight.GetCategoryName())
	{
		return;
	}

	bool bKnown = false;
	const ERavensightLogLevel Level = ToLevel(Verbosity, bKnown);
	if (!bKnown)
	{
		return;
	}
	const ERavensightLineAction Action = RavensightErrors::ClassifyLine(Level, bKeepTrail.load());
	if (Action == ERavensightLineAction::Ignore || !HasRoom(Action == ERavensightLineAction::Capture))
	{
		return;
	}

	// A line naming the API host is about the plugin's own requests.
	if (!OwnHost.IsEmpty() && FCString::Stristr(V, *OwnHost) != nullptr)
	{
		return;
	}

	// An ensure writes a block of lines under LogOutputDevice; the ensure
	// hook reports it once, with its stack, instead.
	if (Category == LogOutputDevice.GetCategoryName())
	{
		const uint32 ThreadId = FPlatformTLS::GetCurrentThreadId();
		if (StartsWith(V, TEXT("=== Handled ensure")))
		{
			if (bEnsureHooked)
			{
				EnsureThreadId.store(ThreadId);
			}
			return;
		}
		if (bEnsureHooked && EnsureThreadId.load() == ThreadId)
		{
			return;
		}
		if (StartsWith(V, TEXT("[Callstack]")))
		{
			return;
		}
		if (StartsWith(V, TEXT("Ensure condition failed:")))
		{
			if (!bEnsureHooked && HasRoom(true))
			{
				// No ensure hook on this engine: the log line is the report.
				FRavensightRawCapture Record;
				Record.bLine = false;
				Record.Error.Severity = TEXT("warning");
				Record.Error.Message = FString(V).Left(MaxCopyChars);
				Record.Error.Source = RavensightErrors::SourceLog;
				Record.Error.ErrorType = TEXT("Ensure");
				Record.AtSeconds = FPlatformTime::Seconds();
				Push(MoveTemp(Record));
			}
			return;
		}
	}

	FRavensightRawCapture Record;
	Record.bLine = true;
	Record.Level = Level;
	Record.Category = Category.ToString();
	Record.Text = FString(V).Left(MaxCopyChars);
	Record.AtSeconds = FPlatformTime::Seconds();
	Push(MoveTemp(Record));
#endif
}

void FRavensightLogCapture::HandleEnsure(const FString& Expression, const TCHAR* Message, const TCHAR* Combined)
{
	const uint32 ThreadId = FPlatformTLS::GetCurrentThreadId();
	uint32 Expected = ThreadId;
	EnsureThreadId.compare_exchange_strong(Expected, 0);
	if (!HasRoom(true))
	{
		return;
	}

	// Combined is "Ensure condition failed: <expr> [File:..] [Line: ..]\n<msg>\nStack: \n<frames>".
	// The message leaves out the file and line, which a long build path
	// would push past 200 characters; the stack carries them.
	const FString Full = Combined ? FString(Combined) : FString();
	const FString Head = FString(TEXT("Ensure condition failed: ")) + Expression;
	FString Stack;
	const int32 Marker = Full.Find(TEXT("Stack:"), ESearchCase::CaseSensitive);
	if (Marker != INDEX_NONE)
	{
		Stack = Full.Mid(Marker + 6).TrimStartAndEnd();
	}

	FRavensightRawCapture Record;
	Record.bLine = false;
	Record.Error.Severity = TEXT("warning");
	Record.Error.Message = (Message && *Message) ? Head + TEXT(": ") + FString(Message) : Head;
	Record.Error.Message.LeftInline(MaxCopyChars);
	Record.Error.Stack = Stack.Left(MaxCopyChars);
	Record.Error.Source = RavensightErrors::SourceLog;
	Record.Error.ErrorType = TEXT("Ensure");
	Record.AtSeconds = FPlatformTime::Seconds();
	Push(MoveTemp(Record));
}

void FRavensightLogCapture::HandleScriptException(const UObject* /*ActiveObject*/, const FFrame& StackFrame, const FBlueprintExceptionInfo& Info)
{
	const EBlueprintExceptionType::Type Type = Info.GetType();
	if (Type == EBlueprintExceptionType::Breakpoint || Type == EBlueprintExceptionType::Tracepoint
		|| Type == EBlueprintExceptionType::WireTracepoint)
	{
		return;
	}

#if DO_BLUEPRINT_GUARD
	// The engine logs the script stack only while nothing is bound to this
	// delegate. Binding it must not take that line away from the game's log.
	if (bScriptWasUnbound)
	{
		static const IConsoleVariable* ShortWarnings = IConsoleManager::Get().FindConsoleVariable(TEXT("bp.ShortScriptWarnings"));
		if (!ShortWarnings || ShortWarnings->GetInt() == 0)
		{
			UE_LOG(LogScript, Warning, TEXT("%s"), *StackFrame.GetStackTrace());
		}
	}
#endif

	if (!HasRoom(true))
	{
		return;
	}

	const FString Trace = StackFrame.GetStackTrace();
	const FString Description = Info.GetDescription().ToString();

	FRavensightRawCapture Record;
	Record.bLine = false;
	Record.Error.Severity = TEXT("error");
	Record.Error.ErrorType = ExceptionTypeName(static_cast<int32>(Type));
	Record.Error.Message = (Description.IsEmpty() ? Record.Error.ErrorType : Description).Left(MaxCopyChars);
	Record.Error.Stack = Trace.Left(MaxCopyChars);
	Record.Error.Source = RavensightErrors::SourceUncaught;
	Record.AtSeconds = FPlatformTime::Seconds();
	Push(MoveTemp(Record));
}
