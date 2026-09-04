#include "WaveParser4.h"

#include <QCoreApplication>
#include <QFile>
#include <QStringList>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <vector>

static QString fullPathForSignal(const WaveFile& wave, int signalIndex) {
    if (signalIndex < 0 || signalIndex >= wave.signalList.size()) return QString();
    if (!wave.tree.valid ||
        signalIndex >= wave.tree.signalIndexToNodeId.size() ||
        wave.tree.signalIndexToNodeId.at(signalIndex) <= 0) {
        return wave.signalList.at(signalIndex).name;
    }

    QStringList parts;
    int nodeId = wave.tree.signalIndexToNodeId.at(signalIndex);
    int guard = 0;
    while (nodeId > 0 &&
           nodeId < wave.tree.nodesById.size() &&
           wave.tree.nodesById.at(nodeId).valid &&
           guard < wave.tree.nodesById.size()) {
        const WaveTreeNode& node = wave.tree.nodesById.at(nodeId);
        parts.prepend(waveTreeNodeSegmentName(wave.tree, nodeId));
        nodeId = node.parentId;
        ++guard;
    }
    return parts.join(QStringLiteral("."));
}

static int findSignalByPath(const WaveFile& wave, const QString& path) {
    for (int i = 0; i < wave.signalList.size(); ++i) {
        if (fullPathForSignal(wave, i) == path) return i;
    }
    return -1;
}

static const WaveSignal* findSignalById(const WaveFile& wave, int signalId) {
    for (const WaveSignal& signal : wave.signalList) {
        if (signal.signalId == signalId) return &signal;
    }
    return nullptr;
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

static const WaveSignal* expectSignal(const WaveFile& wave, const QString& path) {
    const int index = findSignalByPath(wave, path);
    if (index < 0) {
        std::cerr << "missing path " << path.toLocal8Bit().constData() << "\n";
        return nullptr;
    }
    return &wave.signalList.at(index);
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

struct StressKey {
    int arrayIndex = -1;
    quint64 leafIndex = 0;

    bool operator<(const StressKey& rhs) const noexcept {
        return arrayIndex != rhs.arrayIndex
            ? arrayIndex < rhs.arrayIndex
            : leafIndex < rhs.leafIndex;
    }
};

struct ExpectedEvent {
    qint64 time = 0;
    quint64 value = 0;
};

using ExpectedMap = std::map<StressKey, std::vector<ExpectedEvent>>;

static quint64 mix64(quint64 x) noexcept {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

static bool readStressOracle(const QString& path,
                             quint64 expectedSeed,
                             quint64 expectedCycles,
                             ExpectedMap& expected,
                             QString& error) {
    std::ifstream in((path + QStringLiteral(".oracle")).toLocal8Bit().constData());
    std::string magic;
    quint64 seed = 0;
    quint64 cycles = 0;
    quint64 eventCount = 0;
    if (!(in >> magic >> seed >> cycles >> eventCount) || magic != "WAVE_ARRAY_STRESS_V1") {
        error = QStringLiteral("invalid stress oracle header");
        return false;
    }
    if (seed != expectedSeed || cycles != expectedCycles) {
        error = QStringLiteral("stress oracle arguments mismatch");
        return false;
    }
    for (quint64 i = 0; i < eventCount; ++i) {
        StressKey key;
        quint64 time = 0;
        quint64 value = 0;
        if (!(in >> key.arrayIndex >> key.leafIndex >> time >> value)) {
            error = QStringLiteral("truncated stress oracle at event %1").arg(i);
            return false;
        }
        expected[key].push_back(ExpectedEvent{static_cast<qint64>(time), value});
    }
    if (expected.empty()) {
        error = QStringLiteral("empty stress oracle");
        return false;
    }
    return true;
}

static int signalIdForStressKey(const WaveFile& directory, const StressKey& key) {
    if (key.arrayIndex < 0) return static_cast<int>(key.leafIndex);
    if (key.arrayIndex >= directory.tree.arrays.size()) return -1;
    const WaveArrayInfo& array = directory.tree.arrays.at(key.arrayIndex);
    if (key.leafIndex >= array.elementCount * array.leafCountPerElement) return -1;
    const quint64 id = static_cast<quint64>(array.firstVirtualSignalId) + key.leafIndex;
    return id <= static_cast<quint64>(std::numeric_limits<int>::max()) ? static_cast<int>(id) : -1;
}

static int compareExactEvents(const WaveSignal& signal,
                              const std::vector<ExpectedEvent>& expected,
                              const QString& label) {
    if (signal.samples.size() != static_cast<int>(expected.size())) {
        QString actualTimes;
        for (int i = 0; i < signal.samples.size() && i < 16; ++i) {
            if (!actualTimes.isEmpty()) actualTimes += QLatin1Char(',');
            actualTimes += QString::number(signal.samples.at(i).time);
        }
        return fail(label + QStringLiteral(": sample count expected %1 got %2, times=%3")
                    .arg(expected.size()).arg(signal.samples.size()).arg(actualTimes));
    }
    for (int i = 0; i < signal.samples.size(); ++i) {
        const WaveSample& actual = signal.samples.at(i);
        const ExpectedEvent& wanted = expected.at(static_cast<std::size_t>(i));
        if (actual.time != wanted.time || actual.isAbsent || actual.isZ ||
            !actual.rawFieldsReady || actual.rawBits != wanted.value) {
            return fail(label + QStringLiteral(": mismatch at sample %1 expected (%2,0x%3) got (%4,0x%5)")
                        .arg(i).arg(wanted.time).arg(wanted.value, 0, 16)
                        .arg(actual.time).arg(actual.rawBits, 0, 16));
        }
    }
    return 0;
}

static std::vector<ExpectedEvent> expectedWindow(const std::vector<ExpectedEvent>& all,
                                                 qint64 start,
                                                 qint64 end) {
    std::vector<ExpectedEvent> result;
    const ExpectedEvent* anchor = nullptr;
    for (const ExpectedEvent& event : all) {
        if (event.time <= start) anchor = &event;
        else break;
    }
    if (anchor) result.push_back(ExpectedEvent{start, anchor->value});
    for (const ExpectedEvent& event : all) {
        if (event.time > start && event.time < end) result.push_back(event);
    }
    return result;
}

static int runStressParser(const QString& path, quint64 seed, quint64 cycles) {
    ExpectedMap expected;
    QString error;
    if (!readStressOracle(path, seed, cycles, expected, error)) return fail(error);

    WaveParser4Reader reader;
    if (!reader.open(path, error)) return fail(QStringLiteral("stress reader.open failed: ") + error);
    const WaveFile& directory = reader.directoryWave();
    if (directory.signalList.size() != 1 || directory.tree.arrays.size() != 4) {
        return fail(QStringLiteral("stress directory compact layout mismatch"));
    }

    QVector<int> ids;
    std::map<int, const std::vector<ExpectedEvent>*> expectedById;
    quint64 totalSamples = 0;
    for (const auto& pair : expected) {
        const int id = signalIdForStressKey(directory, pair.first);
        if (id <= 0 || expectedById.count(id) != 0) {
            return fail(QStringLiteral("stress oracle maps to invalid or duplicate signal id"));
        }
        ids.push_back(id);
        expectedById[id] = &pair.second;
        totalSamples += static_cast<quint64>(pair.second.size());
    }

    WaveFile full;
    if (!reader.loadSignals(ids, full, error, totalSamples)) {
        return fail(QStringLiteral("stress full load failed at exact budget: ") + error);
    }
    for (const auto& pair : expectedById) {
        const WaveSignal* signal = findSignalById(full, pair.first);
        if (!signal) return fail(QStringLiteral("stress full load omitted signal %1").arg(pair.first));
        const int rc = compareExactEvents(*signal, *pair.second,
                                          QStringLiteral("full signal %1").arg(pair.first));
        if (rc != 0) return rc;
    }

    if (totalSamples > 1) {
        WaveFile overBudget;
        QString budgetError;
        if (reader.loadSignals(ids, overBudget, budgetError, totalSamples - 1) ||
            !budgetError.contains(QStringLiteral("budget"), Qt::CaseInsensitive)) {
            return fail(QStringLiteral("stress exact-minus-one budget was not rejected"));
        }
    }

    const qint64 lastTime = static_cast<qint64>(cycles * 10);
    for (int windowIndex = 0; windowIndex < 16; ++windowIndex) {
        const qint64 start = ((windowIndex * 37) % qMax<qint64>(1, static_cast<qint64>(cycles))) * 10
                           + (windowIndex % 3);
        const qint64 end = qMin(lastTime + 1, start + 17 + (windowIndex % 7) * 23);
        WaveFile window;
        if (!reader.loadSignals(ids, window, error, totalSamples, start, end)) {
            return fail(QStringLiteral("stress window %1 load failed: ").arg(windowIndex) + error);
        }
        for (const auto& pair : expectedById) {
            const WaveSignal* signal = findSignalById(window, pair.first);
            if (!signal) return fail(QStringLiteral("stress window omitted signal %1").arg(pair.first));
            const std::vector<ExpectedEvent> wanted = expectedWindow(*pair.second, start, end);
            const int rc = compareExactEvents(*signal, wanted,
                                              QStringLiteral("window %1 signal %2")
                                              .arg(windowIndex).arg(pair.first));
            if (rc != 0) return rc;
        }
    }

    const QString truncatedPath = path + QStringLiteral(".truncated");
    QFile::remove(truncatedPath);
    if (!QFile::copy(path, truncatedPath)) return fail(QStringLiteral("cannot create truncated stress file"));
    QFile truncated(truncatedPath);
    if (!truncated.open(QIODevice::ReadWrite) || truncated.size() < 64 ||
        !truncated.resize(truncated.size() - 37)) {
        QFile::remove(truncatedPath);
        return fail(QStringLiteral("cannot truncate stress file"));
    }
    truncated.close();
    WaveParser4Reader brokenReader;
    QString brokenError;
    const bool acceptedBroken = brokenReader.open(truncatedPath, brokenError);
    QFile::remove(truncatedPath);
    if (acceptedBroken || brokenError.isEmpty()) {
        return fail(QStringLiteral("truncated stress file was not rejected"));
    }

    std::cout << "nested_wave_array_stress_parser_ok seed=" << seed
              << " cycles=" << cycles
              << " signals=" << ids.size()
              << " samples=" << totalSamples
              << " windows=16 corruption=rejected\n";
    return 0;
}

static int runArrayOnlyParser(const QString& path) {
    WaveParser4Reader reader;
    QString error;
    if (!reader.open(path, error)) return fail(QStringLiteral("array-only open failed: ") + error);
    const WaveFile& directory = reader.directoryWave();
    if (!directory.signalList.empty() || directory.tree.arrays.size() != 1) {
        return fail(QStringLiteral("array-only directory unexpectedly contains scalar signals"));
    }
    const WaveArrayInfo& array = directory.tree.arrays.at(0);
    if (array.elementCount != 8193 || array.elementStride != 8 ||
        array.leafCountPerElement != 1 || array.schema.size() != 1) {
        return fail(QStringLiteral("array-only schema mismatch"));
    }
    const std::vector<quint64> indices = {0, 1, 8191, 8192};
    QVector<int> ids;
    for (quint64 index : indices) ids.push_back(array.firstVirtualSignalId + static_cast<int>(index));
    WaveFile wave;
    if (!reader.loadSignals(ids, wave, error, 100)) {
        return fail(QStringLiteral("array-only load failed: ") + error);
    }
    const quint64 filled = 0x55aa55aa55aa55aaull;
    for (int i = 0; i < ids.size(); ++i) {
        const quint64 index = indices.at(static_cast<std::size_t>(i));
        std::vector<ExpectedEvent> expected = {{0, mix64(0x12340000u + index)}};
        if (index == 0) expected.push_back(ExpectedEvent{10, 0x1111222233334444ull});
        if (index == 8191) expected.push_back(ExpectedEvent{20, 0x5555666677778888ull});
        if (index == 8192) expected.push_back(ExpectedEvent{20, 0x9999aaaabbbbccccull});
        expected.push_back(ExpectedEvent{30, filled});
        const WaveSignal* signal = findSignalById(wave, ids.at(i));
        if (!signal) return fail(QStringLiteral("array-only selected signal missing"));
        const int rc = compareExactEvents(*signal, expected,
                                          QStringLiteral("array-only index %1").arg(index));
        if (rc != 0) return rc;
    }
    if (wave.meta.start != 0 || wave.meta.end < 31) {
        return fail(QStringLiteral("array-only time range mismatch"));
    }
    std::cout << "nested_wave_array_array_only_parser_ok signals=4 dense_fill=ok\n";
    return 0;
}

static int expectOldVersionRejected(const QString& sourcePath, quint32 version) {
    const QString oldPath = sourcePath + QStringLiteral(".v%1").arg(version);
    QFile::remove(oldPath);
    if (!QFile::copy(sourcePath, oldPath)) {
        return fail(QStringLiteral("cannot create v%1 rejection fixture").arg(version));
    }
    QFile file(oldPath);
    char encoded[4] = {
        static_cast<char>(version & 0xffu),
        static_cast<char>((version >> 8) & 0xffu),
        static_cast<char>((version >> 16) & 0xffu),
        static_cast<char>((version >> 24) & 0xffu)};
    if (!file.open(QIODevice::ReadWrite) || !file.seek(8) || file.write(encoded, 4) != 4) {
        file.close();
        QFile::remove(oldPath);
        return fail(QStringLiteral("cannot patch v%1 rejection fixture").arg(version));
    }
    file.close();

    WaveParser4Reader reader;
    QString readerError;
    const bool readerAccepted = reader.open(oldPath, readerError);
    WaveFile legacyWave;
    QString legacyError;
    const bool legacyAccepted = WaveParser4::loadFromFile(oldPath, legacyWave, legacyError);
    QFile::remove(oldPath);
    if (readerAccepted || legacyAccepted ||
        !readerError.contains(QStringLiteral("v17"), Qt::CaseInsensitive) ||
        !legacyError.contains(QStringLiteral("v17"), Qt::CaseInsensitive)) {
        return fail(QStringLiteral("WVZ4 v%1 was not rejected by every Viewer entry point")
                    .arg(version));
    }
    return 0;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2 || !argv[1] || argv[1][0] == '\0') {
        return fail(QStringLiteral("usage: smoke_wvz4_nested_wave_array_parser <nested_array.wvz4>"));
    }
    const QString inputPath = QString::fromLocal8Bit(argv[1]);
    if (argc >= 3 && QString::fromLocal8Bit(argv[2]) == QStringLiteral("--array-only")) {
        return runArrayOnlyParser(inputPath);
    }
    if (argc >= 3 && QString::fromLocal8Bit(argv[2]) == QStringLiteral("--stress")) {
        if (argc < 5) return fail(QStringLiteral("usage: parser <file> --stress <seed> <cycles>"));
        bool seedOk = false;
        bool cyclesOk = false;
        const quint64 seed = QString::fromLocal8Bit(argv[3]).toULongLong(&seedOk, 0);
        const quint64 cycles = QString::fromLocal8Bit(argv[4]).toULongLong(&cyclesOk, 0);
        if (!seedOk || !cyclesOk) return fail(QStringLiteral("invalid stress arguments"));
        return runStressParser(inputPath, seed, cycles);
    }

    WaveParser4Reader reader;
    QString error;
    if (!reader.open(inputPath, error)) {
        return fail(QStringLiteral("reader.open failed: ") + error);
    }
    if (expectOldVersionRejected(inputPath, 15) != 0 ||
        expectOldVersionRejected(inputPath, 16) != 0) {
        return 1;
    }
    const WaveFile& directory = reader.directoryWave();
    if (directory.tree.arrays.size() != 4 || directory.signalList.size() != 1 ||
        directory.signalList.at(0).signalId != 1) {
        return fail(QStringLiteral("directory did not retain one scalar and four lazy compact arrays"));
    }
    const WaveArrayInfo& matrix = directory.tree.arrays.at(0);
    const WaveArrayInfo& cube = directory.tree.arrays.at(1);
    const WaveArrayInfo& cells = directory.tree.arrays.at(2);
    const WaveArrayInfo& crossPage = directory.tree.arrays.at(3);
    if (matrix.elementCount != 2 || matrix.leafCountPerElement != 2 || matrix.schema.size() != 2 ||
        cube.elementCount != 2 || cube.leafCountPerElement != 4 || cube.schema.size() != 3 ||
        cells.elementCount != 2 || cells.leafCountPerElement != 4 || cells.schema.size() != 4 ||
        crossPage.elementCount != 60000 || crossPage.leafCountPerElement != 2 ||
        crossPage.elementStride != 10 || crossPage.schema.size() != 3) {
        return fail(QStringLiteral("compact array schema mismatch"));
    }
    QVector<int> ids;
    ids.push_back(1);
    for (int id = matrix.firstVirtualSignalId; id < matrix.firstVirtualSignalId + 4; ++id) ids.push_back(id);
    for (int id = cube.firstVirtualSignalId; id < cube.firstVirtualSignalId + 8; ++id) ids.push_back(id);
    for (int id = cells.firstVirtualSignalId; id < cells.firstVirtualSignalId + 8; ++id) ids.push_back(id);
    WaveFile wave;
    if (!reader.loadSignals(ids, wave, error, 1000000)) {
        return fail(QStringLiteral("loadSignals failed: ") + error);
    }
    if (wave.meta.start != 0 || wave.meta.end < 31) {
        return fail(QStringLiteral("array-only load lost the file time range"));
    }

    const WaveSignal* m00 = findSignalById(wave, matrix.firstVirtualSignalId + 0);
    const WaveSignal* m01 = findSignalById(wave, matrix.firstVirtualSignalId + 1);
    const WaveSignal* m10 = findSignalById(wave, matrix.firstVirtualSignalId + 2);
    const WaveSignal* m11 = findSignalById(wave, matrix.firstVirtualSignalId + 3);
    const WaveSignal* c010 = findSignalById(wave, cube.firstVirtualSignalId + 2);
    const WaveSignal* c011 = findSignalById(wave, cube.firstVirtualSignalId + 3);
    const WaveSignal* c100 = findSignalById(wave, cube.firstVirtualSignalId + 4);
    const WaveSignal* c101 = findSignalById(wave, cube.firstVirtualSignalId + 5);
    const WaveSignal* cell0Id = findSignalById(wave, cells.firstVirtualSignalId + 0);
    const WaveSignal* cell0Sample0 = findSignalById(wave, cells.firstVirtualSignalId + 1);
    const WaveSignal* cell1Sample2 = findSignalById(wave, cells.firstVirtualSignalId + 7);
    const WaveSignal* scalar = findSignalById(wave, 1);
    if (!scalar || !m00 || !m01 || !m10 || !m11 || !c010 || !c011 || !c100 || !c101 ||
        !cell0Id || !cell0Sample0 || !cell1Sample2) return 2;

    if (expectValueAt(*scalar, 0, 5u, QStringLiteral("scalar@cycle0")) != 0) return 1;
    if (expectValueAt(*scalar, 30, 9u, QStringLiteral("scalar@cycle3")) != 0) return 1;
    if (scalar->samples.size() != 2) return fail(QStringLiteral("scalar expected exactly 2 samples"));

    if (expectValueAt(*m00, 0, 0u, QStringLiteral("m00@cycle0")) != 0) return 1;
    if (expectValueAt(*m00, 10, 100u, QStringLiteral("m00@cycle1")) != 0) return 1;
    if (expectValueAt(*m01, 10, 1u, QStringLiteral("m01@cycle1")) != 0) return 1;
    if (expectValueAt(*m10, 30, 10u, QStringLiteral("m10@cycle3")) != 0) return 1;
    if (expectValueAt(*m11, 30, 211u, QStringLiteral("m11@cycle3")) != 0) return 1;
    if (expectValueAt(*c010, 10, 10u, QStringLiteral("c010@cycle1")) != 0) return 1;
    if (expectValueAt(*c011, 10, 1111u, QStringLiteral("c011@cycle1")) != 0) return 1;
    if (expectValueAt(*c100, 30, 2100u, QStringLiteral("c100@cycle3")) != 0) return 1;
    if (expectValueAt(*c101, 30, 101u, QStringLiteral("c101@cycle3")) != 0) return 1;
    if (expectValueAt(*cell0Id, 0, 7u, QStringLiteral("cells[0].id@cycle0")) != 0) return 1;
    if (expectValueAt(*cell0Id, 30, 17u, QStringLiteral("cells[0].id@cycle3")) != 0) return 1;
    if (expectValueAt(*cell0Sample0, 30, 70u, QStringLiteral("cells[0].samples[0]@cycle3")) != 0) return 1;
    if (expectValueAt(*cell1Sample2, 0, 82u, QStringLiteral("cells[1].samples[2]@cycle0")) != 0) return 1;
    if (expectValueAt(*cell1Sample2, 10, 182u, QStringLiteral("cells[1].samples[2]@cycle1")) != 0) return 1;

    if (m00->samples.size() != 2) return fail(QStringLiteral("m00 expected exactly 2 samples"));
    if (m01->samples.size() != 1) return fail(QStringLiteral("m01 should not be dirtied by matrix[0][0]"));
    if (m10->samples.size() != 1) return fail(QStringLiteral("m10 should not be dirtied by row data()[1]"));
    if (m11->samples.size() != 2) return fail(QStringLiteral("m11 expected exactly 2 samples"));
    if (c010->samples.size() != 1) return fail(QStringLiteral("c010 should not be dirtied by cube[0][1][1]"));
    if (c011->samples.size() != 2) return fail(QStringLiteral("c011 expected exactly 2 samples"));
    if (c100->samples.size() != 2) return fail(QStringLiteral("c100 expected exactly 2 samples"));
    if (c101->samples.size() != 1) return fail(QStringLiteral("c101 should not be dirtied by cube[1][0].data()[0]"));
    if (cell0Id->samples.size() != 2) return fail(QStringLiteral("cells[0].id expected exactly 2 samples"));
    if (cell0Sample0->samples.size() != 1) return fail(QStringLiteral("cells[0].samples[0] should remain unchanged"));
    if (cell1Sample2->samples.size() != 2) return fail(QStringLiteral("cells[1].samples[2] expected exactly 2 samples"));
    if (hasExactSample(*m01, 10)) return fail(QStringLiteral("m01 has unexpected exact cycle1 sample"));
    if (hasExactSample(*m10, 30)) return fail(QStringLiteral("m10 has unexpected exact cycle3 sample"));
    if (hasExactSample(*c010, 10)) return fail(QStringLiteral("c010 has unexpected exact cycle1 sample"));
    if (hasExactSample(*c101, 30)) return fail(QStringLiteral("c101 has unexpected exact cycle3 sample"));
    if (hasExactSample(*cell0Sample0, 30)) return fail(QStringLiteral("cells[0].samples[0] has unexpected cycle3 sample"));

    static const quint64 kCrossPageIndex = 6553;
    const int crossPageValueId = crossPage.firstVirtualSignalId + int(kCrossPageIndex * 2 + 1);
    WaveFile window;
    QVector<int> crossPageIds;
    crossPageIds.push_back(crossPageValueId);
    if (!reader.loadSignals(crossPageIds, window, error, 100, 15, 25)) {
        return fail(QStringLiteral("cross-page window load failed: ") + error);
    }
    const WaveSignal* crossPageValue = findSignalById(window, crossPageValueId);
    if (!crossPageValue) return fail(QStringLiteral("cross-page virtual leaf missing"));
    if (crossPageValue->arrayByteOffset != kCrossPageIndex * 10 + 2 ||
        (crossPageValue->arrayByteOffset % (64 * 1024)) != 65532 ||
        crossPageValue->arrayByteWidth != 8) {
        return fail(QStringLiteral("cross-page virtual leaf address mismatch"));
    }
    if (expectValueAt(*crossPageValue, 15, 0x0102030405060708ull,
                      QStringLiteral("cross-page anchor@15")) != 0) return 1;
    if (expectValueAt(*crossPageValue, 20, 0x8877665544332211ull,
                      QStringLiteral("cross-page update@20")) != 0) return 1;
    if (crossPageValue->samples.size() != 2 ||
        !hasExactSample(*crossPageValue, 15) || !hasExactSample(*crossPageValue, 20)) {
        return fail(QStringLiteral("cross-page window expected anchor and update only"));
    }
    WaveFile overBudget;
    QString budgetError;
    if (reader.loadSignals(crossPageIds, overBudget, budgetError, 1, 15, 25) ||
        !budgetError.contains(QStringLiteral("budget"), Qt::CaseInsensitive)) {
        return fail(QStringLiteral("array decode did not enforce the shared sample budget"));
    }
    WaveFile halfOpen;
    if (!reader.loadSignals(crossPageIds, halfOpen, error, 100, 15, 20)) {
        return fail(QStringLiteral("half-open array window load failed: ") + error);
    }
    const WaveSignal* halfOpenValue = findSignalById(halfOpen, crossPageValueId);
    if (!halfOpenValue || halfOpenValue->samples.size() != 1 ||
        !hasExactSample(*halfOpenValue, 15) || hasExactSample(*halfOpenValue, 20) ||
        expectValueAt(*halfOpenValue, 19, 0x0102030405060708ull,
                      QStringLiteral("half-open array value@19")) != 0) {
        return fail(QStringLiteral("array time window is not half-open"));
    }
    QVector<int> mixedBudgetIds;
    mixedBudgetIds << 1 << crossPageValueId;
    WaveFile mixedOverBudget;
    QString mixedBudgetError;
    if (reader.loadSignals(mixedBudgetIds, mixedOverBudget, mixedBudgetError, 3) ||
        !mixedBudgetError.contains(QStringLiteral("budget"), Qt::CaseInsensitive)) {
        return fail(QStringLiteral("scalar and array decoders did not share one sample budget"));
    }
    WaveParser4::LoadOptions compatibilityOptions;
    compatibilityOptions.signalIds = crossPageIds;
    compatibilityOptions.includeAllSignalDefinitions = false;
    compatibilityOptions.loadAllIfWindowEmpty = false;
    compatibilityOptions.timeStart = 15;
    compatibilityOptions.timeEnd = 25;
    WaveFile compatibilityWave;
    if (!WaveParser4::loadFromFile(QString::fromLocal8Bit(argv[1]), compatibilityWave,
                                   error, compatibilityOptions)) {
        return fail(QStringLiteral("legacy parser API rejected v17 arrays: ") + error);
    }
    const WaveSignal* compatibilityValue = findSignalById(compatibilityWave, crossPageValueId);
    if (!compatibilityValue || compatibilityValue->samples.size() != 2 ||
        expectValueAt(*compatibilityValue, 20, 0x8877665544332211ull,
                      QStringLiteral("legacy-api cross-page update@20")) != 0) {
        return fail(QStringLiteral("legacy parser API returned wrong v17 array samples"));
    }

    std::cout << "nested_wave_array_parser_ok signals=" << wave.signalList.size()
              << " m00_samples=" << m00->samples.size()
              << " m01_samples=" << m01->samples.size()
              << " m10_samples=" << m10->samples.size()
              << " m11_samples=" << m11->samples.size()
              << " c011_samples=" << c011->samples.size()
              << " c100_samples=" << c100->samples.size()
              << "\n";
    return 0;
}
