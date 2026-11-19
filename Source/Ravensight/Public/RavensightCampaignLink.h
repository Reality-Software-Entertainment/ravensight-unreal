// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Campaign links, mirroring the JS SDK: a Ravensight campaign link adds
// rs_link=<id> to the address it redirects to, and the SDK sends the id once,
// as "campaignLink" on the next POST /session. This is the engine-free part:
// the id check and reading rs_link out of an address or a query string. All
// text is UTF-8 in std::string, so the same code runs in the engine and in
// the host check. What the subsystem sends, and when, is in
// RavensightSubsystem.cpp.

#pragma once

#include <cstddef>
#include <string>

namespace RavensightCampaignLink
{
	/** The parameter a Ravensight campaign link adds to the address it redirects to. */
	inline constexpr const char* Parameter = "rs_link";

	/** A campaign link id is this many hex characters. */
	inline constexpr std::size_t IdLength = 24;

	/** The id lower cased when it is exactly 24 hex characters, in any case; otherwise empty. Never trims. */
	std::string Valid(const std::string& Id);

	/**
	 * The campaign link id in the query of an address (a deep link, universal
	 * link or App Link), or in a bare query string such as an install
	 * referrer, lower cased; empty when there is none or it is not an id.
	 * Read the way the browser's URLSearchParams.get reads location.search:
	 * anything from the first "#" on is not part of it, the query starts
	 * after the first "?" (the whole text when there is none), the first
	 * rs_link wins, and "+" is a space and percent escapes are decoded, in the
	 * name and the value.
	 */
	std::string FromUrl(const std::string& UrlUtf8);
}
