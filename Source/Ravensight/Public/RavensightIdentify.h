// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Identify players who sign in (docs/identify/CONTRACT.md in the platform
// repository): the on-device checks, the salted hash, and the identity state
// machine the subsystem drives. All text is UTF-8 in std::string, so the same
// code runs in the engine and in the engine-free host check, and every shared
// identify vector runs against it.
//
// The raw account id lives only in memory, and only until the session answer
// gives the game's salt; from then on only the salt and the hash are kept.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace RavensightIdentify
{
	/** The longest account id, in UTF-8 bytes. */
	inline constexpr std::size_t MaxBytes = 256;

	/** Why an account id is refused ("empty", "too_long", "looks_like_email"), or nullptr. Never trims. */
	const char* Refusal(const std::string& AccountIdUtf8);

	/** Lowercase hex HMAC-SHA256 keyed with the salt string's own bytes (not hex decoded) over the account id. */
	std::string AccountHash(const std::string& Salt, const std::string& AccountIdUtf8);

	/** True for 64 lowercase hex characters: a salt or an account hash. */
	bool IsHex64(const std::string& Text);

	/** The POST /identify body: exactly {"accountHash":"..."}. */
	std::string RequestBody(const std::string& AccountHash);
}

/** What the subsystem does after a call, in this order: delete, reset the device, save, send. */
struct FRavensightIdentityEffects
{
	/** Delete the saved identity. */
	bool bDeleteSaved = false;
	/** Switch to a fresh device id and session exactly as ResetDeviceId does. */
	bool bResetDevice = false;
	/** Save SavedText(). */
	bool bSave = false;
	/** Send for the open session, if there is one (BeginSend decides). */
	bool bSend = false;
};

/** How an identify answer was taken. */
enum class ERavensightIdentifyOutcome : uint8_t
{
	/** For an identity or a session that is gone: changes nothing. */
	Ignored,
	/** Done for this session. */
	Done,
	/** 404 or invalid_account_hash: not tried again this launch; the identity is kept. */
	Stopped,
	/** A second no_session in a row: not tried again until ResetIdentity; the identity is kept. */
	StoppedNoSession,
	/** Anything else: tried again at the next session, never in a loop. */
	NextSession,
};

class FRavensightIdentity
{
public:
	/** Reads a saved value at start. False (and nothing loaded) when it is malformed. */
	bool Load(const std::string& SavedText);

	/**
	 * Load at the start, when an id may already be held (Identify before Init).
	 * A held id is decided when this launch's salt arrives. False (nothing
	 * loaded) when malformed. Out is for future use and left unchanged.
	 */
	bool AdoptSaved(const std::string& SavedText, FRavensightIdentityEffects& Out);

	/** The value to save: the salt and the hash, "<salt>:<hash>". Never the raw id. */
	std::string SavedText() const;

	/** True while an identity is saved, or an accepted id waits for the salt or for consent. */
	bool IsIdentified() const { return !SavedHash.empty() || !Held.empty(); }

	/**
	 * A game's Identify call. Returns the refusal reason (nothing changed), or
	 * nullptr. bMayStore: a key, consent and not switched off.
	 */
	const char* Identify(const std::string& AccountIdUtf8, bool bMayStore, FRavensightIdentityEffects& Out);

	/** Sign-out: forget everything and switch device. Nothing at all when nothing is saved or held. */
	void Reset(FRavensightIdentityEffects& Out);

	/** Opt-out, consent withdrawn, switched off: forget the saved identity and a held id. */
	void Forget(FRavensightIdentityEffects& Out);

	/** Consent given or switched back on: a held id is settled if the salt is known. */
	void MayStoreNow(FRavensightIdentityEffects& Out);

	/**
	 * A session answer: its identify.salt, or none from an older server. With
	 * bResetDevice set the session that just opened belongs to a signed out
	 * account: the subsystem drops it and opens a fresh one.
	 */
	void OnSessionAnswer(bool bHasSalt, const std::string& Salt, bool bMayStore, FRavensightIdentityEffects& Out);

	/** True when a request should go for this session now; fills the body and the generation to answer with. */
	bool BeginSend(const std::string& SessionToken, std::string& OutBody, int32_t& OutGeneration);

	/** The answer to a request BeginSend started. */
	ERavensightIdentifyOutcome OnSendAnswer(int32_t SentGeneration, const std::string& SentToken,
		const std::string& CurrentToken, int32_t Status, const std::string& ErrorCode);

private:
	void Settle(bool bMayStore, FRavensightIdentityEffects& Out);

	std::string Held;
	std::string SavedSalt;
	std::string SavedHash;
	std::string LaunchSalt;
	std::string DoneToken;
	bool bNoSalt = false;
	bool bStopped = false;
	int32_t NoSessionCount = 0;
	bool bNoSessionStopped = false;
	int32_t Generation = 0;
};
