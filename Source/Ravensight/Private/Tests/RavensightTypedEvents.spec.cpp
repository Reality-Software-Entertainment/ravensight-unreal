// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Automation spec for the typed event helpers in RavensightTypedEvents.h:
// the exact property set of each helper, the currency upper casing, the
// message and stack cuts, the reserved keys winning over caller keys, the
// default event names and the override, and the health sampler (percentiles
// from a known sample set, the 600 sample bound, one send per session and
// nothing sent without samples).
//
// Run it from the editor with Tools > Session Frontend > Automation, filter
// "Ravensight.TypedEvents", or headless:
//
//   UnrealEditor-Cmd <YourProject>.uproject -ExecCmds="Automation RunTests Ravensight; Quit" -unattended -nopause -nullrhi
//
// The "send" cases drive the sampler the way URavensightSubsystem does: one
// trigger on background entry and one on quit, counted by a stand in sender.

#include "Misc/AutomationTest.h"
#include "RavensightProtocol.h"
#include "RavensightTypedEvents.h"

#include <cmath>

#if WITH_DEV_AUTOMATION_TESTS

namespace RavensightTypedSpecHelpers
{
	bool SameText(const FString& A, const FString& B)
	{
		return A.Equals(B, ESearchCase::CaseSensitive);
	}

	bool IsText(const FRavensightTypedEvent& Event, const FString& Key, const FString& Expected)
	{
		const FRavensightProp* Prop = Event.Find(Key);
		return Prop && Prop->Kind == ERavensightPropKind::String && SameText(Prop->Text, Expected);
	}

	bool IsNumber(const FRavensightTypedEvent& Event, const FString& Key, double Expected)
	{
		const FRavensightProp* Prop = Event.Find(Key);
		return Prop && Prop->Kind == ERavensightPropKind::Number && std::fabs(Prop->Number - Expected) < 1e-9;
	}

	TArray<FRavensightProp> NoExtra()
	{
		return TArray<FRavensightProp>();
	}

	/** The stand in for the subsystem's SendHealthOnce: claims, and counts what would be sent. */
	struct FFakeHealthSender
	{
		int32 Sent = 0;
		FRavensightHealthSummary Last;

		void Trigger(FRavensightHealthSampler& Sampler)
		{
			FRavensightHealthSummary Summary;
			if (Sampler.TryClaimSend(Summary))
			{
				Last = Summary;
				++Sent;
			}
		}
	};
}

BEGIN_DEFINE_SPEC(
	FRavensightTypedEventsSpec,
	"Ravensight.TypedEvents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

END_DEFINE_SPEC(FRavensightTypedEventsSpec)

void FRavensightTypedEventsSpec::Define()
{
	using namespace RavensightTypedSpecHelpers;

	Describe("sdk tag", [this]()
	{
		It("is unreal/ plus the plugin version", [this]()
		{
			TestTrue("tag", SameText(RavensightVersion::SdkTag(), FString(TEXT("unreal/")) + FString(RavensightVersion::Sdk)));
			TestTrue("version is 0.3.1", SameText(RavensightVersion::Sdk, TEXT("0.3.1")));
		});
	});

	Describe("purchase", [this]()
	{
		It("sets exactly the reserved keys, with a number amount", [this]()
		{
			const FRavensightTypedEvent Event = RavensightTyped::BuildPurchase(
				false, 499, FString(), TEXT("USD"), TEXT("pack"), TEXT("gems_100"), TEXT("cart1"), TEXT("tx9"), NoExtra(), FString());
			TestTrue("name", SameText(Event.Name, TEXT("purchase")));
			TestEqual("count", Event.Props.Num(), 7);
			TestTrue("kind", IsText(Event, TEXT("rs_kind"), TEXT("purchase")));
			TestTrue("amount is a number", IsNumber(Event, TEXT("rs_amount"), 499));
			TestTrue("currency", IsText(Event, TEXT("rs_currency"), TEXT("USD")));
			TestTrue("item type", IsText(Event, TEXT("rs_item_type"), TEXT("pack")));
			TestTrue("item id", IsText(Event, TEXT("rs_item_id"), TEXT("gems_100")));
			TestTrue("cart", IsText(Event, TEXT("rs_cart"), TEXT("cart1")));
			TestTrue("transaction", IsText(Event, TEXT("rs_transaction_id"), TEXT("tx9")));
		});

		It("leaves out an empty cart and transaction id", [this]()
		{
			const FRavensightTypedEvent Event = RavensightTyped::BuildPurchase(
				false, 1, FString(), TEXT("USD"), TEXT("pack"), TEXT("a"), FString(), FString(), NoExtra(), FString());
			TestEqual("count", Event.Props.Num(), 5);
			TestTrue("no cart", Event.Find(TEXT("rs_cart")) == nullptr);
			TestTrue("no transaction", Event.Find(TEXT("rs_transaction_id")) == nullptr);
		});

		It("upper cases the currency", [this]()
		{
			const FRavensightTypedEvent Event = RavensightTyped::BuildPurchase(
				false, 1, FString(), TEXT("eur"), TEXT("pack"), TEXT("a"), FString(), FString(), NoExtra(), FString());
			TestTrue("EUR", IsText(Event, TEXT("rs_currency"), TEXT("EUR")));
		});

		It("passes a digit string amount through as a string", [this]()
		{
			const FRavensightTypedEvent Event = RavensightTyped::BuildPurchase(
				true, 0, TEXT("1234567890123"), TEXT("USD"), TEXT("pack"), TEXT("a"), FString(), FString(), NoExtra(), FString());
			TestTrue("string amount", IsText(Event, TEXT("rs_amount"), TEXT("1234567890123")));
		});

		It("passes bad input through without changing it", [this]()
		{
			const FRavensightTypedEvent Event = RavensightTyped::BuildPurchase(
				true, 0, TEXT("4.99"), TEXT("dollars"), FString(), FString(), FString(), FString(), NoExtra(), FString());
			TestTrue("amount untouched", IsText(Event, TEXT("rs_amount"), TEXT("4.99")));
			TestTrue("currency upper cased only", IsText(Event, TEXT("rs_currency"), TEXT("DOLLARS")));
			TestTrue("empty item type kept", IsText(Event, TEXT("rs_item_type"), FString()));
		});
	});

	Describe("resource", [this]()
	{
		It("sets exactly the reserved keys for a source", [this]()
		{
			const FRavensightTypedEvent Event = RavensightTyped::BuildResource(
				TEXT("source"), TEXT("gold"), 12.5, TEXT("quest"), TEXT("q1"), NoExtra(), FString());
			TestTrue("default name", SameText(Event.Name, TEXT("resource_source")));
			TestEqual("count", Event.Props.Num(), 6);
			TestTrue("kind", IsText(Event, TEXT("rs_kind"), TEXT("resource")));
			TestTrue("flow", IsText(Event, TEXT("rs_flow"), TEXT("source")));
			TestTrue("resource", IsText(Event, TEXT("rs_resource"), TEXT("gold")));
			TestTrue("amount is a number", IsNumber(Event, TEXT("rs_amount"), 12.5));
			TestTrue("item type", IsText(Event, TEXT("rs_item_type"), TEXT("quest")));
			TestTrue("item id", IsText(Event, TEXT("rs_item_id"), TEXT("q1")));
		});

		It("names a sink resource_sink and leaves out an empty item", [this]()
		{
			const FRavensightTypedEvent Event = RavensightTyped::BuildResource(
				TEXT("sink"), TEXT("gold"), 3, FString(), FString(), NoExtra(), FString());
			TestTrue("default name", SameText(Event.Name, TEXT("resource_sink")));
			TestEqual("count", Event.Props.Num(), 4);
			TestTrue("no item type", Event.Find(TEXT("rs_item_type")) == nullptr);
			TestTrue("no item id", Event.Find(TEXT("rs_item_id")) == nullptr);
		});
	});

	Describe("progression", [this]()
	{
		It("sets exactly the reserved keys with a score", [this]()
		{
			const FRavensightTypedEvent Event = RavensightTyped::BuildProgression(
				TEXT("complete"), TEXT("world1"), TEXT("level2"), TEXT("boss"), true, 1500, NoExtra(), FString());
			TestTrue("default name", SameText(Event.Name, TEXT("progression_complete")));
			TestEqual("count", Event.Props.Num(), 6);
			TestTrue("kind", IsText(Event, TEXT("rs_kind"), TEXT("progression")));
			TestTrue("status", IsText(Event, TEXT("rs_status"), TEXT("complete")));
			TestTrue("p1", IsText(Event, TEXT("rs_p1"), TEXT("world1")));
			TestTrue("p2", IsText(Event, TEXT("rs_p2"), TEXT("level2")));
			TestTrue("p3", IsText(Event, TEXT("rs_p3"), TEXT("boss")));
			TestTrue("score is a number", IsNumber(Event, TEXT("rs_score"), 1500));
		});

		It("leaves out p2, p3 and the score when not given", [this]()
		{
			const FRavensightTypedEvent Event = RavensightTyped::BuildProgression(
				TEXT("start"), TEXT("world1"), FString(), FString(), false, 0, NoExtra(), FString());
			TestTrue("default name", SameText(Event.Name, TEXT("progression_start")));
			TestEqual("count", Event.Props.Num(), 3);
			TestTrue("no score", Event.Find(TEXT("rs_score")) == nullptr);
		});

		It("names a failure progression_fail", [this]()
		{
			const FRavensightTypedEvent Event = RavensightTyped::BuildProgression(
				TEXT("fail"), TEXT("w"), FString(), FString(), false, 0, NoExtra(), FString());
			TestTrue("default name", SameText(Event.Name, TEXT("progression_fail")));
		});
	});

	Describe("error", [this]()
	{
		It("sets exactly the reserved keys", [this]()
		{
			const FRavensightTypedEvent Event = RavensightTyped::BuildError(
				TEXT("warning"), TEXT("shader missing"), TEXT("at A\nat B"), NoExtra(), FString());
			TestTrue("default name", SameText(Event.Name, TEXT("error")));
			TestEqual("count", Event.Props.Num(), 4);
			TestTrue("kind", IsText(Event, TEXT("rs_kind"), TEXT("error")));
			TestTrue("severity", IsText(Event, TEXT("rs_severity"), TEXT("warning")));
			TestTrue("message", IsText(Event, TEXT("rs_message"), TEXT("shader missing")));
			TestTrue("stack", IsText(Event, TEXT("rs_stack"), TEXT("at A\nat B")));
		});

		It("cuts the message to 200 characters and the stack to 2000", [this]()
		{
			const FString Long300 = FString::ChrN(300, TEXT('m'));
			const FString Long2500 = FString::ChrN(2500, TEXT('s'));
			const FRavensightTypedEvent Event = RavensightTyped::BuildError(TEXT("error"), Long300, Long2500, NoExtra(), FString());
			TestEqual("message length", Event.Find(TEXT("rs_message"))->Text.Len(), 200);
			TestEqual("stack length", Event.Find(TEXT("rs_stack"))->Text.Len(), 2000);
		});

		It("keeps a message and stack at the limit whole", [this]()
		{
			const FString Exact = FString::ChrN(200, TEXT('m'));
			const FRavensightTypedEvent Event = RavensightTyped::BuildError(TEXT("error"), Exact, FString(), NoExtra(), FString());
			TestEqual("message length", Event.Find(TEXT("rs_message"))->Text.Len(), 200);
			TestTrue("empty stack left out", Event.Find(TEXT("rs_stack")) == nullptr);
		});
	});

	Describe("extra properties and the name override", [this]()
	{
		It("merges caller keys first, so the reserved keys win", [this]()
		{
			TArray<FRavensightProp> Extra;
			Extra.Add(FRavensightProp::MakeString(TEXT("level"), TEXT("castle")));
			Extra.Add(FRavensightProp::MakeString(TEXT("rs_kind"), TEXT("death")));
			Extra.Add(FRavensightProp::MakeNumber(TEXT("rs_amount"), 1));
			const FRavensightTypedEvent Event = RavensightTyped::BuildPurchase(
				false, 499, FString(), TEXT("USD"), TEXT("pack"), TEXT("a"), FString(), FString(), Extra, FString());
			TestTrue("caller key kept", IsText(Event, TEXT("level"), TEXT("castle")));
			TestTrue("reserved kind wins", IsText(Event, TEXT("rs_kind"), TEXT("purchase")));
			TestTrue("reserved amount wins", IsNumber(Event, TEXT("rs_amount"), 499));
			TestEqual("no duplicate keys", Event.Props.Num(), 6);
		});

		It("treats keys as case sensitive", [this]()
		{
			TArray<FRavensightProp> Extra;
			Extra.Add(FRavensightProp::MakeString(TEXT("RS_KIND"), TEXT("custom")));
			const FRavensightTypedEvent Event = RavensightTyped::BuildError(TEXT("info"), TEXT("m"), FString(), Extra, FString());
			TestTrue("upper case key is the caller's", IsText(Event, TEXT("RS_KIND"), TEXT("custom")));
			TestTrue("reserved key set", IsText(Event, TEXT("rs_kind"), TEXT("error")));
		});

		It("uses the event name override on every helper", [this]()
		{
			TestTrue("purchase", SameText(RavensightTyped::BuildPurchase(
				false, 1, FString(), TEXT("USD"), TEXT("t"), TEXT("i"), FString(), FString(), NoExtra(), TEXT("iap")).Name, TEXT("iap")));
			TestTrue("resource", SameText(RavensightTyped::BuildResource(
				TEXT("sink"), TEXT("g"), 1, FString(), FString(), NoExtra(), TEXT("spend")).Name, TEXT("spend")));
			TestTrue("progression", SameText(RavensightTyped::BuildProgression(
				TEXT("start"), TEXT("w"), FString(), FString(), false, 0, NoExtra(), TEXT("enter")).Name, TEXT("enter")));
			TestTrue("error", SameText(RavensightTyped::BuildError(
				TEXT("info"), TEXT("m"), FString(), NoExtra(), TEXT("oops")).Name, TEXT("oops")));
		});
	});

	Describe("health event", [this]()
	{
		It("sets exactly the reserved keys", [this]()
		{
			const FRavensightTypedEvent Event = RavensightTyped::BuildHealth(58.5, 41.25, true, 812.0, true, 3400.0);
			TestTrue("name", SameText(Event.Name, TEXT("health")));
			TestEqual("count", Event.Props.Num(), 5);
			TestTrue("kind", IsText(Event, TEXT("rs_kind"), TEXT("health")));
			TestTrue("median", IsNumber(Event, TEXT("rs_fps_median"), 58.5));
			TestTrue("p10", IsNumber(Event, TEXT("rs_fps_p10"), 41.25));
			TestTrue("memory", IsNumber(Event, TEXT("rs_memory_mb"), 812.0));
			TestTrue("boot", IsNumber(Event, TEXT("rs_boot_ms"), 3400.0));
		});

		It("leaves out memory and boot time when unknown", [this]()
		{
			const FRavensightTypedEvent Event = RavensightTyped::BuildHealth(60, 55, false, 0, false, 0);
			TestEqual("count", Event.Props.Num(), 3);
		});
	});

	Describe("health sampler", [this]()
	{
		It("summarises a known set: 1 to 10 fps gives median 5.5 and p10 1.9", [this]()
		{
			FRavensightHealthSampler Sampler;
			for (int32 Fps = 10; Fps >= 1; --Fps)
			{
				Sampler.AddSample(Fps, 0.0);
			}
			FRavensightHealthSummary Summary;
			TestTrue("summarised", Sampler.Summarize(Summary));
			TestEqual("count", Summary.SampleCount, 10);
			TestEqual("median", Summary.FpsMedian, 5.5);
			TestEqual("p10", Summary.FpsP10, 1.9);
		});

		It("uses the middle sample for an odd count", [this]()
		{
			FRavensightHealthSampler Sampler;
			Sampler.AddSample(30, 0.0);
			Sampler.AddSample(60, 0.0);
			Sampler.AddSample(45, 0.0);
			FRavensightHealthSummary Summary;
			Sampler.Summarize(Summary);
			TestEqual("median", Summary.FpsMedian, 45.0);
			TestEqual("p10", Summary.FpsP10, 33.0);
		});

		It("gives one sample as both median and p10", [this]()
		{
			FRavensightHealthSampler Sampler;
			Sampler.AddSample(59.5, 0.0);
			FRavensightHealthSummary Summary;
			Sampler.Summarize(Summary);
			TestEqual("median", Summary.FpsMedian, 59.5);
			TestEqual("p10", Summary.FpsP10, 59.5);
		});

		It("ignores zero, negative and non finite samples", [this]()
		{
			FRavensightHealthSampler Sampler;
			Sampler.AddSample(0, 0.0);
			Sampler.AddSample(-5, 0.0);
			Sampler.AddSample(std::nan(""), 0.0);
			Sampler.AddSample(HUGE_VAL, 0.0);
			TestEqual("kept", Sampler.NumKept(), 0);
			FRavensightHealthSummary Summary;
			TestFalse("nothing to summarise", Sampler.Summarize(Summary));
		});

		It("never keeps more than 600 samples", [this]()
		{
			FRavensightHealthSampler Sampler;
			for (int32 Index = 0; Index < 5000; ++Index)
			{
				Sampler.AddSample(30.0 + (Index % 60), (Index % 97) / 97.0);
			}
			TestEqual("kept", Sampler.NumKept(), 600);
			TestEqual("seen", Sampler.NumSeen(), 5000);
		});

		It("replaces a kept slot when the reservoir is full and the draw lands inside it", [this]()
		{
			FRavensightHealthSampler Sampler;
			for (int32 Index = 0; Index < 600; ++Index)
			{
				Sampler.AddSample(60.0, 0.0);
			}
			// Sample 601: a draw of 0 picks slot 0, so the new value is kept.
			Sampler.AddSample(10.0, 0.0);
			FRavensightHealthSummary Summary;
			Sampler.Summarize(Summary);
			TestEqual("still 600", Summary.SampleCount, 600);
			TestTrue("p10 unchanged by one low sample", std::fabs(Summary.FpsP10 - 60.0) < 1e-6);

			// A draw near 1 picks a slot past the reservoir, so the sample is dropped.
			Sampler.AddSample(5.0, 0.999);
			Sampler.Summarize(Summary);
			TestEqual("p10 still unchanged", Summary.FpsP10, 60.0);
		});
	});

	Describe("one health send per session", [this]()
	{
		It("sends nothing when no sample was taken", [this]()
		{
			FRavensightHealthSampler Sampler;
			FFakeHealthSender Sender;
			Sender.Trigger(Sampler);
			Sender.Trigger(Sampler);
			TestEqual("sent", Sender.Sent, 0);
			TestFalse("not claimed", Sampler.HasSent());
		});

		It("sends once however many triggers fire", [this]()
		{
			FRavensightHealthSampler Sampler;
			Sampler.AddSample(60, 0.0);
			Sampler.AddSample(30, 0.0);
			FFakeHealthSender Sender;
			Sender.Trigger(Sampler); // background entry
			Sender.Trigger(Sampler); // quit
			Sender.Trigger(Sampler);
			TestEqual("sent", Sender.Sent, 1);
			TestEqual("summary kept", Sender.Last.SampleCount, 2);
			TestTrue("claimed", Sampler.HasSent());
		});

		It("a sample taken after the send does not send again", [this]()
		{
			FRavensightHealthSampler Sampler;
			Sampler.AddSample(60, 0.0);
			FFakeHealthSender Sender;
			Sender.Trigger(Sampler);
			Sampler.AddSample(20, 0.0);
			Sender.Trigger(Sampler);
			TestEqual("sent", Sender.Sent, 1);
		});

		It("a new session starts empty and sends once more", [this]()
		{
			FRavensightHealthSampler Sampler;
			Sampler.AddSample(60, 0.0);
			FFakeHealthSender Sender;
			Sender.Trigger(Sampler);
			Sampler.ResetForNewSession();
			TestEqual("emptied", Sampler.NumKept(), 0);
			Sender.Trigger(Sampler);
			TestEqual("nothing to send yet", Sender.Sent, 1);
			Sampler.AddSample(45, 0.0);
			Sender.Trigger(Sampler);
			TestEqual("second session sent", Sender.Sent, 2);
			TestEqual("second summary", Sender.Last.FpsMedian, 45.0);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
