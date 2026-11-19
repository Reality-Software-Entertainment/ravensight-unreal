// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "RavensightCampaignLink.h"

namespace
{
	bool IsHexChar(const char C)
	{
		return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f') || (C >= 'A' && C <= 'F');
	}

	int HexValue(const char C)
	{
		if (C >= '0' && C <= '9') return C - '0';
		if (C >= 'a' && C <= 'f') return C - 'a' + 10;
		return C - 'A' + 10;
	}

	/**
	 * application/x-www-form-urlencoded decoding as URLSearchParams does it:
	 * "+" is a space and "%XY" is a byte; a "%" without two hex digits stays
	 * as it is. The bytes stay UTF-8: only ASCII is compared afterwards.
	 */
	std::string FormDecode(const std::string& Text)
	{
		std::string Out;
		Out.reserve(Text.size());
		for (std::size_t Index = 0; Index < Text.size(); ++Index)
		{
			const char C = Text[Index];
			if (C == '+')
			{
				Out.push_back(' ');
			}
			else if (C == '%' && Index + 2 < Text.size() && IsHexChar(Text[Index + 1]) && IsHexChar(Text[Index + 2]))
			{
				Out.push_back(static_cast<char>(HexValue(Text[Index + 1]) * 16 + HexValue(Text[Index + 2])));
				Index += 2;
			}
			else
			{
				Out.push_back(C);
			}
		}
		return Out;
	}
}

namespace RavensightCampaignLink
{
	std::string Valid(const std::string& Id)
	{
		if (Id.size() != IdLength) return std::string();
		std::string Lower;
		Lower.reserve(IdLength);
		for (const char C : Id)
		{
			if (!IsHexChar(C)) return std::string();
			Lower.push_back(C >= 'A' && C <= 'F' ? static_cast<char>(C - 'A' + 'a') : C);
		}
		return Lower;
	}

	std::string FromUrl(const std::string& UrlUtf8)
	{
		std::string Query = UrlUtf8.substr(0, UrlUtf8.find('#'));
		const std::size_t QuestionMark = Query.find('?');
		if (QuestionMark != std::string::npos) Query = Query.substr(QuestionMark + 1);

		std::size_t Start = 0;
		while (Start <= Query.size())
		{
			std::size_t End = Query.find('&', Start);
			if (End == std::string::npos) End = Query.size();
			const std::string Pair = Query.substr(Start, End - Start);
			Start = End + 1;
			if (Pair.empty()) continue;

			const std::size_t Equals = Pair.find('=');
			if (FormDecode(Pair.substr(0, Equals)) != Parameter) continue;
			return Valid(Equals == std::string::npos ? std::string() : FormDecode(Pair.substr(Equals + 1)));
		}
		return std::string();
	}
}
