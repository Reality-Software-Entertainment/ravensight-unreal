// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// HOST CHECK ONLY. A stand-in for the spec half of Unreal's automation
// framework (BEGIN_DEFINE_SPEC, Describe, It, BeforeEach, AfterEach and the
// Test* assertions) so the plugin's own .spec.cpp files run unchanged under a
// plain C++17 compiler. This is not Unreal's runner. See Tools/README.md.

#pragma once

#include "CoreMinimal.h"

#include <cmath>
#include <iostream>

#define WITH_DEV_AUTOMATION_TESTS 1

enum class EAutomationTestFlags : uint32_t
{
	EditorContext = 1,
	ClientContext = 2,
	EngineFilter = 4,
};

inline EAutomationTestFlags operator|(EAutomationTestFlags A, EAutomationTestFlags B)
{
	return static_cast<EAutomationTestFlags>(static_cast<uint32_t>(A) | static_cast<uint32_t>(B));
}

class FHostSpec
{
public:
	explicit FHostSpec(const char* InName) : Name(InName) {}
	virtual ~FHostSpec() = default;
	virtual void Define() = 0;

	/** Runs every It with its enclosing BeforeEach and AfterEach blocks. Returns failures. */
	int Run()
	{
		Define();
		int Failures = 0;
		for (const FCase& Case : Cases)
		{
			CaseErrors = 0;
			for (const auto& Before : Case.Befores) Before();
			Case.Body();
			for (auto It = Case.Afters.rbegin(); It != Case.Afters.rend(); ++It) (*It)();

			std::cout << (CaseErrors == 0 ? "  ok    " : "  FAIL  ") << Name << " " << Case.Path << "\n";
			Failures += CaseErrors == 0 ? 0 : 1;
			++Total;
		}
		return Failures;
	}

	int Total = 0;

protected:
	void Describe(const char* What, std::function<void()> Body)
	{
		Scopes.push_back({ What, {}, {} });
		Body();
		Scopes.pop_back();
	}

	void It(const char* What, std::function<void()> Body)
	{
		FCase Case;
		for (const FScope& Scope : Scopes)
		{
			Case.Path += Scope.Name + " / ";
			Case.Befores.insert(Case.Befores.end(), Scope.Befores.begin(), Scope.Befores.end());
			Case.Afters.insert(Case.Afters.end(), Scope.Afters.begin(), Scope.Afters.end());
		}
		Case.Befores.insert(Case.Befores.begin(), RootBefores.begin(), RootBefores.end());
		Case.Afters.insert(Case.Afters.begin(), RootAfters.begin(), RootAfters.end());
		Case.Path += What;
		Case.Body = std::move(Body);
		Cases.push_back(std::move(Case));
	}

	void BeforeEach(std::function<void()> Body)
	{
		(Scopes.empty() ? RootBefores : Scopes.back().Befores).push_back(std::move(Body));
	}

	void AfterEach(std::function<void()> Body)
	{
		(Scopes.empty() ? RootAfters : Scopes.back().Afters).push_back(std::move(Body));
	}

	void AddError(const FString& Message)
	{
		++CaseErrors;
		std::cout << "        " << *Message << "\n";
	}

	bool TestTrue(const char* What, bool bValue)
	{
		if (!bValue) AddError(FString(What) + " expected true");
		return bValue;
	}

	bool TestFalse(const char* What, bool bValue)
	{
		if (bValue) AddError(FString(What) + " expected false");
		return !bValue;
	}

	bool TestEqual(const char* What, int32 Actual, int32 Expected)
	{
		if (Actual != Expected) AddError(FString::Printf("%s: got %d, expected %d", What, Actual, Expected));
		return Actual == Expected;
	}

	bool TestEqual(const char* What, double Actual, double Expected)
	{
		const bool bOk = std::fabs(Actual - Expected) < 1e-4;
		if (!bOk) AddError(FString::Printf("%s: got %f, expected %f", What, Actual, Expected));
		return bOk;
	}

	bool TestEqual(const char* What, const FString& Actual, const FString& Expected)
	{
		const bool bOk = Actual == Expected;
		if (!bOk) AddError(FString::Printf("%s: got '%s', expected '%s'", What, *Actual, *Expected));
		return bOk;
	}

private:
	struct FScope
	{
		std::string Name;
		std::vector<std::function<void()>> Befores;
		std::vector<std::function<void()>> Afters;
	};

	struct FCase
	{
		std::string Path;
		std::vector<std::function<void()>> Befores;
		std::vector<std::function<void()>> Afters;
		std::function<void()> Body;
	};

	std::string Name;
	std::vector<FScope> Scopes;
	std::vector<std::function<void()>> RootBefores;
	std::vector<std::function<void()>> RootAfters;
	std::vector<FCase> Cases;
	int CaseErrors = 0;
};

/** Every spec in the translation unit registers itself here. */
inline std::vector<std::function<FHostSpec*()>>& HostSpecRegistry()
{
	static std::vector<std::function<FHostSpec*()>> Registry;
	return Registry;
}

template <typename T>
struct THostSpecRegistrar
{
	THostSpecRegistrar() { HostSpecRegistry().push_back([]() -> FHostSpec* { return new T(); }); }
};

#define BEGIN_DEFINE_SPEC(Class, PrettyName, Flags) \
	class Class : public FHostSpec \
	{ \
	public: \
		Class() : FHostSpec(PrettyName) {} \
		virtual void Define() override; \
	private:

#define END_DEFINE_SPEC(Class) \
	}; \
	static THostSpecRegistrar<Class> Class##Registrar;
