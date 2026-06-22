#include "wvz4_writer_typed.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string quote_arg(const std::string& arg) {
    std::string out;
    out.reserve(arg.size() + 2);
    out.push_back('"');
    for (std::size_t i = 0; i < arg.size(); ++i) {
        if (arg[i] == '"') out.push_back('\\');
        out.push_back(arg[i]);
    }
    out.push_back('"');
    return out;
}

std::string join_path(const std::string& dir, const std::string& leaf) {
    if (dir.empty()) return leaf;
    const char last = dir[dir.size() - 1];
    if (last == '\\' || last == '/') return dir + leaf;
    return dir + "\\" + leaf;
}

std::string current_dir() {
    char buffer[MAX_PATH];
    DWORD n = GetCurrentDirectoryA(static_cast<DWORD>(sizeof(buffer)), buffer);
    if (n == 0 || n >= sizeof(buffer)) return std::string(".");
    return std::string(buffer, buffer + n);
}

std::string current_exe_path() {
    char buffer[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buffer, static_cast<DWORD>(sizeof(buffer)));
    if (n == 0 || n >= sizeof(buffer)) return std::string();
    return std::string(buffer, buffer + n);
}

bool file_exists(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return static_cast<bool>(in);
}

void remove_if_exists(const std::string& path) {
    DeleteFileA(path.c_str());
}

std::uint64_t load_u64_le(const unsigned char* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

bool validate_finalized_wvz4(const std::string& path, std::string& error) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        error = "file does not exist: " + path;
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size_off = in.tellg();
    if (size_off < 76) {
        error = "file too small";
        return false;
    }
    const std::uint64_t file_size = static_cast<std::uint64_t>(size_off);
    in.seekg(0, std::ios::beg);

    unsigned char header[64];
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!in) {
        error = "failed to read header";
        return false;
    }
    const unsigned char expected_magic[8] = {'W','V','Z','4','\r','\n',0,0};
    if (std::memcmp(header, expected_magic, sizeof(expected_magic)) != 0) {
        error = "bad WVZ4 magic";
        return false;
    }
    const std::uint64_t footer_offset = load_u64_le(header + 24);
    if (footer_offset == 0) {
        error = "missing FOOT/footer_offset";
        return false;
    }
    if (footer_offset + 12u > file_size) {
        error = "footer_offset exceeds file size";
        return false;
    }
    in.seekg(static_cast<std::streamoff>(footer_offset), std::ios::beg);
    char tag[4];
    unsigned char len_bytes[8];
    in.read(tag, sizeof(tag));
    in.read(reinterpret_cast<char*>(len_bytes), sizeof(len_bytes));
    if (!in) {
        error = "failed to read FOOT header";
        return false;
    }
    if (std::memcmp(tag, "FOOT", 4) != 0) {
        error = "footer_offset does not point to FOOT";
        return false;
    }
    const std::uint64_t foot_payload_size = load_u64_le(len_bytes);
    if (footer_offset + 12u + foot_payload_size > file_size) {
        error = "FOOT section exceeds file size";
        return false;
    }
    return true;
}

bool wait_for_finalized_wvz4(const std::string& path, DWORD timeout_ms, std::string& error) {
    const DWORD start = GetTickCount();
    std::string last_error;
    while (GetTickCount() - start < timeout_ms) {
        if (validate_finalized_wvz4(path, last_error)) {
            error.clear();
            return true;
        }
        Sleep(50);
    }
    error = last_error.empty() ? "timed out waiting for finalized WVZ4" : last_error;
    return false;
}

wvz4::Layout make_layout() {
    wvz4::Layout layout;
    layout.names.push_back({1, "top"});
    layout.names.push_back({2, "sig_bool"});
    layout.names.push_back({3, "sig_u32"});
    layout.names.push_back({4, "sig_u64"});
    layout.names.push_back({5, "sig_i64"});

    wvz4::NodeRecord root;
    root.node_id = 1;
    root.parent_id = 0;
    root.name_id = 1;
    root.kind = wvz4::NodeKind::Object;
    root.first_child = 2;
    layout.nodes.push_back(root);

    for (wvz4::u32 i = 0; i < 4; ++i) {
        wvz4::NodeRecord leaf;
        leaf.node_id = 2 + i;
        leaf.parent_id = 1;
        leaf.name_id = 2 + i;
        leaf.kind = wvz4::NodeKind::SignalLeaf;
        leaf.first_child = 0;
        leaf.next_sibling = (i == 3) ? 0 : (3 + i);
        layout.nodes.push_back(leaf);
    }

    wvz4::SignalDefinition s1;
    s1.signal_id = 1;
    s1.storage_id = 1;
    s1.node_id = 2;
    s1.type = wvz4::ValueType::Bool;
    s1.bit_width = 1;
    s1.radix = wvz4::Radix::Bin;
    layout.signals.push_back(s1);

    wvz4::SignalDefinition s2;
    s2.signal_id = 2;
    s2.storage_id = 2;
    s2.node_id = 3;
    s2.type = wvz4::ValueType::U32;
    s2.bit_width = 32;
    s2.radix = wvz4::Radix::Hex;
    layout.signals.push_back(s2);

    wvz4::SignalDefinition s3 = s2;
    s3.signal_id = 3;
    s3.storage_id = 3;
    s3.node_id = 4;
    s3.type = wvz4::ValueType::U64;
    s3.bit_width = 64;
    layout.signals.push_back(s3);

    wvz4::SignalDefinition s4 = s2;
    s4.signal_id = 4;
    s4.storage_id = 4;
    s4.node_id = 5;
    s4.type = wvz4::ValueType::I64;
    s4.bit_width = 64;
    layout.signals.push_back(s4);

    return layout;
}

bool submit_cycles(wvz4::WriterProcessClient& client, int cycles, std::string& error) {
    for (int c = 0; c < cycles; ++c) {
        wvz4::CycleSubmission s;
        s.cycle = c;
        s.updates.push_back(wvz4::CycleValueUpdate::make_bool(1, (c & 1) != 0));
        s.updates.push_back(wvz4::CycleValueUpdate::make<std::uint32_t>(2, static_cast<std::uint32_t>(c * 3 + 1)));
        s.updates.push_back(wvz4::CycleValueUpdate::make<std::uint64_t>(3, 0x123400000000ull + static_cast<std::uint64_t>(c)));
        s.updates.push_back(wvz4::CycleValueUpdate::make<std::int64_t>(4, -static_cast<std::int64_t>(c * 7)));
        if (!client.submit_cycle(s, error)) return false;
    }
    return true;
}

bool write_ready_file(const std::string& path) {
    std::ofstream out(path.c_str(), std::ios::out | std::ios::trunc);
    out << "ready\n";
    return static_cast<bool>(out);
}

int child_main(int argc, char** argv) {
    std::string mode;
    std::string out_path;
    std::string ready_path;
    int cycles = 128;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--mode" && i + 1 < argc) mode = argv[++i];
        else if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
        else if (arg == "--ready" && i + 1 < argc) ready_path = argv[++i];
        else if (arg == "--cycles" && i + 1 < argc) cycles = std::atoi(argv[++i]);
    }
    if (mode.empty() || out_path.empty()) {
        std::cerr << "child missing --mode/--out\n";
        return 2;
    }

    wvz4::WriterOptions opt;
    opt.target_block_span = 16;
    opt.compression = wvz4::Compression::None;
    opt.enable_block_pipeline = false;
    opt.enable_stats_log = false;
    opt.enable_lod_tables = false;

    wvz4::WriterProcessClient client;
    std::string error;
    if (!client.open(out_path, make_layout(), opt, error)) {
        std::cerr << "child open failed: " << error << "\n";
        return 3;
    }
    if (!submit_cycles(client, cycles, error)) {
        std::cerr << "child submit failed: " << error << "\n";
        return 4;
    }

    if (!ready_path.empty() && !write_ready_file(ready_path)) {
        std::cerr << "failed to write ready file\n";
        return 5;
    }

    if (mode == "normal") {
        if (!client.close(error)) {
            std::cerr << "child close failed: " << error << "\n";
            return 6;
        }
        return 0;
    }
    if (mode == "crash") {
        RaiseFailFastException(NULL, NULL, 0);
        return 7;
    }
    if (mode == "kill_wait") {
        Sleep(INFINITE);
        return 8;
    }

    std::cerr << "unknown child mode: " << mode << "\n";
    return 9;
}

bool start_child(const std::string& mode,
                 const std::string& out_path,
                 const std::string& ready_path,
                 PROCESS_INFORMATION& pi,
                 std::string& error) {
    const std::string exe = current_exe_path();
    if (exe.empty()) {
        error = "failed to resolve current exe path";
        return false;
    }
    std::ostringstream cmd;
    cmd << quote_arg(exe)
        << " --child --mode " << quote_arg(mode)
        << " --out " << quote_arg(out_path)
        << " --cycles 128";
    if (!ready_path.empty()) {
        cmd << " --ready " << quote_arg(ready_path);
    }

    std::string cmd_str = cmd.str();
    std::vector<char> mutable_cmd(cmd_str.begin(), cmd_str.end());
    mutable_cmd.push_back('\0');

    STARTUPINFOA si;
    std::memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    std::memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(exe.c_str(), mutable_cmd.data(), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        std::ostringstream os;
        os << "CreateProcess failed, GetLastError=" << GetLastError();
        error = os.str();
        return false;
    }
    return true;
}

bool wait_process(PROCESS_INFORMATION& pi, DWORD timeout_ms, DWORD& exit_code, std::string& error) {
    const DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait != WAIT_OBJECT_0) {
        error = "child process timeout";
        return false;
    }
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        std::ostringstream os;
        os << "GetExitCodeProcess failed, GetLastError=" << GetLastError();
        error = os.str();
        return false;
    }
    return true;
}

void close_process_handles(PROCESS_INFORMATION& pi) {
    if (pi.hThread) {
        CloseHandle(pi.hThread);
        pi.hThread = NULL;
    }
    if (pi.hProcess) {
        CloseHandle(pi.hProcess);
        pi.hProcess = NULL;
    }
}

bool wait_ready_file(const std::string& path, DWORD timeout_ms) {
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeout_ms) {
        if (file_exists(path)) return true;
        Sleep(20);
    }
    return false;
}

bool run_case(const std::string& name, const std::string& mode, bool terminate_child) {
    const std::string base_dir = current_dir();
    const std::string out_path = join_path(base_dir, "build_vs\\helper_kill_crash_" + name + ".wvz4");
    const std::string ready_path = join_path(base_dir, "build_vs\\helper_kill_crash_" + name + ".ready");
    remove_if_exists(out_path);
    remove_if_exists(out_path + ".log");
    remove_if_exists(ready_path);

    std::string error;
    PROCESS_INFORMATION pi;
    if (!start_child(mode, out_path, terminate_child ? ready_path : std::string(), pi, error)) {
        std::cerr << name << ": " << error << "\n";
        return false;
    }

    bool ok = true;
    DWORD exit_code = 0;
    if (terminate_child) {
        if (!wait_ready_file(ready_path, 10000)) {
            std::cerr << name << ": child did not signal ready\n";
            ok = false;
        } else if (!TerminateProcess(pi.hProcess, 0xDEAD0411u)) {
            std::cerr << name << ": TerminateProcess failed, GetLastError=" << GetLastError() << "\n";
            ok = false;
        }
    }

    if (ok && !wait_process(pi, 10000, exit_code, error)) {
        std::cerr << name << ": " << error << "\n";
        ok = false;
    }
    close_process_handles(pi);

    if (ok && mode == "normal" && exit_code != 0) {
        std::cerr << name << ": normal child exit_code=" << exit_code << "\n";
        ok = false;
    }
    if (ok && mode != "normal" && exit_code == 0) {
        std::cerr << name << ": abnormal child exited cleanly\n";
        ok = false;
    }

    if (ok && !wait_for_finalized_wvz4(out_path, 10000, error)) {
        std::cerr << name << ": finalized check failed: " << error << "\n";
        ok = false;
    }

    if (ok) {
        std::cout << name << "_ok"
                  << " exit_code=" << exit_code
                  << " file=" << out_path
                  << "\n";
    }
    remove_if_exists(ready_path);
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--child") {
        return child_main(argc, argv);
    }

    bool ok = true;
    ok = run_case("normal", "normal", false) && ok;
    ok = run_case("crash", "crash", false) && ok;
    ok = run_case("kill", "kill_wait", true) && ok;
    if (!ok) return 1;
    std::cout << "helper kill/crash coverage passed\n";
    return 0;
}
