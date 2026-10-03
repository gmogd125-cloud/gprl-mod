#include "Process.hpp"
#include "ClipUtil.hpp"

#include <Geode/Geode.hpp>

#include <Windows.h>

#include <algorithm>
#include <mutex>
#include <vector>

using namespace geode::prelude;

namespace gprl::clip {

namespace {

HANDLE jobObject() {
    static HANDLE job = [] {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        if (h) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
        return h;
    }();
    return job;
}

void closeHandle(void*& h) {
    if (h && h != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(h));
    h = nullptr;
}

}  // namespace

std::unique_ptr<Process> Process::start(std::filesystem::path const& exe, std::wstring const& args, ProcessOptions const& opts) {
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    auto cleanup = [&] {
        for (HANDLE* h : {&inRead, &inWrite, &outRead, &outWrite}) {
            if (*h) CloseHandle(*h);
            *h = nullptr;
        }
    };

    if (opts.pipeStdin) {
        if (!CreatePipe(&inRead, &inWrite, &sa, 8 * 1024 * 1024)) return nullptr;
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    }
    if (opts.pipeStdout) {
        if (!CreatePipe(&outRead, &outWrite, &sa, 4 * 1024 * 1024)) {
            cleanup();
            return nullptr;
        }
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    }

    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE logFile = INVALID_HANDLE_VALUE;
    if (!opts.logFile.empty()) {
        logFile = CreateFileW(opts.logFile.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    HANDLE errTarget = logFile != INVALID_HANDLE_VALUE ? logFile : nul;

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inRead ? inRead : nul;
    si.StartupInfo.hStdOutput = outWrite ? outWrite : errTarget;
    si.StartupInfo.hStdError = errTarget;

    std::vector<HANDLE> inherit;
    for (HANDLE h : {si.StartupInfo.hStdInput, si.StartupInfo.hStdOutput, si.StartupInfo.hStdError}) {
        if (h && h != INVALID_HANDLE_VALUE && std::find(inherit.begin(), inherit.end(), h) == inherit.end()) inherit.push_back(h);
    }

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<std::uint8_t> attrBuf(attrSize);
    auto attrs = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(static_cast<void*>(attrBuf.data()));
    bool haveAttrs = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                     UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                               inherit.size() * sizeof(HANDLE), nullptr, nullptr);
    si.lpAttributeList = haveAttrs ? attrs : nullptr;

    std::wstring cmd = quoted(exe) + L" " + args;
    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | (haveAttrs ? EXTENDED_STARTUPINFO_PRESENT : 0) |
                  (opts.lowPriority ? BELOW_NORMAL_PRIORITY_CLASS : 0);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.wstring().c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr,
                             &si.StartupInfo, &pi);
    DWORD err = ok ? 0 : GetLastError();
    if (haveAttrs) DeleteProcThreadAttributeList(attrs);

    // The child owns its ends now.
    if (inRead) CloseHandle(inRead);
    if (outWrite) CloseHandle(outWrite);
    inRead = outWrite = nullptr;
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (logFile != INVALID_HANDLE_VALUE) CloseHandle(logFile);

    if (!ok) {
        log::warn("GPRL clip: could not start ffmpeg (error {})", err);
        cleanup();
        return nullptr;
    }
    if (auto job = jobObject()) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    auto proc = std::unique_ptr<Process>(new Process());
    proc->m_process = pi.hProcess;
    proc->m_stdinWrite = inWrite;
    proc->m_stdoutRead = outRead;
    return proc;
}

Process::~Process() {
    closeHandle(m_stdinWrite);
    if (m_process && WaitForSingleObject(static_cast<HANDLE>(m_process), 0) == WAIT_TIMEOUT) {
        TerminateProcess(static_cast<HANDLE>(m_process), 1);
    }
    closeHandle(m_stdoutRead);
    closeHandle(m_process);
}

bool Process::write(std::uint8_t const* data, std::size_t size) {
    if (!m_stdinWrite) return false;
    while (size > 0) {
        DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 16 * 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(m_stdinWrite), data, chunk, &written, nullptr) || written == 0) return false;
        data += written;
        size -= written;
    }
    return true;
}

void Process::closeStdin() {
    closeHandle(m_stdinWrite);
}

std::size_t Process::read(std::uint8_t* data, std::size_t size) {
    if (!m_stdoutRead) return 0;
    DWORD got = 0;
    if (!ReadFile(static_cast<HANDLE>(m_stdoutRead), data, static_cast<DWORD>(std::min<std::size_t>(size, 64 * 1024 * 1024)), &got, nullptr)) {
        return 0;
    }
    return got;
}

bool Process::readExact(std::uint8_t* data, std::size_t size) {
    while (size > 0) {
        auto got = read(data, size);
        if (got == 0) return false;
        data += got;
        size -= got;
    }
    return true;
}

std::optional<unsigned long> Process::wait(unsigned long timeoutMs) {
    if (!m_process) return 1ul;
    if (WaitForSingleObject(static_cast<HANDLE>(m_process), timeoutMs) != WAIT_OBJECT_0) return std::nullopt;
    DWORD code = 1;
    GetExitCodeProcess(static_cast<HANDLE>(m_process), &code);
    return static_cast<unsigned long>(code);
}

void Process::kill() {
    if (m_process) TerminateProcess(static_cast<HANDLE>(m_process), 1);
}

}  // namespace gprl::clip
