// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "RavensightIdentify.h"

#include "RavensightDelivery.h"

namespace RavensightIdentify
{
	const char* Refusal(const std::string& AccountIdUtf8)
	{
		if (AccountIdUtf8.empty()) return "empty";
		if (AccountIdUtf8.size() > MaxBytes) return "too_long";
		if (AccountIdUtf8.find('@') != std::string::npos) return "looks_like_email";
		return nullptr;
	}

	std::string AccountHash(const std::string& Salt, const std::string& AccountIdUtf8)
	{
		return RavensightDelivery::HmacSha256Hex(Salt, AccountIdUtf8);
	}

	bool IsHex64(const std::string& Text)
	{
		if (Text.size() != 64) return false;
		for (const char C : Text)
		{
			if (!((C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'))) return false;
		}
		return true;
	}

	std::string RequestBody(const std::string& Hash)
	{
		return "{\"accountHash\":\"" + Hash + "\"}";
	}
}

bool FRavensightIdentity::Load(const std::string& Text)
{
	if (Text.size() != 129 || Text[64] != ':') return false;
	const std::string Salt = Text.substr(0, 64);
	const std::string Hash = Text.substr(65);
	if (!RavensightIdentify::IsHex64(Salt) || !RavensightIdentify::IsHex64(Hash)) return false;
	SavedSalt = Salt;
	SavedHash = Hash;
	return true;
}

std::string FRavensightIdentity::SavedText() const
{
	return SavedHash.empty() ? std::string() : SavedSalt + ":" + SavedHash;
}

const char* FRavensightIdentity::Identify(const std::string& AccountIdUtf8, bool bMayStore, FRavensightIdentityEffects& Out)
{
	if (const char* Reason = RavensightIdentify::Refusal(AccountIdUtf8))
	{
		return Reason;
	}
	if (bNoSalt)
	{
		// An older server: nothing is held, saved or sent this launch.
		return nullptr;
	}
	if (!SavedHash.empty())
	{
		// The saved salt is the game's: compare the accounts with it.
		if (RavensightIdentify::AccountHash(SavedSalt, AccountIdUtf8) == SavedHash)
		{
			Held.clear();
			Out.bSend = true;
			return nullptr;
		}
		// Another account on this device: sign the first one out.
		Reset(Out);
	}
	Held = AccountIdUtf8;
	Settle(bMayStore, Out);
	return nullptr;
}

void FRavensightIdentity::Reset(FRavensightIdentityEffects& Out)
{
	if (!IsIdentified()) return;
	Forget(Out);
	Out.bResetDevice = true;
}

void FRavensightIdentity::Forget(FRavensightIdentityEffects& Out)
{
	Out.bDeleteSaved = true;
	Held.clear();
	SavedSalt.clear();
	SavedHash.clear();
	++Generation;
}

void FRavensightIdentity::MayStoreNow(FRavensightIdentityEffects& Out)
{
	Settle(true, Out);
	if (!SavedHash.empty()) Out.bSend = true;
}

void FRavensightIdentity::OnSessionAnswer(bool bHasSalt, const std::string& Salt, bool bMayStore, FRavensightIdentityEffects& Out)
{
	if (bNoSalt) return;
	if (!bHasSalt || !RavensightIdentify::IsHex64(Salt))
	{
		// Unavailable this launch: the held id goes; a saved one is kept, unsent.
		bNoSalt = true;
		LaunchSalt.clear();
		Held.clear();
		return;
	}
	LaunchSalt = Salt;
	if (!SavedHash.empty() && SavedSalt != Salt)
	{
		// Made under another game's key: deleted, never sent here.
		SavedSalt.clear();
		SavedHash.clear();
		++Generation;
		Out.bDeleteSaved = true;
	}
	Settle(bMayStore, Out);
	if (!SavedHash.empty()) Out.bSend = true;
}

void FRavensightIdentity::Settle(bool bMayStore, FRavensightIdentityEffects& Out)
{
	if (Held.empty() || LaunchSalt.empty() || !bMayStore) return;
	SavedSalt = LaunchSalt;
	SavedHash = RavensightIdentify::AccountHash(LaunchSalt, Held);
	Held.clear();
	++Generation;
	Out.bSave = true;
	Out.bSend = true;
}

bool FRavensightIdentity::BeginSend(const std::string& SessionToken, std::string& OutBody, int32_t& OutGeneration)
{
	if (SavedHash.empty() || bStopped || bNoSalt || SessionToken.empty() || LaunchSalt != SavedSalt) return false;
	if (SessionToken == DoneToken) return false;
	// Each session is tried once, whatever the answer.
	DoneToken = SessionToken;
	OutBody = RavensightIdentify::RequestBody(SavedHash);
	OutGeneration = Generation;
	return true;
}

ERavensightIdentifyOutcome FRavensightIdentity::OnSendAnswer(int32_t SentGeneration, const std::string& SentToken,
	const std::string& CurrentToken, int32_t Status, const std::string& ErrorCode)
{
	if (SentGeneration != Generation || SentToken != CurrentToken)
	{
		return ERavensightIdentifyOutcome::Ignored;
	}
	if (Status >= 200 && Status < 300)
	{
		return ERavensightIdentifyOutcome::Done;
	}
	if (Status == 404 || (Status == 400 && ErrorCode == "invalid_account_hash"))
	{
		bStopped = true;
		return ERavensightIdentifyOutcome::Stopped;
	}
	return ERavensightIdentifyOutcome::NextSession;
}
