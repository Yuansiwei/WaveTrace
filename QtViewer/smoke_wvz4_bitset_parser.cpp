#include "WaveParser4.h"
#include "../wvz4_writer_typed.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>

#include <iostream>

#ifdef signals
#undef signals
#endif

namespace {

wvz4::NodeRecord node(wvz4::u32 id, wvz4::u32 parent,
                      wvz4::u32 name, wvz4::NodeKind kind,
                      wvz4::u32 firstChild, wvz4::u32 nextSibling) {
    wvz4::NodeRecord result;
    result.node_id = id;
    result.parent_id = parent;
    result.name_id = name;
    result.kind = kind;
    result.first_child = firstChild;
    result.next_sibling = nextSibling;
    return result;
}

wvz4::SignalDefinition storage(wvz4::u32 id) {
    wvz4::SignalDefinition result;
    result.signal_id = id;
    result.storage_id = id;
    result.node_id = 0;
    result.type = wvz4::ValueType::U64;
    result.bit_width = 64;
    result.radix = wvz4::Radix::Hex;
    result.storage_only = true;
    return result;
}

const WaveSignal* findSignal(const WaveFile& wave, int id) {
    for (const WaveSignal& signal : wave.signalList) {
        if (signal.signalId == id) return &signal;
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString path = QCoreApplication::applicationDirPath() +
        QStringLiteral("/smoke_bitset_virtual.wvz4");
    QFile::remove(path);

    wvz4::Layout layout;
    wvz4::NameRecord topName; topName.name_id = 1; topName.name = "top";
    wvz4::NameRecord bitsName; bitsName.name_id = 2; bitsName.name = "bits";
    layout.names.push_back(topName);
    layout.names.push_back(bitsName);
    layout.nodes.push_back(node(1, 0, 1, wvz4::NodeKind::Root, 2, 0));
    layout.nodes.push_back(node(2, 1, 2, wvz4::NodeKind::Field, 0, 0));
    layout.signals.push_back(storage(1));
    layout.signals.push_back(storage(2));
    wvz4::BitsetDefinition bitset;
    bitset.node_id = 2;
    bitset.first_storage_id = 1;
    bitset.bit_count = 70;
    bitset.word_count = 2;
    layout.bitsets.push_back(bitset);

    wvz4::WriterOptions options;
    options.compression = wvz4::Compression::None;
    options.enable_lod_tables = false;
    options.implicit_zero_initial_values = false;
    options.target_block_span = 16;
    std::string writerError;
    const QString helperPath = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(
        QStringLiteral("../../../../build_vs/wvz4_writer_monitor/Release/wvz4_writer_monitor.exe"));
    wvz4::WriterProcessClient writer;
    if (!writer.open(path.toStdString(), layout, options, writerError,
                     helperPath.toStdString())) {
        std::cerr << writerError << "\n";
        return 2;
    }
    wvz4::CycleSubmission cycle0;
    cycle0.cycle = 0;
    cycle0.updates.push_back(wvz4::CycleValueUpdate::make<wvz4::u64>(1, 1ull));
    cycle0.updates.push_back(wvz4::CycleValueUpdate::make<wvz4::u64>(2, 2ull));
    if (!writer.submit_cycle(cycle0, writerError)) return 3;
    wvz4::CycleSubmission cycle1;
    cycle1.cycle = 1;
    cycle1.updates.push_back(wvz4::CycleValueUpdate::make<wvz4::u64>(2, 0ull));
    if (!writer.submit_cycle(cycle1, writerError) || !writer.close(writerError)) {
        std::cerr << writerError << "\n";
        return 4;
    }

    WaveParser4Reader reader;
    QString error;
    if (!reader.open(path, error)) {
        std::cerr << error.toLocal8Bit().constData() << "\n";
        return 5;
    }
    const WaveFile& directory = reader.directoryWave();
    if (!directory.signalList.empty() || directory.tree.bitsets.size() != 1 ||
        directory.tree.bitsets.at(0).nodeId != 2 ||
        directory.tree.bitsets.at(0).firstVirtualSignalId != 3 ||
        directory.tree.bitsets.at(0).bitCount != 70) {
        std::cerr << "bad lazy bitset directory metadata\n";
        return 6;
    }

    WaveFile loaded;
    QVector<int> ids;
    ids << 3 << 68; // bit 0 and bit 65
    if (!reader.loadSignals(ids, loaded, error)) {
        std::cerr << error.toLocal8Bit().constData() << "\n";
        return 7;
    }
    const WaveSignal* bit0 = findSignal(loaded, 3);
    const WaveSignal* bit65 = findSignal(loaded, 68);
    if (!bit0 || !bit65 || !bit0->virtualBitsetBit || !bit65->virtualBitsetBit ||
        bit0->storageId != 1 || bit0->bitOffset != 0 ||
        bit65->storageId != 2 || bit65->bitOffset != 1 ||
        bit0->samples.isEmpty() || bit65->samples.size() < 2 ||
        bit0->samples.last().rawBits != 1 || bit65->samples.last().rawBits != 0) {
        std::cerr << "virtual bit extraction failed\n";
        return 8;
    }
    WaveFile fullLoad;
    WaveParser4::LoadOptions fullOptions;
    fullOptions.includeAllSignalDefinitions = true;
    fullOptions.loadRawSamples = false;
    if (!WaveParser4::loadFromFile(path, fullLoad, error, fullOptions) ||
        fullLoad.tree.bitsets.size() != 1 ||
        fullLoad.tree.bitsets.at(0).firstVirtualSignalId != 3) {
        std::cerr << "legacy full-load API lost bitset metadata: "
                  << error.toLocal8Bit().constData() << "\n";
        return 9;
    }
    QFile::remove(path);
    QFile::remove(path + QStringLiteral(".log"));
    QFile::remove(path + QStringLiteral(".writer.log"));
    std::cout << "wvz4_bitset_virtual_read_ok\n";
    return 0;
}
