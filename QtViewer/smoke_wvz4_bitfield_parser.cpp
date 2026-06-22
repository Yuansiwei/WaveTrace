#include "WaveParser4.h"

#include <QCoreApplication>

#include <iostream>
#include <limits>

namespace {

const WaveSignal* find_signal(const WaveFile& wave, int signal_id) {
    for (const WaveSignal& signal : wave.signalList) {
        if (signal.signalId == signal_id) return &signal;
    }
    return nullptr;
}

bool expect_layout(const WaveFile& wave,
                   int signal_id,
                   int storage_id,
                   int bit_offset,
                   int bit_width) {
    const WaveSignal* signal = find_signal(wave, signal_id);
    if (!signal) {
        std::cerr << "missing signal_id " << signal_id << "\n";
        return false;
    }
    if (signal->storageId != storage_id ||
        signal->bitOffset != bit_offset ||
        signal->width != bit_width) {
        std::cerr << "bad layout for signal_id " << signal_id
                  << ": storage=" << signal->storageId
                  << " offset=" << signal->bitOffset
                  << " width=" << signal->width << "\n";
        return false;
    }
    return true;
}

bool expect_last_raw(const WaveFile& wave, int signal_id, quint64 expected) {
    const WaveSignal* signal = find_signal(wave, signal_id);
    if (!signal) {
        std::cerr << "missing signal_id " << signal_id << "\n";
        return false;
    }
    if (signal->samples.isEmpty()) {
        std::cerr << "no samples for signal_id " << signal_id << "\n";
        return false;
    }
    const WaveSample& sample = signal->samples.last();
    if (!sample.rawFieldsReady || sample.isZ || sample.isAbsent || sample.rawBits != expected) {
        std::cerr << "bad last rawBits for signal_id " << signal_id
                  << ": got=" << sample.rawBits
                  << " expected=" << expected << "\n";
        return false;
    }
    return true;
}

bool expect_lod_last_raw(const WaveFile& wave, int signal_id, quint64 expected) {
    const WaveSignal* signal = find_signal(wave, signal_id);
    if (!signal) {
        std::cerr << "missing signal_id " << signal_id << "\n";
        return false;
    }
    if (signal->lodLevels.isEmpty()) {
        std::cerr << "no LOD levels for signal_id " << signal_id << "\n";
        return false;
    }

    const quint64 mask = signal->width >= 64 ? ~quint64(0) : ((quint64(1) << signal->width) - 1u);
    bool saw_lod_sample = false;
    qint64 last_time = std::numeric_limits<qint64>::min();
    quint64 last_raw = 0;
    for (const WaveLodLevel& level : signal->lodLevels) {
        for (const WaveSample& sample : level.samples) {
            if (!sample.rawFieldsReady || sample.isZ || sample.isAbsent) {
                std::cerr << "bad LOD sample state for signal_id " << signal_id << "\n";
                return false;
            }
            if ((sample.rawBits & ~mask) != 0) {
                std::cerr << "unsliced LOD sample for signal_id " << signal_id
                          << ": rawBits=" << sample.rawBits
                          << " width=" << signal->width << "\n";
                return false;
            }
            saw_lod_sample = true;
            if (sample.time >= last_time) {
                last_time = sample.time;
                last_raw = sample.rawBits;
            }
        }
        for (const WaveLodBucket& bucket : level.buckets) {
            if ((bucket.firstRawBits & ~mask) != 0 ||
                (bucket.lastRawBits & ~mask) != 0 ||
                (bucket.minRawBits & ~mask) != 0 ||
                (bucket.maxRawBits & ~mask) != 0) {
                std::cerr << "unsliced LOD bucket for signal_id " << signal_id << "\n";
                return false;
            }
        }
    }
    if (!saw_lod_sample) {
        std::cerr << "no LOD samples for signal_id " << signal_id << "\n";
        return false;
    }
    if (last_raw != expected) {
        std::cerr << "bad last LOD rawBits for signal_id " << signal_id
                  << ": got=" << last_raw
                  << " expected=" << expected
                  << " time=" << last_time << "\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::cerr << "usage: smoke_wvz4_bitfield_parser <file.wvz4>\n";
        return 2;
    }

    QString error;
    WaveFile wave;
    WaveParser4::LoadOptions full;
    full.includeAllSignalDefinitions = true;
    full.autoLoadFirstSignalCount = -1;
    if (!WaveParser4::loadFromFile(QString::fromLocal8Bit(argv[1]), wave, error, full)) {
        std::cerr << "full load failed: " << error.toLocal8Bit().constData() << "\n";
        return 3;
    }

    if (wave.signalList.size() != 5) {
        std::cerr << "expected 5 visible bitfield signals, got " << wave.signalList.size() << "\n";
        return 4;
    }

    if (!expect_layout(wave, 2, 1, 0, 4) ||
        !expect_layout(wave, 3, 1, 4, 4) ||
        !expect_layout(wave, 4, 1, 8, 8) ||
        !expect_layout(wave, 5, 1, 4, 8) ||
        !expect_layout(wave, 6, 1, 15, 1)) {
        return 5;
    }

    if (!expect_last_raw(wave, 2, 0xFu) ||
        !expect_last_raw(wave, 3, 0xAu) ||
        !expect_last_raw(wave, 4, 0xE1u) ||
        !expect_last_raw(wave, 5, 0x1Au) ||
        !expect_last_raw(wave, 6, 1u)) {
        return 6;
    }

    if (!expect_lod_last_raw(wave, 2, 0xFu) ||
        !expect_lod_last_raw(wave, 3, 0xAu) ||
        !expect_lod_last_raw(wave, 4, 0xE1u) ||
        !expect_lod_last_raw(wave, 5, 0x1Au) ||
        !expect_lod_last_raw(wave, 6, 1u)) {
        return 7;
    }

    WaveFile selected;
    WaveParser4::LoadOptions onDemand;
    onDemand.signalIds.push_back(5);
    onDemand.signalIds.push_back(6);
    onDemand.includeAllSignalDefinitions = false;
    onDemand.loadAllIfWindowEmpty = false;
    if (!WaveParser4::loadFromFile(QString::fromLocal8Bit(argv[1]), selected, error, onDemand)) {
        std::cerr << "on-demand load failed: " << error.toLocal8Bit().constData() << "\n";
        return 8;
    }
    if (selected.signalList.size() != 2 ||
        !expect_last_raw(selected, 5, 0x1Au) ||
        !expect_last_raw(selected, 6, 1u)) {
        std::cerr << "on-demand bitfield alias load failed\n";
        return 9;
    }

    return 0;
}
