#include "stdafx.h"
#include <Windows.h>
#include <memory>
#include <string>
#include <process.h>
#include <codecvt>
#include <tuple>
#include <io.h>
#include <fcntl.h>

#define CONTROL_MAGIC 0xDEADBEEF
#define CONTROL_WINDOW_SIZE 1
#define CONTROL_KILL 2

HPCON hPC = INVALID_HANDLE_VALUE;
HANDLE pipeControl;
PROCESS_INFORMATION childProcess;


HRESULT CreatePseudoConsoleAndPipes(HPCON* phPC, HANDLE* phPipeIn, HANDLE* phPipeOut)
{
	HRESULT hr = E_UNEXPECTED;
	HANDLE hPipePTYIn = INVALID_HANDLE_VALUE;
	HANDLE hPipePTYOut = INVALID_HANDLE_VALUE;

	// Create the pipes to which the ConPTY will connect
	if (CreatePipe(&hPipePTYIn, phPipeOut, NULL, 0) &&
		CreatePipe(phPipeIn, &hPipePTYOut, NULL, 0))
	{
		// Determine required size of Pseudo Console
		COORD consoleSize = { 0 };
		CONSOLE_SCREEN_BUFFER_INFO csbi;
		HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
		if (GetConsoleScreenBufferInfo(hConsole, &csbi))
		{
			consoleSize.X = csbi.srWindow.Right - csbi.srWindow.Left + 1;
			consoleSize.Y = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
		}

		hr = CreatePseudoConsole(consoleSize, hPipePTYIn, hPipePTYOut, 0, phPC);

		if (INVALID_HANDLE_VALUE != hPipePTYOut) CloseHandle(hPipePTYOut);
		if (INVALID_HANDLE_VALUE != hPipePTYIn) CloseHandle(hPipePTYIn);
	}

	return hr;
}

HRESULT InitializeStartupInfoAttachedToPseudoConsole(STARTUPINFOEX* pStartupInfo, HPCON hPC)
{
	HRESULT hr = E_UNEXPECTED;

	if (pStartupInfo)
	{
		SIZE_T attrListSize;

		pStartupInfo->StartupInfo.cb = sizeof(STARTUPINFOEX);

		// Get the size of the thread attribute list.
		InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);

		// Allocate a thread attribute list of the correct size
		pStartupInfo->lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(malloc(attrListSize));

		// Initialize thread attribute list
		if (pStartupInfo->lpAttributeList
			&& InitializeProcThreadAttributeList(pStartupInfo->lpAttributeList, 1, 0, &attrListSize))
		{
			// Set Pseudo Console attribute
			hr = UpdateProcThreadAttribute(
				pStartupInfo->lpAttributeList,
				0,
				PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
				hPC,
				sizeof(HPCON),
				NULL,
				NULL)
				? S_OK
				: HRESULT_FROM_WIN32(GetLastError());
		}
		else
		{
			hr = HRESULT_FROM_WIN32(GetLastError());
		}
	}
	return hr;
}


#define TC_NONE 0
#define TC_UTF16_TO_UTF8 1
#define TC_UTF8_TO_UTF16 2


void __cdecl PipeWriter(LPVOID info)
{
	auto pipes = (std::tuple<HANDLE, HANDLE, int>*)info;
	auto fIn = std::get<0>(*pipes);
	auto fOut = std::get<1>(*pipes);
	auto tcMode = std::get<2>(*pipes);
	tcMode = TC_NONE;
	const DWORD BUFSIZE = 512;
	char szBuffer[BUFSIZE];

	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t> conversion;

	auto readSucceeded = FALSE;
	DWORD bytesRead, totalBytesRead;
	do
	{
		totalBytesRead = 0;

		do {

			readSucceeded = ReadFile(fIn, szBuffer + totalBytesRead, BUFSIZE - totalBytesRead, &bytesRead, NULL);
			totalBytesRead += bytesRead;
			if (!readSucceeded || bytesRead <= 0) {
				break;
			}
			if (szBuffer[totalBytesRead - 1] != '\x1b') {
				break;
			}
		} while (true);

		if (tcMode == TC_NONE) {
			WriteFile(fOut, szBuffer, totalBytesRead, NULL, NULL);
		}
		else if (tcMode == TC_UTF16_TO_UTF8) {
			std::string utf8String = conversion.to_bytes(std::wstring((const wchar_t*)szBuffer, totalBytesRead / 2));
			WriteFile(fOut, utf8String.c_str(), utf8String.length(), NULL, NULL);
		}
		else if (tcMode == TC_UTF8_TO_UTF16) {
			std::wstring utf16String = conversion.from_bytes(szBuffer, szBuffer + totalBytesRead);
			//for (int i = 0; i < utf16String.length(); i++) {
				//utf16String[i] = _byteswap_ushort(utf16String[i]);
			//}
			//utf16String = L"тест";
			WriteFile(fOut, utf16String.c_str(), utf16String.length() * 2, NULL, NULL);
		}
	} while (readSucceeded && bytesRead >= 0);
}


void __cdecl InputListener(LPVOID info) {
	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t> conversion;
	auto pipes = (std::pair<HANDLE, HANDLE>*)info;
	HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
	constexpr int bufferSize = 128;
	INPUT_RECORD records[bufferSize];
	DWORD numRead;
	while (true)
	{
		if (!ReadConsoleInput(input, records, bufferSize, &numRead)) {
			return;
		}
		for (int i = 0; i < numRead; i++) {
			if (records[i].EventType == KEY_EVENT && records[i].Event.KeyEvent.bKeyDown) {
				wchar_t ch[2] = { records[i].Event.KeyEvent.uChar.UnicodeChar, 0 };
				if (ch[0] < 128) {
					WriteFile(pipes->second, &records[i].Event.KeyEvent.uChar.AsciiChar, sizeof(char), NULL, NULL);
				}
				else {
					std::string utf8String = conversion.to_bytes(std::wstring(ch));
					WriteFile(pipes->second, utf8String.c_str(), utf8String.length(), NULL, NULL);
				}
			}
			if (records[i].EventType == WINDOW_BUFFER_SIZE_EVENT) {
				auto size = (WINDOW_BUFFER_SIZE_RECORD)records[i].Event.WindowBufferSizeEvent;
				auto width = size.dwSize.X;
				auto height = size.dwSize.Y;

				uint32_t buffer[] = { CONTROL_MAGIC, CONTROL_WINDOW_SIZE, width, height };
				WriteFile(pipeControl, buffer, sizeof(buffer), NULL, NULL);
			}
		}
	}
}


void __cdecl ControlListener(LPVOID info)
{
	const DWORD BUFSIZE = 128;
	uint32_t buffer[BUFSIZE / sizeof(uint32_t)];
	
	SHORT width = 0, height = 0;

	DWORD bytesRead;
	auto readSucceeded = FALSE;
	do
	{
		readSucceeded = ReadFile(pipeControl, buffer, BUFSIZE, &bytesRead, NULL);
		if (buffer[0] == CONTROL_MAGIC) {
			if (buffer[1] == CONTROL_WINDOW_SIZE) {
				auto newWidth = (SHORT)buffer[2];
				auto newHeight = (SHORT)buffer[3];
				printf("Control: resize %ix%i\n", newWidth, newHeight);
				if (newWidth != width || newHeight != height) {
					width = newWidth;
					height = newHeight;
					ResizePseudoConsole(hPC, { width, height });
				}
			}
			if (buffer[1] == CONTROL_KILL) {
				printf("Control: kill\n");
				TerminateProcess(childProcess.hProcess, 1);
				ExitProcess(1);
			}
		}
	} while (readSucceeded && bytesRead >= 0);
}


BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
	uint32_t buffer[] = { CONTROL_MAGIC, CONTROL_KILL };
	WriteFile(pipeControl, buffer, sizeof(buffer), NULL, NULL);
	return FALSE;
}


int wmain(int argc, wchar_t* argv[])
{
	if (argc > 1 && lstrcmp(argv[1], L"--pipe") != 0) {
		wchar_t pipeName[256];
		wsprintf(pipeName, L"\\\\.\\pipe\\uac-pty-%i", rand());
		HANDLE pipeIn = CreateNamedPipe((std::wstring(pipeName) + L"-in").c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE, PIPE_UNLIMITED_INSTANCES, 256, 256, 0, NULL);
		HANDLE pipeOut = CreateNamedPipe((std::wstring(pipeName) + L"-out").c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE, PIPE_UNLIMITED_INSTANCES, 256, 256, 0, NULL);
		pipeControl = CreateNamedPipe((std::wstring(pipeName) + L"-control").c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE, PIPE_UNLIMITED_INSTANCES, 256, 256, 0, NULL);

		std::wstring cmd = L"--pipe " + std::wstring(pipeName) + L" ";
		for (int i = 1; i < argc; i++) {
			cmd += L"\"";
			cmd += argv[i];
			cmd += L"\" ";
		}

		HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
		DWORD consoleMode;
		GetConsoleMode(hConsole, &consoleMode);
		SetConsoleMode(hConsole, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

		hConsole = GetStdHandle(STD_INPUT_HANDLE);
		GetConsoleMode(hConsole, &consoleMode);
		SetConsoleMode(hConsole, ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);

		SHELLEXECUTEINFO ShExecInfo = { 0 };
		ShExecInfo.cbSize = sizeof(SHELLEXECUTEINFO);
		ShExecInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
		ShExecInfo.hwnd = NULL;
		ShExecInfo.lpVerb = L"runas";
		//ShExecInfo.lpVerb = NULL;
		ShExecInfo.lpFile = argv[0];
		ShExecInfo.lpParameters = cmd.c_str();
		ShExecInfo.lpDirectory = NULL;
		ShExecInfo.nShow = SW_HIDE;
		ShExecInfo.hInstApp = NULL;
		ShellExecuteEx(&ShExecInfo);

		SetConsoleCtrlHandler(CtrlHandler, TRUE);
		//_setmode(_fileno(stdout), _O_U16TEXT);
		SetConsoleOutputCP(CP_UTF8);
		Sleep(1000);
		_beginthread(PipeWriter, 0, new std::tuple<HANDLE, HANDLE, int>{ pipeOut, GetStdHandle(STD_OUTPUT_HANDLE), TC_UTF8_TO_UTF16 });
		_beginthread(InputListener, 0, new std::pair<HANDLE, HANDLE>{ GetStdHandle(STD_INPUT_HANDLE), pipeIn });


		WaitForSingleObject(ShExecInfo.hProcess, INFINITE);
		CloseHandle(ShExecInfo.hProcess);
		return 0;
	}

	HANDLE pipeIn = CreateFile((std::wstring(argv[2]) + L"-in").c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
	HANDLE pipeOut = CreateFile((std::wstring(argv[2]) + L"-out").c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	pipeControl = CreateFile((std::wstring(argv[2]) + L"-control").c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
	std::wstring cmd = L"";
	for (int i = 3; i < argc; i++) {
		cmd += L"\"";
		cmd += argv[i];
		cmd += L"\" ";
	}

	//  Create the Pseudo Console and pipes to it
	HANDLE hPipeIn{ INVALID_HANDLE_VALUE };
	HANDLE hPipeOut{ INVALID_HANDLE_VALUE };
	auto hr = CreatePseudoConsoleAndPipes(&hPC, &hPipeIn, &hPipeOut);
	if (S_OK == hr)
	{
		// Create & start thread to listen to the incoming pipe
		// Note: Using CRT-safe _beginthread() rather than CreateThread()
		_beginthread(PipeWriter, 0, new std::tuple<HANDLE, HANDLE, int>{ hPipeIn, pipeOut, TC_NONE });
		_beginthread(PipeWriter, 0, new std::tuple<HANDLE, HANDLE, int>{ pipeIn, hPipeOut, TC_NONE });
		_beginthread(ControlListener, 0, new std::pair<HANDLE, HANDLE>{ pipeControl, hPipeOut });

		// Initialize the necessary startup info struct        
		STARTUPINFOEX startupInfo{};
		if (S_OK == InitializeStartupInfoAttachedToPseudoConsole(&startupInfo, hPC))
		{
			// Launch ping to emit some text back via the pipe
			hr = CreateProcessW(
				NULL,                           // No module name - use Command Line
				(LPWSTR)cmd.c_str(),                      // Command Line
				NULL,
				NULL,
				FALSE,
				EXTENDED_STARTUPINFO_PRESENT,   // Creation flags
				NULL,                           // Use parent's environment block
				NULL,                           // Use parent's starting directory 
				&startupInfo.StartupInfo,       // Pointer to STARTUPINFO
				&childProcess)                      // Pointer to PROCESS_INFORMATION
				? S_OK
				: GetLastError();

			if (S_OK == hr)
			{
				Sleep(500);
				// Wait up to 10s for ping process to complete
				WaitForSingleObject(childProcess.hProcess, INFINITE);

				// Allow listening thread to catch-up with final output!
				Sleep(500);
			}

			// --- CLOSEDOWN ---

			// Now safe to clean-up client app's process-info & thread
			CloseHandle(childProcess.hThread);
			CloseHandle(childProcess.hProcess);

			// Cleanup attribute list
			DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
			free(startupInfo.lpAttributeList);
		}

		// Close ConPTY - this will terminate client process if running
		ClosePseudoConsole(hPC);

		// Clean-up the pipes
		if (INVALID_HANDLE_VALUE != hPipeOut) CloseHandle(hPipeOut);
		if (INVALID_HANDLE_VALUE != hPipeIn) CloseHandle(hPipeIn);
		CloseHandle(pipeIn);
		CloseHandle(pipeOut);
	}

	return S_OK == hr ? EXIT_SUCCESS : EXIT_FAILURE;
}
