#pragma once

#include <QtGlobal>

/*
 * WVZ3: append-only, crash-tolerant waveform container
 *
 * Design goals:
 *  1) Stream writing during simulation
 *  2) Commit one block at a time (e.g. every 100k cycles)
 *  3) Already committed blocks remain readable after kill -9 / hang kill
 *  4) Block-level compression
 *  5) Signal-subset decoding and time-window loading
 *  6) Optional shared time table inside each block for highly synchronous traces
 *  7) Current writer-side remove semantics are remove-as-Z; SignalRemoveDelta is kept for reader compatibility only
 *
 * Layout:
 *   [FileHeader][MetaBlock][SignalDirBase][Block0][Commit0][SignalDirDelta][Commit]...
 *
 * Reader trusts committed blocks found by scanning [dataRegionOffset, EOF).
 * Header/meta counts are advisory only and may be stale if the producer was killed.
 *
 * Z support revision:
 *   When Signal_SupportsZ is set on a signal, every record layout for that signal
 *   stores an extra per-sample isZ byte for the block initial value and for each
 *   transition/update entry, including shared-time and clock-delta encodings.
 */

namespace wvz3 {

constexpr quint32 kMagic = 0x335A5657u;       // "WVZ3" in little-endian memory
constexpr quint16 kVersionMajor = 3;
// v3.4 change:
//   Enc_BusTransitionsBytes / Enc_BusTransitionsBytesShared keep their
//   legacy text-payload semantics for backward compatibility.
//   New packed/raw byte payloads use Enc_BusTransitionsPackedBytes /
//   Enc_BusTransitionsPackedBytesShared.
constexpr quint16 kVersionMinor = 4;
constexpr quint32 kCommitMagic = 0x334D4357u; // "WCM3" in little-endian memory

enum GlobalFlags : quint32 {
    Global_LittleEndian = 1u << 0,
    Global_HasChecksum  = 1u << 1
};

enum CompressionType : quint8 {
    Comp_None = 0,
    Comp_Zlib = 1,
    Comp_Zstd = 2,
    Comp_Lz4  = 3
};

enum SignalKindOnDisk : quint8 {
    Sig_Bit     = 1,
    Sig_Bus     = 2,
    Sig_Int     = 3,
    Sig_UInt    = 4,
    Sig_Float32 = 5,
    Sig_Float64 = 6
};

enum LogicKindOnDisk : quint8 {
    Logic_TwoState  = 1,
    Logic_FourState = 2,
    Logic_Analog    = 3
};

enum DefaultRadixOnDisk : quint8 {
    Radix_Bin    = 1,
    Radix_Hex    = 2,
    Radix_Dec    = 3,
    Radix_Int    = 4,
    Radix_UInt   = 5,
    Radix_Float  = 6,
    Radix_Int64  = 7,
    Radix_UInt64 = 8,
    Radix_Double = 9
};

enum RecordEncoding : quint8 {
    Enc_BitTransitions            = 1,
    Enc_BusTransitionsU64         = 2,
    Enc_BusTransitionsBytes       = 3,
    Enc_ClockDeltaPattern         = 4,
    Enc_BitSharedTimeIndex        = 5,
    Enc_BusTransitionsU64Shared   = 6,
    Enc_BusTransitionsBytesShared = 7,
    Enc_BusTransitionsPackedBytes = 8,
    Enc_BusTransitionsPackedBytesShared = 9
};

enum SignalFlags : quint16 {
    Signal_None         = 0,
    Signal_Signed       = 1u << 0,
    Signal_ClockLike    = 1u << 1,
    Signal_Alias        = 1u << 2,
    Signal_Hidden       = 1u << 3,
    Signal_SupportsZ    = 1u << 4
};

enum BlockFlags : quint32 {
    Block_None        = 0,
    Block_HasChecksum = 1u << 0
};

enum BlockType : quint32 {
    BlockType_WaveData = 1u,
    BlockType_SignalDirDelta = 2u,
    BlockType_SignalRemoveDelta = 3u
};

#pragma pack(push, 1)

struct FileHeader {
    quint32 magic;
    quint16 versionMajor;
    quint16 versionMinor;

    quint32 globalFlags;
    quint32 headerSize;

    quint64 metaOffset;
    quint64 metaSize;

    quint64 signalDirOffset;
    quint64 signalDirSize;

    quint64 dataRegionOffset;
    quint64 dataRegionSizeHint;

    quint64 fileSizeHint;
    quint32 headerChecksum;
    quint32 reserved0;
};

struct MetaBlock {
    qint64 startTime;
    qint64 endTime;
    quint64 timescalePs;
    qint64 targetBlockSpan;

    quint32 signalCount;
    quint32 committedBlockCount;
    quint32 markerCount;
    quint32 reserved0;
};

struct SignalDirEntry {
    quint32 signalId;
    quint32 nameOffset;

    quint16 bitWidth;
    quint8  signalKind;
    quint8  logicKind;

    quint8  defaultRadix;
    quint8  preferredEncoding;
    quint16 signalFlags;

    quint64 reserved0;
};

struct DataBlockHeader {
    quint32 blockType;
    quint32 blockFlags;

    qint64 blockStartTime;
    qint64 blockEndTime;

    quint32 blockId;
    quint32 signalRecordCount;
    quint32 payloadUncompressedSize;
    quint32 payloadCompressedSize;
    quint32 checksum;
    quint32 reserved0;
};

struct BlockPayloadHeader {
    quint32 signalRecordCount;
    quint32 sharedTimeCount; // if 0, no shared time table follows
};

struct SignalDirDeltaPayloadHeader {
    quint32 signalCount;
    quint32 reserved0;
};

struct SignalDirDeltaEntry {
    SignalDirEntry dir;
    quint32 initialValueOffset;
    quint32 reserved0;
};

struct SignalRemoveDeltaPayloadHeader {
    quint32 signalCount;
    quint32 reserved0;
};

struct SignalRecordHeader {
    quint32 signalId;
    quint8  encoding;
    quint8  reserved0;
    quint16 reserved1;
    quint32 dataSize;
};

struct BlockCommitFooter {
    quint32 magic;
    quint16 versionMajor;
    quint16 versionMinor;

    quint32 blockId;
    quint32 reserved0;

    qint64 blockStartTime;
    qint64 blockEndTime;

    quint64 blockFileOffset;
    quint32 totalBlockBytes;
    quint32 checksum;
};

#pragma pack(pop)

} // namespace wvz3
