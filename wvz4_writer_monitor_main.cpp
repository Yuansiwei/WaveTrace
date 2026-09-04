#include "wvz4_writer_typed.h"

#include <exception>
#include <iostream>
#include <new>
#include <string>

namespace {

bool arg_value(int argc, char** argv, const char* name, std::string& out) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == name) {
            out = argv[i + 1];
            return true;
        }
    }
    return false;
}

bool has_arg(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == name) return true;
    }
    return false;
}

} // namespace

// Helper writer process.
//
// The simulation process sends layout/cycle/finalize frames over a named pipe.
// This process owns the real wvz4::Writer and finalizes the WVZ4 when the pipe
// closes, so VS Stop Debugging / task-manager kill of the simulation process
// still leaves a valid finalized waveform up to the last fully received cycle.
int main(int argc, char** argv) {
    if (!has_arg(argc, argv, "--writer-helper")) {
        std::cerr << "Usage: " << argv[0]
                  << " --writer-helper --pipe <name> --parent-pid <pid> --out <output.wvz4>\n";
        return 2;
    }

    std::string pipe_name;
    std::string parent_pid_text;
    std::string output_path;
    std::string ipc_version_text;
    std::string log_path;
    if (!arg_value(argc, argv, "--pipe", pipe_name) ||
        !arg_value(argc, argv, "--parent-pid", parent_pid_text) ||
        !arg_value(argc, argv, "--out", output_path)) {
        std::cerr << "Missing required writer-helper arguments\n";
        return 2;
    }
    arg_value(argc, argv, "--ipc-version", ipc_version_text);
    arg_value(argc, argv, "--log", log_path);

    char* end = nullptr;
    const unsigned long parent_pid = std::strtoul(parent_pid_text.c_str(), &end, 10);
    if (end == parent_pid_text.c_str() || parent_pid == 0ul) {
        std::cerr << "Invalid --parent-pid value\n";
        return 2;
    }
    if (!ipc_version_text.empty()) {
        end = nullptr;
        const unsigned long ipc_version = std::strtoul(ipc_version_text.c_str(), &end, 10);
        if (end == ipc_version_text.c_str() || *end != '\0' ||
            ipc_version != static_cast<unsigned long>(wvz4::kWriterProcessProtocolVersion)) {
            std::string error = "WVZ4 writer helper IPC protocol mismatch";
            wvz4::detail::writer_process_diag_log(log_path, error);
            std::cerr << error << "\n";
            return 2;
        }
    } else {
        wvz4::detail::writer_process_diag_log(log_path, "WVZ4 writer helper started without --ipc-version");
    }

    std::string error;
    bool ok = false;
    try {
        ok = wvz4::WriterProcessServer::run(pipe_name, output_path, parent_pid, log_path, error);
    } catch (const std::bad_alloc&) {
        error = "WVZ4 writer helper failed: std::bad_alloc";
    } catch (const std::exception& e) {
        error = std::string("WVZ4 writer helper failed with exception: ") + e.what();
    } catch (...) {
        error = "WVZ4 writer helper failed with unknown exception";
    }
    if (!ok) {
        wvz4::detail::writer_process_diag_log(log_path, error);
        std::cerr << "WVZ4 writer helper failed: " << error << "\n";
        return 1;
    }
    return 0;
}
