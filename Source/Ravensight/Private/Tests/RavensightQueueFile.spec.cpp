// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The queue file's write and delete order (RavensightQueueFile.h) against
// fake file operations: exclusion before the move, cleanup on every failure,
// a checked exclusion result with one retry, and the temporary file removed
// with the queue file. Engine-free: runs in the host check too.

#include "Misc/AutomationTest.h"
#include "RavensightQueueFile.h"

#include <map>
#include <string>
#include <vector>

#if WITH_DEV_AUTOMATION_TESTS

namespace RavensightQueueFileSpecFakes
{
	struct FFakeDisk
	{
		std::map<std::string, std::string> Files;
		std::map<std::string, bool> Excluded;
		std::vector<std::string> Calls;
		std::vector<std::string> Warnings;
		bool bSaveFails = false;
		bool bMoveFails = false;
		int ExcludeFailures = 0;

		RavensightQueueFile::FOps Ops()
		{
			RavensightQueueFile::FOps Out;
			Out.Save = [this](const std::string& Path, const std::string& Text)
			{
				Calls.push_back("save " + Path);
				Files[Path] = bSaveFails ? Text.substr(0, Text.size() / 2) : Text;
				return !bSaveFails;
			};
			Out.Move = [this](const std::string& To, const std::string& From)
			{
				Calls.push_back("move " + From + " " + To);
				if (bMoveFails) return false;
				Files[To] = Files[From];
				Excluded[To] = Excluded[From];
				Files.erase(From);
				Excluded.erase(From);
				return true;
			};
			Out.Remove = [this](const std::string& Path)
			{
				Calls.push_back("remove " + Path);
				Files.erase(Path);
				Excluded.erase(Path);
			};
			Out.Exclude = [this](const std::string& Path)
			{
				Calls.push_back("exclude " + Path);
				if (ExcludeFailures > 0)
				{
					--ExcludeFailures;
					return false;
				}
				Excluded[Path] = true;
				return true;
			};
			Out.Warn = [this](const char* Message) { Warnings.push_back(Message); };
			return Out;
		}
	};
}

BEGIN_DEFINE_SPEC(FRavensightQueueFileSpec, "Ravensight.QueueFile", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
END_DEFINE_SPEC(FRavensightQueueFileSpec)

void FRavensightQueueFileSpec::Define()
{
	using RavensightQueueFileSpecFakes::FFakeDisk;

	It("excludes the temporary file before the move, so the queue file arrives excluded", [this]()
	{
		FFakeDisk Disk;
		TestTrue(TEXT("written"), RavensightQueueFile::Write(Disk.Ops(), "q.json", "{}"));
		const std::vector<std::string> Expected = {"save q.json.tmp", "exclude q.json.tmp", "move q.json.tmp q.json"};
		TestTrue(TEXT("order"), Disk.Calls == Expected);
		TestTrue(TEXT("excluded"), Disk.Excluded["q.json"]);
		TestTrue(TEXT("no temporary file left"), Disk.Files.count("q.json.tmp") == 0);
		TestTrue(TEXT("no warning"), Disk.Warnings.empty());
	});

	It("deletes the temporary file when the save or the move fails, leaving the old queue file whole", [this]()
	{
		FFakeDisk Disk;
		Disk.Files["q.json"] = "old";
		Disk.bSaveFails = true;
		TestFalse(TEXT("save failed"), RavensightQueueFile::Write(Disk.Ops(), "q.json", "{\"new\":1}"));
		TestTrue(TEXT("partial removed"), Disk.Files.count("q.json.tmp") == 0);
		TestTrue(TEXT("old intact"), Disk.Files["q.json"] == "old");

		Disk.bSaveFails = false;
		Disk.bMoveFails = true;
		TestFalse(TEXT("move failed"), RavensightQueueFile::Write(Disk.Ops(), "q.json", "{\"new\":1}"));
		TestTrue(TEXT("temporary removed"), Disk.Files.count("q.json.tmp") == 0);
		TestTrue(TEXT("old intact after a failed move"), Disk.Files["q.json"] == "old");
	});

	It("checks the exclusion: warns without a path or contents, then retries once on the final file", [this]()
	{
		FFakeDisk Disk;
		Disk.ExcludeFailures = 1;
		TestTrue(TEXT("written"), RavensightQueueFile::Write(Disk.Ops(), "/secret/q.json", "{\"rs_message\":\"boom\"}"));
		TestTrue(TEXT("retried on the final file"), Disk.Calls.back() == "exclude /secret/q.json");
		TestTrue(TEXT("excluded after the retry"), Disk.Excluded["/secret/q.json"]);
		TestTrue(TEXT("one warning"), Disk.Warnings.size() == 1);
		TestTrue(TEXT("no path or contents in it"), Disk.Warnings[0].find("secret") == std::string::npos && Disk.Warnings[0].find("boom") == std::string::npos);

		FFakeDisk Stubborn;
		Stubborn.ExcludeFailures = 2;
		RavensightQueueFile::Write(Stubborn.Ops(), "q.json", "{}");
		TestTrue(TEXT("a second warning when the retry fails too"), Stubborn.Warnings.size() == 2);
	});

	It("removes the queue file and its temporary file together", [this]()
	{
		FFakeDisk Disk;
		Disk.Files["q.json"] = "x";
		Disk.Files["q.json.tmp"] = "partial";
		RavensightQueueFile::RemoveAll(Disk.Ops(), "q.json");
		TestTrue(TEXT("both gone"), Disk.Files.empty());
	});

	It("excludes the folder and an existing file at load, for files written by older versions", [this]()
	{
		FFakeDisk Disk;
		RavensightQueueFile::ExcludeExisting(Disk.Ops(), "dir", "dir/q.json", true);
		TestTrue(TEXT("folder"), Disk.Excluded["dir"]);
		TestTrue(TEXT("file"), Disk.Excluded["dir/q.json"]);
		FFakeDisk Missing;
		RavensightQueueFile::ExcludeExisting(Missing.Ops(), "dir", "dir/q.json", false);
		TestTrue(TEXT("folder only"), Missing.Calls.size() == 1);
		FFakeDisk Failing;
		Failing.ExcludeFailures = 1;
		RavensightQueueFile::ExcludeExisting(Failing.Ops(), "dir", "dir/q.json", true);
		TestTrue(TEXT("warned"), Failing.Warnings.size() == 1);
	});
}

#endif
