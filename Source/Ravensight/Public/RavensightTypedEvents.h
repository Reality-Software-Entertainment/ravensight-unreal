// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The typed event contract, engine free: which properties a purchase, a
// resource flow, a progression step, an error and a health sample carry, and
// the per session health sampler (a bounded reservoir, percentiles and the
// one send rule).
//
// Nothing here touches UObject, JSON or HTTP, so RavensightTypedEvents.spec.cpp
// can check the exact property set of every helper with plain values. The
// subsystem turns a property list into an FJsonObject (numbers stay numbers)
// and sends it through TrackEventJson, so queueing, batching, retries,
// opt out and the Playtest path are unchanged.
//
// Helpers never validate money beyond what they pass through: a wrong amount
// is sent as given and the server flags the event with rs_invalid.

#pragma once

#include "CoreMinimal.h"

/** A string or a number. Integers are numbers too: the JSON writer prints a whole double without a fraction. */
enum class ERavensightPropKind : uint8
{
	String,
	Number,
};

struct RAVENSIGHT_API FRavensightProp
{
	FString Key;
	ERavensightPropKind Kind = ERavensightPropKind::String;
	FString Text;
	double Number = 0.0;

	static FRavensightProp MakeString(const FString& InKey, const FString& InText);
	static FRavensightProp MakeNumber(const FString& InKey, double InNumber);
};

/** An event name and its properties, in the order they were set. */
struct RAVENSIGHT_API FRavensightTypedEvent
{
	FString Name;
	TArray<FRavensightProp> Props;

	/** The property with this key (case sensitive), or null. */
	const FRavensightProp* Find(const FString& Key) const;
};

namespace RavensightTyped
{
	inline constexpr int32 MaxMessageChars = 200;
	inline constexpr int32 MaxStackChars = 2000;

	/** Replaces a property with the same key (case sensitive), or appends. */
	RAVENSIGHT_API void SetProp(TArray<FRavensightProp>& Props, const FRavensightProp& Prop);

	/**
	 * Purchase. Amount is integer minor units, or a digit string when
	 * bAmountIsText (the text is sent as a string, untouched). Currency is
	 * upper cased. Empty Cart and TransactionId are left out. Default name
	 * "purchase".
	 */
	RAVENSIGHT_API FRavensightTypedEvent BuildPurchase(
		bool bAmountIsText, int64 AmountMinor, const FString& AmountText,
		const FString& Currency, const FString& ItemType, const FString& ItemId,
		const FString& Cart, const FString& TransactionId,
		const TArray<FRavensightProp>& Extra, const FString& EventName);

	/**
	 * Resource flow. Flow is "source" or "sink" and is sent as given. Empty
	 * ItemType and ItemId are left out. Default name "resource_" + Flow.
	 */
	RAVENSIGHT_API FRavensightTypedEvent BuildResource(
		const FString& Flow, const FString& Resource, double Amount,
		const FString& ItemType, const FString& ItemId,
		const TArray<FRavensightProp>& Extra, const FString& EventName);

	/**
	 * Progression. Status is "start", "complete" or "fail". Empty P2 and P3
	 * are left out; the score is only sent when bHasScore. Default name
	 * "progression_" + Status.
	 */
	RAVENSIGHT_API FRavensightTypedEvent BuildProgression(
		const FString& Status, const FString& P1, const FString& P2, const FString& P3,
		bool bHasScore, double Score,
		const TArray<FRavensightProp>& Extra, const FString& EventName);

	/**
	 * Error. Severity is debug, info, warning, error or critical. Message is
	 * cut to 200 characters and Stack to 2000; an empty Stack is left out.
	 * Default name "error".
	 */
	RAVENSIGHT_API FRavensightTypedEvent BuildError(
		const FString& Severity, const FString& Message, const FString& Stack,
		const TArray<FRavensightProp>& Extra, const FString& EventName);

	/** Health, named "health". Memory and boot time are only sent when present. */
	RAVENSIGHT_API FRavensightTypedEvent BuildHealth(
		double FpsMedian, double FpsP10,
		bool bHasMemory, double MemoryMb, bool bHasBoot, double BootMs);
}

/** The result of summarising a session's frame rate samples. */
struct FRavensightHealthSummary
{
	double FpsMedian = 0.0;
	double FpsP10 = 0.0;
	int32 SampleCount = 0;
};

/**
 * Per session frames per second samples. Keeps at most MaxSamples in a
 * reservoir (once full, each new sample replaces a random slot with the
 * right probability, so the kept set stays uniform), summarises them as the
 * median and 10th percentile (linear interpolation between ranks), and hands
 * the summary out at most once per session.
 */
struct RAVENSIGHT_API FRavensightHealthSampler
{
	static constexpr int32 MaxSamples = 600;

	/**
	 * Records one fps sample. Uniform01 is a random number in [0, 1) from the
	 * caller, used only once the reservoir is full. Zero, negative and
	 * non finite fps values are ignored.
	 */
	void AddSample(double Fps, double Uniform01);

	int32 NumKept() const { return Samples.Num(); }
	int32 NumSeen() const { return Seen; }
	bool HasSent() const { return bSent; }

	/** The summary of what is kept, or false with no samples. Does not claim the send. */
	bool Summarize(FRavensightHealthSummary& Out) const;

	/**
	 * True exactly once per session, and only when at least one sample was
	 * taken. Fills Out the first time and never again.
	 */
	bool TryClaimSend(FRavensightHealthSummary& Out);

	/** A new session starts empty and may send once more. */
	void ResetForNewSession();

	/** Percentile of an ascending sorted list, p in [0, 1], linear interpolation. */
	static double Percentile(const TArray<double>& Sorted, double P);

private:
	TArray<double> Samples;
	int32 Seen = 0;
	bool bSent = false;
};
