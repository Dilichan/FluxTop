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
    if(!read){CloseHandle(token);return 4;}
    HANDLE limited=nullptr;
    if(state.TokenIsElevated) {
        // Linked tokens obtained without SeTcb can be identification-only.
        // Duplicate the current user's unelevated desktop token instead.
        DWORD shell_pid{};GetWindowThreadProcessId(GetShellWindow(),&shell_pid);
        HANDLE shell=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,shell_pid),shell_token{};
        if(!shell || !OpenProcessToken(shell,TOKEN_QUERY|TOKEN_DUPLICATE,&shell_token)){
            if(shell)CloseHandle(shell);CloseHandle(token);std::cerr<<"Unelevated desktop token unavailable\n";return 77;
        }
        BYTE own_user[1024]{},desktop_user[1024]{};TOKEN_ELEVATION desktop_state{};
        const bool same=GetTokenInformation(token,TokenUser,own_user,sizeof(own_user),&bytes) &&
            GetTokenInformation(shell_token,TokenUser,desktop_user,sizeof(desktop_user),&bytes) &&
            EqualSid(reinterpret_cast<TOKEN_USER*>(own_user)->User.Sid,reinterpret_cast<TOKEN_USER*>(desktop_user)->User.Sid) &&
            GetTokenInformation(shell_token,TokenElevation,&desktop_state,sizeof(desktop_state),&bytes) && !desktop_state.TokenIsElevated;
        const bool duplicated=same && DuplicateTokenEx(shell_token,TOKEN_ALL_ACCESS,nullptr,SecurityImpersonation,TokenPrimary,&limited);
        CloseHandle(shell_token);CloseHandle(shell);
        if(!duplicated){CloseHandle(token);std::cerr<<"Current user's standard token unavailable\n";return 77;}
    }
    CloseHandle(token);
    const std::wstring app=argv[1];
    std::wstring cmd=L"\""+app+L"\" --samples 1 --interval-ms 1000 --no-gpu --no-network --jsonl";
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL started=limited?CreateProcessWithTokenW(limited,0,app.c_str(),cmd.data(),CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process):
        CreateProcessW(app.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process);
    const auto start_error=started?ERROR_SUCCESS:GetLastError();
    if(limited)CloseHandle(limited);
    if(!started){std::cerr<<"Cannot start standard-user fixture: "<<start_error<<'\n';return 5;}
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
