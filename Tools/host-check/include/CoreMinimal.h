// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// HOST CHECK ONLY. A stand-in for the tiny slice of Unreal's CoreMinimal.h
// that RavensightQueue and RavensightProtocol use, so their logic and their
// automation specs can be compiled and run with a plain C++17 compiler on a
// machine with no engine. This is not Unreal Engine and proves nothing about
// UnrealBuildTool, UnrealHeaderTool or the subsystem. See Tools/README.md.
//
// Where Unreal's semantics matter to the code under test they are copied:
// FString == and EndsWith ignore case, TArray::Contains uses ==.

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define RAVENSIGHT_API
#define TEXT(x) x

using TCHAR = char;
using int32 = std::int32_t;
using int64 = std::int64_t;
using uint8 = std::uint8_t;

template <typename T>
decltype(auto) MoveTemp(T&& Value) { return std::move(Value); }

template <typename T>
class TUniquePtr : public std::unique_ptr<T>
{
public:
	using std::unique_ptr<T>::unique_ptr;
	TUniquePtr(std::unique_ptr<T>&& In) : std::unique_ptr<T>(std::move(In)) {}
	void Reset() { std::unique_ptr<T>::reset(); }
};

template <typename T, typename... Args>
TUniquePtr<T> MakeUnique(Args&&... InArgs) { return TUniquePtr<T>(std::make_unique<T>(std::forward<Args>(InArgs)...)); }

namespace ESearchCase
{
	enum Type { CaseSensitive, IgnoreCase };
}

struct FMath
{
	template <typename T> static T Max(T A, T B) { return A > B ? A : B; }
	template <typename T> static T Min(T A, T B) { return A < B ? A : B; }
	template <typename T> static T Clamp(T V, T Lo, T Hi) { return V < Lo ? Lo : (V > Hi ? Hi : V); }
};

class FString
{
public:
	FString() = default;
	FString(const char* In) : Data(In ? In : "") {}
	FString(std::string In) : Data(std::move(In)) {}

	bool IsEmpty() const { return Data.empty(); }
	int32 Len() const { return static_cast<int32>(Data.size()); }
	void Reset() { Data.clear(); }
	const TCHAR* operator*() const { return Data.c_str(); }

	bool Equals(const FString& Other, ESearchCase::Type Case = ESearchCase::IgnoreCase) const
	{
		return Case == ESearchCase::CaseSensitive ? Data == Other.Data : Lower(Data) == Lower(Other.Data);
	}

	bool EndsWith(const FString& Suffix, ESearchCase::Type Case = ESearchCase::IgnoreCase) const
	{
		if (Suffix.Data.size() > Data.size())
		{
			return false;
		}
		const FString Tail(Data.substr(Data.size() - Suffix.Data.size()));
		return Tail.Equals(Suffix, Case);
	}

	void LeftChopInline(int32 Count)
	{
		Data.resize(Count >= Len() ? 0 : Data.size() - static_cast<size_t>(Count));
	}

	FString TrimStartAndEnd() const
	{
		FString Out(*this);
		Out.TrimStartAndEndInline();
		return Out;
	}

	void TrimStartAndEndInline()
	{
		const auto IsSpace = [](unsigned char C) { return std::isspace(C) != 0; };
		while (!Data.empty() && IsSpace(Data.back())) Data.pop_back();
		size_t Start = 0;
		while (Start < Data.size() && IsSpace(Data[Start])) ++Start;
		Data.erase(0, Start);
	}

	static FString Printf(const char* Format, ...)
	{
		char Buffer[1024];
		va_list Args;
		va_start(Args, Format);
		std::vsnprintf(Buffer, sizeof(Buffer), Format, Args);
		va_end(Args);
		return FString(Buffer);
	}

	friend FString operator+(const FString& A, const FString& B) { return FString(A.Data + B.Data); }
	friend FString operator+(const char* A, const FString& B) { return FString(std::string(A) + B.Data); }
	friend FString operator+(const FString& A, const char* B) { return FString(A.Data + B); }

	// Unreal compares FStrings ignoring case.
	friend bool operator==(const FString& A, const FString& B) { return A.Equals(B); }
	friend bool operator!=(const FString& A, const FString& B) { return !A.Equals(B); }

	const std::string& Str() const { return Data; }

private:
	static std::string Lower(std::string In)
	{
		std::transform(In.begin(), In.end(), In.begin(), [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
		return In;
	}

	std::string Data;
};

template <typename T>
class TArray
{
public:
	TArray() = default;
	TArray(std::initializer_list<T> Init) : Items(Init) {}

	int32 Num() const { return static_cast<int32>(Items.size()); }
	bool IsEmpty() const { return Items.empty(); }
	void Add(const T& Item) { Items.push_back(Item); }
	void Reserve(int32 Count) { Items.reserve(static_cast<size_t>(Count)); }
	void Reset() { Items.clear(); }

	void RemoveAt(int32 Index, int32 Count = 1)
	{
		Items.erase(Items.begin() + Index, Items.begin() + Index + Count);
	}

	bool Contains(const T& Item) const
	{
		return std::find(Items.begin(), Items.end(), Item) != Items.end();
	}

	template <typename Predicate>
	bool ContainsByPredicate(Predicate Pred) const
	{
		return std::any_of(Items.begin(), Items.end(), Pred);
	}

	T& operator[](int32 Index) { return Items[static_cast<size_t>(Index)]; }
	const T& operator[](int32 Index) const { return Items[static_cast<size_t>(Index)]; }

	auto begin() { return Items.begin(); }
	auto end() { return Items.end(); }
	auto begin() const { return Items.begin(); }
	auto end() const { return Items.end(); }

private:
	std::vector<T> Items;
};
