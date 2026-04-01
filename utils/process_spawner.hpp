#ifndef VELIX_PROCESS_SPAWNER_HPP
#define VELIX_PROCESS_SPAWNER_HPP

#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <array>
#include <stdexcept>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace velix::utils {

struct ProcessResult {
    int exit_code = -1;
    std::string stdout_content;
    std::string stderr_content;
    bool success = false;
    bool timed_out = false;
};

class ProcessSpawner {
public:
    /**
     * Executes a command synchronously and captures its output.
     * @param command The binary to run.
     * @param args The arguments to pass.
     * @param env Optional environment variables to inject.
     * @return ProcessResult containing exit code and captured output.
     */
    static ProcessResult run_sync(const std::string& command, 
                                 const std::vector<std::string>& args,
                                 const std::map<std::string, std::string>& env = {}) {
        ProcessResult result;
        
#ifdef _WIN32
        // Windows implementation using CreateProcess and Pipes
        SECURITY_ATTRIBUTES saAttr;
        saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
        saAttr.bInheritHandle = TRUE;
        saAttr.lpSecurityDescriptor = NULL;

        HANDLE hChildStd_OUT_Rd = NULL;
        HANDLE hChildStd_OUT_Wr = NULL;
        if (!CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &saAttr, 0)) {
            throw std::runtime_error("Stdout CreatePipe failed");
        }
        if (!SetHandleInformation(hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0)) {
            throw std::runtime_error("Stdout SetHandleInformation failed");
        }

        std::string cmd_line = "\"" + command + "\"";
        for (const auto& arg : args) {
            cmd_line += " \"" + arg + "\"";
        }

        // Build Environment Block for Windows: null-terminated strings ending with extra null
        std::vector<char> env_block;
        if (!env.empty()) {
            for (auto const& [key, val] : env) {
                std::string entry = key + "=" + val;
                env_block.insert(env_block.end(), entry.begin(), entry.end());
                env_block.push_back('\0');
            }
            env_block.push_back('\0');
        }

        PROCESS_INFORMATION piProcInfo;
        STARTUPINFOA siStartInfo;
        ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));
        ZeroMemory(&siStartInfo, sizeof(STARTUPINFOA));
        siStartInfo.cb = sizeof(STARTUPINFOA);
        siStartInfo.hStdError = hChildStd_OUT_Wr;
        siStartInfo.hStdOutput = hChildStd_OUT_Wr;
        siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

        std::vector<char> cmd_buf(cmd_line.begin(), cmd_line.end());
        cmd_buf.push_back('\0');

        bool bSuccess = CreateProcessA(NULL, cmd_buf.data(), NULL, NULL, TRUE, 0, 
                                      env.empty() ? NULL : env_block.data(), 
                                      NULL, &siStartInfo, &piProcInfo);
        
        if (!bSuccess) {
            CloseHandle(hChildStd_OUT_Wr);
            CloseHandle(hChildStd_OUT_Rd);
            throw std::runtime_error("CreateProcess failed with error: " + std::to_string(GetLastError()));
        }

        CloseHandle(hChildStd_OUT_Wr); // Close write end in parent

        char lpBuffer[4096];
        DWORD nBytesRead;
        std::ostringstream oss;
        while (ReadFile(hChildStd_OUT_Rd, lpBuffer, sizeof(lpBuffer) - 1, &nBytesRead, NULL) && nBytesRead > 0) {
            lpBuffer[nBytesRead] = '\0';
            oss << lpBuffer;
        }

        WaitForSingleObject(piProcInfo.hProcess, INFINITE);
        DWORD exitCode;
        GetExitCodeProcess(piProcInfo.hProcess, &exitCode);
        
        result.exit_code = static_cast<int>(exitCode);
        result.stdout_content = oss.str();
        result.success = (result.exit_code == 0);

        CloseHandle(piProcInfo.hProcess);
        CloseHandle(piProcInfo.hThread);
        CloseHandle(hChildStd_OUT_Rd);

#else
        // POSIX implementation using pipe, fork, and execve/execvp
        int pipefd[2];
        if (pipe(pipefd) == -1) {
            throw std::runtime_error("pipe() failed");
        }

        pid_t pid = fork();
        if (pid == -1) {
            throw std::runtime_error("fork() failed");
        }

        if (pid == 0) {
            // Child process
            close(pipefd[0]);    // close read end
            dup2(pipefd[1], STDOUT_FILENO); // redirect stdout to pipe
            dup2(pipefd[1], STDERR_FILENO); // redirect stderr to pipe
            close(pipefd[1]);

            // Inject custom environment variables using setenv
            for (auto const& [key, val] : env) {
                setenv(key.c_str(), val.c_str(), 1);
            }

            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(command.c_str()));
            for (const auto& arg : args) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);

            execvp(command.c_str(), argv.data());
            exit(127); // exec failed
        } else {
            // Parent process
            close(pipefd[1]); // close write end

            char buffer[4096];
            std::ostringstream oss;
            ssize_t bytes_read;
            while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
                buffer[bytes_read] = '\0';
                oss << buffer;
            }
            close(pipefd[0]);

            int status;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status)) {
                result.exit_code = WEXITSTATUS(status);
                result.success = (result.exit_code == 0);
            }
            result.stdout_content = oss.str();
        }
#endif
        return result;
    }

    /**
     * Executes a command synchronously with timeout.
     * timeout_ms <= 0 means no timeout.
     */
    static ProcessResult run_sync_with_timeout(const std::string& command,
                                               const std::vector<std::string>& args,
                                               const std::map<std::string, std::string>& env,
                                               int timeout_ms,
                                               const std::string& working_dir = "") {
        if (timeout_ms <= 0) {
            timeout_ms = 7 * 24 * 60 * 60 * 1000;
        }

        ProcessResult result;

#ifdef _WIN32
        SECURITY_ATTRIBUTES saAttr;
        saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
        saAttr.bInheritHandle = TRUE;
        saAttr.lpSecurityDescriptor = NULL;

        HANDLE hChildStd_OUT_Rd = NULL;
        HANDLE hChildStd_OUT_Wr = NULL;
        if (!CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &saAttr, 0)) {
            throw std::runtime_error("Stdout CreatePipe failed");
        }
        if (!SetHandleInformation(hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0)) {
            throw std::runtime_error("Stdout SetHandleInformation failed");
        }

        std::string cmd_line = "\"" + command + "\"";
        for (const auto& arg : args) {
            cmd_line += " \"" + arg + "\"";
        }

        std::vector<char> env_block;
        if (!env.empty()) {
            for (auto const& [key, val] : env) {
                std::string entry = key + "=" + val;
                env_block.insert(env_block.end(), entry.begin(), entry.end());
                env_block.push_back('\0');
            }
            env_block.push_back('\0');
        }

        PROCESS_INFORMATION piProcInfo;
        STARTUPINFOA siStartInfo;
        ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));
        ZeroMemory(&siStartInfo, sizeof(STARTUPINFOA));
        siStartInfo.cb = sizeof(STARTUPINFOA);
        siStartInfo.hStdError = hChildStd_OUT_Wr;
        siStartInfo.hStdOutput = hChildStd_OUT_Wr;
        siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

        std::vector<char> cmd_buf(cmd_line.begin(), cmd_line.end());
        cmd_buf.push_back('\0');

        bool ok = CreateProcessA(NULL, cmd_buf.data(), NULL, NULL, TRUE, 0,
                     env.empty() ? NULL : env_block.data(),
                     working_dir.empty() ? NULL : working_dir.c_str(),
                                 &siStartInfo, &piProcInfo);
        if (!ok) {
            CloseHandle(hChildStd_OUT_Wr);
            CloseHandle(hChildStd_OUT_Rd);
            throw std::runtime_error("CreateProcess failed with error: " + std::to_string(GetLastError()));
        }

        CloseHandle(hChildStd_OUT_Wr);

        const DWORD wait_status = WaitForSingleObject(piProcInfo.hProcess, static_cast<DWORD>(timeout_ms));
        if (wait_status == WAIT_TIMEOUT) {
            TerminateProcess(piProcInfo.hProcess, 124);
            result.exit_code = 124;
            result.success = false;
            result.timed_out = true;
        } else {
            DWORD exitCode;
            GetExitCodeProcess(piProcInfo.hProcess, &exitCode);
            result.exit_code = static_cast<int>(exitCode);
            result.success = (result.exit_code == 0);
        }

        char lpBuffer[4096];
        DWORD nBytesRead;
        std::ostringstream oss;
        while (ReadFile(hChildStd_OUT_Rd, lpBuffer, sizeof(lpBuffer) - 1, &nBytesRead, NULL) && nBytesRead > 0) {
            lpBuffer[nBytesRead] = '\0';
            oss << lpBuffer;
        }
        result.stdout_content = oss.str();

        CloseHandle(piProcInfo.hProcess);
        CloseHandle(piProcInfo.hThread);
        CloseHandle(hChildStd_OUT_Rd);

#else
        int pipefd[2];
        if (pipe(pipefd) == -1) {
            throw std::runtime_error("pipe() failed");
        }

        pid_t pid = fork();
        if (pid == -1) {
            throw std::runtime_error("fork() failed");
        }

        if (pid == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);

            if (!working_dir.empty()) {
                if (chdir(working_dir.c_str()) != 0) {
                    exit(127);
                }
            }

            for (auto const& [key, val] : env) {
                setenv(key.c_str(), val.c_str(), 1);
            }

            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(command.c_str()));
            for (const auto& arg : args) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);

            execvp(command.c_str(), argv.data());
            exit(127);
        }

        close(pipefd[1]);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        int status = 0;
        bool done = false;
        while (std::chrono::steady_clock::now() < deadline) {
            const pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                done = true;
                break;
            }
            usleep(20000);
        }

        if (!done) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            result.exit_code = 124;
            result.success = false;
            result.timed_out = true;
        } else if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
            result.success = (result.exit_code == 0);
        } else {
            result.exit_code = -1;
            result.success = false;
        }

        char buffer[4096];
        std::ostringstream oss;
        ssize_t bytes_read;
        while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';
            oss << buffer;
        }
        close(pipefd[0]);
        result.stdout_content = oss.str();
#endif

        return result;
    }

    /**
     * Spawns a process asynchronously.
     * @param command The binary to run.
     * @param args The arguments to pass.
     * @param env Optional environment variables to inject.
     * @param working_dir Optional directory to run in.
     * @return A pair: {OS PID, Error Message}. PID is -1 on failure.
     */
    static std::pair<int, std::string> spawn(const std::string& command, 
                                             const std::vector<std::string>& args,
                                             const std::map<std::string, std::string>& env = {},
                                             const std::string& working_dir = "") {
#ifdef _WIN32
        std::string cmd_line = "\"" + command + "\"";
        for (const auto& arg : args) {
            cmd_line += " \"" + arg + "\"";
        }

        std::vector<char> env_block;
        if (!env.empty()) {
            for (auto const& [key, val] : env) {
                std::string entry = key + "=" + val;
                env_block.insert(env_block.end(), entry.begin(), entry.end());
                env_block.push_back('\0');
            }
            env_block.push_back('\0');
        }

        PROCESS_INFORMATION piProcInfo;
        STARTUPINFOA siStartInfo;
        ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));
        ZeroMemory(&siStartInfo, sizeof(STARTUPINFOA));
        siStartInfo.cb = sizeof(STARTUPINFOA);

        std::vector<char> cmd_buf(cmd_line.begin(), cmd_line.end());
        cmd_buf.push_back('\0');

        bool bSuccess = CreateProcessA(NULL, cmd_buf.data(), NULL, NULL, TRUE, 0, 
                                      env.empty() ? NULL : env_block.data(), 
                                      working_dir.empty() ? NULL : working_dir.c_str(), 
                                      &siStartInfo, &piProcInfo);
        if (!bSuccess) {
            DWORD err = GetLastError();
            std::string msg = "CreateProcess failed (Error: " + std::to_string(err) + ")";
            if (err == ERROR_FILE_NOT_FOUND) msg = "BINARY_NOT_FOUND";
            else if (err == ERROR_ACCESS_DENIED) msg = "PERMISSION_DENIED";
            return {-1, msg};
        }

        int pid = static_cast<int>(piProcInfo.dwProcessId);
        CloseHandle(piProcInfo.hProcess);
        CloseHandle(piProcInfo.hThread);
        return {pid, ""};

#else
        pid_t pid = fork();
        if (pid == -1) return {-1, "fork() failed"};

        if (pid == 0) {
            // Child process
            if (!working_dir.empty()) {
                if (chdir(working_dir.c_str()) != 0) {
                    exit(127);
                }
            }

            for (auto const& [key, val] : env) {
                setenv(key.c_str(), val.c_str(), 1);
            }

            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(command.c_str()));
            for (const auto& arg : args) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);

            execvp(command.c_str(), argv.data());
            
            // If execvp returns, it failed
            int err = errno;
            if (err == ENOENT) exit(1);  // binary not found
            if (err == EACCES) exit(2);  // permission denied
            exit(127);
        }
        
        // Parent: wait a tiny bit to see if child failed exec immediately
        int status;
        pid_t res = waitpid(pid, &status, WNOHANG);
        if (res == pid && WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code == 1) return {-1, "BINARY_NOT_FOUND"};
            if (code == 2) return {-1, "PERMISSION_DENIED"};
            return {-1, "Spawn failed with code " + std::to_string(code)};
        }

        return {static_cast<int>(pid), ""};
#endif
    }
};

} // namespace velix::utils

#endif // VELIX_PROCESS_SPAWNER_HPP
