/*
 * This file is part of the elastic-onec (https://github.com/toreonify/elastic-onec).
 * Copyright (c) 2024 Ivan Korytov <toreonify@outlook.com>.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <tchar.h>
#include <stdint.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <stringapiset.h>
#include <synchapi.h>

#define SHELL_PATH L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe"
#define CMD_LINE L" -File \"%s\" -FileName \"%s\""

#define SOURCE_NAME "1CSendDictionaries"
#define SERVICE_NAME TEXT("1cdsend")

#define WATCH_BUF_SIZE 1024
#define EVENTLOG_MSG_SIZE 4096

typedef struct watch {
	LPWSTR fileName;
	LPWSTR fullFileName;
	FILETIME lastWriteTime;
	HANDLE file;
	uint8_t changeBuf[WATCH_BUF_SIZE];
	OVERLAPPED overlapped;
} watch;

void ProcessEvent(watch* w, size_t fileNameLength, wchar_t* fileName);
void AddHandler(int num, LPWSTR filePath);
void LogEvent(WORD level, WORD type, DWORD id, const wchar_t* format, ...);
void Cleanup();

watch* watches;
int handlesCount = 0;
HANDLE* eventsHandles;
LPWSTR scriptPath;
DWORD lastAction = 0;

int _argc = 0;
wchar_t** _argv = NULL;
HANDLE eventLog = NULL;

VOID WINAPI ServiceCtrlHandler(DWORD);
VOID WINAPI ServiceMain(DWORD argc, wchar_t** argv);
DWORD WINAPI ServiceWorkerThread(LPVOID lpParam);

SERVICE_STATUS        ServiceStatus = { 0 };
SERVICE_STATUS_HANDLE StatusHandle = NULL;
HANDLE                ServiceStopEvent = INVALID_HANDLE_VALUE;

int wmain(int argc, wchar_t* argv[])
{
	_argc = argc;
	_argv = argv;

	SERVICE_TABLE_ENTRY ServiceTable[] =
	{
		{ SERVICE_NAME, (LPSERVICE_MAIN_FUNCTION)ServiceMain },
		{ NULL, NULL }
	};

	if (StartServiceCtrlDispatcher(ServiceTable) == FALSE)
	{
		return GetLastError();
	}

	return 0;
}

VOID WINAPI ServiceCtrlHandler(DWORD CtrlCode)
{
	switch (CtrlCode)
	{
	case SERVICE_CONTROL_STOP:

		if (ServiceStatus.dwCurrentState != SERVICE_RUNNING)
			break;

		/*
		* Perform tasks necessary to stop the service here
		*/

		ServiceStatus.dwControlsAccepted = 0;
		ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
		ServiceStatus.dwWin32ExitCode = 0;
		ServiceStatus.dwCheckPoint = 4;

		if (SetServiceStatus(StatusHandle, &ServiceStatus) == FALSE)
		{
			OutputDebugStringW(L"1CSendDictionaries: ServiceCtrlHandler: SetServiceStatus returned error");
		}

		// This will signal the worker thread to start shutting down
		SetEvent(ServiceStopEvent);

		break;

	default:
		break;
	}
}

void LogEvent(WORD level, WORD type, DWORD id, const wchar_t* format, ...) {
	wchar_t* message = (wchar_t*) malloc(sizeof(wchar_t) * (wcslen(format) + EVENTLOG_MSG_SIZE));
	
	va_list args;
    va_start(args, format);
 
    StringCchVPrintfW(message, wcslen(format) + EVENTLOG_MSG_SIZE, format, args);
	
	va_end(args);
	
	ReportEventW(eventLog, level, type, id, NULL, 1, 0, (const wchar_t**)&message, NULL);

	free(message);
}

VOID WINAPI ServiceMain(DWORD argc, wchar_t** argv)
{
	DWORD Status = E_FAIL;
	HANDLE hThread = NULL;

	// Register our service control handler with the SCM
	StatusHandle = RegisterServiceCtrlHandler(SERVICE_NAME, ServiceCtrlHandler);

	if (StatusHandle == NULL)
	{
		return;
	}

	// Tell the service controller we are starting
	ZeroMemory(&ServiceStatus, sizeof(ServiceStatus));
	ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	ServiceStatus.dwControlsAccepted = 0;
	ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
	ServiceStatus.dwWin32ExitCode = 0;
	ServiceStatus.dwServiceSpecificExitCode = 0;
	ServiceStatus.dwCheckPoint = 0;

	if (SetServiceStatus(StatusHandle, &ServiceStatus) == FALSE)
	{
		OutputDebugStringW(L"1CSendDictionaries: ServiceMain: SetServiceStatus returned error");
	}

	/*
	* Perform tasks necessary to start the service here
	*/

	// Create a service stop event to wait on later
	ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (ServiceStopEvent == NULL)
	{
		// Error creating event
		// Tell service controller we are stopped and exit
		ServiceStatus.dwControlsAccepted = 0;
		ServiceStatus.dwCurrentState = SERVICE_STOPPED;
		ServiceStatus.dwWin32ExitCode = GetLastError();
		ServiceStatus.dwCheckPoint = 1;

		if (SetServiceStatus(StatusHandle, &ServiceStatus) == FALSE)
		{
			OutputDebugStringW(L"1CSendDictionaries: ServiceMain: SetServiceStatus returned error");
		}

		CloseHandle(StatusHandle);
		return;
	}

	// Tell the service controller we are started
	ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
	ServiceStatus.dwCurrentState = SERVICE_RUNNING;
	ServiceStatus.dwWin32ExitCode = 0;
	ServiceStatus.dwCheckPoint = 0;

	if (SetServiceStatus(StatusHandle, &ServiceStatus) == FALSE)
	{
		wprintf(L"1CSendDictionaries: ServiceMain: SetServiceStatus returned error");
	}

	// Initialization
	eventLog = RegisterEventSource(NULL, SOURCE_NAME);

	if (eventLog == NULL) {
		wprintf(L"ERROR: RegisterEventSource function failed.\n");
		ExitProcess(GetLastError());
	}

	LogEvent(EVENTLOG_SUCCESS, 1, 1000, L"1CSendDictionaries service started\n");

	if (_argc < 3) {
		LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"Not enough input parameters\n");
		DeregisterEventSource(eventLog);
		return;
	}

	handlesCount = (_argc - 2);

	if (handlesCount > MAXIMUM_WAIT_OBJECTS) {
		LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"Too much input files (%i > %i)\n", handlesCount, MAXIMUM_WAIT_OBJECTS);
		DeregisterEventSource(eventLog);
		return;
	}

	watches = (watch*)malloc(sizeof(watch) * handlesCount);
	eventsHandles = (HANDLE*)malloc(sizeof(HANDLE) * (handlesCount + 1)); // We need a service stop event to monitor as well
	eventsHandles[handlesCount] = ServiceStopEvent;

	scriptPath = (wchar_t*)malloc(sizeof(wchar_t) * (MAX_PATH + 1));
	StringCchCopyW(scriptPath, MAX_PATH + 1, _argv[1]);

	for (int i = 0; i < handlesCount; i++) {
		AddHandler(i, _argv[i + 2]);

		ProcessEvent(&watches[i], wcslen(watches[i].fileName), watches[i].fileName);
	}

	// Start a thread that will perform the main task of the service
	hThread = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);

	// Wait until our worker thread exits signaling that the service needs to stop
	WaitForSingleObject(hThread, INFINITE);

	// Perform any cleanup tasks
	Cleanup();

	// Tell the service controller we are stopped
	ServiceStatus.dwControlsAccepted = 0;
	ServiceStatus.dwCurrentState = SERVICE_STOPPED;
	ServiceStatus.dwWin32ExitCode = 0;
	ServiceStatus.dwCheckPoint = 3;

	if (SetServiceStatus(StatusHandle, &ServiceStatus) == FALSE)
	{
		OutputDebugStringW(L"1CSendDictionaries: ServiceMain: SetServiceStatus returned error");
	}

	return;
}

void AddHandler(int num, LPWSTR filePath) {
	WCHAR* wFilePart;
	WCHAR  dirPath[MAX_PATH + 1];

	ZeroMemory(&(watches[num]), sizeof(watch));

	DWORD retval = GetFullPathNameW(filePath, MAX_PATH, dirPath, &wFilePart);

	watches[num].fullFileName = (wchar_t*)malloc(sizeof(wchar_t) * (wcslen(filePath) + 1));
	watches[num].fileName = (wchar_t*)malloc(sizeof(wchar_t) * (wcslen(wFilePart) + 1));

	StringCchCopyW(watches[num].fullFileName, wcslen(filePath) + 1, filePath);
	StringCchCopyW(watches[num].fileName, wcslen(wFilePart) + 1, wFilePart);
	PathRemoveFileSpecW(dirPath);

	LogEvent(EVENTLOG_SUCCESS, 1, 1000, L"Added %s in %s for watching\n", watches[num].fileName, dirPath);

	watches[num].file = CreateFileW(dirPath,
		FILE_LIST_DIRECTORY,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
		NULL);

	if (watches[num].file == INVALID_HANDLE_VALUE)
	{
		LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"\n ERROR: CreateFile function failed.\n");
		ExitProcess(GetLastError());
	}

	watches[num].overlapped.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	eventsHandles[num] = watches[num].overlapped.hEvent;

	BOOL success = ReadDirectoryChangesW(
		watches[num].file, watches[num].changeBuf, WATCH_BUF_SIZE, FALSE,
		FILE_NOTIFY_CHANGE_FILE_NAME |
		FILE_NOTIFY_CHANGE_DIR_NAME |
		FILE_NOTIFY_CHANGE_LAST_WRITE,
		NULL, &(watches[num].overlapped), NULL);

	if (eventsHandles[num] == INVALID_HANDLE_VALUE)
	{
		LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"\n ERROR: ReadDirectoryChangesW function failed.\n");
		ExitProcess(GetLastError());
	}
}

DWORD WINAPI ServiceWorkerThread(LPVOID lpParam)
{
	DWORD dwWaitStatus;

	for (int i = 0; i < handlesCount; i++) {
		if (eventsHandles[i] == NULL) {
			LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"\n ERROR: Unexpected NULL from ReadDirectoryChangesW.\n");
			return GetLastError();
		}
	}

	while (TRUE) {
		dwWaitStatus = WaitForMultipleObjects(handlesCount + 1, eventsHandles, FALSE, INFINITE);

		if (dwWaitStatus == WAIT_OBJECT_0 + handlesCount) {
			LogEvent(EVENTLOG_WARNING_TYPE, 3, 1000, L"Stopping service\n");
			break;
		}

		if (dwWaitStatus >= WAIT_OBJECT_0 && dwWaitStatus < WAIT_OBJECT_0 + handlesCount) {
			int num = dwWaitStatus - WAIT_OBJECT_0;
			DWORD bytes_transferred;
			GetOverlappedResult(watches[num].file, &(watches[num].overlapped), &bytes_transferred, FALSE);

			FILE_NOTIFY_INFORMATION* event = (FILE_NOTIFY_INFORMATION*)watches[num].changeBuf;

			for (;;) {
				DWORD name_len = event->FileNameLength / sizeof(wchar_t);

				switch (event->Action) {
				case FILE_ACTION_MODIFIED: {
					lastAction = event->Action;
					ProcessEvent(&watches[num], name_len, event->FileName);
				} break;

				case FILE_ACTION_ADDED: {
					break;
				}

				case FILE_ACTION_REMOVED: {
					break;
				}

				case FILE_ACTION_RENAMED_OLD_NAME: {
					break;
				}

				case FILE_ACTION_RENAMED_NEW_NAME: {
					// Simple and stupid: if new file is 1CV8Clst.lst, then it was renamed from 1CV8Clstn.lst and thus changed
					// 1C doesn't write to a file directly and creates a temporary then renames it and also keeps and old version.
					lastAction = event->Action;
					ProcessEvent(&watches[num], name_len, event->FileName);
					break;
				}

				default: {
					LogEvent(EVENTLOG_WARNING_TYPE, 3, 1000, L"Unhandled action (0x%X, %.*s)!\n", event->Action, name_len, event->FileName);
					break;
				}
				}

				// Are there more events to handle?
				if (event->NextEntryOffset) {
					*((uint8_t**)&event) += event->NextEntryOffset;
				}
				else {
					break;
				}
			}

			// Queue the next event
			ZeroMemory(&watches[num].changeBuf, WATCH_BUF_SIZE);
			BOOL success = ReadDirectoryChangesW(
				watches[num].file, watches[num].changeBuf, WATCH_BUF_SIZE, FALSE,
				FILE_NOTIFY_CHANGE_FILE_NAME |
				FILE_NOTIFY_CHANGE_DIR_NAME |
				FILE_NOTIFY_CHANGE_LAST_WRITE,
				NULL, &(watches[num].overlapped), NULL);
		}
		else {
			switch (dwWaitStatus) {
			case WAIT_TIMEOUT:
				LogEvent(EVENTLOG_INFORMATION_TYPE, 1, 1000, L"\nNo changes in the timeout period.\n");
				break;
			default:
				LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"\n ERROR: Unhandled dwWaitStatus.\n");
				return GetLastError();
				break;
			}
		}
	}

	return ERROR_SUCCESS;
}

void ProcessEvent(watch* w, size_t fileNameLength, wchar_t* fileName)
{
	wchar_t* tempName = (wchar_t*)malloc(sizeof(wchar_t) * (fileNameLength + 1));
	StringCchCopyNW(tempName, fileNameLength + 1, fileName, fileNameLength);

	if (_wcsicmp(tempName, w->fileName) == 0) {
		if (lastAction == FILE_ACTION_MODIFIED) {
			LogEvent(EVENTLOG_INFORMATION_TYPE, 1, 1000, L"File '%s' changed.\n", tempName);
		}
		if (lastAction == FILE_ACTION_RENAMED_NEW_NAME) {
			LogEvent(EVENTLOG_INFORMATION_TYPE, 3, 1000, L"File '%s' renamed\n", tempName);
		}
		
		int retries = 0;
		HANDLE hFile = CreateFileW(w->fullFileName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

		if (hFile == INVALID_HANDLE_VALUE)
		{			
			LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"CreateFile failed with %d\n", GetLastError());
			goto end;
		}
	
		FILETIME ftCreate, ftAccess, ftWrite;
		BOOL result = GetFileTime(hFile, &ftCreate, &ftAccess, &ftWrite);

		CloseHandle(hFile);
		
		if (!result)
		{
			LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"Failed to obtain last write time for '%s'.\n", tempName);
			goto end;
		}
		
		if (CompareFileTime(&w->lastWriteTime, &ftWrite) < 0) {
			w->lastWriteTime = ftWrite;
			
			STARTUPINFOW si;
			PROCESS_INFORMATION pi;
			ZeroMemory(&si, sizeof(si));
			si.cb = sizeof(si);
			ZeroMemory(&pi, sizeof(pi));

			wchar_t* cmdLine = (wchar_t*) malloc(sizeof(wchar_t) * (wcslen(CMD_LINE) + MAX_PATH));
			StringCchPrintfW(cmdLine, wcslen(CMD_LINE) + MAX_PATH, CMD_LINE, scriptPath, w->fullFileName);
			
			LogEvent(EVENTLOG_INFORMATION_TYPE, 1, 1000, L"Command line is '%s'\n", cmdLine);

			// Start the child process.
			if (!CreateProcessW(
				SHELL_PATH,
				(LPWSTR)cmdLine,
				NULL,           // Process handle not inheritable
				NULL,           // Thread handle not inheritable
				FALSE,          // Set handle inheritance to FALSE
				0,              // No creation flags
				NULL,           // Use parent's environment block
				NULL,           // Use parent's starting directory
				&si,            // Pointer to STARTUPINFO structure
				&pi)            // Pointer to PROCESS_INFORMATION structure
				)
			{
				LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"CreateProcess failed (%d).\n", GetLastError());
			}
			else
			{
				LogEvent(EVENTLOG_SUCCESS, 1, 1000, L"CreateProcess succeded.\n");
			}
			
			WaitForSingleObject(pi.hProcess, INFINITE);

			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			
			free(cmdLine);
		} else {
			LogEvent(EVENTLOG_INFORMATION_TYPE, 1, 1000, L"File '%s' last write time didn't change.\n", tempName);
		}
	} else {
		// Kind of excessive, but if you want...
		// LogEvent(EVENTLOG_INFORMATION_TYPE, 1, 1000, L"File is '%s' not in watch\n", tempName);
	}

	end:
	free(tempName);
	return;
}

void Cleanup() {
	for (int i = 0; i < handlesCount; i++) {
		if (eventsHandles[i] != NULL) {
			CloseHandle(eventsHandles[i]);
		}
		
		if (watches[i].file != NULL) {
			CloseHandle(watches[i].file);
		}
		
		if (watches[i].fileName != NULL) {
			free(watches[i].fileName);
		}
		
		if (watches[i].fullFileName != NULL) {
			free(watches[i].fullFileName);
		}
	}

	if (scriptPath != NULL) {
		free(scriptPath);
	}

	if (eventLog != NULL) {
		DeregisterEventSource(eventLog);
	}
	
	if (ServiceStopEvent != NULL) {
		CloseHandle(ServiceStopEvent);
	}
}
