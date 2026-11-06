// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "RavensightDelivery.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
	constexpr uint32_t KRound[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

	uint32_t Rotr(uint32_t X, int N)
	{
		return (X >> N) | (X << (32 - N));
	}

	/** SHA-256 of a byte string, as 32 raw bytes. */
	std::string Sha256(const std::string& Data)
	{
		uint32_t H[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
		std::string Padded = Data;
		Padded.push_back(static_cast<char>(0x80));
		while (Padded.size() % 64 != 56)
		{
			Padded.push_back('\0');
		}
		const uint64_t Bits = static_cast<uint64_t>(Data.size()) * 8u;
		for (int Shift = 56; Shift >= 0; Shift -= 8)
		{
			Padded.push_back(static_cast<char>((Bits >> Shift) & 0xff));
		}
		for (size_t Offset = 0; Offset < Padded.size(); Offset += 64)
		{
			uint32_t W[64];
			const unsigned char* Block = reinterpret_cast<const unsigned char*>(Padded.data()) + Offset;
			for (int I = 0; I < 16; ++I)
			{
				W[I] = (uint32_t(Block[I * 4]) << 24) | (uint32_t(Block[I * 4 + 1]) << 16) | (uint32_t(Block[I * 4 + 2]) << 8) | uint32_t(Block[I * 4 + 3]);
			}
			for (int I = 16; I < 64; ++I)
			{
				const uint32_t S0 = Rotr(W[I - 15], 7) ^ Rotr(W[I - 15], 18) ^ (W[I - 15] >> 3);
				const uint32_t S1 = Rotr(W[I - 2], 17) ^ Rotr(W[I - 2], 19) ^ (W[I - 2] >> 10);
				W[I] = W[I - 16] + S0 + W[I - 7] + S1;
			}
			uint32_t A = H[0], B = H[1], C = H[2], D = H[3], E = H[4], F = H[5], G = H[6], Hh = H[7];
			for (int I = 0; I < 64; ++I)
			{
				const uint32_t T1 = Hh + (Rotr(E, 6) ^ Rotr(E, 11) ^ Rotr(E, 25)) + ((E & F) ^ (~E & G)) + KRound[I] + W[I];
				const uint32_t T2 = (Rotr(A, 2) ^ Rotr(A, 13) ^ Rotr(A, 22)) + ((A & B) ^ (A & C) ^ (B & C));
				Hh = G; G = F; F = E; E = D + T1; D = C; C = B; B = A; A = T1 + T2;
			}
			H[0] += A; H[1] += B; H[2] += C; H[3] += D; H[4] += E; H[5] += F; H[6] += G; H[7] += Hh;
		}
		std::string Out;
		for (uint32_t Word : H)
		{
			for (int Shift = 24; Shift >= 0; Shift -= 8)
			{
				Out.push_back(static_cast<char>((Word >> Shift) & 0xff));
			}
		}
		return Out;
	}

	std::string Hex(const std::string& Bytes)
	{
		static const char Digits[] = "0123456789abcdef";
		std::string Out;
		for (unsigned char Byte : Bytes)
		{
			Out.push_back(Digits[Byte >> 4]);
			Out.push_back(Digits[Byte & 15]);
		}
		return Out;
	}

	bool IsDigits(const std::string& Text)
	{
		if (Text.empty())
		{
			return false;
		}
		for (char C : Text)
		{
			if (C < '0' || C > '9')
			{
				return false;
			}
		}
		return true;
	}

	std::string Upper(std::string Text)
	{
		for (char& C : Text)
		{
			C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
		}
		return Text;
	}

	std::string Trim(const std::string& Text)
	{
		size_t Start = 0;
		size_t End = Text.size();
		while (Start < End && std::isspace(static_cast<unsigned char>(Text[Start])))
		{
			++Start;
		}
		while (End > Start && std::isspace(static_cast<unsigned char>(Text[End - 1])))
		{
			--End;
		}
		return Text.substr(Start, End - Start);
	}

	/** The byte length of the UTF-8 sequence starting with Lead. */
	size_t SequenceLength(unsigned char Lead)
	{
		if (Lead < 0x80) return 1;
		if ((Lead >> 5) == 0x6) return 2;
		if ((Lead >> 4) == 0xE) return 3;
		if ((Lead >> 3) == 0x1E) return 4;
		return 1;
	}
}

namespace RavensightDelivery
{
	std::string HmacSha256Hex(const std::string& Secret, const std::string& Message)
	{
		std::string Key = Secret.size() > 64 ? Sha256(Secret) : Secret;
		Key.resize(64, '\0');
		std::string Inner(64, '\0');
		std::string Outer(64, '\0');
		for (int I = 0; I < 64; ++I)
		{
			Inner[I] = static_cast<char>(Key[I] ^ 0x36);
			Outer[I] = static_cast<char>(Key[I] ^ 0x5c);
		}
		return Hex(Sha256(Outer + Sha256(Inner + Message)));
	}

	std::string SignRequest(const std::string& Secret, int64_t Timestamp, const std::string& Body)
	{
		return "v1=" + HmacSha256Hex(Secret, "v1:" + std::to_string(Timestamp) + ":" + Body);
	}

	std::string QueueKeyHash(const std::string& Utf8)
	{
		uint64_t Hash = 0xcbf29ce484222325ULL;
		for (unsigned char Byte : Utf8)
		{
			Hash ^= Byte;
			Hash *= 0x100000001b3ULL;
		}
		char Out[17];
		std::snprintf(Out, sizeof(Out), "%016llx", static_cast<unsigned long long>(Hash));
		return Out;
	}

	std::string JsonString(const std::string& Utf8)
	{
		std::string Out = "\"";
		for (unsigned char C : Utf8)
		{
			switch (C)
			{
			case '"': Out += "\\\""; break;
			case '\\': Out += "\\\\"; break;
			case '\b': Out += "\\b"; break;
			case '\f': Out += "\\f"; break;
			case '\n': Out += "\\n"; break;
			case '\r': Out += "\\r"; break;
			case '\t': Out += "\\t"; break;
			default:
				if (C < 0x20)
				{
					char Escape[7];
					std::snprintf(Escape, sizeof(Escape), "\\u%04x", C);
					Out += Escape;
				}
				else
				{
					Out.push_back(static_cast<char>(C));
				}
			}
		}
		Out.push_back('"');
		return Out;
	}

	int Utf16Length(const std::string& Utf8)
	{
		int Units = 0;
		for (size_t I = 0; I < Utf8.size();)
		{
			const size_t Length = SequenceLength(static_cast<unsigned char>(Utf8[I]));
			Units += Length == 4 ? 2 : 1;
			I += Length;
		}
		return Units;
	}

	int ExemptOverheadBytes(const std::string& Utf8Value, int CapChars)
	{
		const int Chars = Utf16Length(Utf8Value);
		if (Chars > CapChars)
		{
			return 0;
		}
		return static_cast<int>(JsonString(Utf8Value).size()) - Chars;
	}

	int EventDataBytes(const std::string& DataJsonUtf8, bool bTypedError, const std::string& Message, const std::string& Stack)
	{
		int Bytes = static_cast<int>(DataJsonUtf8.size());
		if (bTypedError)
		{
			if (!Message.empty()) Bytes -= ExemptOverheadBytes(Message, MaxMessageChars);
			if (!Stack.empty()) Bytes -= ExemptOverheadBytes(Stack, MaxStackChars);
		}
		return Bytes;
	}

	std::string MicrosFromDecimalString(const std::string& Input)
	{
		std::string Text = Trim(Input);
		if (!Text.empty() && Text[0] == '+')
		{
			Text.erase(0, 1);
		}
		std::string IntPart;
		std::string Frac;
		int Exp = 0;
		size_t I = 0;
		while (I < Text.size() && std::isdigit(static_cast<unsigned char>(Text[I]))) IntPart.push_back(Text[I++]);
		if (I < Text.size() && Text[I] == '.')
		{
			++I;
			while (I < Text.size() && std::isdigit(static_cast<unsigned char>(Text[I]))) Frac.push_back(Text[I++]);
		}
		if (IntPart.empty() && Frac.empty())
		{
			return std::string();
		}
		if (I < Text.size() && (Text[I] == 'e' || Text[I] == 'E'))
		{
			++I;
			bool bNegative = false;
			if (I < Text.size() && (Text[I] == '+' || Text[I] == '-')) bNegative = Text[I++] == '-';
			std::string Digits;
			while (I < Text.size() && std::isdigit(static_cast<unsigned char>(Text[I]))) Digits.push_back(Text[I++]);
			if (Digits.empty() || Digits.size() > 3) return std::string();
			Exp = std::atoi(Digits.c_str()) * (bNegative ? -1 : 1);
		}
		if (I != Text.size())
		{
			return std::string();
		}
		if (Exp > 0)
		{
			std::string Moved = Frac.substr(0, static_cast<size_t>(Exp));
			Moved.resize(static_cast<size_t>(Exp), '0');
			IntPart += Moved;
			Frac = Frac.size() > static_cast<size_t>(Exp) ? Frac.substr(static_cast<size_t>(Exp)) : std::string();
		}
		else if (Exp < 0)
		{
			const size_t Shift = static_cast<size_t>(-Exp);
			std::string Padded = IntPart;
			if (Padded.size() < Shift) Padded.insert(0, Shift - Padded.size(), '0');
			Frac = Padded.substr(Padded.size() - Shift) + Frac;
			IntPart = Padded.substr(0, Padded.size() - Shift);
		}
		std::string Six = Frac.substr(0, 6);
		Six.resize(6, '0');
		const bool bRoundUp = Frac.size() > 6 && Frac[6] >= '5';
		std::string Digits = (IntPart.empty() ? std::string("0") : IntPart) + Six;
		if (bRoundUp)
		{
			int J = static_cast<int>(Digits.size()) - 1;
			while (J >= 0)
			{
				if (Digits[J] == '9') { Digits[J] = '0'; --J; }
				else { ++Digits[J]; break; }
			}
			if (J < 0) Digits.insert(0, "1");
		}
		const size_t First = Digits.find_first_not_of('0');
		Digits = First == std::string::npos ? std::string("0") : Digits.substr(First);
		return Digits.size() <= 18 ? Digits : std::string();
	}

	std::string MicrosFromRevenue(double Revenue)
	{
		if (!std::isfinite(Revenue) || Revenue < 0.0)
		{
			return std::string();
		}
		// The shortest text that reads back as the same double, so 0.0015 is
		// "0.0015" and not its binary expansion.
		char Buffer[64];
		for (int Precision = 1; Precision <= 17; ++Precision)
		{
			std::snprintf(Buffer, sizeof(Buffer), "%.*g", Precision, Revenue);
			if (std::strtod(Buffer, nullptr) == Revenue) break;
		}
		return MicrosFromDecimalString(Buffer);
	}

	std::string AdFormatFrom(const std::string& Name)
	{
		const std::string Key = Upper(Trim(Name));
		if (Key == "BANNER" || Key == "MREC" || Key == "LEADER") return "banner";
		if (Key == "INTER" || Key == "INTERSTITIAL") return "interstitial";
		if (Key == "REWARDED" || Key == "REWARDED_VIDEO" || Key == "REWARDED_INTER") return "rewarded";
		if (Key == "NATIVE") return "native";
		if (Key == "APP_OPEN") return "app_open";
		return "other";
	}

	std::string AdProblem(const std::string& Network, const std::string& Format, const std::string& Result,
		const std::string& Placement, const std::string& RevenueMicros, const std::string& Currency)
	{
		if (Network.empty() || Utf16Length(Network) > 64) return "network must be 1 to 64 characters";
		if (Format != "banner" && Format != "interstitial" && Format != "rewarded" && Format != "native"
			&& Format != "app_open" && Format != "other") return "format is not one of banner, interstitial, rewarded, native, app_open, other";
		if (Result != "shown" && Result != "clicked" && Result != "failed" && Result != "rewarded") return "result is not one of shown, clicked, failed, rewarded";
		if (Utf16Length(Placement) > 100) return "placement must be at most 100 characters";
		if (!RevenueMicros.empty())
		{
			if (!IsDigits(RevenueMicros) || RevenueMicros.size() > 18) return "revenue micros must be 1 to 18 digits";
			if (Currency.size() != 3) return "revenue needs a 3 letter currency";
			for (char C : Currency)
			{
				if (!std::isalpha(static_cast<unsigned char>(C))) return "revenue needs a 3 letter currency";
			}
			if (Result != "shown") return "revenue goes on the shown result only";
		}
		else if (!Currency.empty())
		{
			return "a currency needs a revenue";
		}
		return std::string();
	}

	std::string AttributionProblem(const std::string& Source, const std::string& Network, const std::string& Campaign,
		const std::string& Adset, const std::string& Creative, int64_t TouchTimeMs)
	{
		if (Source.empty() || Utf16Length(Source) > 64) return "source must be 1 to 64 characters";
		if (Utf16Length(Network) > 100) return "network must be at most 100 characters";
		if (Utf16Length(Campaign) > 200 || Utf16Length(Adset) > 200 || Utf16Length(Creative) > 200) return "campaign, adset and creative must be at most 200 characters";
		if (TouchTimeMs < 0 || TouchTimeMs > 9007199254740991LL) return "touch time must be epoch milliseconds";
		return std::string();
	}

	std::string DeviceField(const std::string& Utf8)
	{
		std::string Clean;
		for (size_t I = 0; I < Utf8.size();)
		{
			const unsigned char Lead = static_cast<unsigned char>(Utf8[I]);
			const size_t Length = SequenceLength(Lead);
			const bool bC0 = Lead < 0x20 || Lead == 0x7f;
			const bool bC1 = Length == 2 && Lead == 0xC2 && I + 1 < Utf8.size()
				&& static_cast<unsigned char>(Utf8[I + 1]) >= 0x80 && static_cast<unsigned char>(Utf8[I + 1]) <= 0x9f;
			if (!bC0 && !bC1)
			{
				Clean.append(Utf8, I, Length);
			}
			I += Length;
		}
		Clean = Trim(Clean);
		std::string Cut;
		int Units = 0;
		for (size_t I = 0; I < Clean.size();)
		{
			const size_t Length = SequenceLength(static_cast<unsigned char>(Clean[I]));
			const int Add = Length == 4 ? 2 : 1;
			if (Units + Add > MaxDeviceField) break;
			Cut.append(Clean, I, Length);
			Units += Add;
			I += Length;
		}
		return Trim(Cut);
	}

	std::string OsVersionFrom(const std::string& Utf8)
	{
		static const char* const Names[] = {"Mac OS X", "macOS", "iPadOS", "iOS", "Android OS", "Android", "Windows", "Linux", "tvOS", "visionOS"};
		std::string Text = Trim(Utf8);
		const std::string Lower = Upper(Text);
		for (const char* Name : Names)
		{
			const std::string Key = Upper(Name);
			if (Lower.compare(0, Key.size(), Key) == 0)
			{
				Text = Text.substr(Key.size());
				break;
			}
		}
		return DeviceField(Text);
	}
}
