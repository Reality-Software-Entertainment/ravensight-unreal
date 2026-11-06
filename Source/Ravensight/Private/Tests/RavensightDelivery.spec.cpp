// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The platform's delivery contract vectors (docs/sdk/delivery-vectors.json in
// the platform repo), the queue file name, micros, the ad and attribution
// checks and the device fields. Engine-free: runs in the host check too.

#include "Misc/AutomationTest.h"
#include "RavensightDelivery.h"

#include <string>

#if WITH_DEV_AUTOMATION_TESTS

namespace RavensightDeliverySpecVectors
{
	const std::string Secret = "dGVzdC1zZWNyZXQtZm9yLWRlbGl2ZXJ5LWNvbnRyYWN0";

	std::string Repeat(const std::string& Unit, int Count)
	{
		std::string Out;
		for (int I = 0; I < Count; ++I) Out += Unit;
		return Out;
	}

	/** A typed error's data as the SDK writes it, with an optional extra key. */
	std::string ErrorData(const std::string& Message, const std::string& Stack, const std::string& Context = std::string())
	{
		std::string Json = R"J({"rs_kind":"error","rs_severity":"error","rs_message":)J" + RavensightDelivery::JsonString(Message)
			+ R"J(,"rs_stack":)J" + RavensightDelivery::JsonString(Stack);
		if (!Context.empty()) Json += R"J(,"context":)J" + RavensightDelivery::JsonString(Context);
		return Json + "}";
	}

	bool Fits(const std::string& Message, const std::string& Stack, const std::string& Context = std::string())
	{
		return RavensightDelivery::EventDataBytes(ErrorData(Message, Stack, Context), true, Message, Stack) <= RavensightDelivery::MaxEventBytes;
	}
}

BEGIN_DEFINE_SPEC(FRavensightDeliverySpec, "Ravensight.Delivery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
END_DEFINE_SPEC(FRavensightDeliverySpec)

void FRavensightDeliverySpec::Define()
{
	using namespace RavensightDeliverySpecVectors;

	Describe("the shared delivery vectors", [this]()
	{
		It("signs every header vector", [this]()
		{
			TestTrue(TEXT("batch"), RavensightDelivery::SignRequest(Secret, 1767225600,
				R"J({"events":[{"event":"pickup","timestamp":1767225590123,"data":{"rs_event_id":"a1b2c3d4e5f6a7b8-0001"}}]})J")
				== "v1=1a08dbdfe81eac30363314d0a0b2cf79389c96c539fc56a4a1edaaa3f51146f6");
			TestTrue(TEXT("single"), RavensightDelivery::SignRequest(Secret, 1767225600, R"J({"event":"game_started"})J")
				== "v1=03331575ba8750faafc921588376ae6489dc6b799737ed1aa52e15a289ce2ddd");
			TestTrue(TEXT("empty body"), RavensightDelivery::SignRequest(Secret, 1767225600, "")
				== "v1=a99ea76fc1e223dc0b5c81696000e33eff661ccad01cc0e8f42781d3a000a7a4");
			TestTrue(TEXT("non-ASCII as UTF-8"), RavensightDelivery::SignRequest(Secret, 1767225601,
				"{\"events\":[{\"event\":\"caf\xc3\xa9\",\"data\":{\"note\":\"\xc3\xbc" "ber \xe2\x98\x83\"}}]}")
				== "v1=50c51eff7578c945aeb8cc57fd2ff6431443a38102d3bbee67da106bac8b163a");
		});

		It("signs the beacon vector", [this]()
		{
			TestTrue(TEXT("beacon sig"), RavensightDelivery::HmacSha256Hex(Secret,
				R"J(v1:1767225600:{"events":[{"event":"game_exited","timestamp":1767225599000,"data":{"rs_event_id":"a1b2c3d4e5f6a7b8-0002"}}]})J")
				== "dedeb1fd59529ea9b54a14401d8f4ef13bacb6325b68bb8a2e916f30ff4c6e65");
		});

		It("measures every 8 KB vector as the server does", [this]()
		{
			const std::string Control = "\x01";
			const std::string Emoji = "\xf0\x9f\x98\x80";
			TestTrue(TEXT("2,000 control characters of stack and 200 of message fit"), Fits(Repeat(Control, 200), Repeat(Control, 2000)));
			TestTrue(TEXT("2,000 characters of four-byte text fit"), Fits("boom", Repeat(Emoji, 1000)));
			const std::string Plain = R"J({"rs_stack":)J" + RavensightDelivery::JsonString(Repeat(Control, 2000)) + "}";
			TestFalse(TEXT("the same on an event that is not a typed error"),
				RavensightDelivery::EventDataBytes(Plain, false, std::string(), std::string()) <= RavensightDelivery::MaxEventBytes);
			TestFalse(TEXT("the rest of the data counts in full"), Fits("boom", Repeat("s", 2000), Repeat(Control, 1100)));
			TestFalse(TEXT("a stack past 2,000 characters counts in bytes"), Fits("boom", Repeat(Control, 2001)));
		});

		It("names the queue file by 64-bit FNV-1a", [this]()
		{
			TestTrue(TEXT("empty"), RavensightDelivery::QueueKeyHash("") == "cbf29ce484222325");
			TestTrue(TEXT("a"), RavensightDelivery::QueueKeyHash("a") == "af63dc4c8601ec8c");
			TestTrue(TEXT("foobar"), RavensightDelivery::QueueKeyHash("foobar") == "85944171f73967e8");
		});
	});

	Describe("ads and attribution", [this]()
	{
		It("converts mediation doubles to micros on their digits, half up", [this]()
		{
			TestTrue(TEXT("0.0015"), RavensightDelivery::MicrosFromRevenue(0.0015) == "1500");
			TestTrue(TEXT("1.23456789"), RavensightDelivery::MicrosFromRevenue(1.23456789) == "1234568");
			TestTrue(TEXT("0.1 + 0.2"), RavensightDelivery::MicrosFromRevenue(0.1 + 0.2) == "300000");
			TestTrue(TEXT("5e-7 rounds up"), RavensightDelivery::MicrosFromRevenue(5e-7) == "1");
			TestTrue(TEXT("negative"), RavensightDelivery::MicrosFromRevenue(-1.0).empty());
			TestTrue(TEXT("text"), RavensightDelivery::MicrosFromDecimalString("0.0015") == "1500");
			TestTrue(TEXT("carry"), RavensightDelivery::MicrosFromDecimalString("0.9999995") == "1000000");
			TestTrue(TEXT("exponent"), RavensightDelivery::MicrosFromDecimalString("1.5e-3") == "1500");
			TestTrue(TEXT("not a number"), RavensightDelivery::MicrosFromDecimalString("abc").empty());
			TestTrue(TEXT("past 18 digits"), RavensightDelivery::MicrosFromDecimalString("1000000000000").empty());
		});

		It("refuses revenue without a currency and the other values the server flags", [this]()
		{
			TestTrue(TEXT("sendable"), RavensightDelivery::AdProblem("admob", "rewarded", "shown", "level_end", "1500", "USD").empty());
			TestFalse(TEXT("no currency"), RavensightDelivery::AdProblem("admob", "rewarded", "shown", "x", "1500", "").empty());
			TestFalse(TEXT("revenue on a click"), RavensightDelivery::AdProblem("admob", "banner", "clicked", "", "10", "USD").empty());
			TestFalse(TEXT("decimal micros"), RavensightDelivery::AdProblem("admob", "banner", "shown", "", "1.5", "USD").empty());
			TestFalse(TEXT("bad format"), RavensightDelivery::AdProblem("admob", "video", "shown", "", "", "").empty());
			TestFalse(TEXT("bad result"), RavensightDelivery::AdProblem("admob", "banner", "seen", "", "", "").empty());
			TestFalse(TEXT("no network"), RavensightDelivery::AdProblem("", "banner", "shown", "", "", "").empty());
			TestFalse(TEXT("currency alone"), RavensightDelivery::AdProblem("admob", "banner", "shown", "", "", "USD").empty());
			TestTrue(TEXT("attribution"), RavensightDelivery::AttributionProblem("store_referrer", "google", "spring", "", "", 0).empty());
			TestFalse(TEXT("no source"), RavensightDelivery::AttributionProblem("", "", "", "", "", 0).empty());
		});

		It("maps mediation format names", [this]()
		{
			TestTrue(TEXT("MREC"), RavensightDelivery::AdFormatFrom("MREC") == "banner");
			TestTrue(TEXT("INTER"), RavensightDelivery::AdFormatFrom("INTER") == "interstitial");
			TestTrue(TEXT("rewarded_video"), RavensightDelivery::AdFormatFrom("rewarded_video") == "rewarded");
			TestTrue(TEXT("APP_OPEN"), RavensightDelivery::AdFormatFrom("APP_OPEN") == "app_open");
			TestTrue(TEXT("other"), RavensightDelivery::AdFormatFrom("SPLASH") == "other");
		});
	});

	Describe("device fields", [this]()
	{
		It("keeps printable text, trimmed, at most 128 characters", [this]()
		{
			TestTrue(TEXT("control removed"), RavensightDelivery::DeviceField("  MacBookPro18,3\x01 ") == "MacBookPro18,3");
			TestTrue(TEXT("cut"), RavensightDelivery::DeviceField(Repeat("g", 200)) == Repeat("g", 128));
			TestTrue(TEXT("empty"), RavensightDelivery::DeviceField(" \x02 ").empty());
		});

		It("drops the OS name from the version", [this]()
		{
			TestTrue(TEXT("mac"), RavensightDelivery::OsVersionFrom("Mac OS X 14.2.1") == "14.2.1");
			TestTrue(TEXT("android"), RavensightDelivery::OsVersionFrom("Android OS 14 / API-34") == "14 / API-34");
			TestTrue(TEXT("version only"), RavensightDelivery::OsVersionFrom("10.0.22631") == "10.0.22631");
			TestTrue(TEXT("name only"), RavensightDelivery::OsVersionFrom("Linux").empty());
		});
	});
}

#endif
