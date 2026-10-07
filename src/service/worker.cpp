#include "worker.hpp"
#include "audio/windows_audio.hpp"
#include "transport/windows.hpp"
#include <sddl.h>
#include <array>
#include <filesystem>
#include <iostream>
#include <syncstream>
namespace lhdc {
namespace {
void check(BOOL success,const char* operation) {
    if(!success) throw WindowsError(operation,GetLastError());
}
void enable_session_privilege() {
    HANDLE raw=nullptr;check(OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&raw),"Worker privilege token");
    AudioHandle token(raw);
    TOKEN_PRIVILEGES privilege{};privilege.PrivilegeCount=1;
    check(LookupPrivilegeValueW(nullptr,SE_TCB_NAME,&privilege.Privileges[0].Luid),"Worker session privilege");
    privilege.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
    check(AdjustTokenPrivileges(token.get(),FALSE,&privilege,sizeof(privilege),nullptr,nullptr),"Worker session privilege enable");
    if(GetLastError()==ERROR_NOT_ALL_ASSIGNED) throw WindowsError("Worker session privilege unavailable",ERROR_PRIVILEGE_NOT_HELD);
}
class StopEvent {
public:
    StopEvent() {
        GUID id{};audio_check(CoCreateGuid(&id),"Worker event identity");
        std::array<wchar_t,40> guid{};
        if(!StringFromGUID2(id,guid.data(),static_cast<int>(guid.size()))) throw std::runtime_error("Worker event identity conversion failed");
        name=L"Global\\LHDC-Win-Stop-"+std::wstring(guid.data());
        PSECURITY_DESCRIPTOR descriptor=nullptr;
        check(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)",SDDL_REVISION_1,&descriptor,nullptr),"Worker stop event security");
        SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
        handle=CreateEventW(&attributes,TRUE,FALSE,name.c_str());
        const auto error=GetLastError();LocalFree(descriptor);
        if(!handle) throw WindowsError("Worker stop event",error);
        if(error==ERROR_ALREADY_EXISTS) { CloseHandle(handle);handle=nullptr;throw std::runtime_error("Worker stop event already exists"); }
    }
    ~StopEvent() { CloseHandle(handle); }
    HANDLE handle=nullptr;
    std::wstring name;
};
class Worker {
public:
    Worker(DWORD session,const std::wstring& event):job_(CreateJobObjectW(nullptr,nullptr)) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        check(SetInformationJobObject(job_.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"Worker job lifetime");
        HANDLE raw=nullptr;check(OpenProcessToken(GetCurrentProcess(),TOKEN_DUPLICATE|TOKEN_QUERY,&raw),"Worker source token");
        AudioHandle source(raw);
        check(DuplicateTokenEx(source.get(),TOKEN_ALL_ACCESS,nullptr,SecurityImpersonation,TokenPrimary,&raw),"Worker primary token");
        AudioHandle token(raw);
        check(SetTokenInformation(token.get(),TokenSessionId,&session,sizeof(session)),"Worker desktop session");
        std::array<wchar_t,32768> path{};
        const auto length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
        if(!length || length==path.size()) throw WindowsError("Worker executable path",GetLastError());
        const std::filesystem::path executable(path.data());
        auto command=L"\""+executable.wstring()+L"\" worker \""+event+L"\"";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);
        startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
        check(CreateProcessAsUserW(token.get(),executable.c_str(),command.data(),nullptr,nullptr,FALSE,
            CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,executable.parent_path().c_str(),&startup,&process_),"Audio worker launch");
        if(!AssignProcessToJobObject(job_.get(),process_.hProcess) || ResumeThread(process_.hThread)==static_cast<DWORD>(-1)) {
            const auto error=GetLastError();TerminateProcess(process_.hProcess,error);
            CloseHandle(process_.hThread);CloseHandle(process_.hProcess);process_={};
            throw WindowsError("Audio worker job assignment/start",error);
        }
        CloseHandle(process_.hThread);process_.hThread=nullptr;
        std::osyncstream(std::cout)<<"{\"event\":\"audio_worker_started\",\"session\":"<<session<<",\"pid\":"<<process_.dwProcessId<<"}"<<std::endl;
    }
    ~Worker() { if(process_.hProcess) CloseHandle(process_.hProcess); }
    HANDLE process() const { return process_.hProcess; }
    void stop(HANDLE event) {
        check(SetEvent(event),"Audio worker stop signal");
        const auto wait=WaitForSingleObject(process_.hProcess,20000);
        if(wait==WAIT_TIMEOUT) {
            check(TerminateJobObject(job_.get(),ERROR_TIMEOUT),"Audio worker stop timeout");
            std::osyncstream(std::cerr)<<"{\"event\":\"audio_worker_stop_timeout\"}"<<std::endl;
        } else if(wait!=WAIT_OBJECT_0) throw WindowsError("Audio worker stop wait",GetLastError());
    }
private:
    AudioHandle job_;
    PROCESS_INFORMATION process_{};
};
}
void supervise_audio(HANDLE stop) {
    enable_session_privilege();
    while(WaitForSingleObject(stop,0)==WAIT_TIMEOUT) {
        const auto session=WTSGetActiveConsoleSessionId();
        if(session==0xFFFFFFFF) { if(WaitForSingleObject(stop,500)==WAIT_OBJECT_0) break;continue; }
        StopEvent event;
        Worker worker(session,event.name);
        const HANDLE handles[]={stop,worker.process()};
        for(;;) {
            const auto result=WaitForMultipleObjects(2,handles,FALSE,500);
            if(result==WAIT_OBJECT_0) { worker.stop(event.handle);return; }
            if(result==WAIT_OBJECT_0+1) {
                DWORD code=0;check(GetExitCodeProcess(worker.process(),&code),"Audio worker result");
                throw std::runtime_error("Audio worker exited unexpectedly, code="+std::to_string(code));
            }
            if(result!=WAIT_TIMEOUT) throw WindowsError("Audio worker supervision wait",GetLastError());
            if(WTSGetActiveConsoleSessionId()!=session) { worker.stop(event.handle);break; }
        }
    }
}
}
