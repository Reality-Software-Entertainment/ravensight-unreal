// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "RavensightTypedEvents.h"

#include <cmath>

FRavensightProp FRavensightProp::MakeString(const FString& InKey, const FString& InText)
{
	FRavensightProp Prop;
	Prop.Key = InKey;
	Prop.Kind = ERavensightPropKind::String;
	Prop.Text = InText;
	return Prop;
}

FRavensightProp FRavensightProp::MakeNumber(const FString& InKey, double InNumber)
{
	FRavensightProp Prop;
	Prop.Key = InKey;
	Prop.Kind = ERavensightPropKind::Number;
	Prop.Number = InNumber;
	return Prop;
}

const FRavensightProp* FRavensightTypedEvent::Find(const FString& Key) const
{
	for (const FRavensightProp& Prop : Props)
	{
		if (Prop.Key.Equals(Key, ESearchCase::CaseSensitive))
		{
			return &Prop;
		}
	}
	return nullptr;
}

namespace RavensightTyped
{
	void SetProp(TArray<FRavensightProp>& Props, const FRavensightProp& Prop)
	{
		for (FRavensightProp& Existing : Props)
		{
			if (Existing.Key.Equals(Prop.Key, ESearchCase::CaseSensitive))
			{
				Existing = Prop;
				return;
			}
		}
		Props.Add(Prop);
	}

	namespace
	{
		/** Caller keys first, so every reserved key set afterwards wins. */
		FRavensightTypedEvent Start(const TArray<FRavensightProp>& Extra, const FString& Name, const FString& DefaultName)
		{
			FRavensightTypedEvent Event;
			Event.Name = Name.IsEmpty() ? DefaultName : Name;
			for (const FRavensightProp& Prop : Extra)
			{
				SetProp(Event.Props, Prop);
			}
			return Event;
		}

		void Str(FRavensightTypedEvent& Event, const TCHAR* Key, const FString& Value)
		{
			SetProp(Event.Props, FRavensightProp::MakeString(Key, Value));
		}

		void OptionalStr(FRavensightTypedEvent& Event, const TCHAR* Key, const FString& Value)
		{
			if (!Value.IsEmpty())
			{
				Str(Event, Key, Value);
			}
		}

		void Num(FRavensightTypedEvent& Event, const TCHAR* Key, double Value)
		{
			SetProp(Event.Props, FRavensightProp::MakeNumber(Key, Value));
		}
	}

	FRavensightTypedEvent BuildPurchase(
		bool bAmountIsText, int64 AmountMinor, const FString& AmountText,
		const FString& Currency, const FString& ItemType, const FString& ItemId,
		const FString& Cart, const FString& TransactionId,
		const TArray<FRavensightProp>& Extra, const FString& EventName)
	{
		FRavensightTypedEvent Event = Start(Extra, EventName, TEXT("purchase"));
		Str(Event, TEXT("rs_kind"), TEXT("purchase"));
		if (bAmountIsText)
		{
			Str(Event, TEXT("rs_amount"), AmountText);
		}
		else
		{
			Num(Event, TEXT("rs_amount"), static_cast<double>(AmountMinor));
		}
		Str(Event, TEXT("rs_currency"), Currency.ToUpper());
		Str(Event, TEXT("rs_item_type"), ItemType);
		Str(Event, TEXT("rs_item_id"), ItemId);
		OptionalStr(Event, TEXT("rs_cart"), Cart);
		OptionalStr(Event, TEXT("rs_transaction_id"), TransactionId);
		return Event;
	}

	FRavensightTypedEvent BuildResource(
		const FString& Flow, const FString& Resource, double Amount,
		const FString& ItemType, const FString& ItemId,
		const TArray<FRavensightProp>& Extra, const FString& EventName)
	{
		FRavensightTypedEvent Event = Start(Extra, EventName, FString(TEXT("resource_")) + Flow);
		Str(Event, TEXT("rs_kind"), TEXT("resource"));
		Str(Event, TEXT("rs_flow"), Flow);
		Str(Event, TEXT("rs_resource"), Resource);
		Num(Event, TEXT("rs_amount"), Amount);
		OptionalStr(Event, TEXT("rs_item_type"), ItemType);
		OptionalStr(Event, TEXT("rs_item_id"), ItemId);
		return Event;
	}

	FRavensightTypedEvent BuildProgression(
		const FString& Status, const FString& P1, const FString& P2, const FString& P3,
		bool bHasScore, double Score,
		const TArray<FRavensightProp>& Extra, const FString& EventName)
	{
		FRavensightTypedEvent Event = Start(Extra, EventName, FString(TEXT("progression_")) + Status);
		Str(Event, TEXT("rs_kind"), TEXT("progression"));
		Str(Event, TEXT("rs_status"), Status);
		Str(Event, TEXT("rs_p1"), P1);
		OptionalStr(Event, TEXT("rs_p2"), P2);
		OptionalStr(Event, TEXT("rs_p3"), P3);
		if (bHasScore)
		{
			Num(Event, TEXT("rs_score"), Score);
		}
		return Event;
	}

	FRavensightTypedEvent BuildError(
		const FString& Severity, const FString& Message, const FString& Stack,
		const TArray<FRavensightProp>& Extra, const FString& EventName)
	{
		FRavensightTypedEvent Event = Start(Extra, EventName, TEXT("error"));
		Str(Event, TEXT("rs_kind"), TEXT("error"));
		Str(Event, TEXT("rs_severity"), Severity);
		Str(Event, TEXT("rs_message"), Message.Left(MaxMessageChars));
		OptionalStr(Event, TEXT("rs_stack"), Stack.Left(MaxStackChars));
		return Event;
	}

	FRavensightTypedEvent BuildHealth(
		double FpsMedian, double FpsP10,
		bool bHasMemory, double MemoryMb, bool bHasBoot, double BootMs)
	{
		FRavensightTypedEvent Event;
		Event.Name = TEXT("health");
		Str(Event, TEXT("rs_kind"), TEXT("health"));
		Num(Event, TEXT("rs_fps_median"), FpsMedian);
		Num(Event, TEXT("rs_fps_p10"), FpsP10);
		if (bHasMemory)
		{
			Num(Event, TEXT("rs_memory_mb"), MemoryMb);
		}
		if (bHasBoot)
		{
			Num(Event, TEXT("rs_boot_ms"), BootMs);
		}
		return Event;
	}
}

// --- Health sampler ----------------------------------------------------------

void FRavensightHealthSampler::AddSample(double Fps, double Uniform01)
{
	if (!std::isfinite(Fps) || Fps <= 0.0)
	{
		return;
	}

	++Seen;
	if (Samples.Num() < MaxSamples)
	{
		Samples.Add(Fps);
		return;
	}

	// Algorithm R: the Nth sample replaces a kept one with probability Max/N.
	const double Clamped = Uniform01 < 0.0 ? 0.0 : (Uniform01 >= 1.0 ? 0.999999999 : Uniform01);
	const int32 Slot = static_cast<int32>(Clamped * static_cast<double>(Seen));
	if (Slot < MaxSamples)
	{
		Samples[Slot] = Fps;
	}
}

double FRavensightHealthSampler::Percentile(const TArray<double>& Sorted, double P)
{
	const int32 Count = Sorted.Num();
	if (Count == 0)
	{
		return 0.0;
	}
	if (Count == 1)
	{
		return Sorted[0];
	}

	const double Rank = P * static_cast<double>(Count - 1);
	const int32 Low = static_cast<int32>(Rank);
	const int32 High = Low + 1 < Count ? Low + 1 : Low;
	const double Fraction = Rank - static_cast<double>(Low);
	return Sorted[Low] + (Sorted[High] - Sorted[Low]) * Fraction;
}

bool FRavensightHealthSampler::Summarize(FRavensightHealthSummary& Out) const
{
	if (Samples.Num() == 0)
	{
		return false;
	}

	TArray<double> Sorted = Samples;
	Sorted.Sort();

	Out.FpsMedian = Percentile(Sorted, 0.5);
	Out.FpsP10 = Percentile(Sorted, 0.1);
	Out.SampleCount = Sorted.Num();
	return true;
}

bool FRavensightHealthSampler::TryClaimSend(FRavensightHealthSummary& Out)
{
	if (bSent || !Summarize(Out))
	{
		return false;
	}
	bSent = true;
	return true;
}

void FRavensightHealthSampler::ResetForNewSession()
{
	Samples.Reset();
	Seen = 0;
	bSent = false;
}
