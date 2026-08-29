// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Automation spec for FRavensightQueue.
//
// Run it from the editor with Tools > Session Frontend > Automation, filter
// "Ravensight", or headless:
//
//   UnrealEditor-Cmd <YourProject>.uproject -ExecCmds="Automation RunTests Ravensight.Queue; Quit" -unattended -nopause -nullrhi
//
// Every case drives the state machine with an explicit clock reading rather
// than real time, so nothing here sleeps and nothing here is flaky.

#include "Misc/AutomationTest.h"
#include "RavensightQueue.h"

#if WITH_DEV_AUTOMATION_TESTS

BEGIN_DEFINE_SPEC(
	FRavensightQueueSpec,
	"Ravensight.Queue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	TUniquePtr<FRavensightQueue> Queue;

	/** Unix seconds. Tests advance it by assignment. */
	double Now = 1'700'000'000.0;

	/** Gives the queue a token valid for the given span from Now. */
	void GiveSession(double ValidForSeconds);

	/** Enqueues Count events named ev_0 .. ev_(Count-1). */
	void Fill(int32 Count);

END_DEFINE_SPEC(FRavensightQueueSpec)

void FRavensightQueueSpec::GiveSession(double ValidForSeconds)
{
	Queue->SetSession(TEXT("tok_test"), Now + ValidForSeconds);
}

void FRavensightQueueSpec::Fill(int32 Count)
{
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Queue->Enqueue(FRavensightEvent(FString::Printf(TEXT("ev_%d"), Index), TEXT("{}"), 1));
	}
}

void FRavensightQueueSpec::Define()
{
	BeforeEach([this]()
	{
		Queue = MakeUnique<FRavensightQueue>();
		Now = 1'700'000'000.0;
	});

	AfterEach([this]()
	{
		Queue.Reset();
	});

	Describe("the offline queue", [this]()
	{
		It("keeps events in the order they were tracked", [this]()
		{
			Fill(3);

			TestEqual(TEXT("queued"), Queue->Num(), 3);
			TestEqual(TEXT("first"), Queue->GetEvents()[0].EventName, FString(TEXT("ev_0")));
			TestEqual(TEXT("last"), Queue->GetEvents()[2].EventName, FString(TEXT("ev_2")));
		});

		It("drops the oldest events once the cap is reached", [this]()
		{
			Queue->SetMaxQueueSize(3);
			Fill(5);

			TestEqual(TEXT("held at the cap"), Queue->Num(), 3);
			TestEqual(TEXT("oldest survivor"), Queue->GetEvents()[0].EventName, FString(TEXT("ev_2")));
			TestEqual(TEXT("newest kept"), Queue->GetEvents()[2].EventName, FString(TEXT("ev_4")));
			TestEqual(TEXT("counted as dropped"), Queue->GetStats().Dropped, 2);
		});

		It("reports how many events an enqueue displaced", [this]()
		{
			Queue->SetMaxQueueSize(2);
			Fill(2);

			const int32 Dropped = Queue->Enqueue(FRavensightEvent(TEXT("late"), TEXT("{}"), 1));
			TestEqual(TEXT("one displaced"), Dropped, 1);
		});

		It("defaults to a cap of 500", [this]()
		{
			TestEqual(TEXT("cap"), Queue->GetMaxQueueSize(), RavensightLimits::DefaultQueueCap);
		});

		It("trims immediately when the cap is lowered", [this]()
		{
			Fill(10);
			Queue->SetMaxQueueSize(4);

			TestEqual(TEXT("trimmed"), Queue->Num(), 4);
			TestEqual(TEXT("kept the newest"), Queue->GetEvents()[3].EventName, FString(TEXT("ev_9")));
		});
	});

	Describe("flush decisions", [this]()
	{
		It("reports an empty queue rather than sending nothing", [this]()
		{
			GiveSession(3600.0);

			const FRavensightFlushPlan Plan = Queue->BeginFlush(Now, false);
			TestTrue(TEXT("decision"), Plan.Decision == ERavensightFlushDecision::Empty);
			TestFalse(TEXT("no flush marked"), Queue->IsFlushInProgress());
		});

		It("asks for a session when there is no valid token", [this]()
		{
			Fill(1);

			const FRavensightFlushPlan Plan = Queue->BeginFlush(Now, false);
			TestTrue(TEXT("decision"), Plan.Decision == ERavensightFlushDecision::NeedsSession);
			TestFalse(TEXT("nothing in flight yet"), Queue->IsFlushInProgress());
		});

		It("treats a token inside the expiry skew as already expired", [this]()
		{
			Fill(1);
			GiveSession(RavensightLimits::SessionExpirySkewSeconds - 1.0);

			const FRavensightFlushPlan Plan = Queue->BeginFlush(Now, false);
			TestTrue(TEXT("decision"), Plan.Decision == ERavensightFlushDecision::NeedsSession);
		});

		It("caps a batch at the 50 event server limit", [this]()
		{
			GiveSession(3600.0);
			Fill(120);

			const FRavensightFlushPlan Plan = Queue->BeginFlush(Now, false);
			TestTrue(TEXT("sending"), Plan.ShouldSend());
			TestEqual(TEXT("batch size"), Plan.Batch.Num(), RavensightLimits::MaxBatchSize);
			TestEqual(TEXT("nothing removed yet"), Queue->Num(), 120);
			TestTrue(TEXT("flush marked"), Queue->IsFlushInProgress());
		});

		It("refuses a second batch while one is in flight", [this]()
		{
			GiveSession(3600.0);
			Fill(60);

			Queue->BeginFlush(Now, false);
			const FRavensightFlushPlan Second = Queue->BeginFlush(Now, false);

			TestTrue(TEXT("decision"), Second.Decision == ERavensightFlushDecision::AlreadyFlushing);
		});

		It("refuses to send while tracking is off", [this]()
		{
			GiveSession(3600.0);
			Fill(5);
			Queue->SetLocallyEnabled(false);

			const FRavensightFlushPlan Plan = Queue->BeginFlush(Now, false);
			TestTrue(TEXT("decision"), Plan.Decision == ERavensightFlushDecision::Disabled);
		});
	});

	Describe("a batch the server accepts", [this]()
	{
		It("removes exactly the events that were sent", [this]()
		{
			GiveSession(3600.0);
			Fill(60);

			const FRavensightFlushPlan Plan = Queue->BeginFlush(Now, false);
			Queue->EndFlush();
			Queue->OnBatchAccepted(Plan.Batch.Num());

			TestEqual(TEXT("remaining"), Queue->Num(), 10);
			TestEqual(TEXT("next up"), Queue->GetEvents()[0].EventName, FString(TEXT("ev_50")));
			TestEqual(TEXT("sent counter"), Queue->GetStats().Sent, 50);
		});

		It("clears any backoff so the next batch goes out immediately", [this]()
		{
			GiveSession(3600.0);
			Fill(5);

			Queue->OnTransportFailure(Now);
			TestTrue(TEXT("backing off"), Queue->SecondsUntilNextAttempt(Now) > 0.0);

			Queue->OnBatchAccepted(0);
			TestEqual(TEXT("window cleared"), Queue->SecondsUntilNextAttempt(Now), 0.0);
			TestEqual(TEXT("ladder reset"), Queue->GetBackoffSeconds(), RavensightLimits::DefaultRetrySeconds);
		});
	});

	Describe("a 401 from the server", [this]()
	{
		It("drops the session but keeps every event queued for a re send", [this]()
		{
			GiveSession(3600.0);
			Fill(20);

			const FRavensightFlushPlan Plan = Queue->BeginFlush(Now, false);
			TestTrue(TEXT("sending"), Plan.ShouldSend());

			Queue->EndFlush();
			Queue->OnUnauthorized();

			TestFalse(TEXT("session gone"), Queue->IsSessionValid(Now));
			TestEqual(TEXT("nothing lost"), Queue->Num(), 20);

			// Once a fresh token arrives the same events go back out.
			GiveSession(3600.0);
			const FRavensightFlushPlan Retry = Queue->BeginFlush(Now, false);
			TestTrue(TEXT("re sending"), Retry.ShouldSend());
			TestEqual(TEXT("same first event"), Retry.Batch[0].EventName, FString(TEXT("ev_0")));
		});
	});

	Describe("a 429 from the server", [this]()
	{
		It("honors a Retry-After header over its own backoff", [this]()
		{
			Fill(1);
			Queue->OnRateLimited(Now, 42.0);

			TestEqual(TEXT("wait"), Queue->SecondsUntilNextAttempt(Now), 42.0);

			// The local ladder is untouched, because the server was explicit.
			TestEqual(TEXT("ladder untouched"), Queue->GetBackoffSeconds(), RavensightLimits::DefaultRetrySeconds);
		});

		It("falls back to exponential backoff with no usable header", [this]()
		{
			Fill(1);
			Queue->OnRateLimited(Now, -1.0);

			TestEqual(TEXT("first wait"), Queue->SecondsUntilNextAttempt(Now), RavensightLimits::DefaultRetrySeconds);
		});

		It("blocks a flush until the window passes, and force overrides it", [this]()
		{
			GiveSession(3600.0);
			Fill(5);
			Queue->OnRateLimited(Now, 30.0);

			const FRavensightFlushPlan Blocked = Queue->BeginFlush(Now, false);
			TestTrue(TEXT("blocked"), Blocked.Decision == ERavensightFlushDecision::BackingOff);

			const FRavensightFlushPlan Forced = Queue->BeginFlush(Now, true);
			TestTrue(TEXT("force wins"), Forced.ShouldSend());
			Queue->EndFlush();

			Now += 31.0;
			const FRavensightFlushPlan Later = Queue->BeginFlush(Now, false);
			TestTrue(TEXT("clear once the window passes"), Later.ShouldSend());
		});
	});

	Describe("the backoff ladder", [this]()
	{
		It("doubles from 10 seconds and caps at 300", [this]()
		{
			const TArray<double> Expected = { 10.0, 20.0, 40.0, 80.0, 160.0, 300.0, 300.0, 300.0 };

			for (int32 Step = 0; Step < Expected.Num(); ++Step)
			{
				Queue->ScheduleBackoff(Now);
				TestEqual(
					*FString::Printf(TEXT("step %d"), Step),
					Queue->SecondsUntilNextAttempt(Now),
					Expected[Step]);
			}
		});

		It("never hands a timer a zero delay", [this]()
		{
			TestEqual(TEXT("floor"), Queue->GetRetryTimerSeconds(Now), RavensightLimits::MinRetrySeconds);
		});

		It("returns to 10 seconds after a success", [this]()
		{
			Queue->ScheduleBackoff(Now);
			Queue->ScheduleBackoff(Now);
			Queue->ScheduleBackoff(Now);
			TestTrue(TEXT("climbed"), Queue->GetBackoffSeconds() > RavensightLimits::DefaultRetrySeconds);

			Queue->ResetBackoff();
			TestEqual(TEXT("reset"), Queue->GetBackoffSeconds(), RavensightLimits::DefaultRetrySeconds);
			TestEqual(TEXT("no window"), Queue->SecondsUntilNextAttempt(Now), 0.0);
		});
	});

	Describe("the kill switch and the local opt out", [this]()
	{
		It("discards the queue and refuses new events when the server turns tracking off", [this]()
		{
			Fill(10);
			Queue->SetServerTrackingEnabled(false);

			TestEqual(TEXT("queue emptied"), Queue->Num(), 0);
			TestFalse(TEXT("inactive"), Queue->IsActive());

			Queue->Enqueue(FRavensightEvent(TEXT("after"), TEXT("{}"), 1));
			TestEqual(TEXT("still empty"), Queue->Num(), 0);
		});

		It("discards the queue on a local opt out", [this]()
		{
			Fill(10);
			Queue->SetLocallyEnabled(false);

			TestEqual(TEXT("queue emptied"), Queue->Num(), 0);
			TestEqual(TEXT("counted as dropped"), Queue->GetStats().Dropped, 10);
		});

		It("tracks again after opting back in", [this]()
		{
			Queue->SetLocallyEnabled(false);
			Queue->SetLocallyEnabled(true);

			Queue->Enqueue(FRavensightEvent(TEXT("welcome_back"), TEXT("{}"), 1));
			TestEqual(TEXT("queued"), Queue->Num(), 1);
		});

		It("needs both switches on to be active", [this]()
		{
			Queue->SetServerTrackingEnabled(false);
			Queue->SetLocallyEnabled(true);
			TestFalse(TEXT("server still off"), Queue->IsActive());

			Queue->SetServerTrackingEnabled(true);
			TestTrue(TEXT("both on"), Queue->IsActive());
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
