// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The order of operations for writing and deleting the persisted queue file,
// kept free of the engine so the host check can prove it with fake file
// operations: write a temporary file, mark it excluded from backups, move it
// over the queue file, and clean up on every failure. The subsystem supplies
// the real operations (IFileManager and RavensightBackupExclusion).

#pragma once

#include <functional>
#include <string>

namespace RavensightQueueFile
{
	struct FOps
	{
		/** Writes the whole text to the path. False on any failure. */
		std::function<bool(const std::string& Path, const std::string& Text)> Save;
		/** Moves From over To, replacing it. False on failure. */
		std::function<bool(const std::string& To, const std::string& From)> Move;
		/** Deletes the path if it exists. */
		std::function<void(const std::string& Path)> Remove;
		/** Marks the path excluded from backups. False when it could not. */
		std::function<bool(const std::string& Path)> Exclude;
		/** A warning with no file contents and no path in it. */
		std::function<void(const char* Message)> Warn;
	};

	/** The temporary file a write goes through: the queue file's path plus ".tmp". */
	inline std::string TempPath(const std::string& Path)
	{
		return Path + ".tmp";
	}

	/**
	 * Writes the queue file. The temporary file is marked excluded from
	 * backups before the move (the attribute moves with it on a same-volume
	 * rename); if that fails, a warning is logged and the exclusion is tried
	 * once more on the final file. A failed save or move deletes the
	 * temporary file. Returns true when the queue file holds the text.
	 */
	inline bool Write(const FOps& Ops, const std::string& Path, const std::string& Text)
	{
		const std::string Temp = TempPath(Path);
		if (!Ops.Save(Temp, Text))
		{
			Ops.Remove(Temp);
			return false;
		}
		const bool bExcluded = Ops.Exclude(Temp);
		if (!Ops.Move(Path, Temp))
		{
			Ops.Remove(Temp);
			return false;
		}
		if (!bExcluded)
		{
			Ops.Warn("Ravensight: could not exclude the queue file from backups; retrying on the final file.");
			if (!Ops.Exclude(Path))
			{
				Ops.Warn("Ravensight: the queue file could not be excluded from backups.");
			}
		}
		return true;
	}

	/** Deletes the queue file and any temporary file a write left behind. */
	inline void RemoveAll(const FOps& Ops, const std::string& Path)
	{
		Ops.Remove(Path);
		Ops.Remove(TempPath(Path));
	}

	/**
	 * At load: marks the queue directory and an existing queue file excluded
	 * from backups, so files written by older versions are covered too.
	 * Logs one warning when either could not be marked.
	 */
	inline void ExcludeExisting(const FOps& Ops, const std::string& Directory, const std::string& Path, bool bFileExists)
	{
		bool bOk = Ops.Exclude(Directory);
		if (bFileExists)
		{
			bOk = Ops.Exclude(Path) && bOk;
		}
		if (!bOk)
		{
			Ops.Warn("Ravensight: the queue folder could not be excluded from backups.");
		}
	}
}
