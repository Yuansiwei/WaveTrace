#include "WaveParser4.h"

#include <QCoreApplication>

#include <cstdint>
#include <iostream>

static int findSignalById(const WaveFile& wave, int signalId) {
    for (int i = 0; i < static_cast<int>(wave.signalList.size()); ++i) {
        if (wave.signalList.at(i).signalId == signalId) return i;
    }
    return -1;
}

static bool valueAtOrBefore(const WaveSignal& sig, qint64 time, quint64& out) {
    bool found = false;
    WaveSample best;
    for (const WaveSample& s : sig.samples) {
        if (s.time > time) break;
        best = s;
        found = true;
    }
    if (!found || best.isAbsent || best.isZ || !best.rawFieldsReady) return false;
    out = best.rawBits;
    return true;
}

static bool hasExactSample(const WaveSignal& sig, qint64 time) {
    for (const WaveSample& s : sig.samples) {
        if (s.time == time) return true;
    }
    return false;
}

static int fail(const QString& msg) {
    std::cerr << msg.toLocal8Bit().constData() << "\n";
    return 1;
}

static int expectValueAt(const WaveSignal& sig,
                         qint64 time,
                         quint64 expected,
                         const QString& label) {
    quint64 actual = 0;
    if (!valueAtOrBefore(sig, time, actual)) {
        return fail(label + QStringLiteral(": missing sample at/before t=") + QString::number(time));
    }
    if (actual != expected) {
        return fail(label +
                    QStringLiteral(": expected 0x") + QString::number(expected, 16) +
                    QStringLiteral(" got 0x") + QString::number(actual, 16) +
                    QStringLiteral(" at t=") + QString::number(time));
    }
    return 0;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2 || !argv[1] || argv[1][0] == '\0') {
        return fail(QStringLiteral("usage: smoke_wvz4_dirty_array_parser <dirty_array.wvz4>"));
    }

    WaveParser4Reader reader;
    QString error;
    if (!reader.open(QString::fromLocal8Bit(argv[1]), error)) {
        return fail(QStringLiteral("reader.open failed: ") + error);
    }
    const WaveFile& directory = reader.directoryWave();
    if (directory.tree.arrays.size() != 1 || !directory.signalList.empty()) {
        return fail(QStringLiteral("dirty array was not retained as one lazy compact block"));
    }
    const WaveArrayInfo& array = directory.tree.arrays.at(0);
    if (array.elementCount != 128 || array.elementStride != 64 ||
        array.leafCountPerElement != 16 || array.schema.size() != 17) {
        return fail(QStringLiteral("dirty array compact schema mismatch"));
    }
    QVector<int> ids;
    ids << array.firstVirtualSignalId + 7 * 16 + 3
        << array.firstVirtualSignalId + 9 * 16 + 4
        << array.firstVirtualSignalId + 7 * 16 + 4;
    WaveFile wave;
    if (!reader.loadSignals(ids, wave, error, 1000000)) {
        return fail(QStringLiteral("loadSignals failed: ") + error);
    }

    const int f3Index = findSignalById(wave, ids.at(0));
    const int f4Index = findSignalById(wave, ids.at(1));
    const int stableIndex = findSignalById(wave, ids.at(2));
    if (f3Index < 0) return fail(QStringLiteral("missing virtual signal top.slots.[7].f3"));
    if (f4Index < 0) return fail(QStringLiteral("missing virtual signal top.slots.[9].f4"));
    if (stableIndex < 0) return fail(QStringLiteral("missing virtual signal top.slots.[7].f4"));

    const WaveSignal& f3 = wave.signalList.at(f3Index);
    const WaveSignal& f4 = wave.signalList.at(f4Index);
    const WaveSignal& stable = wave.signalList.at(stableIndex);

    if (f3.samples.size() != 2) return fail(QStringLiteral("top.slots.[7].f3 expected 2 compacted samples"));
    if (f4.samples.size() != 2) return fail(QStringLiteral("top.slots.[9].f4 expected 2 compacted samples"));
    if (stable.samples.size() != 1) return fail(QStringLiteral("top.slots.[7].f4 expected only initial sample"));

    if (expectValueAt(f3, 0, 7003u, QStringLiteral("f3@cycle0")) != 0) return 1;
    if (expectValueAt(f3, 10, 0x12345678u, QStringLiteral("f3@cycle1")) != 0) return 1;
    if (expectValueAt(f3, 20, 0x12345678u, QStringLiteral("f3@cycle2")) != 0) return 1;
    if (expectValueAt(f3, 30, 0x12345678u, QStringLiteral("f3@cycle3")) != 0) return 1;

    if (expectValueAt(f4, 0, 9004u, QStringLiteral("f4@cycle0")) != 0) return 1;
    if (expectValueAt(f4, 20, 9004u, QStringLiteral("f4@cycle2")) != 0) return 1;
    if (expectValueAt(f4, 30, 0x87654321u, QStringLiteral("f4@cycle3")) != 0) return 1;

    if (expectValueAt(stable, 30, 7004u, QStringLiteral("stable@cycle3")) != 0) return 1;
    if (hasExactSample(f3, 20)) return fail(QStringLiteral("top.slots.[7].f3 has unexpected exact cycle2 sample"));
    if (hasExactSample(f4, 20)) return fail(QStringLiteral("top.slots.[9].f4 has unexpected exact cycle2 sample"));

    std::cout << "dirty_array_wvz4_parser_ok signals=" << wave.signalList.size()
              << " f3_samples=" << f3.samples.size()
              << " f4_samples=" << f4.samples.size() << "\n";
    return 0;
}
