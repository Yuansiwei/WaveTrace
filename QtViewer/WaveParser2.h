#pragma once

#include "WaveTypes.h"
#include "WVZ2Format.h"

#include <QHash>
#include <QString>
#include <QVector>
#include <limits>

class QIODevice;

class WaveParser2 {
public:
    struct LoadOptions {
        QVector<int> signalIds;
        qint64 timeStart = 0;
        qint64 timeEnd = std::numeric_limits<qint64>::max();
        bool loadAllIfWindowEmpty = true;
    };

    struct SaveOptions {
        quint64 timescalePs = 1000;          // 1ns
        qint64 targetBlockSpan = 100000;    // logical ticks per block
        wvz2::CompressionType compression = wvz2::Comp_Zstd;
        bool enableChecksum = false;
        bool enableClockDeltaOptimization = true;
        quint64 clockHalfPeriodTolerance = 0;
        int minClockToggleCount = 6;
    };

    static bool saveToFile(const QString& filePath,
                           const WaveFile& wave,
                           QString& error,
                           const SaveOptions& options = SaveOptions());

    static bool loadFromFile(const QString& filePath,
                             WaveFile& outWave,
                             QString& error,
                             const LoadOptions& options = LoadOptions());

public:
    struct IndexedSignal {
        int signalIndex = -1;
        quint32 signalId = 0;
        QString name;
        int width = 1;
        SignalKind kind = SignalKind::Bit;
        ValueRadix radix = ValueRadix::Bin;
        bool supportsZState = false;
        QVector<WaveSample> samples;
    };

private:
    struct BlockBuildSignalRecord {
        quint32 signalId = 0;
        quint8 encoding = 0;
        QByteArray encoded;
    };

    struct BlockBuildResult {
        qint64 blockStart = 0;
        qint64 blockEnd = 0;
        QVector<BlockBuildSignalRecord> records;
    };

    static qint64 timeToTicks(qint64 t, quint64 timescalePs);
    static qint64 ticksToTime(qint64 ticks, quint64 timescalePs);

    static quint8 encodeBitState(const QString& s);
    static QString decodeBitState(quint8 state);
    static quint64 parseValueU64(const QString& raw);
    static QByteArray encodeWideValue(const QString& raw, int width);
    static QString decodeWideValue(const QByteArray& raw, int width);

    static QByteArray encodeVarUInt(quint64 v);
    static bool decodeVarUInt(const QByteArray& data, int& pos, quint64& out);

    static QVector<IndexedSignal> buildIndexedSignals(const WaveFile& wave);
    static QVector<BlockBuildResult> buildBlocks(const QVector<IndexedSignal>& indexedSignals,
                                                 quint64 timescalePs,
                                                 qint64 blockSpanTicks,
                                                 const SaveOptions& options);

    static bool buildBitRecord(const IndexedSignal& sig,
                               qint64 blockStart,
                               qint64 blockEnd,
                               QByteArray& outEncoded);

    static bool buildBusU64Record(const IndexedSignal& sig,
                                  qint64 blockStart,
                                  qint64 blockEnd,
                                  QByteArray& outEncoded);

    static bool buildBusBytesRecord(const IndexedSignal& sig,
                                    qint64 blockStart,
                                    qint64 blockEnd,
                                    QByteArray& outEncoded);

    static bool tryBuildClockRecord(const IndexedSignal& sig,
                                    qint64 blockStart,
                                    qint64 blockEnd,
                                    const SaveOptions& options,
                                    QByteArray& outEncoded);

    static QByteArray compressPayload(const QByteArray& raw,
                                      wvz2::CompressionType comp,
                                      QString& error);
    static QByteArray decompressPayload(const QByteArray& raw,
                                        wvz2::CompressionType comp,
                                        QString& error);

    static bool writeHeaderAndBlocks(QIODevice& dev,
                                     const WaveFile& wave,
                                     const QVector<IndexedSignal>& indexed,
                                     const QVector<BlockBuildResult>& blocks,
                                     const SaveOptions& options,
                                     QString& error);

    static bool readHeader(QIODevice& dev,
                           wvz2::FileHeader& header,
                           QString& error);

    static bool decodeBlockPayload(const QByteArray& payload,
                                   const QHash<quint32, wvz2::SignalDirEntry>& sigDir,
                                   qint64 blockStart,
                                   qint64 blockEnd,
                                   quint64 timescalePs,
                                   const LoadOptions& options,
                                   QHash<quint32, QVector<WaveSample>>& outSamples,
                                   QString& error);

    static bool decodeBitRecord(const QByteArray& encoded,
                                qint64 blockStart,
                                qint64 blockEnd,
                                quint64 timescalePs,
                                bool supportsZ,
                                QVector<WaveSample>& outSamples);

    static bool decodeBusU64Record(const QByteArray& encoded,
                                   qint64 blockStart,
                                   qint64 blockEnd,
                                   quint64 timescalePs,
                                   int bitWidth,
                                   bool supportsZ,
                                   QVector<WaveSample>& outSamples);

    static bool decodeBusBytesRecord(const QByteArray& encoded,
                                     qint64 blockStart,
                                     qint64 blockEnd,
                                     quint64 timescalePs,
                                     int bitWidth,
                                     bool supportsZ,
                                     QVector<WaveSample>& outSamples);

    static bool decodeClockRecord(const QByteArray& encoded,
                                  qint64 blockStart,
                                  qint64 blockEnd,
                                  quint64 timescalePs,
                                  bool supportsZ,
                                  QVector<WaveSample>& outSamples);
};
