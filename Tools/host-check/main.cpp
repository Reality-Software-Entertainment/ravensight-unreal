// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// HOST CHECK ONLY. Runs every spec registered by the .spec.cpp files compiled
// alongside it. See run.sh and Tools/README.md.

#include "Misc/AutomationTest.h"

int main()
{
	int Failures = 0;
	int Total = 0;
	for (const auto& Make : HostSpecRegistry())
	{
		std::unique_ptr<FHostSpec> Spec(Make());
		Failures += Spec->Run();
		Total += Spec->Total;
	}

	std::cout << "\n" << Total << " cases, " << Failures << " failed\n";
	return Failures == 0 ? 0 : 1;
}
