#pragma once

#include "WaveTypes.h"
#include "WVZ3Format.h"

#include <QFile>
#include <QHash>
#include <QString>
#include <QVector>
#include <limits>

class WaveParser3 {
public:
    struct LoadOptions {
        // WVZ3 signal ids to decode. Empty means decode all unless autoLoadFirstSignalCount >= 0.
        QVector<int> signalIds;
        qint64 timeStart = 0;
        qint64 timeEnd = std::numeric_limits<qint64>::max();
        bool loadAllIfWindowEmpty = true;

        // When true, outWave.signalList contains every signal definition from the WVZ3
        // directory even if its samples are not decoded in this call. Unloaded signals
        // have WaveSignal::samplesLoaded=false and an empty samples vector.
        bool includeAllSignalDefinitions = false;

        // If signalIds is empty and this value is >= 0, load samples only for the
        // first N valid signal ids while still allowing includeAllSignalDefinitions
        // to expose the whole directory to the UI. Use 0 for directory-only load.
        int autoLoadFirstSignalCount = -1;
    };

    struct SaveOptions {
        quint64 timescalePs = 1000;          // 1ns
        qint64 targetBlockSpan = 100000;     // logical ticks per block
        wvz3::CompressionType compression = wvz3::Comp_Zstd;
        bool enableChecksum = false;          // reserved: current writer does not emit checksum data
        bool enableClockDeltaOptimization = true;
        quint64 clockHalfPeriodTolerance = 0;
        int minClockToggleCount = 6;
        bool forceDurableCommit = true;
        bool enableSharedTimeTable = true;
    };

    static bool saveToFile(const QString& filePath,
                           const WaveFile& wave,
                           QString& error,
                           const SaveOptions& options = SaveOptions());

    static bool loadFromFile(const QString& filePath,
                             WaveFile& outWave,
                             QString& error,
                             const LoadOptions& options = LoadOptions());
};

class Wvz3StreamWriter {
public:
    struct PendingChange {
        qint64 time = 0;
        QString value;
    };

    struct SignalDefinition {
        int signalId = -1;
        QString name;
        SignalKind kind = SignalKind::Bit;
        int width = 1;
        ValueRadix defaultRadix = ValueRadix::Bin;
        bool supportsZState = true;
        QString initialValue;
    };

    struct Options {
        quint64 timescalePs = 1000;          // 1ns
        qint64 targetBlockSpan = 100000;     // logical ticks per block
        wvz3::CompressionType compression = wvz3::Comp_Zstd;
        bool enableChecksum = false;          // reserved: current writer does not emit checksum data
        bool enableClockDeltaOptimization = true;
        quint64 clockHalfPeriodTolerance = 0;
        int minClockToggleCount = 6;
        bool forceDurableCommit = true;
        bool enableSharedTimeTable = true;
    };

    Wvz3StreamWriter();
    ~Wvz3StreamWriter();

    bool open(const QString& filePath,
              const QVector<SignalDefinition>& sigDefs,
              QString& error,
              const QString& title = QStringLiteral("stream_wave"),
              qint64 startCycle = 0,
              const Options& options = Options());

    bool addSignals(const QVector<SignalDefinition>& sigDefs, qint64 cycle, QString& error);
    // remove-as-Z: does not emit SignalRemoveDelta; writes a Z sample at cycle for each signal.
    bool deleteSignals(const QVector<int>& signalIds, qint64 cycle, QString& error);
    bool appendSample(int signalId, qint64 cycle, const QString& value, QString& error);
    bool flushUntil(qint64 currentCycle, QString& error);
    bool close(QString& error, bool flushPartialBlock = true);

    bool isOpen() const { return m_file.isOpen(); }

private:
    struct RuntimeSignalState {
        SignalDefinition def;
        QString blockInitialValue;
        QString currentValue;
        QVector<PendingChange> pending;
    };

    QFile m_file;
    QString m_title;
    Options m_options;
    qint64 m_startCycle = 0;
    qint64 m_currentBlockStart = 0;
    qint64 m_lastAppendedCycle = std::numeric_limits<qint64>::min();
    qint64 m_lastObservedCycle = std::numeric_limits<qint64>::min();
    bool m_haveAnySample = false;
    bool m_finalized = false;
    quint32 m_nextBlockId = 0;

    qint64 m_metaOffset = 0;
    qint64 m_signalDirOffset = 0;
    qint64 m_dataRegionOffset = 0;

    QVector<int> m_signalOrder;
    QHash<int, RuntimeSignalState> m_stateById;
    QHash<int, bool> m_knownSignalIds;

    bool writeInitialLayout(QString& error);
    bool appendSignalDirDeltaBlock(const QVector<SignalDefinition>& sigDefs, qint64 activationCycle, QString& error);
    // Kept only for backward-format compatibility experiments; deleteSignals() intentionally uses remove-as-Z.
    bool appendSignalRemoveDeltaBlock(const QVector<int>& signalIds, qint64 removalCycle, QString& error);
    bool commitBlock(qint64 blockEndExclusive, QString& error);
    bool rewriteFinalHeaderAndMeta(QString& error);
    void advanceToNextBlock(qint64 nextBlockStart);
    void resetState();
};
