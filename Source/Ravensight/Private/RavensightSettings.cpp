// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "RavensightSettings.h"

URavensightSettings::URavensightSettings()
{
	ApiUrl = TEXT("https://api.ravensight.io/api/v1");
	IngestKey = FString();
	GameVersion = FString();
}

FName URavensightSettings::GetCategoryName() const
{
	return FName(TEXT("Plugins"));
}

const URavensightSettings* URavensightSettings::Get()
{
	return GetDefault<URavensightSettings>();
}
