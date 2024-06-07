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

#include <stdlib.h>
#include <windows.h>
#include <strsafe.h>
#include <stringapiset.h>
#include <bcrypt.h>

#define SHELL_PATH L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe"
#define CMD_LINE L" -File \"%s\" -FileName \"%s\""

#define SOURCE_NAME "1CSendDictionaries"
#define SERVICE_NAME TEXT("1cdsend")

#define BUF_SIZE 1024
#define EVENTLOG_MSG_SIZE 4096

typedef struct watch {
	LPWSTR filePath;

	PBYTE currentHash;
	PBYTE previousHash;
} watch;

void ProcessEvent(watch* w);
void AddFile(int num, LPWSTR filePath);
void LogEvent(WORD level, WORD type, DWORD id, const wchar_t* format, ...);
void Cleanup();

watch* watches;
int watchesCount = 0;

LPWSTR scriptPath;

int _argc = 0;
wchar_t** _argv = NULL;
HANDLE eventLog = NULL;

VOID WINAPI ServiceCtrlHandler(DWORD);
VOID WINAPI ServiceMain(DWORD argc, wchar_t** argv);
DWORD WINAPI ServiceWorkerThread(LPVOID lpParam);

SERVICE_STATUS        ServiceStatus = { 0 };
SERVICE_STATUS_HANDLE StatusHandle = NULL;
HANDLE                ServiceStopEvent = INVALID_HANDLE_VALUE;

void CalculateHash(watch* w);

BCRYPT_ALG_HANDLE algorithm = NULL;
BCRYPT_HASH_HANDLE hash = NULL;
PBYTE hashObject = NULL;
ULONG hashLength = 0;

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
			OutputDebugStringW(L"1CSendDictionaries: SetServiceStatus returned error");
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
	
	if (message != NULL)
	{
		va_list args;
		va_start(args, format);

		StringCchVPrintfW(message, wcslen(format) + EVENTLOG_MSG_SIZE, format, args);

		va_end(args);

		ReportEventW(eventLog, level, type, id, NULL, 1, 0, (const wchar_t**)&message, NULL);

		free(message);
	}
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
		wprintf(L"1CSendDictionaries: SetServiceStatus returned error");
		Cleanup();
		ExitProcess(1);
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
			wprintf(L"1CSendDictionaries: SetServiceStatus returned error");
			Cleanup();
			ExitProcess(1);
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
		wprintf(L"1CSendDictionaries: SetServiceStatus returned error");
		Cleanup();
		ExitProcess(1);
	}

	// Initialization
	eventLog = RegisterEventSource(NULL, SOURCE_NAME);

	if (eventLog == NULL) {
		wprintf(L"1CSendDictionaries: RegisterEventSource function failed.\n");
		Cleanup();
		ExitProcess(1);
	}

	LogEvent(EVENTLOG_SUCCESS, 1, 1000, L"1CSendDictionaries service started\n");

	if (_argc < 3) {
		LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"Not enough input parameters\n");
		Cleanup();
		ExitProcess(1);
	}

	DWORD rc = 0, status = 0;
	status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_HASH_REUSABLE_FLAG);
	if (status != 0) {
		LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"Failed to open algorithm provider\n");
		Cleanup();
		ExitProcess(1);
	}

	ULONG hashObjectSize = 0, receivedBytes = 0;
	status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PBYTE)&hashObjectSize, sizeof(ULONG), &receivedBytes, 0);
	if (status != 0) {
		LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"Failed to get hash object size\n");
		Cleanup();
		ExitProcess(1);
	}

	hashObject = (PBYTE)HeapAlloc(GetProcessHeap(), 0, hashObjectSize);
	if (hashObject == NULL)
	{
		LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"Failed to allocate hash object on heap\n");
		Cleanup();
		ExitProcess(1);
	}
	memset(hashObject, 0, hashObjectSize);

	status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, (PBYTE)&hashLength, sizeof(ULONG), &receivedBytes, 0);
	if (status != 0)
	{
		LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"Failed to get hash length\n");
		Cleanup();
		ExitProcess(1);
	}

	status = BCryptCreateHash(algorithm, &hash, hashObject, hashObjectSize, NULL, 0, BCRYPT_HASH_REUSABLE_FLAG);
	if (status != 0)
	{
		LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"Failed to create hash object\n");
		Cleanup();
		ExitProcess(1);
	}

	watchesCount = (_argc - 2);
	watches = (watch*)malloc(sizeof(watch) * watchesCount);
	
	scriptPath = (wchar_t*)malloc(sizeof(wchar_t) * (MAX_PATH + 1));
	
	if (scriptPath != NULL)
	{
		StringCchCopyW(scriptPath, MAX_PATH + 1, _argv[1]);
	}
	else
	{
		LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"Failed to allocate memory\n");
		Cleanup();
		ExitProcess(1);
	}

	for (int i = 0; i < watchesCount; i++) {
		AddFile(i, _argv[i + 2]);

		CalculateHash(&watches[i]);
		ProcessEvent(&watches[i]);

		memcpy(watches[i].previousHash, watches[i].currentHash, hashLength);
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
		OutputDebugStringW(L"1CSendDictionaries: SetServiceStatus returned error");
		ExitProcess(1);
	}

	return;
}

void AddFile(int num, LPWSTR filePath) {
	ZeroMemory(&(watches[num]), sizeof(watch));

	watches[num].filePath = (wchar_t*)malloc(sizeof(wchar_t) * (wcslen(filePath) + 1));

	StringCchCopyW(watches[num].filePath, wcslen(filePath) + 1, filePath);

	watches[num].currentHash = (PBYTE)HeapAlloc(GetProcessHeap(), 0, hashLength);
	if (watches[num].currentHash == NULL)
	{
		LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"Failed to allocate hash buffers on heap!\n");
		Cleanup();
		ExitProcess(GetLastError());
	}
	memset(watches[num].currentHash, 0, hashLength);

	watches[num].previousHash = (PBYTE)HeapAlloc(GetProcessHeap(), 0, hashLength);
	if (watches[num].previousHash == NULL)
	{
		LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"Failed to allocate hash buffers on heap!\n");
		Cleanup();
		ExitProcess(GetLastError());
	}
	memset(watches[num].previousHash, 0, hashLength);

	LogEvent(EVENTLOG_SUCCESS, 1, 1000, L"Added %s for watching\n", watches[num].filePath);
}

DWORD WINAPI ServiceWorkerThread(LPVOID lpParam)
{
	DWORD dwWaitStatus;

	while (TRUE) {
		dwWaitStatus = WaitForSingleObject(ServiceStopEvent, 500);

		if (dwWaitStatus == WAIT_OBJECT_0) {
			LogEvent(EVENTLOG_WARNING_TYPE, 3, 1000, L"Stopping service\n");
			break;
		}

		for (int i = 0; i < watchesCount; i++) {
			CalculateHash(&watches[i]);

			if (memcmp(watches[i].currentHash, watches[i].previousHash, hashLength) != 0) {
				ProcessEvent(&watches[i]);

				memcpy(watches[i].previousHash, watches[i].currentHash, hashLength);
			}
		}
	}

	return ERROR_SUCCESS;
}

void CalculateHash(watch* w)
{
	BYTE fileBuf[BUF_SIZE];
	DWORD fileRead;

	DWORD status, rc;
	HANDLE file = CreateFileW(w->filePath, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		FILE_FLAG_SEQUENTIAL_SCAN, NULL);

	if (file == INVALID_HANDLE_VALUE) {
		status = GetLastError();
		goto done;
	}

	while (rc = ReadFile(file, fileBuf, BUF_SIZE, &fileRead, NULL))
	{
		if (rc == FALSE) {
			LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"Failed to read file\n");
			goto done;
		}

		if (0 == fileRead)
		{
			break;
		}

		status = BCryptHashData(hash, (PBYTE)&fileBuf, BUF_SIZE, 0);
		if (status != 0)
		{
			LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"Failed to calculate hash\n");
			goto done;
		}
	}

	status = BCryptFinishHash(hash, w->currentHash, hashLength, 0);

	if (status != 0)
	{
		LogEvent(EVENTLOG_ERROR_TYPE, 2, 1000, L"Failed to finish hashing\n");
		memcpy(w->currentHash, w->previousHash, hashLength);
		goto done;
	}

	done:
		CloseHandle(file);
}

void ProcessEvent(watch* w)
{
	SYSTEMTIME lt;
	GetLocalTime(&lt);

	LogEvent(EVENTLOG_INFORMATION_TYPE, 1, 1000, L"File '%s' changed at %02d-%02d-%02d %02d:%02d:%02d\n", w->filePath, lt.wDay, lt.wMonth, lt.wYear, lt.wHour, lt.wMinute, lt.wSecond);
			
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	wchar_t* cmdLine = (wchar_t*) malloc(sizeof(wchar_t) * (wcslen(CMD_LINE) + MAX_PATH));
	StringCchPrintfW(cmdLine, wcslen(CMD_LINE) + MAX_PATH, CMD_LINE, scriptPath, w->filePath);
			
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
	
	return;
}

void Cleanup() {
	for (int i = 0; i < watchesCount; i++) {	
		if (watches[i].filePath != NULL) {
			free(watches[i].filePath);
		}

		if (watches[i].currentHash != NULL) {
			HeapFree(GetProcessHeap(), 0, watches[i].currentHash);
		}

		if (watches[i].previousHash != NULL) {
			HeapFree(GetProcessHeap(), 0, watches[i].previousHash);
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

	if (algorithm != NULL)
	{
		BCryptCloseAlgorithmProvider(algorithm, 0);
	}

	if (hash != NULL)
	{
		BCryptDestroyHash(hash);
	}

	if (hashObject) {
		HeapFree(GetProcessHeap(), 0, hashObject);
	}
}