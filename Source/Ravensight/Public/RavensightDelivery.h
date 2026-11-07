// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The pure parts of the platform's SDK contracts: request signing, the beacon
// signature, the queue file name, the server's event size measure, micros,
// the ad format names, ad and attribution checks, and device fields. All text
// is UTF-8 in std::string, so the same code runs in the engine and in the
// engine-free host check, and every shared delivery vector runs against it.

#pragma once

#include <cstdint>
#include <string>

namespace RavensightDelivery
{
	/** A body this large or larger, before compression, is sent gzip compressed. */
	inline constexpr int GzipMinBytes = 1024;

	/** The largest data the server takes for one event, as it measures it. */
	inline constexpr int MaxEventBytes = 8192;

	/** Typed error text caps the server exempts from the byte ceiling. */
	inline constexpr int MaxMessageChars = 200;
	inline constexpr int MaxStackChars = 2000;
	inline constexpr int MaxTrailChars = 1500;

	/** Device fields are cut to this many characters. */
	inline constexpr int MaxDeviceField = 128;

	/** The persisted queue keeps at most this many events. */
	inline constexpr int MaxPersistedEvents = 500;

	/** Events older than this are dropped when the queue is loaded. */
	inline constexpr int64_t MaxQueueAgeMs = 89LL * 86400LL * 1000LL;

	/** Lowercase hex HMAC-SHA256 keyed with the UTF-8 bytes of Secret over the UTF-8 bytes of Message. */
	std::string HmacSha256Hex(const std::string& Secret, const std::string& Message);

	/** The X-Ravensight-Signature value: "v1=" and the HMAC of "v1:<timestamp>:<body>", body before gzip. */
	std::string SignRequest(const std::string& Secret, int64_t Timestamp, const std::string& Body);

	/** 64-bit FNV-1a of the bytes as 16 lowercase hex characters: the queue file's name. */
	std::string QueueKeyHash(const std::string& Utf8);

	/** Text as a JSON string literal, quotes included, escaping as the engine's condensed writer does. */
	std::string JsonString(const std::string& Utf8);

	/** UTF-16 code units in UTF-8 text: the server's character count. */
	int Utf16Length(const std::string& Utf8);

	/**
	 * How many bytes a typed error's message, stack or trail, within its cap, takes
	 * as JSON beyond one byte a character. EventDataBytes subtracts it.
	 */
	int ExemptOverheadBytes(const std::string& Utf8Value, int CapChars);

	/**
	 * One event's data size as the server measures it: the UTF-8 bytes of its
	 * JSON, less the overhead of a typed error's message, stack and trail when
	 * the event is a typed error (pass empty text otherwise).
	 */
	int EventDataBytes(const std::string& DataJsonUtf8, bool bTypedError, const std::string& Message, const std::string& Stack,
		const std::string& Trail = std::string());

	/** "0.0015" is "1500": whole micros on the decimal digits, half up past six decimals. Empty when invalid. */
	std::string MicrosFromDecimalString(const std::string& Text);

	/** A mediation callback's double in major units as micros, from its shortest round-trip text. */
	std::string MicrosFromRevenue(double Revenue);

	/** A mediation SDK's ad format name mapped onto rs_ad_format. */
	std::string AdFormatFrom(const std::string& Name);

	/** Why an ad event would be flagged by the server, or empty when it is sendable. */
	std::string AdProblem(const std::string& Network, const std::string& Format, const std::string& Result,
		const std::string& Placement, const std::string& RevenueMicros, const std::string& Currency);

	/** Why an attribution event would be flagged, or empty. Empty optional fields are omitted, not flagged. */
	std::string AttributionProblem(const std::string& Source, const std::string& Network, const std::string& Campaign,
		const std::string& Adset, const std::string& Creative, int64_t TouchTimeMs);

	/** A device field as the contract allows: printable, trimmed, at most 128 characters, or empty. */
	std::string DeviceField(const std::string& Utf8);

	/** The version part of an OS description, its name removed: "Mac OS X 14.2.1" is "14.2.1". */
	std::string OsVersionFrom(const std::string& Utf8);
}
