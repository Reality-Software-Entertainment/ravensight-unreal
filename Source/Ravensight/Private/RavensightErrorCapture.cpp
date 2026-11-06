// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "RavensightErrorCapture.h"

// --- Character classes --------------------------------------------------------
//
// ASCII only, like the server's regular expressions: \w, \d and \s never match
// a character outside ASCII there, so they never do here.

namespace
{
	uint32 Code(TCHAR C) { return static_cast<uint32>(C); }

	bool IsDigit(TCHAR C) { return Code(C) >= '0' && Code(C) <= '9'; }
	bool IsUpper(TCHAR C) { return Code(C) >= 'A' && Code(C) <= 'Z'; }
	bool IsLower(TCHAR C) { return Code(C) >= 'a' && Code(C) <= 'z'; }
	bool IsAlpha(TCHAR C) { return IsUpper(C) || IsLower(C); }
	bool IsAlnum(TCHAR C) { return IsAlpha(C) || IsDigit(C); }
	bool IsWord(TCHAR C) { return IsAlnum(C) || Code(C) == '_'; }
	bool IsHex(TCHAR C) { return IsDigit(C) || (Code(C) >= 'a' && Code(C) <= 'f') || (Code(C) >= 'A' && Code(C) <= 'F'); }
	bool IsSpace(TCHAR C)
	{
		const uint32 V = Code(C);
		return V == ' ' || V == '\t' || V == '\n' || V == '\r' || V == '\v' || V == '\f';
	}
	bool Is(TCHAR C, char Ascii) { return Code(C) == static_cast<uint32>(static_cast<unsigned char>(Ascii)); }
	bool IsQuoteOrAngle(TCHAR C) { return Is(C, '\'') || Is(C, '"') || Is(C, '<') || Is(C, '>'); }

	TCHAR Lower(TCHAR C) { return IsUpper(C) ? static_cast<TCHAR>(Code(C) + 32) : C; }

	/** Characters [From, To) of Text. */
	FString Slice(const FString& Text, int32 From, int32 To)
	{
		FString Out;
		for (int32 Index = FMath::Max(0, From); Index < To && Index < Text.Len(); ++Index)
		{
			Out.AppendChar(Text[Index]);
		}
		return Out;
	}

	/** True when Text at Index starts with Ascii, ignoring case when asked. */
	bool At(const FString& Text, int32 Index, const char* Ascii, bool bIgnoreCase)
	{
		for (int32 Offset = 0; Ascii[Offset] != 0; ++Offset)
		{
			if (Index + Offset >= Text.Len())
			{
				return false;
			}
			const TCHAR C = Text[Index + Offset];
			const TCHAR Want = static_cast<TCHAR>(static_cast<unsigned char>(Ascii[Offset]));
			if (bIgnoreCase ? Lower(C) != Lower(Want) : C != Want)
			{
				return false;
			}
		}
		return true;
	}

	/** A regular expression \b before Index: the word state changes there. */
	bool WordStart(const FString& Text, int32 Index)
	{
		const bool bBefore = Index > 0 && IsWord(Text[Index - 1]);
		const bool bHere = Index < Text.Len() && IsWord(Text[Index]);
		return bBefore != bHere;
	}

	/** Copies Text, letting Match decide at each index: return the end of a match and fill Replacement, or -1. */
	template <typename FMatcher>
	FString ReplaceAll(const FString& Text, FMatcher Match)
	{
		FString Out;
		int32 Index = 0;
		while (Index < Text.Len())
		{
			FString Replacement;
			const int32 End = Match(Index, Replacement);
			if (End > Index)
			{
				Out += Replacement;
				Index = End;
			}
			else
			{
				Out.AppendChar(Text[Index]);
				++Index;
			}
		}
		return Out;
	}

	bool HasDigit(const FString& Text, int32 From, int32 To)
	{
		for (int32 Index = From; Index < To; ++Index)
		{
			if (IsDigit(Text[Index]))
			{
				return true;
			}
		}
		return false;
	}

	// --- Scrub steps ------------------------------------------------------------

	/** scheme://[user@]host/path[?query][#fragment] keeps scheme, host and path. */
	FString ScrubUrls(const FString& Text)
	{
		return ReplaceAll(Text, [&Text](int32 Start, FString& Out) -> int32
		{
			if (!IsAlpha(Text[Start]) || !WordStart(Text, Start))
			{
				return -1;
			}
			int32 Index = Start + 1;
			while (Index < Text.Len() && (IsAlnum(Text[Index]) || Is(Text[Index], '+') || Is(Text[Index], '.') || Is(Text[Index], '-')))
			{
				++Index;
			}
			if (!At(Text, Index, "://", false))
			{
				return -1;
			}
			Index += 3;
			const int32 SchemeEnd = Index;

			// user:password@ before the host.
			int32 Probe = Index;
			while (Probe < Text.Len() && !IsSpace(Text[Probe]) && !Is(Text[Probe], '/') && !Is(Text[Probe], '?')
				&& !Is(Text[Probe], '#') && !Is(Text[Probe], '@') && !IsQuoteOrAngle(Text[Probe]))
			{
				++Probe;
			}
			if (Probe < Text.Len() && Is(Text[Probe], '@'))
			{
				Index = Probe + 1;
			}

			const int32 PathStart = Index;
			while (Index < Text.Len() && !IsSpace(Text[Index]) && !Is(Text[Index], '?') && !Is(Text[Index], '#') && !IsQuoteOrAngle(Text[Index]))
			{
				++Index;
			}
			const int32 PathEnd = Index;

			if (Index < Text.Len() && (Is(Text[Index], '?') || Is(Text[Index], '#')))
			{
				while (Index < Text.Len() && !IsSpace(Text[Index]) && !IsQuoteOrAngle(Text[Index]))
				{
					++Index;
				}
			}

			Out = Slice(Text, Start, SchemeEnd) + Slice(Text, PathStart, PathEnd);
			return Index;
		});
	}

	bool IsBearerChar(TCHAR C)
	{
		return IsAlnum(C) || Is(C, '-') || Is(C, '.') || Is(C, '_') || Is(C, '~') || Is(C, '+') || Is(C, '/');
	}

	bool IsUrlSafeB64(TCHAR C) { return IsAlnum(C) || Is(C, '_') || Is(C, '-'); }

	/** Bearer tokens, JWT shaped strings and gt_ keys. */
	FString ScrubTokens(const FString& Text)
	{
		FString Step = ReplaceAll(Text, [&Text](int32 Start, FString& Out) -> int32
		{
			if (!WordStart(Text, Start) || !At(Text, Start, "bearer", true))
			{
				return -1;
			}
			int32 Index = Start + 6;
			const int32 SpaceStart = Index;
			while (Index < Text.Len() && IsSpace(Text[Index])) ++Index;
			if (Index == SpaceStart) return -1;
			const int32 TokenStart = Index;
			while (Index < Text.Len() && IsBearerChar(Text[Index])) ++Index;
			if (Index == TokenStart) return -1;
			while (Index < Text.Len() && Is(Text[Index], '=')) ++Index;
			Out = TEXT("[token]");
			return Index;
		});

		const FString AfterBearer = Step;
		Step = ReplaceAll(AfterBearer, [&AfterBearer](int32 Start, FString& Out) -> int32
		{
			const FString& T = AfterBearer;
			if (!WordStart(T, Start) || !At(T, Start, "eyJ", false))
			{
				return -1;
			}
			int32 Index = Start + 3;
			const int32 First = Index;
			while (Index < T.Len() && IsUrlSafeB64(T[Index])) ++Index;
			if (Index == First || Index >= T.Len() || !Is(T[Index], '.')) return -1;
			++Index;
			const int32 Second = Index;
			while (Index < T.Len() && IsUrlSafeB64(T[Index])) ++Index;
			if (Index == Second || Index >= T.Len() || !Is(T[Index], '.')) return -1;
			++Index;
			while (Index < T.Len() && IsUrlSafeB64(T[Index])) ++Index;
			Out = TEXT("[token]");
			return Index;
		});

		const FString AfterJwt = Step;
		return ReplaceAll(AfterJwt, [&AfterJwt](int32 Start, FString& Out) -> int32
		{
			const FString& T = AfterJwt;
			if (!WordStart(T, Start) || !At(T, Start, "gt_", false))
			{
				return -1;
			}
			int32 Index = Start + 3;
			while (Index < T.Len() && IsWord(T[Index])) ++Index;
			if (Index == Start + 3) return -1;
			Out = TEXT("[token]");
			return Index;
		});
	}

	bool IsEmailLocal(TCHAR C) { return IsAlnum(C) || Is(C, '.') || Is(C, '_') || Is(C, '%') || Is(C, '+') || Is(C, '-'); }
	bool IsEmailDomain(TCHAR C) { return IsAlnum(C) || Is(C, '.') || Is(C, '-'); }

	/**
	 * local@domain.tld. The domain is the longest run of domain characters
	 * cut back to its last dot followed by two letters or more, as the
	 * server's greedy expression backtracks.
	 */
	int32 EmailDomainEnd(const FString& Text, int32 DomainStart)
	{
		int32 RunEnd = DomainStart;
		while (RunEnd < Text.Len() && IsEmailDomain(Text[RunEnd])) ++RunEnd;
		for (int32 Dot = RunEnd - 1; Dot > DomainStart; --Dot)
		{
			if (!Is(Text[Dot], '.'))
			{
				continue;
			}
			int32 Letters = Dot + 1;
			while (Letters < RunEnd && IsAlpha(Text[Letters])) ++Letters;
			if (Letters - (Dot + 1) >= 2)
			{
				return Letters;
			}
		}
		return -1;
	}

	FString ScrubEmails(const FString& Text)
	{
		FString Out;
		int32 Copied = 0;
		for (int32 At_ = 0; At_ < Text.Len(); ++At_)
		{
			if (!Is(Text[At_], '@'))
			{
				continue;
			}
			int32 LocalStart = At_;
			while (LocalStart > Copied && IsEmailLocal(Text[LocalStart - 1])) --LocalStart;
			if (LocalStart == At_)
			{
				continue;
			}
			const int32 End = EmailDomainEnd(Text, At_ + 1);
			if (End < 0)
			{
				continue;
			}
			Out += Slice(Text, Copied, LocalStart);
			Out += TEXT("[email]");
			Copied = End;
			At_ = End - 1;
		}
		Out += Slice(Text, Copied, Text.Len());
		return Out;
	}

	/** A home folder is the whole segment up to the next slash, quote, <, >,
	 *  ':' or end of line, spaces included (contract 5): a name can be Jo Ann. */
	bool IsHomeNameChar(TCHAR C)
	{
		return !(Is(C, '/') || Is(C, '\\') || Is(C, '\'') || Is(C, '"') || Is(C, '<') || Is(C, '>')
			|| Is(C, ':') || Is(C, '\r') || Is(C, '\n'));
	}

	FString ScrubHomes(const FString& Text)
	{
		const FString Windows = ReplaceAll(Text, [&Text](int32 Start, FString& Out) -> int32
		{
			if (!IsAlpha(Text[Start]) || !WordStart(Text, Start) || Start + 2 >= Text.Len() || !Is(Text[Start + 1], ':'))
			{
				return -1;
			}
			const TCHAR Slash = Text[Start + 2];
			if (!(Is(Slash, '\\') || Is(Slash, '/')))
			{
				return -1;
			}
			int32 Index = -1;
			if (At(Text, Start + 3, "users", true)) Index = Start + 3 + 5;
			else if (At(Text, Start + 3, "documents and settings", true)) Index = Start + 3 + 22;
			if (Index < 0) return -1;
			if (Index >= Text.Len() || !(Is(Text[Index], '\\') || Is(Text[Index], '/')))
			{
				return -1;
			}
			++Index;
			const int32 NameStart = Index;
			while (Index < Text.Len() && IsHomeNameChar(Text[Index])) ++Index;
			if (Index == NameStart) return -1;
			Out = TEXT("~");
			return Index;
		});

		return ReplaceAll(Windows, [&Windows](int32 Start, FString& Out) -> int32
		{
			int32 Index = -1;
			if (At(Windows, Start, "/Users/", false)) Index = Start + 7;
			else if (At(Windows, Start, "/home/", false)) Index = Start + 6;
			if (Index < 0) return -1;
			const int32 NameStart = Index;
			while (Index < Windows.Len() && IsHomeNameChar(Windows[Index])) ++Index;
			if (Index == NameStart) return -1;
			Out = TEXT("~");
			return Index;
		});
	}

	FString ScrubIpv4(const FString& Text)
	{
		return ReplaceAll(Text, [&Text](int32 Start, FString& Out) -> int32
		{
			if (!IsDigit(Text[Start]) || !WordStart(Text, Start))
			{
				return -1;
			}
			int32 Index = Start;
			for (int32 Group = 0; Group < 4; ++Group)
			{
				const int32 GroupStart = Index;
				while (Index < Text.Len() && IsDigit(Text[Index])) ++Index;
				const int32 Digits = Index - GroupStart;
				if (Digits < 1 || Digits > 3) return -1;
				if (Group < 3)
				{
					if (Index >= Text.Len() || !Is(Text[Index], '.')) return -1;
					++Index;
				}
			}
			if (Index < Text.Len() && IsWord(Text[Index])) return -1;
			Out = TEXT("[ip]");
			return Index;
		});
	}

	bool IsIpv6Char(TCHAR C) { return IsHex(C) || Is(C, ':'); }

	/** A full (eight groups) or :: compressed IPv6 address with a digit, as one maximal run of hex and colons. */
	bool IsIpv6(const FString& Run)
	{
		if (!HasDigit(Run, 0, Run.Len()))
		{
			return false;
		}

		int32 Doubles = 0;
		for (int32 Index = 0; Index + 1 < Run.Len(); ++Index)
		{
			if (Is(Run[Index], ':') && Is(Run[Index + 1], ':'))
			{
				if (Index + 2 < Run.Len() && Is(Run[Index + 2], ':')) return false;
				++Doubles;
				++Index;
			}
		}
		if (Doubles > 1) return false;

		int32 Groups = 0;
		int32 Filled = 0;
		int32 Length = 0;
		bool bEmpty = false;
		for (int32 Index = 0; Index <= Run.Len(); ++Index)
		{
			if (Index == Run.Len() || Is(Run[Index], ':'))
			{
				++Groups;
				if (Length == 0) bEmpty = true; else ++Filled;
				if (Length > 4) return false;
				Length = 0;
			}
			else
			{
				++Length;
			}
		}

		if (Doubles == 0)
		{
			return Groups == 8 && !bEmpty;
		}
		return Filled <= 7;
	}

	FString ScrubIpv6(const FString& Text)
	{
		return ReplaceAll(Text, [&Text](int32 Start, FString& Out) -> int32
		{
			if (!IsIpv6Char(Text[Start]))
			{
				return -1;
			}
			if (Start > 0 && (IsWord(Text[Start - 1]) || Is(Text[Start - 1], ':')))
			{
				return -1;
			}
			int32 Index = Start;
			bool bColon = false;
			while (Index < Text.Len() && IsIpv6Char(Text[Index]))
			{
				bColon = bColon || Is(Text[Index], ':');
				++Index;
			}
			if (!bColon || (Index < Text.Len() && IsWord(Text[Index])))
			{
				return -1;
			}
			const FString Run = Slice(Text, Start, Index);
			if (!IsIpv6(Run))
			{
				return -1;
			}
			Out = TEXT("[ip]");
			return Index;
		});
	}

	/** [A-Za-z0-9+/=_-]. */
	bool IsLongRunChar(TCHAR C) { return IsAlnum(C) || Is(C, '+') || Is(C, '/') || Is(C, '=') || Is(C, '_') || Is(C, '-'); }

	/** ^[a-z0-9]+$ */
	bool IsLowerWord(const FString& T, int32 From, int32 To)
	{
		if (From >= To) return false;
		for (int32 Index = From; Index < To; ++Index)
		{
			if (!IsLower(T[Index]) && !IsDigit(T[Index])) return false;
		}
		return true;
	}

	/** ^[A-Z][a-z0-9]*(?:[A-Z][a-z0-9]+)*$ */
	bool IsPascalWord(const FString& T, int32 From, int32 To)
	{
		if (From >= To || !IsUpper(T[From])) return false;
		int32 Index = From + 1;
		while (Index < To && (IsLower(T[Index]) || IsDigit(T[Index]))) ++Index;
		while (Index < To)
		{
			if (!IsUpper(T[Index])) return false;
			const int32 Hump = ++Index;
			while (Index < To && (IsLower(T[Index]) || IsDigit(T[Index]))) ++Index;
			if (Index == Hump) return false;
		}
		return true;
	}

	/** ^[A-Z0-9]{1,6}$ */
	bool IsShortCaps(const FString& T, int32 From, int32 To)
	{
		if (To - From < 1 || To - From > 6) return false;
		for (int32 Index = From; Index < To; ++Index)
		{
			if (!IsUpper(T[Index]) && !IsDigit(T[Index])) return false;
		}
		return true;
	}

	/**
	 * A path word: one or more parts joined by '_' or '-', each part
	 * lowercase, CamelCase, or an acronym of 1 to 6 capitals or digits, so
	 * BP_Player_Controller_2026 is a word.
	 */
	bool IsPathWord(const FString& T, int32 From, int32 To)
	{
		int32 PartStart = From;
		for (int32 Index = From; Index <= To; ++Index)
		{
			if (Index < To && !Is(T[Index], '_') && !Is(T[Index], '-'))
			{
				continue;
			}
			if (!IsLowerWord(T, PartStart, Index) && !IsPascalWord(T, PartStart, Index) && !IsShortCaps(T, PartStart, Index))
			{
				return false;
			}
			PartStart = Index + 1;
		}
		return true;
	}

	/**
	 * A run with a '/' whose every piece is empty or a path word under 32
	 * characters: a file path, kept. Anything else with a '/' may be a
	 * secret and is not.
	 */
	bool IsPathRun(const FString& T, int32 From, int32 To)
	{
		bool bSlash = false;
		int32 PieceStart = From;
		for (int32 Index = From; Index <= To; ++Index)
		{
			if (Index < To && !Is(T[Index], '/'))
			{
				continue;
			}
			bSlash = bSlash || Index < To;
			const int32 Length = Index - PieceStart;
			if (Length >= 32) return false;
			if (Length > 0 && !IsPathWord(T, PieceStart, Index))
			{
				return false;
			}
			PieceStart = Index + 1;
		}
		return bSlash;
	}

	/** 32 or more of [A-Za-z0-9+/=_-] with a digit in the run, unless it is a path. */
	FString ScrubLongRuns(const FString& Text)
	{
		FString Out;
		int32 Index = 0;
		while (Index < Text.Len())
		{
			if (!IsLongRunChar(Text[Index]))
			{
				Out.AppendChar(Text[Index]);
				++Index;
				continue;
			}
			const int32 Start = Index;
			while (Index < Text.Len() && IsLongRunChar(Text[Index])) ++Index;
			if (Index - Start >= 32 && HasDigit(Text, Start, Index) && !IsPathRun(Text, Start, Index))
			{
				Out += TEXT("[token]");
				continue;
			}
			Out += Slice(Text, Start, Index);
		}
		return Out;
	}
}

// --- Public helpers ------------------------------------------------------------

namespace RavensightErrors
{
	FString Scrub(const FString& Text)
	{
		if (Text.IsEmpty())
		{
			return Text;
		}
		FString Out = ScrubUrls(Text);
		Out = ScrubTokens(Out);
		Out = ScrubEmails(Out);
		Out = ScrubHomes(Out);
		Out = ScrubIpv4(Out);
		Out = ScrubIpv6(Out);
		return ScrubLongRuns(Out);
	}

	FString Template(const FString& Message)
	{
		// Quoted text first, so a number inside quotes does not survive as 0.
		FString Quoted;
		for (int32 Index = 0; Index < Message.Len(); ++Index)
		{
			const TCHAR C = Message[Index];
			if (Is(C, '"') || Is(C, '\''))
			{
				int32 Close = Index + 1;
				while (Close < Message.Len() && Message[Close] != C) ++Close;
				if (Close < Message.Len())
				{
					Quoted.AppendChar(C);
					Quoted.AppendChar(TEXT('*'));
					Quoted.AppendChar(C);
					Index = Close;
					continue;
				}
			}
			Quoted.AppendChar(C);
		}

		const FString NoUrls = ReplaceAll(Quoted, [&Quoted](int32 Start, FString& Out) -> int32
		{
			if (!IsAlpha(Quoted[Start]) || !WordStart(Quoted, Start)) return -1;
			int32 Index = Start + 1;
			while (Index < Quoted.Len() && (IsAlnum(Quoted[Index]) || Is(Quoted[Index], '+') || Is(Quoted[Index], '.') || Is(Quoted[Index], '-'))) ++Index;
			if (!At(Quoted, Index, "://", false)) return -1;
			Index += 3;
			while (Index < Quoted.Len() && !IsSpace(Quoted[Index])) ++Index;
			Out = TEXT("<url>");
			return Index;
		});

		// Runs of hex, dashes and digits: a UUID, a long hex run, or a number.
		FString Out;
		int32 Index = 0;
		while (Index < NoUrls.Len())
		{
			if (!IsHex(NoUrls[Index]))
			{
				if (IsDigit(NoUrls[Index]))
				{
					while (Index < NoUrls.Len() && IsDigit(NoUrls[Index])) ++Index;
					Out.AppendChar(TEXT('0'));
					continue;
				}
				Out.AppendChar(NoUrls[Index]);
				++Index;
				continue;
			}
			int32 End = Index;
			int32 Dashes = 0;
			while (End < NoUrls.Len() && (IsHex(NoUrls[End]) || Is(NoUrls[End], '-')))
			{
				Dashes += Is(NoUrls[End], '-') ? 1 : 0;
				++End;
			}
			const bool bBounded = End >= NoUrls.Len() || !IsAlnum(NoUrls[End]);
			const bool bDigit = HasDigit(NoUrls, Index, End);
			if (bBounded && bDigit && Dashes == 4 && End - Index >= 36)
			{
				Out += TEXT("<id>");
				Index = End;
				continue;
			}
			int32 HexEnd = Index;
			while (HexEnd < NoUrls.Len() && IsHex(NoUrls[HexEnd])) ++HexEnd;
			if (HexEnd - Index >= 8 && HasDigit(NoUrls, Index, HexEnd))
			{
				Out += TEXT("<hex>");
				Index = HexEnd;
				continue;
			}
			if (IsDigit(NoUrls[Index]))
			{
				while (Index < NoUrls.Len() && IsDigit(NoUrls[Index])) ++Index;
				Out.AppendChar(TEXT('0'));
				continue;
			}
			Out.AppendChar(NoUrls[Index]);
			++Index;
		}
		return Out;
	}

	FString FirstFrame(const FString& Stack)
	{
		int32 Start = 0;
		for (int32 Index = 0; Index <= Stack.Len(); ++Index)
		{
			if (Index < Stack.Len() && !Is(Stack[Index], '\n'))
			{
				continue;
			}
			const FString Line = Slice(Stack, Start, Index).TrimStartAndEnd();
			Start = Index + 1;
			if (Line.IsEmpty())
			{
				continue;
			}

			bool bSkip = false;
			static const char* const Skipped[] = { "FDebug::", "AssertionMacros", "StackWalk", "FOutputDevice", "Ravensight", "ravensight" };
			for (const char* Word : Skipped)
			{
				for (int32 Probe = 0; Probe < Line.Len() && !bSkip; ++Probe)
				{
					bSkip = At(Line, Probe, Word, false);
				}
			}
			if (!bSkip)
			{
				return Line;
			}
		}
		return FString();
	}

	FString Cut(const FString& Text, int32 Count)
	{
		return Text.Len() <= Count ? Text : Text.Left(Count);
	}

	FString ScrubField(const FString& Text, int32 Limit)
	{
		// Some scrub rules are quadratic on hostile input, so the text is cut
		// to four times the field's limit first, then to the limit.
		return Cut(Scrub(Cut(Text, Limit * 4)), Limit);
	}

	FString OneLine(const FString& Text)
	{
		FString Out;
		bool bSpace = false;
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			const TCHAR C = Text[Index];
			if (Is(C, '\r') || Is(C, '\n') || Is(C, '\t'))
			{
				if (!bSpace)
				{
					Out.AppendChar(TEXT(' '));
					bSpace = true;
				}
				continue;
			}
			bSpace = false;
			Out.AppendChar(C);
		}
		return Out.TrimStartAndEnd();
	}

	FString LevelWord(ERavensightLogLevel Level)
	{
		switch (Level)
		{
		case ERavensightLogLevel::Fatal: return TEXT("critical");
		case ERavensightLogLevel::Error: return TEXT("error");
		case ERavensightLogLevel::Warning: return TEXT("warning");
		case ERavensightLogLevel::Display:
		case ERavensightLogLevel::Log: return TEXT("info");
		default: return TEXT("debug");
		}
	}

	ERavensightLineAction ClassifyLine(ERavensightLogLevel Level, bool bTrailOn)
	{
		switch (Level)
		{
		case ERavensightLogLevel::Fatal:
		case ERavensightLogLevel::Error:
			return ERavensightLineAction::Capture;
		case ERavensightLogLevel::Warning:
		case ERavensightLogLevel::Display:
		case ERavensightLogLevel::Log:
			// With the trail off nothing below Error is kept, so a game that
			// logs every frame pays one comparison per line.
			return bTrailOn ? ERavensightLineAction::Trail : ERavensightLineAction::Ignore;
		default:
			return ERavensightLineAction::Ignore;
		}
	}

	FRavensightTypedEvent BuildEvent(const FRavensightCapturedError& Error, const FString& Trail)
	{
		FRavensightTypedEvent Event;
		Event.Name = EventName;
		Event.Props.Add(FRavensightProp::MakeString(TEXT("rs_kind"), TEXT("error")));
		Event.Props.Add(FRavensightProp::MakeString(TEXT("rs_severity"), Error.Severity.IsEmpty() ? FString(TEXT("error")) : Error.Severity));
		Event.Props.Add(FRavensightProp::MakeString(TEXT("rs_message"), ScrubField(Error.Message, MessageChars)));
		Event.Props.Add(FRavensightProp::MakeString(TEXT("rs_source"), Error.Source));

		const FString Stack = ScrubField(Error.Stack, StackChars);
		if (!Stack.IsEmpty())
		{
			Event.Props.Add(FRavensightProp::MakeString(TEXT("rs_stack"), Stack));
		}
		const FString Type = ScrubField(Error.ErrorType, TypeChars);
		if (!Type.IsEmpty())
		{
			Event.Props.Add(FRavensightProp::MakeString(TEXT("rs_error_type"), Type));
		}
		if (!Trail.IsEmpty())
		{
			Event.Props.Add(FRavensightProp::MakeString(TEXT("rs_trail"), Cut(Trail, TrailChars)));
		}
		return Event;
	}
}

// --- Trail ring ------------------------------------------------------------------

void FRavensightTrailRing::Add(double MsSinceStart, ERavensightLogLevel Level, const FString& Text)
{
	const int64 Ms = MsSinceStart > 0.0 ? static_cast<int64>(MsSinceStart) : 0;
	const FString Line = FString::Printf(TEXT("+%lld "), static_cast<long long>(Ms))
		+ RavensightErrors::LevelWord(Level) + TEXT(" ") + RavensightErrors::ScrubField(RavensightErrors::OneLine(RavensightErrors::Cut(Text, RavensightErrors::TrailLineChars * 4)), RavensightErrors::TrailLineChars);
	Lines.Add(RavensightErrors::Cut(Line, RavensightErrors::TrailLineChars));
	while (Lines.Num() > RavensightErrors::TrailLines)
	{
		Lines.RemoveAt(0);
	}
}

FString FRavensightTrailRing::Join() const
{
	int32 From = 0;
	for (;;)
	{
		int32 Size = 0;
		for (int32 Index = From; Index < Lines.Num(); ++Index)
		{
			Size += Lines[Index].Len() + (Index > From ? 1 : 0);
		}
		if (Size <= RavensightErrors::TrailChars || From >= Lines.Num())
		{
			break;
		}
		++From;
	}

	FString Out;
	for (int32 Index = From; Index < Lines.Num(); ++Index)
	{
		if (Index > From)
		{
			Out.AppendChar(TEXT('\n'));
		}
		Out += Lines[Index];
	}
	return Out;
}

// --- Limiter ---------------------------------------------------------------------

bool FRavensightErrorLimiter::HasRoom(double NowSeconds) const
{
	if (Captures >= RavensightErrors::MaxCaptures)
	{
		return false;
	}
	const double Start = bBucketStarted ? RefilledAt : NowSeconds;
	const double Rate = RavensightErrors::BucketSize / RavensightErrors::BucketSeconds;
	const double Refilled = FMath::Min(static_cast<double>(RavensightErrors::BucketSize), Tokens + FMath::Max(0.0, NowSeconds - Start) * Rate);
	return Refilled >= 1.0;
}

bool FRavensightErrorLimiter::TryAdmit(const FString& DedupeKey, double NowSeconds)
{
	if (!bBucketStarted)
	{
		bBucketStarted = true;
		RefilledAt = NowSeconds;
	}

	if (Captures >= RavensightErrors::MaxCaptures)
	{
		return false;
	}
	const int32 Seen = PerKey.FindRef(DedupeKey);
	if (Seen >= RavensightErrors::MaxPerKey)
	{
		return false;
	}

	const double Rate = RavensightErrors::BucketSize / RavensightErrors::BucketSeconds;
	const double Elapsed = FMath::Max(0.0, NowSeconds - RefilledAt);
	Tokens = FMath::Min(static_cast<double>(RavensightErrors::BucketSize), Tokens + Elapsed * Rate);
	RefilledAt = NowSeconds;
	if (Tokens < 1.0)
	{
		return false;
	}

	Tokens -= 1.0;
	++Captures;
	PerKey.Add(DedupeKey, Seen + 1);
	return true;
}

bool FRavensightErrorLimiter::TryClaimTrail()
{
	if (Trails >= RavensightErrors::MaxTrails)
	{
		return false;
	}
	++Trails;
	return true;
}

void FRavensightErrorLimiter::Reset(double NowSeconds)
{
	Captures = 0;
	Trails = 0;
	PerKey.Reset();
	Tokens = RavensightErrors::BucketSize;
	RefilledAt = NowSeconds;
	bBucketStarted = true;
}

// --- Pipeline --------------------------------------------------------------------

void FRavensightErrorPipeline::Start(double NowSeconds)
{
	StartedAt = NowSeconds;
	Limiter.Reset(NowSeconds);
}

FString FRavensightErrorPipeline::DedupeKey(const FRavensightCapturedError& Error)
{
	const FString Message = RavensightErrors::ScrubField(Error.Message, RavensightErrors::MessageChars);
	const FString Stack = RavensightErrors::ScrubField(Error.Stack, RavensightErrors::StackChars);
	return RavensightErrors::ScrubField(Error.ErrorType, RavensightErrors::TypeChars) + TEXT("|") + RavensightErrors::Template(Message) + TEXT("|") + RavensightErrors::FirstFrame(Stack);
}

TArray<FRavensightTypedEvent> FRavensightErrorPipeline::Answer(bool bEnabled, bool bTrail)
{
	TArray<FRavensightTypedEvent> Ready;
	CaptureAnswer = bEnabled ? ERavensightCaptureAnswer::On : ERavensightCaptureAnswer::Off;
	bTrailOn = bTrail;

	if (bEnabled)
	{
		for (const FHeld& Hold : Held)
		{
			const bool bAttach = bTrailOn && !Hold.Trail.IsEmpty() && Limiter.TryClaimTrail();
			Ready.Add(RavensightErrors::BuildEvent(Hold.Error, bAttach ? Hold.Trail : FString()));
		}
	}
	else
	{
		Ring.Reset();
	}
	Held.Reset();
	return Ready;
}

bool FRavensightErrorPipeline::OnLine(ERavensightLogLevel Level, const FString& Category, const FString& Text, double NowSeconds, FRavensightTypedEvent& Out)
{
	if (CaptureAnswer == ERavensightCaptureAnswer::Off)
	{
		return false;
	}

	const FString Body = Category.IsEmpty() ? Text : Category + TEXT(": ") + Text;
	const ERavensightLineAction Action = RavensightErrors::ClassifyLine(Level, IsTrailOn());
	if (Action == ERavensightLineAction::Ignore)
	{
		return false;
	}
	if (Action == ERavensightLineAction::Trail)
	{
		Ring.Add((NowSeconds - StartedAt) * 1000.0, Level, Body);
		return false;
	}

	FRavensightCapturedError Error;
	// The contract's engine log error is a warning; Fatal is critical.
	Error.Severity = Level == ERavensightLogLevel::Fatal ? TEXT("critical") : TEXT("warning");
	Error.Message = RavensightErrors::OneLine(Text);
	Error.Source = RavensightErrors::SourceLog;
	Error.ErrorType = Category;
	const bool bReady = OnCapture(Error, NowSeconds, Out);

	// After the capture, so the error's own line is not part of its trail.
	Ring.Add((NowSeconds - StartedAt) * 1000.0, Level, Body);
	return bReady;
}

bool FRavensightErrorPipeline::OnCapture(const FRavensightCapturedError& Error, double NowSeconds, FRavensightTypedEvent& Out)
{
	if (CaptureAnswer == ERavensightCaptureAnswer::Off)
	{
		return false;
	}
	if (CaptureAnswer == ERavensightCaptureAnswer::Unknown && Held.Num() >= RavensightErrors::MaxHeld)
	{
		return false;
	}
	// The cheap checks first: once the session cap or the bucket is spent,
	// nothing is scrubbed, templated or keyed.
	if (!Limiter.HasRoom(NowSeconds) || !Limiter.TryAdmit(DedupeKey(Error), NowSeconds))
	{
		return false;
	}

	if (CaptureAnswer == ERavensightCaptureAnswer::Unknown)
	{
		FHeld Hold;
		Hold.Error = Error;
		Hold.Trail = Ring.Join();
		Held.Add(Hold);
		return false;
	}

	const bool bAttach = bTrailOn && Ring.Num() > 0 && Limiter.TryClaimTrail();
	Out = RavensightErrors::BuildEvent(Error, bAttach ? Ring.Join() : FString());
	return true;
}

void FRavensightErrorPipeline::ResetForNewSession(double NowSeconds)
{
	Ring.Reset();
	Held.Reset();
	Start(NowSeconds);
}
