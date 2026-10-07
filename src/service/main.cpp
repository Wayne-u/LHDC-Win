#include "stream.hpp"
#include "worker.hpp"
#include "audio/windows_audio.hpp"
#include "host/json.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
namespace {
SERVICE_STATUS_HANDLE status_handle=nullptr;
SERVICE_STATUS status{SERVICE_WIN32_OWN_PROCESS,SERVICE_START_PENDING,0,NO_ERROR,0,0,5000};
HANDLE stop_event=nullptr;
DWORD run_logged(HANDLE stop,bool supervisor) {
    std::ofstream log(L"C:\\ProgramData\\LHDC-Win\\service.jsonl",std::ios::binary|std::ios::app);
    if(!log) return ERROR_OPEN_FAILED;
    const auto out=std::cout.rdbuf(log.rdbuf()),err=std::cerr.rdbuf(log.rdbuf());
    DWORD result=NO_ERROR;
    try {
        if(supervisor) lhdc::supervise_audio(stop);
        else lhdc::serve_audio(stop);
    } catch(const std::exception& error) {
        std::cerr<<"{\"event\":\"service_fatal_error\",\"message\":"<<lhdc::json_string(error.what())<<"}"<<std::endl;
        result=ERROR_SERVICE_SPECIFIC_ERROR;
    }
    std::cout.rdbuf(out);std::cerr.rdbuf(err);
    return result;
}
void report(DWORD state,DWORD error=NO_ERROR) {
    status.dwCurrentState=state; status.dwWin32ExitCode=error;
    status.dwControlsAccepted=(state==SERVICE_RUNNING)?SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN:0;
    status.dwWaitHint=(state==SERVICE_START_PENDING || state==SERVICE_STOP_PENDING)?5000:0;
    status.dwCheckPoint=(status.dwWaitHint)?status.dwCheckPoint+1:0;
    SetServiceStatus(status_handle,&status);
}
DWORD WINAPI control(DWORD code,DWORD,LPVOID,LPVOID) {
    if (code==SERVICE_CONTROL_STOP || code==SERVICE_CONTROL_SHUTDOWN) { report(SERVICE_STOP_PENDING); SetEvent(stop_event); }
    return NO_ERROR;
}
void WINAPI service_main(DWORD,LPWSTR*) {
    stop_event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    status_handle=RegisterServiceCtrlHandlerExW(L"LHDC-Win",control,nullptr);
    if (!status_handle) { if (stop_event) CloseHandle(stop_event); return; }
    if (!stop_event) { report(SERVICE_STOPPED,GetLastError()); return; }
    report(SERVICE_RUNNING);
    const auto result=run_logged(stop_event,true);
    report(SERVICE_STOPPED,result); CloseHandle(stop_event);
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc==3 && std::wstring(argv[1])==L"worker") {
        lhdc::AudioHandle stop(OpenEventW(SYNCHRONIZE|EVENT_MODIFY_STATE,FALSE,argv[2]));
        return static_cast<int>(run_logged(stop.get(),false));
    }
    if (argc==3 && std::wstring(argv[1])==L"console") {
        lhdc::AudioHandle stop(CreateEventW(nullptr,TRUE,FALSE,nullptr));
        lhdc::serve_audio(stop.get(),static_cast<unsigned>(std::stoul(argv[2]))); return 0;
    }
    SERVICE_TABLE_ENTRYW table[]={{const_cast<LPWSTR>(L"LHDC-Win"),service_main},{nullptr,nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) { std::cerr << "SCM dispatcher failed: " << GetLastError() << '\n'; return 1; }
    return 0;
}
