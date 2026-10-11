// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Automation spec for the pure protocol decisions in RavensightProtocol.h and
// the request plans FRavensightQueue hands the subsystem.
//
// Run it from the editor with Tools > Session Frontend > Automation, filter
// "Ravensight", or headless:
//
//   UnrealEditor-Cmd <YourProject>.uproject -ExecCmds="Automation RunTests Ravensight; Quit" -unattended -nopause -nullrhi
//
// The "fake transport" cases below drive FRavensightQueue and ClassifyResponse
// together the way URavensightSubsystem does, with a scripted server instead
// of HTTP and an explicit clock instead of real time.

#include "Misc/AutomationTest.h"
#include "RavensightProtocol.h"
#include "RavensightQueue.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace RavensightProtocolSpecHelpers
{
	FRavensightResponseVerdict Answer(ERavensightEndpoint Endpoint, int32 Status, const FString& Error = FString(), int32 BatchSize = 0, int32 BatchUnauthorizedInARow = 0, bool bRetryAfter = false)
	{
		FRavensightResponseInput Input;
		Input.Endpoint = Endpoint;
		Input.Status = Status;
		Input.Error = Error;
		Input.BatchSize = BatchSize;
		Input.ConsecutiveBatchUnauthorized = BatchUnauthorizedInARow;
		Input.bHasRetryAfter = bRetryAfter;
		return RavensightProtocol::ClassifyResponse(Input);
	}
}

BEGIN_DEFINE_SPEC(
	FRavensightProtocolSpec,
	"Ravensight.Protocol",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	TUniquePtr<FRavensightQueue> Queue;

	/** Unix seconds. Tests advance it by assignment. */
	double Now = 1'700'000'000.0;

	void Fill(int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Queue->Enqueue(FRavensightEvent(FString::Printf(TEXT("ev_%d"), Index), TEXT("{}"), 1));
		}
	}

	bool IsAction(const FRavensightResponseVerdict& Verdict, ERavensightResponseAction Action)
	{
		return Verdict.Action == Action;
	}

END_DEFINE_SPEC(FRavensightProtocolSpec)

void FRavensightProtocolSpec::Define()
{
	using namespace RavensightProtocolSpecHelpers;

	BeforeEach([this]()
	{
		Queue = MakeUnique<FRavensightQueue>();
		Now = 1'700'000'000.0;
	});

	AfterEach([this]()
	{
		Queue.Reset();
	});

	Describe("response classification", [this]()
	{
		It("accepts any 2xx", [this]()
		{
			TestTrue(TEXT("202 batch"), IsAction(Answer(ERavensightEndpoint::Batch, 202), ERavensightResponseAction::Accept));
			TestTrue(TEXT("201 session"), IsAction(Answer(ERavensightEndpoint::Session, 201), ERavensightResponseAction::Accept));
			TestTrue(TEXT("200 settings"), IsAction(Answer(ERavensightEndpoint::Settings, 200), ERavensightResponseAction::Accept));
		});

		It("treats a settings 401 as a refused key, never as tracking enabled", [this]()
		{
			const FRavensightResponseVerdict Verdict = Answer(ERavensightEndpoint::Settings, 401, TEXT("invalid_api_key"));
			TestTrue(TEXT("fatal key"), IsAction(Verdict, ERavensightResponseAction::FatalKey));
			TestEqual(TEXT("reason"), Verdict.Reason, FString(RavensightReason::InvalidKey));
		});

		It("treats a session 401 or an unexplained 403 as a refused key", [this]()
		{
			TestTrue(TEXT("session 401"), IsAction(Answer(ERavensightEndpoint::Session, 401), ERavensightResponseAction::FatalKey));
			TestTrue(TEXT("session 403"), IsAction(Answer(ERavensightEndpoint::Session, 403, TEXT("forbidden")), ERavensightResponseAction::FatalKey));
			TestTrue(TEXT("suggestions 401"), IsAction(Answer(ERavensightEndpoint::Suggestions, 401), ERavensightResponseAction::FatalKey));
		});

		It("stops for the run on a 403 tracking_disabled from any endpoint", [this]()
		{
			const TArray<ERavensightEndpoint> Endpoints = { ERavensightEndpoint::Settings, ERavensightEndpoint::Session, ERavensightEndpoint::Batch, ERavensightEndpoint::Feedback };
			for (const ERavensightEndpoint Endpoint : Endpoints)
			{
				const FRavensightResponseVerdict Verdict = Answer(Endpoint, 403, TEXT("tracking_disabled"));
				TestTrue(TEXT("kill switch"), IsAction(Verdict, ERavensightResponseAction::KillSwitch));
				TestEqual(TEXT("reason"), Verdict.Reason, FString(RavensightReason::TrackingDisabled));
			}
		});

		It("stops a Playtest run on playtest_job_closed and invalid_playtest_token", [this]()
		{
			const FRavensightResponseVerdict Closed = Answer(ERavensightEndpoint::Batch, 403, TEXT("playtest_job_closed"));
			TestTrue(TEXT("closed stops"), IsAction(Closed, ERavensightResponseAction::PlaytestStop));
			TestEqual(TEXT("closed reason"), Closed.Reason, FString(RavensightReason::PlaytestJobClosed));

			const FRavensightResponseVerdict Invalid = Answer(ERavensightEndpoint::Session, 403, TEXT("invalid_playtest_token"));
			TestTrue(TEXT("invalid stops"), IsAction(Invalid, ERavensightResponseAction::PlaytestStop));
			TestEqual(TEXT("invalid reason"), Invalid.Reason, FString(RavensightReason::SessionFailed));
		});

		It("re opens the session on a batch 401 at most twice in a row, then backs off", [this]()
		{
			TestTrue(TEXT("first"), IsAction(Answer(ERavensightEndpoint::Batch, 401, FString(), 10, 1), ERavensightResponseAction::RefreshSession));
			TestTrue(TEXT("second"), IsAction(Answer(ERavensightEndpoint::Batch, 401, FString(), 10, 2), ERavensightResponseAction::RefreshSession));
			TestTrue(TEXT("third"), IsAction(Answer(ERavensightEndpoint::Batch, 401, FString(), 10, 3), ERavensightResponseAction::BackOff));
		});

		It("waits Retry-After on a 429, and on a 503 that sends one", [this]()
		{
			const FRavensightResponseVerdict Limited = Answer(ERavensightEndpoint::Session, 429);
			TestTrue(TEXT("429 waits"), IsAction(Limited, ERavensightResponseAction::WaitRetryAfter));
			TestEqual(TEXT("429 reason"), Limited.Reason, FString(RavensightReason::RateLimited));

			TestTrue(TEXT("503 with header"), IsAction(Answer(ERavensightEndpoint::Batch, 503, TEXT("storage_unavailable"), 5, 0, true), ERavensightResponseAction::WaitRetryAfter));
			TestTrue(TEXT("503 without header"), IsAction(Answer(ERavensightEndpoint::Batch, 503, FString(), 5, 0, false), ERavensightResponseAction::BackOff));
		});

		It("backs off when offline or on a server error, naming the reason on session", [this]()
		{
			const FRavensightResponseVerdict Offline = Answer(ERavensightEndpoint::Session, 0);
			TestTrue(TEXT("offline backs off"), IsAction(Offline, ERavensightResponseAction::BackOff));
			TestEqual(TEXT("offline reason"), Offline.Reason, FString(RavensightReason::Offline));

			const FRavensightResponseVerdict ServerError = Answer(ERavensightEndpoint::Session, 500);
			TestTrue(TEXT("500 backs off"), IsAction(ServerError, ERavensightResponseAction::BackOff));
			TestEqual(TEXT("500 reason"), ServerError.Reason, FString(RavensightReason::SessionFailed));
		});

		It("halves a batch the server answers 400 and drops a single event it still refuses", [this]()
		{
			TestTrue(TEXT("50 halves"), IsAction(Answer(ERavensightEndpoint::Batch, 400, TEXT("payload_too_large"), 50), ERavensightResponseAction::HalveBatch));
			TestTrue(TEXT("2 halves"), IsAction(Answer(ERavensightEndpoint::Batch, 400, FString(), 2), ERavensightResponseAction::HalveBatch));
			TestTrue(TEXT("1 drops"), IsAction(Answer(ERavensightEndpoint::Batch, 400, FString(), 1), ERavensightResponseAction::DropHead));
		});

		It("reports feedback and suggestion failures to the caller instead of retrying", [this]()
		{
			TestTrue(TEXT("feedback 400"), IsAction(Answer(ERavensightEndpoint::Feedback, 400, TEXT("invalid_rating")), ERavensightResponseAction::Fail));
			TestTrue(TEXT("feedback 429"), IsAction(Answer(ERavensightEndpoint::Feedback, 429), ERavensightResponseAction::Fail));
			TestTrue(TEXT("feedback 401 refreshes"), IsAction(Answer(ERavensightEndpoint::Feedback, 401), ERavensightResponseAction::RefreshSession));
			TestTrue(TEXT("suggestions offline"), IsAction(Answer(ERavensightEndpoint::Suggestions, 0), ERavensightResponseAction::Fail));
		});
	});

	Describe("the API URL", [this]()
	{
		It("accepts every documented form", [this]()
		{
			const FString Expected = TEXT("https://api.example.com/api/v1");
			TestEqual(TEXT("bare host"), RavensightProtocol::NormalizeApiUrl(TEXT("https://api.example.com")), Expected);
			TestEqual(TEXT("trailing slash"), RavensightProtocol::NormalizeApiUrl(TEXT("https://api.example.com/")), Expected);
			TestEqual(TEXT("several slashes"), RavensightProtocol::NormalizeApiUrl(TEXT("https://api.example.com///")), Expected);
			TestEqual(TEXT("versioned"), RavensightProtocol::NormalizeApiUrl(TEXT("https://api.example.com/api/v1")), Expected);
			TestEqual(TEXT("versioned slash"), RavensightProtocol::NormalizeApiUrl(TEXT("https://api.example.com/api/v1/")), Expected);
			TestEqual(TEXT("whitespace"), RavensightProtocol::NormalizeApiUrl(TEXT("  https://api.example.com/api/v1/  ")), Expected);
		});

		It("defaults to the bare production host", [this]()
		{
			TestEqual(TEXT("empty"), RavensightProtocol::NormalizeApiUrl(FString()), FString(TEXT("https://api.ravensight.io/api/v1")));
			TestEqual(TEXT("blank"), RavensightProtocol::NormalizeApiUrl(TEXT("   ")), FString(TEXT("https://api.ravensight.io/api/v1")));
		});
	});

	Describe("feedback validation", [this]()
	{
		It("lists exactly the six categories", [this]()
		{
			const TArray<FString>& Categories = RavensightProtocol::FeedbackCategories();
			TestEqual(TEXT("count"), Categories.Num(), 6);
			const TArray<FString> Expected = { TEXT("bug"), TEXT("suggestion"), TEXT("complaint"), TEXT("praise"), TEXT("playtest"), TEXT("other") };
			for (const FString& Name : Expected)
			{
				TestTrue(*Name, Categories.Contains(Name));
			}
		});

		It("accepts a valid call and treats empty category and zero rating as omitted", [this]()
		{
			TestEqual(TEXT("full"), RavensightProtocol::ValidateFeedback(TEXT("Jump feels late"), TEXT("bug"), 4), FString());
			TestEqual(TEXT("omitted"), RavensightProtocol::ValidateFeedback(TEXT("Nice"), FString(), 0), FString());
		});

		It("refuses locally what the server would refuse", [this]()
		{
			TestEqual(TEXT("empty message"), RavensightProtocol::ValidateFeedback(TEXT("   "), TEXT("bug"), 0), FString(TEXT("empty_message")));
			TestEqual(TEXT("bad category"), RavensightProtocol::ValidateFeedback(TEXT("x"), TEXT("feature"), 0), FString(TEXT("invalid_category")));
			TestEqual(TEXT("wrong case"), RavensightProtocol::ValidateFeedback(TEXT("x"), TEXT("Bug"), 0), FString(TEXT("invalid_category")));
			TestEqual(TEXT("rating 6"), RavensightProtocol::ValidateFeedback(TEXT("x"), TEXT("bug"), 6), FString(TEXT("invalid_rating")));
			TestEqual(TEXT("rating -1"), RavensightProtocol::ValidateFeedback(TEXT("x"), TEXT("bug"), -1), FString(TEXT("invalid_rating")));
		});
	});

	Describe("lifecycle once-guards", [this]()
	{
		It("lets game_started through once however often the start path runs", [this]()
		{
			FRavensightLifecycleGuards Guards;
			TestTrue(TEXT("first"), Guards.TryClaimStarted());
			TestFalse(TEXT("repeated settings check"), Guards.TryClaimStarted());
			TestFalse(TEXT("opt in again"), Guards.TryClaimStarted());
		});

		It("lets game_exited through once", [this]()
		{
			FRavensightLifecycleGuards Guards;
			TestTrue(TEXT("first"), Guards.TryClaimExited());
			TestFalse(TEXT("second quit hook"), Guards.TryClaimExited());
		});

		It("allows a new game_started for a new session, but never a second game_exited", [this]()
		{
			FRavensightLifecycleGuards Guards;
			Guards.TryClaimStarted();
			Guards.TryClaimExited();
			Guards.ResetForNewSession();
			TestTrue(TEXT("new session starts"), Guards.TryClaimStarted());
			TestFalse(TEXT("exit stays claimed"), Guards.TryClaimExited());
		});
	});

	Describe("the mobile 30 minute rule", [this]()
	{
		It("ends the session after 30 minutes in the background and not before", [this]()
		{
			const double Timeout = RavensightProtocol::DefaultSessionTimeoutSeconds;
			TestFalse(TEXT("29 minutes"), RavensightProtocol::ShouldEndSessionAfterBackground(Now + 29.0 * 60.0, Now, Timeout));
			TestTrue(TEXT("30 minutes"), RavensightProtocol::ShouldEndSessionAfterBackground(Now + 30.0 * 60.0, Now, Timeout));
			TestFalse(TEXT("never backgrounded"), RavensightProtocol::ShouldEndSessionAfterBackground(Now, 0.0, Timeout));
		});

		It("reports a marker exit only for an unexpired token and a player who has not opted out", [this]()
		{
			TestTrue(TEXT("live marker"), RavensightProtocol::ShouldReportMarkerExit(Now, TEXT("tok"), Now + 60.0, false));
			TestFalse(TEXT("expired"), RavensightProtocol::ShouldReportMarkerExit(Now, TEXT("tok"), Now - 1.0, false));
			TestFalse(TEXT("opted out"), RavensightProtocol::ShouldReportMarkerExit(Now, TEXT("tok"), Now + 60.0, true));
			TestFalse(TEXT("no marker"), RavensightProtocol::ShouldReportMarkerExit(Now, FString(), Now + 60.0, false));
		});
	});

	Describe("the Playtest context", [this]()
	{
		It("takes the explicit value first, then the environment, then the command line", [this]()
		{
			RavensightProtocol::FPlaytestSources Sources;
			Sources.CommandLineToken = TEXT("cli");
			Sources.CommandLineRunId = TEXT("run_cli");
			TestEqual(TEXT("command line"), RavensightProtocol::ResolvePlaytest(Sources).Token, FString(TEXT("cli")));

			Sources.EnvToken = TEXT("env");
			Sources.EnvRunId = TEXT("run_env");
			TestEqual(TEXT("environment"), RavensightProtocol::ResolvePlaytest(Sources).Token, FString(TEXT("env")));

			Sources.ExplicitToken = TEXT("explicit");
			Sources.ExplicitRunId = TEXT("run_explicit");
			const RavensightProtocol::FPlaytestResolution Resolved = RavensightProtocol::ResolvePlaytest(Sources);
			TestEqual(TEXT("explicit token"), Resolved.Token, FString(TEXT("explicit")));
			TestEqual(TEXT("pt- device id"), Resolved.DeviceId(), FString(TEXT("pt-run_explicit")));
		});

		It("flags a token without a run id so it never runs as a real player", [this]()
		{
			RavensightProtocol::FPlaytestSources Sources;
			Sources.EnvToken = TEXT("env");
			const RavensightProtocol::FPlaytestResolution Resolved = RavensightProtocol::ResolvePlaytest(Sources);
			TestTrue(TEXT("is a Playtest run"), Resolved.IsPlaytest());
			TestTrue(TEXT("missing run id"), Resolved.IsMissingRunId());
		});

		It("is not a Playtest run with no token anywhere", [this]()
		{
			TestFalse(TEXT("ordinary run"), RavensightProtocol::ResolvePlaytest(RavensightProtocol::FPlaytestSources()).IsPlaytest());
		});
	});

	Describe("request plans while tracking is off", [this]()
	{
		It("yields no request plan when the Enabled setting is off", [this]()
		{
			Queue->SetSession(TEXT("tok"), Now + 3600.0);
			Fill(5);
			Queue->SetEnabled(false);

			TestEqual(TEXT("queue cleared"), Queue->Num(), 0);
			Fill(5);
			TestEqual(TEXT("nothing queued while disabled"), Queue->Num(), 0);
			TestTrue(TEXT("no plan"), Queue->BeginFlush(Now, true).Decision == ERavensightFlushDecision::Disabled);
		});

		It("yields no request plan, not even a session request, for an opted out player", [this]()
		{
			Queue->SetLocallyEnabled(false);
			Fill(5);

			const FRavensightFlushPlan Plan = Queue->BeginFlush(Now, true);
			TestTrue(TEXT("disabled, not NeedsSession"), Plan.Decision == ERavensightFlushDecision::Disabled);
			TestFalse(TEXT("nothing in flight"), Queue->IsFlushInProgress());
		});
	});

	Describe("a fake transport over the queue", [this]()
	{
		It("splits a batch around one event the server refuses and keeps draining", [this]()
		{
			Queue->SetSession(TEXT("tok"), Now + 3600.0);
			Fill(50);

			int32 Limit = RavensightLimits::MaxBatchSize;
			int32 Requests = 0;
			const FString Poison = TEXT("ev_0");

			while (!Queue->IsEmpty() && Requests < 100)
			{
				const FRavensightFlushPlan Plan = Queue->BeginFlush(Now, false, Limit);
				if (!Plan.ShouldSend())
				{
					AddError(TEXT("expected a batch to send"));
					break;
				}
				++Requests;
				Queue->EndFlush();

				bool bHasPoison = false;
				for (const FRavensightEvent& Event : Plan.Batch)
				{
					bHasPoison |= Event.EventName == Poison;
				}

				const FRavensightResponseVerdict Verdict = Answer(ERavensightEndpoint::Batch, bHasPoison ? 400 : 202, FString(), Plan.Batch.Num());
				switch (Verdict.Action)
				{
				case ERavensightResponseAction::Accept:
					Queue->OnBatchAccepted(Plan.Batch.Num());
					Limit = RavensightLimits::MaxBatchSize;
					break;
				case ERavensightResponseAction::HalveBatch:
					Limit = FMath::Max(1, Plan.Batch.Num() / 2);
					break;
				case ERavensightResponseAction::DropHead:
					Queue->DropHead(1);
					Limit = RavensightLimits::MaxBatchSize;
					break;
				default:
					AddError(TEXT("unexpected action"));
					break;
				}
			}

			TestTrue(TEXT("drained"), Queue->IsEmpty());
			TestEqual(TEXT("49 sent"), Queue->GetStats().Sent, 49);
			TestEqual(TEXT("1 dropped"), Queue->GetStats().Dropped, 1);
			// 50, 25, 12, 6, 3, 1 refused; drop; then 49 in one batch.
			TestEqual(TEXT("requests"), Requests, 7);
		});

		It("stops re opening the session after two batch 401s in a row", [this]()
		{
			Fill(3);
			int32 InARow = 0;
			int32 SessionOpens = 0;

			for (int32 Attempt = 0; Attempt < 5; ++Attempt)
			{
				Queue->SetSession(TEXT("tok"), Now + 3600.0);
				const FRavensightFlushPlan Plan = Queue->BeginFlush(Now, false);
				if (!Plan.ShouldSend())
				{
					break;
				}
				Queue->EndFlush();

				++InARow;
				const FRavensightResponseVerdict Verdict = Answer(ERavensightEndpoint::Batch, 401, FString(), Plan.Batch.Num(), InARow);
				Queue->OnUnauthorized();
				if (Verdict.Action == ERavensightResponseAction::RefreshSession)
				{
					++SessionOpens;
					continue;
				}

				TestTrue(TEXT("backs off"), Verdict.Action == ERavensightResponseAction::BackOff);
				Queue->OnTransportFailure(Now);
				break;
			}

			TestEqual(TEXT("two re opens"), SessionOpens, 2);
			TestTrue(TEXT("now waiting"), Queue->SecondsUntilNextAttempt(Now) > 0.0);
			TestEqual(TEXT("events kept"), Queue->Num(), 3);
		});

		It("waits exactly Retry-After on a 429", [this]()
		{
			Queue->SetSession(TEXT("tok"), Now + 3600.0);
			Fill(1);
			Queue->BeginFlush(Now, false);
			Queue->EndFlush();

			const FRavensightResponseVerdict Verdict = Answer(ERavensightEndpoint::Batch, 429, TEXT("rate_limited"), 1, 0, true);
			TestTrue(TEXT("waits"), Verdict.Action == ERavensightResponseAction::WaitRetryAfter);
			Queue->OnRateLimited(Now, 17.0);
			TestEqual(TEXT("exact wait"), Queue->SecondsUntilNextAttempt(Now), 17.0);
		});

		It("caps the queue at 500 and a batch at 50", [this]()
		{
			Queue->SetSession(TEXT("tok"), Now + 3600.0);
			Fill(600);
			TestEqual(TEXT("cap"), Queue->Num(), RavensightLimits::DefaultQueueCap);
			TestEqual(TEXT("batch"), Queue->BeginFlush(Now, false).Batch.Num(), RavensightLimits::MaxBatchSize);
		});

		It("drops the head and counts it", [this]()
		{
			Fill(3);
			TestEqual(TEXT("dropped one"), Queue->DropHead(1), 1);
			TestEqual(TEXT("next is ev_1"), Queue->GetEvents()[0].EventName, FString(TEXT("ev_1")));
			TestEqual(TEXT("counted"), Queue->GetStats().Dropped, 1);
			TestEqual(TEXT("never more than queued"), Queue->DropHead(10), 2);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
