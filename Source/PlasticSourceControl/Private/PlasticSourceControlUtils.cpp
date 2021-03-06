// Copyright (c) 2016 Codice Software - Sebastien Rombauts (sebastien.rombauts@gmail.com)

#include "PlasticSourceControlPrivatePCH.h"
#include "PlasticSourceControlUtils.h"
#include "PlasticSourceControlState.h"
#include "PlasticSourceControlModule.h"
#include "PlasticSourceControlCommand.h"
#include "XmlParser.h"

#if PLATFORM_LINUX
#include <sys/ioctl.h>
#endif


namespace PlasticSourceControlConstants
{
#if PLATFORM_WINDOWS
	const TCHAR* pchDelim = TEXT("\r\n");
#else
	const TCHAR* pchDelim = TEXT("\n");
#endif
}

FScopedTempFile::FScopedTempFile(const FText& InText)
{
	Filename = FPaths::CreateTempFilename(*FPaths::GameLogDir(), TEXT("Plastic-Temp"), TEXT(".txt"));
	if(!FFileHelper::SaveStringToFile(InText.ToString(), *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogSourceControl, Error, TEXT("Failed to write to temp file: %s"), *Filename);
	}
}

FScopedTempFile::~FScopedTempFile()
{
	if(FPaths::FileExists(Filename))
	{
		if(!FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*Filename))
		{
			UE_LOG(LogSourceControl, Error, TEXT("Failed to delete temp file: %s"), *Filename);
		}
	}
}

const FString& FScopedTempFile::GetFilename() const
{
	return Filename;
}

// Needed to SetHandleInformation() on WritePipe for input (opposite of ReadPipe, for output) (idem FInteractiveProcess)
static FORCEINLINE bool CreatePipeWrite(void*& ReadPipe, void*& WritePipe)
{
#if PLATFORM_WINDOWS
	SECURITY_ATTRIBUTES Attr = { sizeof(SECURITY_ATTRIBUTES), NULL, true };

	if (!::CreatePipe(&ReadPipe, &WritePipe, &Attr, 0))
	{
		return false;
	}

	if (!::SetHandleInformation(WritePipe, HANDLE_FLAG_INHERIT, 0))
	{
		return false;
	}

	return true;
#else
	return FPlatformProcess::CreatePipe(ReadPipe, WritePipe);
#endif // PLATFORM_WINDOWS
}

namespace PlasticSourceControlUtils
{
// In/Out Pipes for the 'cm shell' persistent process
static void*		ShellOutputPipeRead = nullptr;
static void*		ShellOutputPipeWrite = nullptr;
static void*		ShellInputPipeRead = nullptr;
static void*		ShellInputPipeWrite = nullptr;
static FProcHandle	ShellProcessHandle;

static void CleanupBackgroundCommandLineShell()
{
	FPlatformProcess::ClosePipe(ShellInputPipeRead, ShellInputPipeWrite);
	FPlatformProcess::ClosePipe(ShellOutputPipeRead, ShellOutputPipeWrite);
	ShellOutputPipeRead = ShellOutputPipeWrite = nullptr;
	ShellInputPipeRead = ShellInputPipeWrite = nullptr;
}

// Launch the Plastic SCM background 'cm shell' process in background for optimized successive commands,
// if possible (and not already running)
bool LaunchBackgroundPlasticShell(const FString& InPathToPlasticBinary, const FString& InWorkingDirectory)
{
	// only if shell not already running
	if (!ShellProcessHandle.IsValid())
	{
		const FString FullCommand(TEXT("shell"));

		const bool bLaunchDetached = false;				// the new process will NOT have its own window
		const bool bLaunchHidden = true;				// the new process will be minimized in the task bar
		const bool bLaunchReallyHidden = bLaunchHidden; // the new process will not have a window or be in the task bar

		verify(FPlatformProcess::CreatePipe(ShellOutputPipeRead, ShellOutputPipeWrite));	// For reading from child process
		verify(CreatePipeWrite(ShellInputPipeRead, ShellInputPipeWrite));	// For writing to child process

		UE_LOG(LogSourceControl, Log, TEXT("LaunchBackgroundPlasticShell: '%s %s'"), *InPathToPlasticBinary, *FullCommand);
		ShellProcessHandle = FPlatformProcess::CreateProc(*InPathToPlasticBinary, *FullCommand, bLaunchDetached, bLaunchHidden, bLaunchReallyHidden, nullptr, 0, *InWorkingDirectory, ShellOutputPipeWrite, ShellInputPipeRead);
		if (!ShellProcessHandle.IsValid())
		{
			UE_LOG(LogSourceControl, Warning, TEXT("Failed to launch 'cm shell'")); // not a bug, just no Plastic SCM cli found
			CleanupBackgroundCommandLineShell();
		}
	}

	return ShellProcessHandle.IsValid();
}

static void RestartBackgroundCommandLineShell()
{
	FPlasticSourceControlModule& PlasticSourceControl = FModuleManager::LoadModuleChecked<FPlasticSourceControlModule>("PlasticSourceControl");
	const FString& PathToPlasticBinary = PlasticSourceControl.AccessSettings().GetBinaryPath();
	const FString& WorkingDirectory = PlasticSourceControl.GetProvider().GetPathToWorkspaceRoot();

	FPlatformProcess::CloseProc(ShellProcessHandle);
	CleanupBackgroundCommandLineShell();
	LaunchBackgroundPlasticShell(PathToPlasticBinary, WorkingDirectory);
}

bool RunCommandInternal(const FString& InCommand, const TArray<FString>& InParameters, const TArray<FString>& InFiles, FString& OutResults, FString& OutErrors)
{
	bool bResult = false;

	if (ShellProcessHandle.IsValid())
	{
		// Detect previous crash of cm.exe and restart 'cm shell'
		if (!FPlatformProcess::IsProcRunning(ShellProcessHandle))
		{
			UE_LOG(LogSourceControl, Warning, TEXT("RunCommandInternal: 'cm shell' has stopped. Restarting!"), FPlatformProcess::IsProcRunning(ShellProcessHandle));
			RestartBackgroundCommandLineShell();
		}

		// Start with the Plastic command itself ("status", "log", "chekin"...)
		FString FullCommand = InCommand;
		// Append to the command all parameters, and then finally the files
		for (const FString& Parameter : InParameters)
		{
			FullCommand += TEXT(" ");
			FullCommand += Parameter;
		}
		for (const FString& File : InFiles)
		{
			FullCommand += TEXT(" \"");
			FullCommand += File;
			FullCommand += TEXT("\"");
		}
		// @todo: temporary debug logs (before end of line)
		UE_LOG(LogSourceControl, Log, TEXT("RunCommandInternal: '%s'"), *FullCommand);
		FullCommand += TEXT('\n'); // Finalize the command line

		// Send command to 'cm shell' process
		const bool bWriteOk = FPlatformProcess::WritePipe(ShellInputPipeWrite, FullCommand);

		// And wait up to 60 seconds for any kind of output from cm shell: in case of lengthier operation, intermediate output (like percentage of progress) is expected, which would refresh the timout
		const double Timeout = 60.0;
		const double StartTimestamp = FPlatformTime::Seconds();
		double LastActivity = StartTimestamp;
		int32 PreviousLogLen = 0;
		while (FPlatformProcess::IsProcRunning(ShellProcessHandle))
		{
			FString Output = FPlatformProcess::ReadPipe(ShellOutputPipeRead);
			if (0 < Output.Len())
			{
				LastActivity = FPlatformTime::Seconds(); // freshen the timestamp while cm is still actively outputting information
				OutResults.Append(MoveTemp(Output));
				// Search the output for the line containing the result code, also indicating the end of the command
				const uint32 IndexCommandResult = OutResults.Find(TEXT("CommandResult "), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
				if (INDEX_NONE != IndexCommandResult)
				{
					const uint32 IndexEndResult = OutResults.Find(PlasticSourceControlConstants::pchDelim, ESearchCase::CaseSensitive, ESearchDir::FromStart, IndexCommandResult + 14);
					if (INDEX_NONE != IndexEndResult)
					{
						const FString Result = OutResults.Mid(IndexCommandResult + 14, IndexEndResult - IndexCommandResult - 14);
						const int32 ResultCode = FCString::Atoi(*Result);
						bResult = (0 == ResultCode);
						// remove the CommandResult line from the OutResults
						OutResults.RemoveAt(IndexCommandResult, OutResults.Len() - IndexCommandResult);
						break;
					}
				}
			}
			else if (FPlatformTime::Seconds() - LastActivity > Timeout)
			{
				// Shut-down and restart the connexion to 'cm shell' in case of timeout!
				UE_LOG(LogSourceControl, Warning, TEXT("RunCommandInternal(%s)=%d TIMEOUT after '%lf's Out=\n%s"), *InCommand, bResult, (FPlatformTime::Seconds() - StartTimestamp), *OutResults.Mid(PreviousLogLen));
				PreviousLogLen = OutResults.Len();
				LastActivity = FPlatformTime::Seconds(); // freshen the timestamp to reinit timeout warning
			}

			FPlatformProcess::Sleep(0.0f); // 0.0 means release the current time slice to let other threads get some attention
		}
		if (!InCommand.Equals(TEXT("exit")) && !FPlatformProcess::IsProcRunning(ShellProcessHandle))
		{
			// 'cm shell' normally only terminates in case of 'exit' command. Will restart on next command.
			UE_LOG(LogSourceControl, Error, TEXT("RunCommandInternal(%s): 'cm shell' stopped after '%lf's Out=\n%s"), *InCommand, FPlatformProcess::IsProcRunning(ShellProcessHandle), (FPlatformTime::Seconds() - StartTimestamp), *OutResults);
		}
		else
		{
			// @todo: temporary debug logs
			UE_LOG(LogSourceControl, Log, TEXT("RunCommandInternal(%s)=%d in '%lf's Out=\n%s"), *InCommand, bResult, (FPlatformTime::Seconds() - StartTimestamp), *OutResults);
		}
		
		// Return output as error if result code is an error
		if (!bResult)
		{
			OutErrors = MoveTemp(OutResults);
		}
	}
	else
	{
		UE_LOG(LogSourceControl, Error, TEXT("RunCommandInternal(%s): cm shell not running"), *InCommand);
		OutErrors = InCommand + ": Plastic SCM shell not running!";
	}

	return bResult;
}

static void ExitBackgroundCommandLineShell()
{
	if (ShellProcessHandle.IsValid())
	{
		// Tell the 'cm shell' to exit
		FString Results, Errors;
		RunCommandInternal(TEXT("exit"), TArray<FString>(), TArray<FString>(), Results, Errors);
		// And wait up to one seconde for its termination
		int timeout = 100;
		while (FPlatformProcess::IsProcRunning(ShellProcessHandle) && (0 < timeout--))
		{
			FPlatformProcess::Sleep(0.01f);
		}
		FPlatformProcess::CloseProc(ShellProcessHandle);
		CleanupBackgroundCommandLineShell();
	}
}

// Terminate the background 'cm shell' process and associated pipes
void Terminate()
{
	if (ShellProcessHandle.IsValid())
	{
		ExitBackgroundCommandLineShell();
	}
}

// Basic parsing or results & errors from the Plastic command line process
bool RunCommand(const FString& InCommand, const TArray<FString>& InParameters, const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	bool bResult;
	FString Results;
	FString Errors;

	bResult = RunCommandInternal(InCommand, InParameters, InFiles, Results, Errors);

	Results.ParseIntoArray(OutResults, PlasticSourceControlConstants::pchDelim, true);
	Errors.ParseIntoArray(OutErrorMessages, PlasticSourceControlConstants::pchDelim, true);

	return bResult;
}

FString FindPlasticBinaryPath()
{
#if PLATFORM_WINDOWS
	return FString(TEXT("cm"));
#else
	return FString(TEXT("/usr/bin/cm"));
#endif
}

// Find the root of the Plastic workspace, looking from the provided path and upward in its parent directories.
bool FindRootDirectory(const FString& InPath, FString& OutWorkspaceRoot)
{
	bool bFound = false;
	FString PathToPlasticSubdirectory;
	OutWorkspaceRoot = InPath;

	auto TrimTrailing = [](FString& Str, const TCHAR Char)
	{
		int32 Len = Str.Len();
		while(Len && Str[Len - 1] == Char)
		{
			Str = Str.LeftChop(1);
			Len = Str.Len();
		}
	};

	TrimTrailing(OutWorkspaceRoot, '\\');
	TrimTrailing(OutWorkspaceRoot, '/');

	while(!bFound && !OutWorkspaceRoot.IsEmpty())
	{
		// Look for the ".plastic" subdirectory present at the root of every Plastic workspace
		PathToPlasticSubdirectory = OutWorkspaceRoot / TEXT(".plastic");
		bFound = IFileManager::Get().DirectoryExists(*PathToPlasticSubdirectory);
		if(!bFound)
		{
			int32 LastSlashIndex;
			if(OutWorkspaceRoot.FindLastChar('/', LastSlashIndex))
			{
				OutWorkspaceRoot = OutWorkspaceRoot.Left(LastSlashIndex);
			}
			else
			{
				OutWorkspaceRoot.Empty();
			}
		}
	}
	if (!bFound)
	{
		OutWorkspaceRoot = InPath; // If not found, return the provided dir as best possible root.
	}
	return bFound;
}

void GetPlasticScmVersion(FString& OutPlasticScmVersion)
{
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	const bool bResult = RunCommand(TEXT("version"), TArray<FString>(), TArray<FString>(), InfoMessages, ErrorMessages);
	if (bResult && InfoMessages.Num() > 0)
	{
		OutPlasticScmVersion = InfoMessages[0];
	}
}

void GetUserName(FString& OutUserName)
{
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	const bool bResult = RunCommand(TEXT("whoami"), TArray<FString>(), TArray<FString>(), InfoMessages, ErrorMessages);
	if (bResult && InfoMessages.Num() > 0)
	{
		OutUserName = InfoMessages[0];
	}
}

bool GetWorkspaceName(const FString& InWorkspaceRoot, FString& OutWorkspaceName)
{
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("--format={0}"));
	TArray<FString> Files;
	Files.Add(InWorkspaceRoot);
	// Get the workspace name
	const bool bResult = RunCommand(TEXT("getworkspacefrompath"), Parameters, Files, InfoMessages, ErrorMessages);
	if (bResult && InfoMessages.Num() > 0)
	{
		OutWorkspaceName = MoveTemp(InfoMessages[0]);
	}

	return bResult;
}

bool GetRepositorySpecification(const FString& InWorkspaceRoot, FString& OutRepositoryName, FString& OutServerUrl) 
{
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("--nochanges"));
	TArray<FString> Files;
	Files.Add(InWorkspaceRoot);
	// Get the workspace status, looking like "cs:41@rep:UE4PlasticPlugin@repserver:localhost:8087"
	bool bResult = RunCommand(TEXT("status"), Parameters, Files, InfoMessages, ErrorMessages);
	if (bResult && InfoMessages.Num() > 0)
	{
		static const FString Changeset(TEXT("cs:"));
		static const FString Rep(TEXT("rep:"));
		static const FString Server(TEXT("repserver:"));
		const FString& WorkspaceStatus = InfoMessages[0];
		TArray<FString> RepositorySpecification;
 		WorkspaceStatus.ParseIntoArray(RepositorySpecification, TEXT("@"));
		if (3 >= RepositorySpecification.Num())
		{
//			OutChangeset = RepositorySpecification[0].RightChop(Changeset.Len());
			OutRepositoryName = RepositorySpecification[1].RightChop(Rep.Len());
			OutServerUrl = RepositorySpecification[2].RightChop(Server.Len());
		}
		else
		{
			bResult = false;
		}
	}

	return bResult;
}

void GetBranchName(const FString& InWorkspaceRoot, FString& OutBranchName)
{
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("--wkconfig"));
	Parameters.Add(TEXT("--nochanges"));
	Parameters.Add(TEXT("--nostatus"));
	TArray<FString> Files;
	Files.Add(InWorkspaceRoot);
	const bool bResult = RunCommand(TEXT("status"), Parameters, Files, InfoMessages, ErrorMessages);
	if(bResult && InfoMessages.Num() > 0)
	{
		OutBranchName = InfoMessages[0];
	}
}

/**
 * Extract and interpret the file state from the given Plastic "status" result.
 * empty string = unmodified/controlled or hidden changes
 CH Content\Changed_BP.uasset
 CO Content\CheckedOut_BP.uasset
 CP Content\Copied_BP.uasset
 RP Content\Replaced_BP.uasset
 AD Content\Added_BP.uasset
 PR Content\Private_BP.uasset
 IG Content\Ignored_BP.uasset
 DE Content\Deleted_BP.uasset
 LD Content\Deleted2_BP.uasset
 MV 100% Content\ToMove_BP.uasset -> Content\Moved_BP.uasset
 LM 100% Content\ToMove2_BP.uasset -> Content\Moved2_BP.uasset

 TODO: Conflicted files?
*/
class FPlasticStatusParser
{
public:
	FPlasticStatusParser(const FString& InResult)
	{
		const FString FileStatus = InResult.Mid(1, 2);

		if (FileStatus == "CH") // Modified but not Checked-Out
		{
			State = EWorkspaceState::Changed;
		}
		else if (FileStatus == "CO") // Checked-Out for modification
		{
			State = EWorkspaceState::CheckedOut;
		}
		else if (FileStatus == "CP")
		{
			State = EWorkspaceState::Copied;
		}
		else if (FileStatus == "RP")
		{
			State = EWorkspaceState::Replaced;
		}
		else if (FileStatus == "AD")
		{
			State = EWorkspaceState::Added;
		}
		else if (FileStatus == "PR") // Not Controlled/Not in Depot/Untracked
		{
			State = EWorkspaceState::Private;
		}
		else if (FileStatus == "IG")
		{
			State = EWorkspaceState::Ignored;
		}
		else if ((FileStatus == "DE") || (FileStatus == "LD")) // TODO: need to differentiate for CanEdit/CanCheckout/CanCheckIn?
		{
			State = EWorkspaceState::Deleted; // Deleted or Locally Deleted (ie. missing)
		}
		else if ((FileStatus == "MV") || (FileStatus == "LM")) // Renamed TODO: need to differentiate for CanEdit/CanCheckout/CanCheckIn?
		{
			State = EWorkspaceState::Moved; // Moved/Renamed or Locally Moved
		}
		else if (FileStatus == "conflited") // TODO: what is the appropriate status?
		{
			State = EWorkspaceState::Conflicted;
		}
		else
		{
			UE_LOG(LogSourceControl, Warning, TEXT("Unknown"));
			State = EWorkspaceState::Unknown;
		}
	}

	EWorkspaceState::Type State;
};

/** Parse the array of strings results of a 'cm status --nostatus --noheaders --all --ignore' command
 *
 * Example cm status results:
 CH Content\Changed_BP.uasset
 CO Content\CheckedOut_BP.uasset
 CP Content\Copied_BP.uasset
 RP Content\Replaced_BP.uasset
 AD Content\Added_BP.uasset
 PR Content\Private_BP.uasset
 IG Content\Ignored_BP.uasset
 DE Content\Deleted_BP.uasset
 LD Content\Deleted2_BP.uasset
 MV 100% Content\ToMove_BP.uasset -> Content\Moved_BP.uasset
 LM 100% Content\ToMove2_BP.uasset -> Content\Moved2_BP.uasset
 */
static void ParseStatusResult(const FString& InFile, const TArray<FString>& InResults, FPlasticSourceControlState& OutFileState)
{
	// Assuming one line of results for one file.
	static const FString EmptyString(TEXT(""));
	if (0 < InResults.Num())
	{
		const FString& Status = InResults[InResults.Num()-1]; // NOTE: in case of rename by editor, there are two results: checkouted AND renamed
		const FPlasticStatusParser StatusParser(Status);
		OutFileState.WorkspaceState = StatusParser.State;
	}
	else
	{
		// No result means Controlled/Unchanged file/Hidden changes
		OutFileState.WorkspaceState = EWorkspaceState::Controlled;
	}
	// @todo: temporary debug log
	UE_LOG(LogSourceControl, Log, TEXT("%s = %d:%s"), *InFile, static_cast<uint32>(OutFileState.WorkspaceState), OutFileState.ToString());
	OutFileState.TimeStamp.Now();
}

// Run a "status" command for each file to get workspace states
static bool RunStatus(const TArray<FString>& InFiles, TArray<FString>& OutErrorMessages, TArray<FPlasticSourceControlState>& OutStates)
{
	bool bResult = true;

	TArray<FString> Status;
	Status.Add(TEXT("--nostatus"));
	Status.Add(TEXT("--noheaders"));
	Status.Add(TEXT("--all"));
	Status.Add(TEXT("--ignored"));

	if (1 == InFiles.Num() && !FPaths::FileExists(InFiles[0]))
	{
		// Special case for "status" of a non-existing file (newly created/deleted)
		OutStates.Add(FPlasticSourceControlState(InFiles[0]));
		FPlasticSourceControlState& FileState = OutStates.Last();
		FileState.WorkspaceState = EWorkspaceState::Private; // Not Controlled
		bResult = false; // false so that we do not try to get it's lock state with "fileinfo"
	}
	else
	{
		for (const FString& File : InFiles)
		{
			// The "status" command only operate on one file at a time (TODO or one Folder!)
			OutStates.Add(FPlasticSourceControlState(File));
			FPlasticSourceControlState& FileState = OutStates.Last();

			// Do not run status commands anymore after the first failure (optimization, useful for global "submit to source control")
			if (bResult)
			{
				TArray<FString> OneFile;
				OneFile.Add(File);
				TArray<FString> Results;
				TArray<FString> ErrorMessages;
				bResult = RunCommand(TEXT("status"), Status, OneFile, Results, ErrorMessages);
				OutErrorMessages.Append(ErrorMessages);
				if (bResult)
				{
					ParseStatusResult(File, Results, FileState);
					if (FileState.IsConflicted())
					{
						// TODO In case of a conflict (unmerged file) get the base revision to merge
					}
				}
			}
		}
	}

	return bResult;
}

// Parse the fileinfo output format "{RevisionChangeset};{RevisionHeadChangeset};{LockedBy};{LockedWhere}"
class FPlasticFileinfoParser
{
public:
	FPlasticFileinfoParser(const FString& InResult)
	{
		TArray<FString> Fileinfos;
		const int32 NbElmts = InResult.ParseIntoArray(Fileinfos, TEXT(";"));
		if (NbElmts >= 2)
		{
			RevisionChangeset = FCString::Atoi(*Fileinfos[0]);
			RevisionHeadChangeset = FCString::Atoi(*Fileinfos[1]);
			if (NbElmts >= 3)
			{
				LockedBy = MoveTemp(Fileinfos[2]);
				if (NbElmts >=4)
				{
					LockedWhere = MoveTemp(Fileinfos[3]);
				}
			}
		}
	}

	int32 RevisionChangeset;
	int32 RevisionHeadChangeset;
	FString LockedBy;
	FString LockedWhere;
};

/** Parse the array of strings results of a 'cm fileinfo --format="{RevisionChangeset};{RevisionHeadChangeset};{LockedBy};{LockedWhere}"' command
 *
 * Example cm fileinfo results:
16;16;;
14;15;;
17;17;srombauts;Workspace_2
*/
static void ParseFileinfoResults(const TArray<FString>& InFiles, const TArray<FString>& InResults, TArray<FPlasticSourceControlState>& InOutStates)
{
	FPlasticSourceControlModule& PlasticSourceControl = FModuleManager::LoadModuleChecked<FPlasticSourceControlModule>("PlasticSourceControl");
	FPlasticSourceControlProvider& Provider = PlasticSourceControl.GetProvider();

	// Iterate on all files and all status of the result (assuming no more line of results than number of files)
	for (int32 IdxResult = 0; IdxResult < InResults.Num(); IdxResult++)
	{
		const FString& File = InFiles[IdxResult];
		const FString& Fileinfo = InResults[IdxResult];
		FPlasticSourceControlState& FileState = InOutStates[IdxResult];
		FPlasticFileinfoParser FileinfoParser(Fileinfo);

		FileState.LocalRevisionChangeset = FileinfoParser.RevisionChangeset;
		FileState.DepotRevisionChangeset = FileinfoParser.RevisionHeadChangeset;
		FileState.LockedBy = MoveTemp(FileinfoParser.LockedBy);
		FileState.LockedWhere = MoveTemp(FileinfoParser.LockedWhere);

		if ((0 < FileState.LockedBy.Len()) && ((FileState.LockedBy != Provider.GetUserName()) || (FileState.LockedWhere != Provider.GetWorkspaceName())))
		{
			// @todo: temporary debug log
			UE_LOG(LogSourceControl, Warning, TEXT("LockedByOther(%s) by '%s!=%s' (or %s!=%s)"), *File, *FileState.LockedBy, *Provider.GetUserName(), *FileState.LockedWhere, *Provider.GetWorkspaceName());
			FileState.WorkspaceState = EWorkspaceState::LockedByOther;
		}

		// @todo: temporary debug log
		UE_LOG(LogSourceControl, Log, TEXT("%s: %d;%d by '%s' (%s)"), *File, FileState.LocalRevisionChangeset, FileState.DepotRevisionChangeset, *FileState.LockedBy, *FileState.LockedWhere);
	}
}

// Run a Plastic "fileinfo" (similar to "status") command to update status of given files.
static bool RunFileinfo(const TArray<FString>& InFiles, TArray<FString>& OutErrorMessages, TArray<FPlasticSourceControlState>& OutStates)
{
	TArray<FString> Results;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("--format=\"{RevisionChangeset};{RevisionHeadChangeset};{LockedBy};{LockedWhere}\""));

	TArray<FString> ErrorMessages;
	const bool bResult = RunCommand(TEXT("fileinfo"), Parameters, InFiles, Results, ErrorMessages);
	OutErrorMessages.Append(ErrorMessages);
	if (bResult)
	{
		ParseFileinfoResults(InFiles, Results, OutStates);
	}

	return bResult;
}

// Run a Plastic "status" and "fileinfo" commands to update status of given files.
bool RunUpdateStatus(const TArray<FString>& InFiles, TArray<FString>& OutErrorMessages, TArray<FPlasticSourceControlState>& OutStates)
{
	bool bResult = true;

	// Plastic fileinfo does not return any results when called with at least one file not in a workspace
	// 1) So here we group files by path (ie. by subdirectory)
	TMap<FString, TArray<FString>> GroupOfFiles;
	for (const FString& File : InFiles)
	{
		const FString Path = FPaths::GetPath(*File);
		TArray<FString>* Group = GroupOfFiles.Find(Path);
		if (Group != nullptr)
		{
			Group->Add(File);
		}
		else
		{
			TArray<FString> NewGroup;
			NewGroup.Add(File);
			GroupOfFiles.Add(Path, NewGroup);
		}
	}

	// 2) then we can batch Plastic status operation by subdirectory
	for (const auto& Files : GroupOfFiles)
	{
		// Run a "status" command for each file to get workspace states
		const bool bGroupOk = RunStatus(Files.Value, OutErrorMessages, OutStates);
		if (bGroupOk)
		{
			// Run a Plastic "fileinfo" (similar to "status") command to update status of given files.
			bResult &= RunFileinfo(Files.Value, OutErrorMessages, OutStates);
		}
	}

	return bResult;
}

// Run a Plastic "cat" command to dump the binary content of a revision into a file.
// cm cat revid:1230@rep:myrep@repserver:myserver:8084 --raw --file=Name124.tmp
bool RunDumpToFile(const FString& InPathToPlasticBinary, const FString& InRevSpec, const FString& InDumpFileName)
{
	int32	ReturnCode = 0;
	FString Results;
	FString Errors;

	// start with the Plastic command itself, then add revspec and temp filename to dump
	FString FullCommand = TEXT("cat ");
	FullCommand += InRevSpec;
	FullCommand += TEXT(" --raw --file=\"");
	FullCommand += InDumpFileName;
	FullCommand += TEXT("\"");

	// @todo: temporary debug logs
	UE_LOG(LogSourceControl, Log, TEXT("RunDumpToFile: '%s %s'"), *InPathToPlasticBinary, *FullCommand);
	const bool bResult = FPlatformProcess::ExecProcess(*InPathToPlasticBinary, *FullCommand, &ReturnCode, &Results, &Errors);
	UE_LOG(LogSourceControl, Log, TEXT("RunDumpToFile: ExecProcess ReturnCode=%d Results='%s'"), ReturnCode, *Results);
	if (!bResult || !Errors.IsEmpty())
	{
		UE_LOG(LogSourceControl, Error, TEXT("RunDumpToFile: ExecProcess ReturnCode=%d Errors='%s'"), ReturnCode, *Errors);
	}

	return bResult;
}


// Translate actions from Plastic 'cm log' command to keywords used by the Editor UI 
FString TranslateAction(const FString& InAction)
{
	if (InAction.Equals(TEXT("Added")))
	{
		return TEXT("add");
	}
	else if (InAction.Equals(TEXT("Moved")))
	{
		return TEXT("branch");
	}
	else if (InAction.Equals(TEXT("Deleted")))
	{
		return TEXT("delete");
	}
	else // if (InAction.Equals(TEXT("Changed")))
	{
		return TEXT("edit");
	}
	// TODO: "integrate" for merged?
}

/**
 * Parse the array of strings results of a 'cm log --xml' command
 *
 * Example cm log results:
<?xml version="1.0" encoding="utf-8"?>
<LogList>
  <Changeset>
    <ObjId>989</ObjId>
    <ChangesetId>2</ChangesetId>
    <Branch>/main</Branch>
    <Comment>Ignore Collections and Developers content</Comment>
    <Owner>dev</Owner>
    <GUID>a985c487-0f54-45c5-b0ef-9b87c4c3c3f9</GUID>
    <Changes>
      <Item>
        <Branch>/main</Branch>
        <RevNo>2</RevNo>
        <Owner>dev</Owner>
        <RevId>985</RevId>
        <ParentRevId>282</ParentRevId>
        <SrcCmPath>/ignore.conf</SrcCmPath>
        <SrcParentItemId>2</SrcParentItemId>
        <DstCmPath>/ignore.conf</DstCmPath>
        <DstParentItemId>2</DstParentItemId>
        <Date>2016-04-18T10:44:49.0000000+02:00</Date>
        <Type>Changed</Type>
      </Item>
    </Changes>
    <Date>2016-04-18T10:44:49.0000000+02:00</Date>
  </Changeset>
</LogList>
*/
static void ParseLogResults(const FXmlFile& InXmlResult, FPlasticSourceControlRevision& OutSourceControlRevision)
{
	static const FString LogList(TEXT("LogList"));
	static const FString Changeset(TEXT("Changeset"));
	static const FString Comment(TEXT("Comment"));
	static const FString Date(TEXT("Date"));
	static const FString Owner(TEXT("Owner"));
	static const FString Changes(TEXT("Changes"));
	static const FString Item(TEXT("Item"));
	static const FString RevId(TEXT("RevId"));
	static const FString ParentRevId(TEXT("ParentRevId"));
	static const FString SrcCmPath(TEXT("SrcCmPath"));
	static const FString DstCmPath(TEXT("DstCmPath"));
	static const FString Type(TEXT("Type"));

	const FXmlNode* LogListNode = InXmlResult.GetRootNode();
	if (LogListNode == nullptr || LogListNode->GetTag() != LogList)
	{
		return;
	}

	const FXmlNode* ChangesetNode = LogListNode->FindChildNode(Changeset);
	if (ChangesetNode == nullptr)
	{
		return;
	}

	const FXmlNode* CommentNode = ChangesetNode->FindChildNode(Comment);
	if (CommentNode != nullptr)
	{
		OutSourceControlRevision.Description = CommentNode->GetContent();
	}
	const FXmlNode* OwnerNode = ChangesetNode->FindChildNode(Owner);
	if (CommentNode != nullptr)
	{
		OutSourceControlRevision.UserName = OwnerNode->GetContent();
	}
	const FXmlNode* DateNode = ChangesetNode->FindChildNode(Date);
	if (DateNode != nullptr)
	{	//                           |--|
		//    2016-04-18T10:44:49.0000000+02:00
		// => 2016-04-18T10:44:49.000+02:00
		const FString DateIso = DateNode->GetContent().LeftChop(10) + DateNode->GetContent().RightChop(27);
		FDateTime::ParseIso8601(*DateIso, OutSourceControlRevision.Date);
	}

	const FXmlNode* ChangesNode = ChangesetNode->FindChildNode(Changes);
	if (ChangesNode == nullptr)
	{
		return;
	}

	// Iterate on files to find the one we are tracking
	for (const FXmlNode* ItemNode : ChangesNode->GetChildrenNodes())
	{
		int32 RevisionNumber = -1;
		const FXmlNode* RevIdNode = ItemNode->FindChildNode(RevId);
		if (RevIdNode != nullptr)
		{
			RevisionNumber = FCString::Atoi(*RevIdNode->GetContent());
		}
		// Is this about the file we are looking for?
		if (RevisionNumber == OutSourceControlRevision.RevisionNumber)
		{
			const FXmlNode* DstCmPathNode = ItemNode->FindChildNode(DstCmPath);
			if (DstCmPathNode != nullptr)
			{
				OutSourceControlRevision.Filename = DstCmPathNode->GetContent();

				const FXmlNode* SrcCmPathNode = ItemNode->FindChildNode(SrcCmPath);
				const FXmlNode* ParentRevIdNode = ItemNode->FindChildNode(ParentRevId);
				// Detect case of rename ("branch" in Perforce vocabulary)
				if (ParentRevIdNode != nullptr && SrcCmPathNode != nullptr && !SrcCmPathNode->GetContent().Equals(DstCmPathNode->GetContent()))
				{
					TSharedRef<FPlasticSourceControlRevision, ESPMode::ThreadSafe> MovedFromRevision = MakeShareable(new FPlasticSourceControlRevision);
					MovedFromRevision->Filename = SrcCmPathNode->GetContent();
					MovedFromRevision->RevisionNumber = FCString::Atoi(*ParentRevIdNode->GetContent());
	
					OutSourceControlRevision.BranchSource = MovedFromRevision;
				}
			}
			const FXmlNode* TypeNode = ItemNode->FindChildNode(Type);
			if (TypeNode != nullptr)
			{
				OutSourceControlRevision.Action = TranslateAction(TypeNode->GetContent());
			}
			// 	Do not stop at first match, because in case of rename there are multiple log nodes: Changed+Moved (in this order)
		}
	}
}

// Run "cm log" on the changeset
static bool RunLogCommand(const FString& InChangeset, FPlasticSourceControlRevision& OutSourceControlRevision)
{
	FPlasticSourceControlModule& PlasticSourceControl = FModuleManager::LoadModuleChecked<FPlasticSourceControlModule>("PlasticSourceControl");
	FPlasticSourceControlProvider& Provider = PlasticSourceControl.GetProvider();
	const FString RepositorySpecification = FString::Printf(TEXT("cs:%s@rep:%s@repserver:%s"), *InChangeset, *Provider.GetRepositoryName(), *Provider.GetServerUrl());

	FString Results;
	FString Errors;
	TArray<FString> Parameters;
	Parameters.Add(RepositorySpecification);
	Parameters.Add(TEXT("--xml"));
	Parameters.Add(TEXT("--encoding=\"utf-8\""));

	// Uses the raw RunCommandInternal() that does not split results in an array of strings, for XML parsing
	bool bResult = RunCommandInternal(TEXT("log"), Parameters, TArray<FString>(), Results, Errors);
	if (bResult)
	{
		FXmlFile XmlFile;
		bResult = XmlFile.LoadFile(Results, EConstructMethod::ConstructFromBuffer);
		if (bResult)
		{
			ParseLogResults(XmlFile, OutSourceControlRevision);
		}
	}
	return bResult;
}

/**
 * Parse results of the 'cm history --format="{1};{6}"' command, then run "cm log" on each
 * 
 * Results of the history command are with one changeset number and revision id by line, like that:
14;176
17;220
18;223
*/
static bool ParseHistoryResults(const TArray<FString>& InResults, TPlasticSourceControlHistory& OutHistory)
{
	bool bResult = true;

	OutHistory.Reserve(InResults.Num());

	// parse history in reverse: needed to get most recent at the top (implied by the UI)
	for (int32 Index = InResults.Num() - 1; Index >= 0; Index--)
	{
		const FString& Result = InResults[Index];
		if (bResult)
		{
			TArray<FString> Infos;
			const int32 NbElmts = Result.ParseIntoArray(Infos, TEXT(";"));
			if (NbElmts == 2)
			{
				const TSharedRef<FPlasticSourceControlRevision, ESPMode::ThreadSafe> SourceControlRevision = MakeShareable(new FPlasticSourceControlRevision);
				const FString& Changeset = Infos[0];
				const FString& RevisionId = Infos[1];
				SourceControlRevision->ChangesetNumber = FCString::Atoi(*Changeset);
				SourceControlRevision->RevisionNumber = FCString::Atoi(*RevisionId);
				SourceControlRevision->Revision = RevisionId;

				// Run "cm log" on the changeset number
				bResult = RunLogCommand(Changeset, *SourceControlRevision);
				OutHistory.Add(SourceControlRevision);
			}
			else
			{
				bResult = false;
			}
		}
	}

	return bResult;
}

// Run a Plastic "history" command and multiple "log" commands and parse them.
bool RunGetHistory(const FString& InFile, TArray<FString>& OutErrorMessages, TPlasticSourceControlHistory& OutHistory)
{
	TArray<FString> Results;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("--format=\"{1};{6}\"")); // Get Changeset number and revision Id of each revision of the asset
	TArray<FString> OneFile;
	OneFile.Add(*InFile);

	bool bResult = RunCommand(TEXT("history"), Parameters, OneFile, Results, OutErrorMessages);
	if (bResult)
	{
		bResult = ParseHistoryResults(Results, OutHistory);
	}

	return bResult;
}

bool UpdateCachedStates(const TArray<FPlasticSourceControlState>& InStates)
{
	FPlasticSourceControlModule& PlasticSourceControl = FModuleManager::LoadModuleChecked<FPlasticSourceControlModule>( "PlasticSourceControl" );
	FPlasticSourceControlProvider& Provider = PlasticSourceControl.GetProvider();
	int NbStatesUpdated = 0;

	for (const auto& InState : InStates)
	{
		TSharedRef<FPlasticSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(InState.LocalFilename);
		if (State->WorkspaceState != InState.WorkspaceState)
		{
			State->WorkspaceState = InState.WorkspaceState;
			State->PendingMergeBaseFileHash = InState.PendingMergeBaseFileHash;
			State->TimeStamp = InState.TimeStamp; // TODO: Bug report: Workaround a bug with the Source Control Module not updating file state after a "Save"
			NbStatesUpdated++;
		}
	}

	return (NbStatesUpdated > 0);
}

/**
 * Helper struct for RemoveRedundantErrors()
 */
struct FRemoveRedundantErrors
{
	FRemoveRedundantErrors(const FString& InFilter)
		: Filter(InFilter)
	{
	}

	bool operator()(const FString& String) const
	{
		if(String.Contains(Filter))
		{
			return true;
		}

		return false;
	}

	/** The filter string we try to identify in the reported error */
	FString Filter;
};

void RemoveRedundantErrors(FPlasticSourceControlCommand& InCommand, const FString& InFilter)
{
	bool bFoundRedundantError = false;
	for(const FString& ErrorMessage : InCommand.ErrorMessages)
	{
		if(ErrorMessage.Contains(InFilter, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			InCommand.InfoMessages.Add(ErrorMessage);
			bFoundRedundantError = true;
		}
	}

	InCommand.ErrorMessages.RemoveAll( FRemoveRedundantErrors(InFilter) );

	// if we have no error messages now, assume success!
	if(bFoundRedundantError && InCommand.ErrorMessages.Num() == 0 && !InCommand.bCommandSuccessful)
	{
		InCommand.bCommandSuccessful = true;
	}
}

}
