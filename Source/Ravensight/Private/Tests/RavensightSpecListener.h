// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Records what the subsystem's dynamic delegates fire, for
// RavensightSubsystem.spec.cpp. Dynamic delegates only bind to UFUNCTIONs,
// so this has to be a UObject. It is tiny, transient and does nothing on its
// own.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "RavensightSpecListener.generated.h"

UCLASS(Transient, NotBlueprintable, NotBlueprintType)
class URavensightSpecListener : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void HandleFeedbackResult(bool bSuccess, const FString& Reason)
	{
		++FeedbackCalls;
		bLastFeedbackSuccess = bSuccess;
		LastFeedbackReason = Reason;
	}

	UFUNCTION()
	void HandleSuggestions(bool bSuccess, const TArray<FString>& Suggestions)
	{
		++SuggestionCalls;
		bLastSuggestionsSuccess = bSuccess;
		LastSuggestionCount = Suggestions.Num();
	}

	int32 FeedbackCalls = 0;
	bool bLastFeedbackSuccess = false;
	FString LastFeedbackReason;

	int32 SuggestionCalls = 0;
	bool bLastSuggestionsSuccess = false;
	int32 LastSuggestionCount = -1;
};
