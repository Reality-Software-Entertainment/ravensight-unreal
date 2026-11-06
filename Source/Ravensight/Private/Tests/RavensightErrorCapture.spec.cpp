// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Automation spec for the engine free half of automatic error capture in
// RavensightErrorCapture.h: the scrub (the platform's own cases), the dedupe
// template, the trail ring, the per session caps, the hold before the
// server's answer, and the event shape.
//
// Runs in the editor ("Ravensight.ErrorCapture") and under Tools/host-check.

#include "Misc/AutomationTest.h"
#include "RavensightErrorCapture.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace RavensightErrorSpecHelpers
{
	bool Same(const FString& A, const FString& B)
	{
		return A.Equals(B, ESearchCase::CaseSensitive);
	}

	FString Text(const FRavensightTypedEvent& Event, const FString& Key)
	{
		const FRavensightProp* Prop = Event.Find(Key);
		return Prop ? Prop->Text : FString(TEXT("<absent>"));
	}

	FRavensightCapturedError Make(const FString& Message, const FString& Type = TEXT("LogTemp"))
	{
		FRavensightCapturedError Error;
		Error.Severity = TEXT("error");
		Error.Message = Message;
		Error.Source = RavensightErrors::SourceLog;
		Error.ErrorType = Type;
		return Error;
	}
}

BEGIN_DEFINE_SPEC(FRavensightErrorCaptureSpec, "Ravensight.ErrorCapture", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
END_DEFINE_SPEC(FRavensightErrorCaptureSpec)

void FRavensightErrorCaptureSpec::Define()
{
	using namespace RavensightErrorSpecHelpers;

	Describe("Scrub", [this]()
	{
		It("matches the platform's replacements", [this]()
		{
			struct FCase { const char* In; const char* Out; };
			const FCase Cases[] = {
				{ "mail bob.smith+x@example.co.uk now", "mail [email] now" },
				{ "GET https://api.example.com/v1/a?token=abc&x=1#frag failed", "GET https://api.example.com/v1/a failed" },
				{ "at https://user:secret@host.example/path", "at https://host.example/path" },
				{ "Authorization: Bearer abc.def-123_456", "Authorization: [token]" },
				{ "jwt eyJhbGciOi.eyJzdWIiOiIx.sig-_x end", "jwt [token] end" },
				{ "key gt_live_ab12CD34 and gt_mcp_x", "key [token] and [token]" },
				{ "at /Users/jacob/game/main.js:1", "at ~/game/main.js:1" },
				{ "at /home/al/game/main.lua", "at ~/game/main.lua" },
				{ "at C:\\Users\\Jo Ann\\x.cs", "at ~ Ann\\x.cs" },
				{ "at C:/Users/jo/x.cs", "at ~/x.cs" },
				{ "from 192.168.1.10 ok", "from [ip] ok" },
				{ "from 2001:db8::1 and ::1 ok", "from [ip] and [ip] ok" },
				{ "full fe80:0:0:0:0:0:0:1", "full [ip]" },
				{ "hash 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08", "hash [token]" },
				{ "b64 QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVo0NTY3ODk=", "b64 [token]" },
			};
			for (const FCase& Case : Cases)
			{
				TestTrue(Case.In, Same(RavensightErrors::Scrub(Case.In), Case.Out));
			}
		});

		It("leaves ordinary text, frames and identifiers without a digit alone", [this]()
		{
			const char* const Kept[] = {
				"on_pointer_click_handler_for_the_menu_screen failed",
				"OnPointerClickHandlerImplementationForTheMenu failed",
				"cpp Foo::Bar at 12:30:45",
				"at onKick (index.js:12:34)",
				"Kick Off button: TypeError: x is undefined",
				"std::vector<int>::size",
				"version 1.2.3 build 4",
			};
			for (const char* Input : Kept)
			{
				TestTrue(Input, Same(RavensightErrors::Scrub(Input), Input));
			}
		});

		It("is idempotent", [this]()
		{
			const FString Once = RavensightErrors::Scrub(TEXT("bob@x.io at https://h.io/a?b=1 from 10.0.0.1 gt_pub_abc Bearer t"));
			TestTrue("twice", Same(RavensightErrors::Scrub(Once), Once));
			TestTrue("empty", RavensightErrors::Scrub(FString()).IsEmpty());
		});
	});

	Describe("Template and first frame", [this]()
	{
		It("folds what varies between occurrences", [this]()
		{
			TestTrue("numbers", Same(RavensightErrors::Template(TEXT("Actor 12 failed at 300")), TEXT("Actor 0 failed at 0")));
			TestTrue("quoted", Same(RavensightErrors::Template(TEXT("Missing asset 'Hero_7'")), TEXT("Missing asset '*'")));
			TestTrue("hex", Same(RavensightErrors::Template(TEXT("ptr 0x7ffeefbff5c8 bad")), TEXT("ptr 0x<hex> bad")));
			TestTrue("uuid", Same(RavensightErrors::Template(TEXT("id 123e4567-e89b-12d3-a456-426614174000 gone")), TEXT("id <id> gone")));
			TestTrue("url", Same(RavensightErrors::Template(TEXT("GET https://a.b/c/1 failed")), TEXT("GET <url> failed")));
			TestTrue("same key", Same(RavensightErrors::Template(TEXT("Actor 1")), RavensightErrors::Template(TEXT("Actor 99"))));
		});

		It("skips the assertion machinery and the plugin", [this]()
		{
			const FString Stack = TEXT("[Callstack] 0x01 UnrealEditor-Core.dylib!FDebug::EnsureFailed()\n\n[Callstack] 0x02 UnrealEditor-Game.dylib!AHero::Tick() [Hero.cpp:42]\n");
			TestTrue("frame", Same(RavensightErrors::FirstFrame(Stack), TEXT("[Callstack] 0x02 UnrealEditor-Game.dylib!AHero::Tick() [Hero.cpp:42]")));
			TestTrue("none", RavensightErrors::FirstFrame(TEXT("URavensightSubsystem::Tick")).IsEmpty());
		});
	});

	Describe("Levels", [this]()
	{
		It("captures Error and Fatal, trails Warning, trails lower levels only with the trail on", [this]()
		{
			TestTrue("fatal", RavensightErrors::ClassifyLine(ERavensightLogLevel::Fatal, false) == ERavensightLineAction::Capture);
			TestTrue("error", RavensightErrors::ClassifyLine(ERavensightLogLevel::Error, false) == ERavensightLineAction::Capture);
			TestTrue("warning", RavensightErrors::ClassifyLine(ERavensightLogLevel::Warning, false) == ERavensightLineAction::Trail);
			TestTrue("log off", RavensightErrors::ClassifyLine(ERavensightLogLevel::Log, false) == ERavensightLineAction::Ignore);
			TestTrue("log on", RavensightErrors::ClassifyLine(ERavensightLogLevel::Log, true) == ERavensightLineAction::Trail);
			TestTrue("display on", RavensightErrors::ClassifyLine(ERavensightLogLevel::Display, true) == ERavensightLineAction::Trail);
			TestTrue("verbose", RavensightErrors::ClassifyLine(ERavensightLogLevel::Verbose, true) == ERavensightLineAction::Ignore);
		});
	});

	Describe("Trail ring", [this]()
	{
		It("keeps the last 20 lines, each scrubbed and cut to 120", [this]()
		{
			FRavensightTrailRing Ring;
			for (int32 Index = 0; Index < 25; ++Index)
			{
				Ring.Add(Index * 10.0, ERavensightLogLevel::Warning, FString::Printf(TEXT("line %d"), Index));
			}
			TestEqual("lines", Ring.Num(), 20);
			const FString Joined = Ring.Join();
			TestTrue("oldest kept is 5", Same(Joined.Left(19), TEXT("+50 warning line 5\n")));

			FRavensightTrailRing Long;
			Long.Add(1.0, ERavensightLogLevel::Error, FString(TEXT("mail bob@example.com\nnext")) + FString::ChrN(300, TCHAR('a')));
			const FString Line = Long.Join();
			TestEqual("cut", Line.Len(), RavensightErrors::TrailLineChars);
			TestTrue("scrubbed and flat", Same(Line.Left(27), TEXT("+1 error mail [email] nexta")));
		});

		It("joins within 1,500 characters, oldest dropped first", [this]()
		{
			FRavensightTrailRing Ring;
			for (int32 Index = 0; Index < 20; ++Index)
			{
				Ring.Add(0.0, ERavensightLogLevel::Warning, FString::ChrN(200, TCHAR('w')));
			}
			const FString Joined = Ring.Join();
			TestTrue("fits", Joined.Len() <= RavensightErrors::TrailChars);
			TestEqual("twelve lines", Joined.Len(), 12 * 120 + 11);
		});
	});

	Describe("Limiter", [this]()
	{
		It("admits 3 per key and refills 6 a minute", [this]()
		{
			FRavensightErrorLimiter Limiter;
			Limiter.Reset(0.0);
			int32 Admitted = 0;
			for (int32 Index = 0; Index < 5; ++Index) Admitted += Limiter.TryAdmit(TEXT("a"), 0.0) ? 1 : 0;
			TestEqual("per key", Admitted, 3);
			TestTrue("b", Limiter.TryAdmit(TEXT("b"), 0.0));
			TestTrue("c", Limiter.TryAdmit(TEXT("c"), 0.0));
			TestTrue("d", Limiter.TryAdmit(TEXT("d"), 0.0));
			TestFalse("bucket empty", Limiter.TryAdmit(TEXT("e"), 0.0));
			TestFalse("not yet", Limiter.TryAdmit(TEXT("e"), 9.0));
			TestTrue("one refilled after 10 s", Limiter.TryAdmit(TEXT("e"), 10.0));
		});

		It("stops at 30 captures and 10 trails a session", [this]()
		{
			FRavensightErrorLimiter Limiter;
			Limiter.Reset(0.0);
			int32 Admitted = 0;
			for (int32 Index = 0; Index < 100; ++Index)
			{
				Admitted += Limiter.TryAdmit(FString::Printf(TEXT("k%d"), Index), Index * 60.0) ? 1 : 0;
			}
			TestEqual("captures", Admitted, RavensightErrors::MaxCaptures);
			int32 Trails = 0;
			for (int32 Index = 0; Index < 20; ++Index) Trails += Limiter.TryClaimTrail() ? 1 : 0;
			TestEqual("trails", Trails, RavensightErrors::MaxTrails);
			Limiter.Reset(0.0);
			TestTrue("new session", Limiter.TryAdmit(TEXT("k1"), 0.0));
		});
	});

	Describe("Pipeline", [this]()
	{
		It("holds at most 5 before the answer and sends them when capture is on", [this]()
		{
			FRavensightErrorPipeline Pipeline;
			Pipeline.Start(0.0);
			FRavensightTypedEvent Out;
			for (int32 Index = 0; Index < 7; ++Index)
			{
				TestFalse("held", Pipeline.OnCapture(Make(FString::Printf(TEXT("boot %d"), Index), FString::Printf(TEXT("T%d"), Index)), 0.0, Out));
			}
			TestEqual("held five", Pipeline.NumHeld(), RavensightErrors::MaxHeld);
			const TArray<FRavensightTypedEvent> Ready = Pipeline.Answer(true, false);
			TestEqual("released", Ready.Num(), 5);
			TestEqual("cleared", Pipeline.NumHeld(), 0);
			TestTrue("message", Same(Text(Ready[0], TEXT("rs_message")), TEXT("boot 0")));
		});

		It("discards held captures unsent when capture is off, and captures nothing after", [this]()
		{
			FRavensightErrorPipeline Pipeline;
			Pipeline.Start(0.0);
			FRavensightTypedEvent Out;
			Pipeline.OnCapture(Make(TEXT("boot")), 0.0, Out);
			TestEqual("nothing sent", Pipeline.Answer(false, true).Num(), 0);
			TestFalse("off", Pipeline.OnCapture(Make(TEXT("later")), 1.0, Out));
			TestFalse("off line", Pipeline.OnLine(ERavensightLogLevel::Error, TEXT("LogTemp"), TEXT("x"), 1.0, Out));
		});

		It("builds the event shape from a log line, with the trail only when it is on", [this]()
		{
			FRavensightErrorPipeline Pipeline;
			Pipeline.Start(0.0);
			Pipeline.Answer(true, true);
			FRavensightTypedEvent Out;
			TestFalse("warning trails", Pipeline.OnLine(ERavensightLogLevel::Warning, TEXT("LogTemp"), TEXT("low ammo"), 0.5, Out));
			TestTrue("error", Pipeline.OnLine(ERavensightLogLevel::Error, TEXT("LogGame"), TEXT("Door 12 failed for bob@example.com"), 1.0, Out));
			TestTrue("name", Same(Out.Name, TEXT("error")));
			TestTrue("kind", Same(Text(Out, TEXT("rs_kind")), TEXT("error")));
			TestTrue("severity", Same(Text(Out, TEXT("rs_severity")), TEXT("error")));
			TestTrue("source", Same(Text(Out, TEXT("rs_source")), TEXT("log")));
			TestTrue("type", Same(Text(Out, TEXT("rs_error_type")), TEXT("LogGame")));
			TestTrue("scrubbed", Same(Text(Out, TEXT("rs_message")), TEXT("Door 12 failed for [email]")));
			TestTrue("trail", Same(Text(Out, TEXT("rs_trail")), TEXT("+500 warning LogTemp: low ammo")));
			TestTrue("no stack", Out.Find(TEXT("rs_stack")) == nullptr);
			TestTrue("no fp", Out.Find(TEXT("rs_fp")) == nullptr);

			FRavensightErrorPipeline NoTrail;
			NoTrail.Start(0.0);
			NoTrail.Answer(true, false);
			NoTrail.OnLine(ERavensightLogLevel::Warning, TEXT("LogTemp"), TEXT("low ammo"), 0.5, Out);
			TestTrue("fatal", NoTrail.OnLine(ERavensightLogLevel::Fatal, TEXT("LogGame"), TEXT("boom"), 1.0, Out));
			TestTrue("critical", Same(Text(Out, TEXT("rs_severity")), TEXT("critical")));
			TestTrue("trail off", Out.Find(TEXT("rs_trail")) == nullptr);
		});

		It("cuts message, stack and type to the contract's lengths", [this]()
		{
			FRavensightCapturedError Error = Make(FString::ChrN(500, TCHAR('m')), FString::ChrN(300, TCHAR('t')));
			Error.Stack = FString::ChrN(5000, TCHAR('s'));
			const FRavensightTypedEvent Event = RavensightErrors::BuildEvent(Error, FString());
			TestEqual("message", Text(Event, TEXT("rs_message")).Len(), RavensightErrors::MessageChars);
			TestEqual("stack", Text(Event, TEXT("rs_stack")).Len(), RavensightErrors::StackChars);
			TestEqual("type", Text(Event, TEXT("rs_error_type")).Len(), RavensightErrors::TypeChars);
		});

		It("drops a storm on the device: 3 of one error, 30 in all", [this]()
		{
			FRavensightErrorPipeline Pipeline;
			Pipeline.Start(0.0);
			Pipeline.Answer(true, false);
			FRavensightTypedEvent Out;
			int32 Sent = 0;
			for (int32 Index = 0; Index < 1000; ++Index)
			{
				Sent += Pipeline.OnLine(ERavensightLogLevel::Error, TEXT("LogGame"), FString::Printf(TEXT("Actor %d failed"), Index), Index * 0.01, Out) ? 1 : 0;
			}
			TestEqual("same template", Sent, 3);
			int32 Many = 0;
			for (int32 Index = 0; Index < 1000; ++Index)
			{
				Many += Pipeline.OnCapture(Make(TEXT("x"), FString::Printf(TEXT("Type%d"), Index)), 100.0 + Index * 60.0, Out) ? 1 : 0;
			}
			TestEqual("session cap", Many, RavensightErrors::MaxCaptures - 3);
		});
	});
}

#endif
