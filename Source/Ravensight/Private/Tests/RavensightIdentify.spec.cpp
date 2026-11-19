// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Identify against the platform's binding vectors (docs/identify/
// identify-vectors.json, version 2, generated into RavensightIdentifyVectors.inl)
// and the identity state machine the subsystem drives, through a small host
// that plays the subsystem's part: device ids, sessions and their salt
// answers, the saved value and the requests. Engine-free: runs in the host
// check too. The subsystem's own requests are covered in RavensightSubsystem.spec.cpp.

#include "Misc/AutomationTest.h"
#include "RavensightIdentify.h"

#include <string>
#include <utility>
#include <vector>

#if WITH_DEV_AUTOMATION_TESTS

#include "RavensightIdentifyVectors.inl"

namespace RavensightIdentifySpecHost
{
	/** Plays the subsystem: applies the effects the way URavensightSubsystem does. */
	struct FHost
	{
		FRavensightIdentity Identity;
		std::string Saved;
		int32 Device = 1;
		int32 SessionCount = 0;
		std::string Session;
		bool bMayStore = true;
		std::vector<std::pair<std::string, std::string>> Sends;
		int32 AnswerStatus = 200;
		std::string AnswerCode;

		void Apply(const FRavensightIdentityEffects& Effects)
		{
			if (Effects.bDeleteSaved) Saved.clear();
			if (Effects.bResetDevice)
			{
				++Device;
				Session.clear();
			}
			if (Effects.bSave) Saved = Identity.SavedText();
			if (Effects.bSend) TrySend();
		}

		void TrySend()
		{
			if (Session.empty()) return;
			std::string Body;
			int32 Generation = 0;
			if (!Identity.BeginSend(Session, Body, Generation)) return;
			Sends.emplace_back(Session, Body);
			Identity.OnSendAnswer(Generation, Session, Session, AnswerStatus, AnswerCode);
		}

		void OpenSession(const char* Salt)
		{
			Session = "S" + std::to_string(++SessionCount);
			FRavensightIdentityEffects Effects;
			Identity.OnSessionAnswer(Salt != nullptr, Salt != nullptr ? Salt : "", bMayStore, Effects);
			Apply(Effects);
		}

		const char* Identify(const std::string& AccountId)
		{
			FRavensightIdentityEffects Effects;
			const char* Refusal = Identity.Identify(AccountId, bMayStore, Effects);
			Apply(Effects);
			return Refusal;
		}

		void Reset()
		{
			FRavensightIdentityEffects Effects;
			Identity.Reset(Effects);
			Apply(Effects);
		}

		void SetMayStore(bool bValue)
		{
			bMayStore = bValue;
			FRavensightIdentityEffects Effects;
			if (bValue) Identity.MayStoreNow(Effects);
			else Identity.Forget(Effects);
			Apply(Effects);
		}

		/** A new launch: the saved value is read back, the device id kept. */
		void Relaunch()
		{
			Identity = FRavensightIdentity();
			if (!Identity.Load(Saved)) Saved.clear();
			Session.clear();
		}

		/** The Index-th request as (session, body), or two empty strings when there is none. */
		std::pair<std::string, std::string> SendAt(size_t Index) const
		{
			return Index < Sends.size() ? Sends[Index] : std::pair<std::string, std::string>();
		}

		std::string SentHash(size_t Index) const
		{
			const std::string Body = SendAt(Index).second;
			if (Body.empty()) return std::string();
			const std::string Prefix = "{\"accountHash\":\"";
			return Body.substr(Prefix.size(), 64);
		}

		std::string SavedHash() const
		{
			return Saved.size() == 129 ? Saved.substr(65) : std::string();
		}
	};
}

BEGIN_DEFINE_SPEC(FRavensightIdentifySpec, "Ravensight.Identify", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
END_DEFINE_SPEC(FRavensightIdentifySpec)

void FRavensightIdentifySpec::Define()
{
	using namespace RavensightIdentifyVectors;
	using RavensightIdentifySpecHost::FHost;

	Describe("the shared identify vectors", [this]()
	{
		It("are version 2 with a 64 character salt", [this]()
		{
			TestEqual(TEXT("version"), Version, 2);
			TestEqual(TEXT("salt length"), static_cast<int32>(std::string(Salt).size()), 64);
			TestEqual(TEXT("max bytes"), MaxBytes, static_cast<int32>(RavensightIdentify::MaxBytes));
		});

		It("hashes every accept case to its wire, longer than the block included", [this]()
		{
			int32 Count = 0;
			for (const FAccept& Case : Accept)
			{
				TestTrue(Case.Note, RavensightIdentify::Refusal(Case.AccountId) == nullptr);
				TestTrue(Case.Note, RavensightIdentify::AccountHash(Salt, Case.AccountId) == Case.Wire);
				TestTrue(Case.Note, "px_" + std::string(Case.Wire).substr(0, 32) == Case.PlayerId);
				++Count;
			}
			TestEqual(TEXT("accept cases"), Count, 10);
			TestTrue(TEXT("salt a"), RavensightIdentify::AccountHash(DifferSaltA, DifferAccountId) == DifferWireA);
			TestTrue(TEXT("salt b"), RavensightIdentify::AccountHash(DifferSaltB, DifferAccountId) == DifferWireB);
		});

		It("refuses every refuse case with nothing held, saved or sent", [this]()
		{
			int32 Count = 0;
			for (const FRefuse& Case : Refuse)
			{
				const char* Reason = RavensightIdentify::Refusal(Case.AccountId);
				TestTrue(Case.Note, Reason != nullptr && std::string(Reason) == Case.Reason);

				FHost Host;
				Host.OpenSession(Salt);
				const int32 Device = Host.Device;
				const char* Refused = Host.Identify(Case.AccountId);
				TestTrue(Case.Note, Refused != nullptr && std::string(Refused) == Case.Reason);
				TestFalse(Case.Note, Host.Identity.IsIdentified());
				Host.OpenSession(Salt);
				TestTrue(Case.Note, Host.Sends.empty());
				TestTrue(Case.Note, Host.Saved.empty());
				TestEqual(Case.Note, Host.Device, Device);
				++Count;
			}
			TestEqual(TEXT("refuse cases"), Count, 7);
		});

		It("keeps what is saved after a refusal", [this]()
		{
			FHost Host;
			Host.Identify("u1");
			Host.OpenSession(Salt);
			const std::string Before = Host.Saved;
			TestFalse(TEXT("saved"), Before.empty());
			TestTrue(TEXT("too long"), Host.Identify(std::string(257, 'a')) != nullptr);
			TestTrue(TEXT("empty"), Host.Identify("") != nullptr);
			TestTrue(TEXT("unchanged"), Host.Saved == Before);
			TestTrue(TEXT("still identified"), Host.Identity.IsIdentified());
		});
	});

	Describe("the sequence", [this]()
	{
		It("holds step by step", [this]()
		{
			auto StepIs = [this](int32 Index, const char* Start)
			{
				TestTrue(Start, std::string(Sequence[Index].Step).rfind(Start, 0) == 0);
			};
			FHost Host;

			// 0: identify before any session: held in memory only.
			StepIs(0, "init with a key and consent");
			TestTrue(TEXT("0 accepted"), Host.Identify("u1") == nullptr);
			TestTrue(TEXT("0 held"), Host.Identity.IsIdentified());
			TestTrue(TEXT("0 nothing sent"), Host.Sends.empty());
			TestTrue(TEXT("0 nothing saved"), Host.Saved.empty());

			// 1: S1 opens with the salt: hashed, saved, sent on S1.
			StepIs(1, "session S1 opens");
			Host.OpenSession(Salt);
			TestEqual(TEXT("1 one send"), static_cast<int32>(Host.Sends.size()), 1);
			TestTrue(TEXT("1 on S1"), Host.SendAt(0).first == "S1");
			TestTrue(TEXT("1 hash"), Host.SentHash(0) == Sequence[1].Send);
			TestTrue(TEXT("1 saved"), Host.SavedHash() == Sequence[1].Stored);
			TestTrue(TEXT("1 saved salt"), Host.Saved.substr(0, 64) == Salt);

			// 2: the same account again in S1: nothing.
			StepIs(2, "identify(\"u1\") again in S1");
			const int32 Device = Host.Device;
			Host.Identify("u1");
			TestEqual(TEXT("2 no send"), static_cast<int32>(Host.Sends.size()), 1);
			TestEqual(TEXT("2 device kept"), Host.Device, Device);

			// 3: relaunch, S2 with the same salt, no call: sent on S2.
			StepIs(3, "relaunch, session S2 opens");
			Host.Relaunch();
			TestTrue(TEXT("3 identified from the saved value"), Host.Identity.IsIdentified());
			Host.OpenSession(Salt);
			TestEqual(TEXT("3 two sends"), static_cast<int32>(Host.Sends.size()), 2);
			TestTrue(TEXT("3 on S2"), Host.SendAt(1).first == "S2");
			TestTrue(TEXT("3 hash"), Host.SentHash(1) == Sequence[3].Send);

			// 4: an email is refused; nothing changes.
			StepIs(4, "identify(\"player@example.com\")");
			TestTrue(TEXT("4 refused"), Host.Identify("player@example.com") != nullptr);
			TestEqual(TEXT("4 no send"), static_cast<int32>(Host.Sends.size()), 2);
			TestTrue(TEXT("4 saved"), Host.SavedHash() == Sequence[4].Stored);
			TestEqual(TEXT("4 device kept"), Host.Device, Device);

			// 5: another account: fresh device and session S3, sent on S3.
			StepIs(5, "identify(\"u2\")");
			Host.Identify("u2");
			TestTrue(TEXT("5 device changed"), Host.Device != Device);
			TestTrue(TEXT("5 saved"), Host.SavedHash() == Sequence[5].Stored);
			Host.OpenSession(Salt);
			TestEqual(TEXT("5 three sends"), static_cast<int32>(Host.Sends.size()), 3);
			TestTrue(TEXT("5 on S3"), Host.SendAt(2).first == "S3");
			TestTrue(TEXT("5 hash"), Host.SentHash(2) == Sequence[5].Send);

			// 6: reset: fresh device, S4, nothing sent, nothing saved.
			StepIs(6, "resetIdentity()");
			const int32 BeforeReset = Host.Device;
			Host.Reset();
			TestTrue(TEXT("6 device changed"), Host.Device != BeforeReset);
			TestFalse(TEXT("6 not identified"), Host.Identity.IsIdentified());
			TestTrue(TEXT("6 nothing saved"), Host.Saved.empty());
			Host.OpenSession(Salt);
			TestEqual(TEXT("6 S4"), Host.SessionCount, 4);
			TestEqual(TEXT("6 no send"), static_cast<int32>(Host.Sends.size()), 3);

			// 7: reset again with nothing saved: nothing at all.
			StepIs(7, "resetIdentity() again");
			const int32 Kept = Host.Device;
			Host.Reset();
			TestEqual(TEXT("7 device kept"), Host.Device, Kept);
			TestEqual(TEXT("7 no send"), static_cast<int32>(Host.Sends.size()), 3);

			// 8: u3 answered 429 needs_funds: saved, sent once on S4, no loop.
			StepIs(8, "identify(\"u3\")");
			Host.AnswerStatus = 429;
			Host.AnswerCode = "quota_exceeded";
			Host.Identify("u3");
			TestEqual(TEXT("8 four sends"), static_cast<int32>(Host.Sends.size()), 4);
			TestTrue(TEXT("8 on S4"), Host.SendAt(3).first == "S4");
			TestTrue(TEXT("8 hash"), Host.SentHash(3) == Sequence[8].Send);
			TestTrue(TEXT("8 saved"), Host.SavedHash() == Sequence[8].Stored);
			Host.Identify("u3");
			Host.TrySend();
			TestEqual(TEXT("8 no loop"), static_cast<int32>(Host.Sends.size()), 4);

			// 9: relaunch under another salt: the saved identity is deleted, never sent.
			StepIs(9, "relaunch; session S5 answers a DIFFERENT salt");
			Host.AnswerStatus = 200;
			Host.AnswerCode.clear();
			Host.Relaunch();
			Host.OpenSession(DifferSaltB);
			TestEqual(TEXT("9 no send"), static_cast<int32>(Host.Sends.size()), 4);
			TestTrue(TEXT("9 deleted"), Host.Saved.empty());
			TestFalse(TEXT("9 not identified"), Host.Identity.IsIdentified());

			// 10: an answer with no salt: unavailable this launch, the held id dropped.
			StepIs(10, "identify(\"u4\")");
			Host.Relaunch();
			Host.Identify("u4");
			Host.OpenSession(nullptr);
			TestFalse(TEXT("10 dropped"), Host.Identity.IsIdentified());
			Host.Identify("u4");
			TestFalse(TEXT("10 not held later either"), Host.Identity.IsIdentified());
			Host.OpenSession(nullptr);
			TestEqual(TEXT("10 no send"), static_cast<int32>(Host.Sends.size()), 4);
			TestTrue(TEXT("10 nothing saved"), Host.Saved.empty());

			// 11: opt out deletes the saved identity.
			StepIs(11, "opt out");
			Host.Relaunch();
			Host.OpenSession(Salt);
			Host.Identify("u1");
			TestEqual(TEXT("11 five sends"), static_cast<int32>(Host.Sends.size()), 5);
			TestFalse(TEXT("11 saved"), Host.Saved.empty());
			Host.SetMayStore(false);
			TestTrue(TEXT("11 deleted"), Host.Saved.empty());
			TestFalse(TEXT("11 not identified"), Host.Identity.IsIdentified());
			Host.SetMayStore(true);
			Host.OpenSession(Salt);
			TestEqual(TEXT("11 no send"), static_cast<int32>(Host.Sends.size()), 5);
		});

		It("retries needs_funds at the next session, and a device reset keeps the identity", [this]()
		{
			FHost Host;
			Host.AnswerStatus = 429;
			Host.AnswerCode = "quota_exceeded";
			Host.OpenSession(Salt);
			Host.Identify("u3");
			TestEqual(TEXT("one send"), static_cast<int32>(Host.Sends.size()), 1);
			Host.TrySend();
			TestEqual(TEXT("no loop"), static_cast<int32>(Host.Sends.size()), 1);
			Host.AnswerStatus = 200;
			Host.AnswerCode.clear();
			Host.Session.clear(); // ResetDeviceId alone: the identity stays
			++Host.Device;
			TestTrue(TEXT("kept"), Host.Identity.IsIdentified());
			Host.OpenSession(Salt);
			TestEqual(TEXT("retried"), static_cast<int32>(Host.Sends.size()), 2);
			TestTrue(TEXT("on S2"), Host.SendAt(1).first == "S2");
			TestTrue(TEXT("same hash"), Host.SendAt(1).second == Host.SendAt(0).second);
		});

		It("follows the answer table", [this]()
		{
			struct FCase { int32 Status; const char* Code; bool bNextSessionRetries; };
			const FCase Cases[] = {
				{ 400, "invalid_account_hash", false },
				{ 400, "no_session", true },
				{ 404, "", false },
				{ 409, "session_taken", true },
				{ 409, "synthetic_session", true },
				{ 429, "identify_limited", true },
				{ 429, "rate_limited", true },
				{ 503, "", true },
				{ 0, "", true },
			};
			for (const FCase& Case : Cases)
			{
				FHost Host;
				Host.AnswerStatus = Case.Status;
				Host.AnswerCode = Case.Code;
				Host.OpenSession(Salt);
				Host.Identify("u1");
				Host.TrySend();
				TestEqual(Case.Code, static_cast<int32>(Host.Sends.size()), 1);
				TestFalse(Case.Code, Host.Saved.empty());
				Host.Session.clear();
				Host.OpenSession(Salt);
				TestEqual(Case.Code, static_cast<int32>(Host.Sends.size()), Case.bNextSessionRetries ? 2 : 1);
			}
		});
	});

	Describe("the request and the gates", [this]()
	{
		It("sends exactly the hash, never a device id", [this]()
		{
			const std::string Wire = Accept[0].Wire;
			TestTrue(TEXT("body"), RavensightIdentify::RequestBody(Wire) == "{\"accountHash\":\"" + Wire + "\"}");
			FHost Host;
			Host.OpenSession(Salt);
			Host.Identify(Accept[0].AccountId);
			TestEqual(TEXT("at once in an open session"), static_cast<int32>(Host.Sends.size()), 1);
			TestTrue(TEXT("same body"), Host.SendAt(0).second == RavensightIdentify::RequestBody(Wire));
		});

		It("holds an id in memory without consent or a key, then sends on the yes", [this]()
		{
			FHost Host;
			Host.bMayStore = false;
			TestTrue(TEXT("accepted"), Host.Identify("u1") == nullptr);
			TestTrue(TEXT("held"), Host.Identity.IsIdentified());
			TestTrue(TEXT("nothing saved"), Host.Saved.empty());
			TestTrue(TEXT("nothing sent"), Host.Sends.empty());
			Host.SetMayStore(true);
			TestTrue(TEXT("still nothing before a session"), Host.Sends.empty());
			Host.OpenSession(Salt);
			TestEqual(TEXT("sent"), static_cast<int32>(Host.Sends.size()), 1);
			TestTrue(TEXT("hash"), Host.SentHash(0) == Sequence[1].Send);
			TestFalse(TEXT("saved"), Host.Saved.empty());
		});

		It("drops a held id on opt-out and on reset", [this]()
		{
			FHost Host;
			Host.Identify("u1");
			Host.SetMayStore(false);
			TestFalse(TEXT("opt-out drops it"), Host.Identity.IsIdentified());
			Host.SetMayStore(true);
			const int32 Device = Host.Device;
			Host.Identify("u2");
			Host.Reset();
			TestFalse(TEXT("reset drops it"), Host.Identity.IsIdentified());
			TestTrue(TEXT("reset makes a fresh device"), Host.Device != Device);
			Host.OpenSession(Salt);
			TestTrue(TEXT("nothing sent"), Host.Sends.empty());
		});

		It("reads back only a well formed saved value", [this]()
		{
			FRavensightIdentity Identity;
			TestFalse(TEXT("empty"), Identity.Load(""));
			TestFalse(TEXT("short"), Identity.Load("abc:def"));
			TestFalse(TEXT("upper case"), Identity.Load(std::string(64, 'A') + ":" + std::string(64, 'a')));
			TestFalse(TEXT("not identified"), Identity.IsIdentified());
			TestTrue(TEXT("good"), Identity.Load(std::string(Salt) + ":" + Accept[0].Wire));
			TestTrue(TEXT("identified"), Identity.IsIdentified());
		});

		It("signs out a saved account when another was identified before the start", [this]()
		{
			// Identify("u2") before Init, then Init finds u1 saved.
			FRavensightIdentity Identity;
			FRavensightIdentityEffects Before;
			TestTrue(TEXT("held"), Identity.Identify("u2", false, Before) == nullptr);
			FRavensightIdentityEffects Effects;
			TestTrue(TEXT("adopted"), Identity.AdoptSaved(std::string(Salt) + ":" + Sequence[1].Stored, Effects));
			TestTrue(TEXT("saved one deleted"), Effects.bDeleteSaved);
			TestTrue(TEXT("fresh device"), Effects.bResetDevice);
			TestTrue(TEXT("u2 still held"), Identity.IsIdentified() && Identity.SavedText().empty());

			// The same account: kept, no reset.
			FRavensightIdentity Same;
			FRavensightIdentityEffects Held;
			Same.Identify("u1", false, Held);
			FRavensightIdentityEffects SameEffects;
			TestTrue(TEXT("adopted"), Same.AdoptSaved(std::string(Salt) + ":" + Sequence[1].Stored, SameEffects));
			TestFalse(TEXT("no delete"), SameEffects.bDeleteSaved);
			TestFalse(TEXT("no reset"), SameEffects.bResetDevice);
			TestTrue(TEXT("kept"), Same.SavedText() == std::string(Salt) + ":" + Sequence[1].Stored);

			// Nothing held: just loaded; malformed: refused.
			FRavensightIdentity Plain;
			FRavensightIdentityEffects PlainEffects;
			TestTrue(TEXT("loaded"), Plain.AdoptSaved(std::string(Salt) + ":" + Sequence[1].Stored, PlainEffects));
			TestFalse(TEXT("malformed"), Plain.AdoptSaved("x", PlainEffects));
		});
	});

	Describe("a late answer", [this]()
	{
		for (const int32 LateStatus : { 200, 404 })
		{
			It(LateStatus == 200 ? "a late 200 after a reset does not mark the new session"
				: "a late 404 after a reset changes nothing", [this, LateStatus]()
			{
				FHost Host;
				Host.OpenSession(Salt);
				FRavensightIdentityEffects Effects;
				Host.Identity.Identify("u1", true, Effects);
				std::string Body;
				int32 Generation = 0;
				const std::string OldSession = Host.Session;
				TestTrue(TEXT("begun"), Host.Identity.BeginSend(OldSession, Body, Generation));
				// Sign out and in as another account while the u1 answer is in flight.
				Host.Reset();
				Host.Identify("u2");
				Host.Identity.OnSendAnswer(Generation, OldSession, Host.Session, LateStatus, "");
				Host.OpenSession(Salt);
				TestEqual(TEXT("u2 sent"), static_cast<int32>(Host.Sends.size()), 1);
				TestTrue(TEXT("on a new session"), Host.SendAt(0).first != OldSession);
				TestTrue(TEXT("another hash"), Host.SendAt(0).second != Body);
			});
		}
	});
}

#endif
