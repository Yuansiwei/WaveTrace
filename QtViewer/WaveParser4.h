#pragma once

#include "WaveTypes.h"

#include <QString>
#include <QVector>
#include <limits>

class WaveParser4 {
public:
    struct LoadOptions {
        // WVZ4 signal ids to decode. Empty means decode all unless
        // autoLoadFirstSignalCount >= 0.
        QVector<int> signalIds;
        qint64 timeStart = 0;
        qint64 timeEnd = std::numeric_limits<qint64>::max();
        bool loadAllIfWindowEmpty = true;

        // When true, outWave.signalList contains every signal definition from
        // NAME/NODE/SIGT even if its samples are not decoded in this call.
        // Unloaded signal entries have samplesLoaded=false and an empty samples vector.
        bool includeAllSignalDefinitions = false;

        // If signalIds is empty and this value is >= 0, load samples only for the
        // first N signal entries while still exposing the whole directory when
        // includeAllSignalDefinitions=true. Use 0 for directory-only load.
        int autoLoadFirstSignalCount = -1;

        // Directory-only WVZ4 opens can still prefetch LOD for the first visible
        // rows so wide views can defer raw sample loading.
        int autoLoadFirstSignalLodCount = -1;

        // Safety guard for very large files. 0 means unlimited. The count is
        // checked while WDAT samples are materialized, before appending to memory.
        quint64 maxDecodedSamples = 0;

        // New WVZ4 writer versions finalize files by writing FOOT and patching
        // footer_offset in the header. Keep this false in viewer paths so a
        // killed direct writer cannot be mistaken for a complete waveform.
        bool allowUnfinalized = false;
    };

    static bool loadFromFile(const QString& filePath,
                             WaveFile& outWave,
                             QString& error,
                             const LoadOptions& options = LoadOptions());
};
