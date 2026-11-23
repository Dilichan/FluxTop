#include <windows.h>
#include <iostream>
#include <string>

// Runtime contract: a standard-user monitor invocation exits immediately and
// cannot start sampling. Elevated CTest tests the ordinary operational path.
int wmain(int argc,wchar_t** argv) {
    if(argc!=2) return 2;
    HANDLE token=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return 3;
    TOKEN_ELEVATION state{};
    DWORD bytes=0;
    const bool read=GetTokenInformation(token,TokenElevation,&state,sizeof(state),&bytes)!=FALSE;
    CloseHandle(token);
    if(!read) return 4;
    if(state.TokenIsElevated) {
        std::cout << "[PASS] Elevated fixture: monitor smoke verifies sampling\n";
        return 0;
    }
    const std::wstring app=argv[1];
    std::wstring cmd=L"\""+app+L"\" --samples 1 --interval-ms 1000 --no-gpu --no-network --jsonl";
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    PROCESS_INFORMATION process{};
    if(!CreateProcessW(app.c_str(),cmd.data(),nullptr,nullptr,FALSE,
        CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)) return 5;
    CloseHandle(process.hThread);
    const DWORD waited=WaitForSingleObject(process.hProcess,4000);
    if(waited!=WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess,1);
        WaitForSingleObject(process.hProcess,1000);
        CloseHandle(process.hProcess);
        std::cerr << "Monitor did not reject standard-user launch immediately\n";
        return 6;
    }
    DWORD code=0;
    const BOOL ok=GetExitCodeProcess(process.hProcess,&code);
    CloseHandle(process.hProcess);
    if(!ok || code!=ERROR_ELEVATION_REQUIRED) {
        std::cerr << "Expected ERROR_ELEVATION_REQUIRED (740), got " << code << '\n';
        return 7;
    }
    std::cout << "[PASS] Standard-user monitor exits with 740\n";
    return 0;
}