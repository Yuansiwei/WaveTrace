#include "wvz4_writer_typed.h"
#include <iostream>

// Minimal monitor/finalizer process entry.
// Usage:
//   wvz4_writer_monitor.exe out.wvz4.spool out.wvz4
// It drains committed WAL records and writes a finalized crash-consistent WVZ4.
// In production the parent launcher can start this process and call it after the
// main simulation process exits, is killed, or sends a finalize command.
int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <spool.wal> <output.wvz4>\n";
        return 2;
    }
    std::string error;
    if (!wvz4::WalMonitor::replay_committed_spool_to_file(argv[1], argv[2], error)) {
        std::cerr << "WVZ4 monitor finalize failed: " << error << "\n";
        return 1;
    }
    return 0;
}
