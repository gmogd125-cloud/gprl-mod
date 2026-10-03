#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace gprl::clip {

struct ProcessOptions {
    bool pipeStdin = false;
    bool pipeStdout = false;
    std::filesystem::path logFile;  // stderr (and stdout when not piped); empty = discarded
    bool lowPriority = false;
};

// A child process (ffmpeg) with optional pipes. Every child is in a job object that
// kills it when the game exits, and only the child's own pipe ends are inherited so
// closing stdin really reaches it.
class Process {
public:
    static std::unique_ptr<Process> start(std::filesystem::path const& exe, std::wstring const& args, ProcessOptions const& opts);
    ~Process();

    Process(Process const&) = delete;
    Process& operator=(Process const&) = delete;

    bool write(std::uint8_t const* data, std::size_t size);
    void closeStdin();
    // Reads up to size bytes; 0 means end of stream.
    std::size_t read(std::uint8_t* data, std::size_t size);
    bool readExact(std::uint8_t* data, std::size_t size);
    // Exit code once the process has ended within the timeout.
    std::optional<unsigned long> wait(unsigned long timeoutMs);
    void kill();

private:
    Process() = default;
    void* m_process = nullptr;
    void* m_stdinWrite = nullptr;
    void* m_stdoutRead = nullptr;
};

}  // namespace gprl::clip
