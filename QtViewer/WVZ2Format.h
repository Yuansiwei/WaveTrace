#pragma once

#include <QtGlobal>
#include <QString>
#include <QByteArray>
#include <QVector>

/*
 * WVZ2: block-indexed waveform container format
 *
 * Design goals:
 *  1) Random access by time window
 *  2) Signal-subset decoding
 *  3) Integer timestamp on disk
 *  4) Block-level compression
 *  5) Clock-delta optimization for regular toggling signals
 *
 * This revision supports a whole-signal Z state via a per-sample isZ flag.
 *
 * This header defines the on-disk binary structures and helper enums only.
 * Reader/writer logic is implemented in WaveParser2.cpp.
 */

namespace wvz2 {

constexpr quint32 kMagic = 0x325A5657u; // "WVZ2" in little-endian memory
constexpr quint16 kVersionMajor = 2;
constexpr quint16 kVersionMinor = 2;

// ---------------------------------
// Global flags
// ---------------------------------
enum GlobalFlags : quint32 {
    Global_LittleEndian = 1u << 0,
    Global_HasSession   = 1u << 1,
    Global_HasChecksum  = 1u << 2,
    Global_HasHierarchy = 1u << 3,
    Global_HasMarkers   = 1u << 4
};

// ---------------------------------
// Compression at block level
// ---------------------------------
enum CompressionType : quint8 {
    Comp_None = 0,
    Comp_Zlib = 1,
    Comp_Zstd = 2,
    Comp_Lz4  = 3
};

// ---------------------------------
// Signal storage/data semantics
// ---------------------------------
enum SignalKindOnDisk : quint8 {
    Sig_Bit     = 1,
    Sig_Bus     = 2,
    Sig_Int     = 3,
    Sig_UInt    = 4,
    Sig_Float32 = 5,
    Sig_Float64 = 6
};

enum LogicKindOnDisk : quint8 {
    Logic_TwoState = 1,   // 0 / 1
    Logic_Analog   = 2
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

// ---------------------------------
// Signal record encoding
// ---------------------------------
enum RecordEncoding : quint8 {
    Enc_BitTransitions       = 1, // initial state + (dt, new_state)
    Enc_BusTransitionsU64    = 2, // width <= 64
    Enc_BusTransitionsBytes  = 3, // width > 64
    Enc_ClockDeltaPattern    = 4  // initial state + dt0 + half-period list/run
};

// Signal-level flags
enum SignalFlags : quint16 {
    Signal_None      = 0,
    Signal_Signed    = 1u << 0,
    Signal_ClockLike = 1u << 1,
    Signal_Alias     = 1u << 2,
    Signal_Hidden    = 1u << 3,
    Signal_SupportsZ = 1u << 4
};

// Per-block flags
enum BlockFlags : quint32 {
    Block_None          = 0,
    Block_HasChecksum   = 1u << 0
};

#pragma pack(push, 1)

// 64 bytes. Fixed-size header.
struct FileHeader {
    quint32 magic;               // kMagic
    quint16 versionMajor;        // 2
    quint16 versionMinor;        // 0

    quint32 globalFlags;         // GlobalFlags
    quint32 headerSize;          // sizeof(FileHeader)

    quint64 metaOffset;
    quint64 metaSize;

    quint64 signalDirOffset;
    quint64 signalDirSize;

    quint64 timeIndexOffset;
    quint64 timeIndexSize;

    quint64 sessionOffset;       // 0 if absent
    quint64 sessionSize;         // 0 if absent

    quint64 fileSize;
    quint32 headerChecksum;      // optional, can be 0 for now
    quint32 reserved0;
};

struct MetaBlock {
    qint64 startTime;
    qint64 endTime;
    quint64 timescalePs;         // e.g. 1000 => 1ns

    quint32 signalCount;
    quint32 scopeCount;
    quint32 blockCount;
    quint32 markerCount;
};

struct ScopeEntry {
    quint32 scopeId;
    quint32 parentScopeId;
    quint32 nameOffset;          // into string table
    quint16 scopeType;
    quint16 reserved0;
};

struct SignalDirEntry {
    quint32 signalId;
    quint32 scopeId;

    quint32 nameOffset;          // into string table
    quint16 bitWidth;
    quint8  signalKind;          // SignalKindOnDisk
    quint8  logicKind;           // LogicKindOnDisk

    quint8  defaultRadix;        // DefaultRadixOnDisk
    quint8  preferredEncoding;   // RecordEncoding
    quint16 signalFlags;         // SignalFlags

    quint64 firstBlockIndex;     // optional; 0 if unused
    quint64 reserved0;
};

struct TimeIndexEntry {
    qint64 startTime;
    qint64 endTime;

    quint64 blockFileOffset;
    quint32 compressedSize;
    quint32 uncompressedSize;

    quint32 blockId;
    quint32 checksum;
};

struct DataBlockHeader {
    quint32 blockType;           // 1 => waveform data
    quint32 blockFlags;          // BlockFlags

    qint64 blockStartTime;
    qint64 blockEndTime;

    quint32 signalRecordCount;
    quint32 payloadUncompressedSize;
    quint32 payloadCompressedSize;
    quint32 checksum;            // payload checksum if enabled
};

struct BlockPayloadHeader {
    quint32 signalRecordCount;
};

// Followed by record bytes inside a decompressed payload
struct SignalRecordHeader {
    quint32 signalId;
    quint8  encoding;            // RecordEncoding
    quint8  reserved0;
    quint16 reserved1;
    quint32 dataSize;            // bytes after this header
};

#pragma pack(pop)

// ---------------------------------
// In-memory helper types used by the parser skeleton
// ---------------------------------
struct DecodedTransitionBit {
    qint64 time = 0;
    quint8 state = 0;            // 0/1
};

struct DecodedTransitionBusU64 {
    qint64 time = 0;
    quint64 value = 0;
};

struct ClockPatternRun {
    qint64 firstEdgeTime = 0;   // absolute time of first edge within block
    quint64 halfPeriod = 0;      // dt between toggles
    quint32 toggleCount = 0;     // number of toggles in this run
    quint8 initialState = 0;     // state before first toggle
};

} // namespace wvz2
