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

namespace RavensightQueueSpecRandom
{
	/** The middle of the jitter range: the unjittered ladder. */
	double Middle() { return 0.5; }
	double Lowest() { return 0.0; }
	double Highest() { return 0.999999; }
}

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
		// The exact waits below are the unjittered ladder; the jitter has its
		// own cases.
		Queue->SetRandomSource(&RavensightQueueSpecRandom::Middle);
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

	Describe("errors behind gameplay", [this]()
	{
		It("queues a gameplay event ahead of every queued error", [this]()
		{
			Queue->Enqueue(FRavensightEvent(TEXT("ev_0"), TEXT("{}"), 1));
			Queue->Enqueue(FRavensightEvent(TEXT("error"), TEXT("{}"), 1, true));
			Queue->Enqueue(FRavensightEvent(TEXT("error"), TEXT("{}"), 1, true));
			Queue->Enqueue(FRavensightEvent(TEXT("ev_1"), TEXT("{}"), 1));

			TestEqual(TEXT("first"), Queue->GetEvents()[0].EventName, FString(TEXT("ev_0")));
			TestEqual(TEXT("gameplay jumped the errors"), Queue->GetEvents()[1].EventName, FString(TEXT("ev_1")));
			TestTrue(TEXT("errors last"), Queue->GetEvents()[2].bError && Queue->GetEvents()[3].bError);
		});

		It("never inserts into a batch in flight", [this]()
		{
			GiveSession(3600.0);
			Queue->Enqueue(FRavensightEvent(TEXT("error"), TEXT("{}"), 1, true));
			const FRavensightFlushPlan Plan = Queue->BeginFlush(Now, false);
			TestEqual(TEXT("in flight"), Plan.Batch.Num(), 1);
			Queue->Enqueue(FRavensightEvent(TEXT("ev_0"), TEXT("{}"), 1));
			TestTrue(TEXT("error still first"), Queue->GetEvents()[0].bError);
			Queue->EndFlush();
			Queue->OnBatchAccepted(1);
			TestEqual(TEXT("gameplay left"), Queue->GetEvents()[0].EventName, FString(TEXT("ev_0")));
		});

		It("drops queued errors before any gameplay event when full", [this]()
		{
			Queue->SetMaxQueueSize(3);
			Queue->Enqueue(FRavensightEvent(TEXT("ev_0"), TEXT("{}"), 1));
			Queue->Enqueue(FRavensightEvent(TEXT("error"), TEXT("{}"), 1, true));
			Queue->Enqueue(FRavensightEvent(TEXT("ev_1"), TEXT("{}"), 1));
			Queue->Enqueue(FRavensightEvent(TEXT("ev_2"), TEXT("{}"), 1));

			TestEqual(TEXT("at the cap"), Queue->Num(), 3);
			TestEqual(TEXT("oldest gameplay kept"), Queue->GetEvents()[0].EventName, FString(TEXT("ev_0")));
			TestFalse(TEXT("no error left"), Queue->GetEvents()[2].bError);
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

			// 10 seconds times the jitter factor, 0.75 at the specs' random 0.5.
			TestEqual(TEXT("first wait"), Queue->SecondsUntilNextAttempt(Now), 7.5);
		});

		It("blocks a flush until the window passes, and not even force cuts it short", [this]()
		{
			GiveSession(3600.0);
			Fill(5);
			Queue->OnRateLimited(Now, 30.0);

			const FRavensightFlushPlan Blocked = Queue->BeginFlush(Now, false);
			TestTrue(TEXT("blocked"), Blocked.Decision == ERavensightFlushDecision::BackingOff);

			const FRavensightFlushPlan Forced = Queue->BeginFlush(Now, true);
			TestTrue(TEXT("a Retry-After holds against force"), Forced.Decision == ERavensightFlushDecision::BackingOff);

			Queue->ScheduleBackoff(Now);
			const FRavensightFlushPlan CutShort = Queue->BeginFlush(Now, true);
			TestTrue(TEXT("force cuts an ordinary backoff short"), CutShort.ShouldSend());
			Queue->EndFlush();
			Queue->OnRateLimited(Now, 30.0);

			Now += 31.0;
			const FRavensightFlushPlan Later = Queue->BeginFlush(Now, false);
			TestTrue(TEXT("clear once the window passes"), Later.ShouldSend());
		});
	});

	Describe("the persisted queue's merge", [this]()
	{
		It("puts restored events ahead of this run's and stamps origins only where missing", [this]()
		{
			Queue->Enqueue(FRavensightEvent(TEXT("now"), TEXT("{}"), 2));
			TArray<FRavensightEvent> Restored;
			FRavensightEvent Old(TEXT("old"), TEXT("{}"), 1);
			Old.OriginJson = TEXT("{\"session\":\"ses_old\"}");
			Old.OriginSession = TEXT("ses_old");
			Restored.Add(Old);
			TestEqual(TEXT("restored"), Queue->Restore(Restored), 1);
			TestEqual(TEXT("old first"), Queue->GetEvents()[0].EventName, FString(TEXT("old")));
			Queue->StampOrigins(TEXT("{\"session\":\"ses_new\"}"), TEXT("ses_new"));
			TestEqual(TEXT("kept its own"), Queue->GetEvents()[0].OriginSession, FString(TEXT("ses_old")));
			TestEqual(TEXT("stamped"), Queue->GetEvents()[1].OriginSession, FString(TEXT("ses_new")));
		});

		It("refuses a batch 403 until the next launch, keeping every event", [this]()
		{
			GiveSession(3600.0);
			Fill(2);
			Queue->SetSendBlocked(true);
			TestTrue(TEXT("refused"), Queue->BeginFlush(Now, true).Decision == ERavensightFlushDecision::Refused);
			TestEqual(TEXT("kept"), Queue->Num(), 2);
		});

		It("drops the event a 400 names by index", [this]()
		{
			Fill(3);
			TestEqual(TEXT("one"), Queue->DropAt(1, 1), 1);
			TestEqual(TEXT("ev_2 moved up"), Queue->GetEvents()[1].EventName, FString(TEXT("ev_2")));
			TestEqual(TEXT("out of range"), Queue->DropAt(5, 1), 0);
		});
	});

	Describe("the backoff ladder", [this]()
	{
		It("doubles from 10 seconds and caps at 300", [this]()
		{
			// The ladder doubles from 10 to 300; each wait is the step times the
			// jitter factor, 0.75 at the specs' random 0.5.
			const TArray<double> Expected = { 7.5, 15.0, 30.0, 60.0, 120.0, 225.0, 225.0, 225.0 };

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

	Describe("the retry jitter", [this]()
	{
		It("is a factor from 0.5 to 1, within the floor and the ceiling", [this]()
		{
			TestEqual(TEXT("lowest"), FRavensightQueue::JitteredWaitSeconds(10.0, 0.0), 5.0);
			TestEqual(TEXT("middle"), FRavensightQueue::JitteredWaitSeconds(10.0, 0.5), 7.5);
			const double High = FRavensightQueue::JitteredWaitSeconds(10.0, 0.999999);
			TestTrue(TEXT("highest at most 10"), High <= 10.0 && High > 9.99);
			TestEqual(TEXT("never under the floor"), FRavensightQueue::JitteredWaitSeconds(0.1, 0.0), RavensightLimits::MinRetrySeconds);
			TestTrue(TEXT("never over the ceiling"), FRavensightQueue::JitteredWaitSeconds(300.0, 0.999999) <= RavensightLimits::MaxBackoffSeconds);
		});

		It("jitters each wait while the ladder itself still doubles", [this]()
		{
			Fill(1);
			Queue->SetRandomSource(&RavensightQueueSpecRandom::Lowest);
			Queue->ScheduleBackoff(Now);
			TestEqual(TEXT("first wait"), Queue->SecondsUntilNextAttempt(Now), 5.0);
			Queue->ScheduleBackoff(Now);
			TestEqual(TEXT("second wait"), Queue->SecondsUntilNextAttempt(Now), 10.0);

			Queue->SetRandomSource(&RavensightQueueSpecRandom::Highest);
			Queue->ScheduleBackoff(Now);
			const double Third = Queue->SecondsUntilNextAttempt(Now);
			TestTrue(TEXT("third wait"), Third > 39.9 && Third <= 40.0);
		});

		It("stays in range with the default random source", [this]()
		{
			Queue->SetRandomSource(nullptr);
			for (int32 Round = 0; Round < 200; ++Round)
			{
				Queue->ResetBackoff();
				Queue->ScheduleBackoff(Now);
				const double Wait = Queue->SecondsUntilNextAttempt(Now);
				if (!TestTrue(TEXT("between 5 and 10 seconds"), Wait >= 5.0 && Wait <= 10.0))
				{
					break;
				}
			}
		});

		It("leaves an exact Retry-After alone", [this]()
		{
			Queue->SetRandomSource(&RavensightQueueSpecRandom::Lowest);
			Queue->OnRateLimited(Now, 42.0);
			TestEqual(TEXT("exact"), Queue->SecondsUntilNextAttempt(Now), 42.0);
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
