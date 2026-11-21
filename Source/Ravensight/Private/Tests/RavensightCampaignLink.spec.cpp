// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The campaign link id: the 24 hex character check and reading rs_link out
// of an incoming address or query string the way the browser's
// URLSearchParams.get reads location.search (the JS SDK is the reference).
// Engine-free: runs in the host check too. What the subsystem sends, and
// when, is covered in RavensightSubsystem.spec.cpp.

#include "Misc/AutomationTest.h"
#include "RavensightCampaignLink.h"

#include <string>

#if WITH_DEV_AUTOMATION_TESTS

namespace RavensightCampaignLinkSpecCases
{
	struct FLinkCase { const char* Note; const char* Input; const char* Expected; };

	constexpr const char* Id = "64b0000000000000000000a1";

	/** URLSearchParams on location.search, as the contract lists them, then more. */
	constexpr FLinkCase Searches[] = {
		{ "the id among other parameters, upper case in", "?lang=en&rs_link=64B0000000000000000000A1&utm_source=x", Id },
		{ "too short", "?rs_link=abc", "" },
		{ "24 characters that are not hex", "?rs_link=gggggggggggggggggggggggg", "" },
		{ "no rs_link", "?ref=x", "" },
		{ "empty", "", "" },
		{ "two rs_link: the first wins", "?rs_link=64b0000000000000000000a1&rs_link=64b0000000000000000000b2", Id },
		{ "the hash is not part of it", "?a=1&rs_link=64b0000000000000000000a1#frag", Id },
		{ "percent escapes are decoded first", "?rs_link=%36%34b0000000000000000000a1", Id },
		{ "the first rs_link wins even when it is not an id", "?rs_link=abc&rs_link=64b0000000000000000000a1", "" },
		{ "a plus is a space", "?rs_link=+64b0000000000000000000a1", "" },
		{ "a trailing plus is a space", "?rs_link=64b0000000000000000000a1+", "" },
		{ "an escaped space", "?rs_link=%2064b0000000000000000000a1", "" },
		{ "the name is decoded too", "?rs%5Flink=64b0000000000000000000a1", Id },
		{ "the name is matched exactly", "?RS_LINK=64b0000000000000000000a1", "" },
		{ "empty pairs are skipped", "?&&rs_link=64b0000000000000000000a1&", Id },
		{ "a name without a value", "?rs_link", "" },
		{ "an empty value", "?rs_link=", "" },
		{ "a broken escape stays as it is", "?rs_link=%zz%", "" },
		{ "only the first = splits", "?rs_link=64b0000000000000000000a1=", "" },
		{ "23 characters", "?rs_link=64b0000000000000000000a", "" },
		{ "25 characters", "?rs_link=64b0000000000000000000a1f", "" },
	};

	/** Whole addresses a deep link, universal link or App Link handler receives, and referrers. */
	constexpr FLinkCase Urls[] = {
		{ "a universal link", "https://play.example.com/open?rs_link=64B0000000000000000000A1", Id },
		{ "a custom scheme", "mygame://open?level=2&rs_link=64b0000000000000000000a1", Id },
		{ "a fragment after the query", "https://example.com/p?rs_link=64b0000000000000000000a1#top", Id },
		{ "a query inside the fragment is not the query", "https://example.com/#/route?rs_link=64b0000000000000000000a1", "" },
		{ "no query", "https://example.com/rs_link", "" },
		{ "a bare query string, such as an install referrer", "utm_source=store&rs_link=64b0000000000000000000a1", Id },
		{ "non-ASCII elsewhere in the address", "https://example.com/caf\303\251?n=\303\251&rs_link=64b0000000000000000000a1", Id },
	};

	struct FValidCase { const char* Note; const char* Input; const char* Expected; };

	constexpr FValidCase Valid[] = {
		{ "upper case is lower cased", "64B0000000000000000000A1", Id },
		{ "lower case", "64b0000000000000000000a1", Id },
		{ "empty", "", "" },
		{ "23 characters", "aaaaaaaaaaaaaaaaaaaaaaa", "" },
		{ "25 characters", "aaaaaaaaaaaaaaaaaaaaaaaaa", "" },
		{ "24 that are not hex", "gggggggggggggggggggggggg", "" },
		{ "a leading space", " 64b0000000000000000000a1", "" },
		{ "a trailing space", "64b0000000000000000000a1 ", "" },
	};
}

BEGIN_DEFINE_SPEC(FRavensightCampaignLinkSpec, "Ravensight.CampaignLink", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
END_DEFINE_SPEC(FRavensightCampaignLinkSpec)

void FRavensightCampaignLinkSpec::Define()
{
	using namespace RavensightCampaignLinkSpecCases;

	Describe("the id check", [this]()
	{
		It("takes exactly 24 hex characters in any case, lower cased, and never trims", [this]()
		{
			int32 Count = 0;
			for (const FValidCase& Case : Valid)
			{
				TestTrue(Case.Note, RavensightCampaignLink::Valid(Case.Input) == std::string(Case.Expected));
				++Count;
			}
			TestEqual(TEXT("cases"), Count, 8);
			TestEqual(TEXT("id length"), static_cast<int32>(RavensightCampaignLink::IdLength), 24);
			TestTrue(TEXT("parameter name"), std::string(RavensightCampaignLink::Parameter) == "rs_link");
		});
	});

	Describe("rs_link in a query string", [this]()
	{
		It("reads location.search as URLSearchParams.get does", [this]()
		{
			int32 Count = 0;
			for (const FLinkCase& Case : Searches)
			{
				TestTrue(Case.Note, RavensightCampaignLink::FromUrl(Case.Input) == std::string(Case.Expected));
				++Count;
			}
			TestEqual(TEXT("cases"), Count, 21);
		});

		It("reads the query of a whole address", [this]()
		{
			int32 Count = 0;
			for (const FLinkCase& Case : Urls)
			{
				TestTrue(Case.Note, RavensightCampaignLink::FromUrl(Case.Input) == std::string(Case.Expected));
				++Count;
			}
			TestEqual(TEXT("cases"), Count, 7);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
