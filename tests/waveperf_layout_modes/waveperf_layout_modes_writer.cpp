#include <wvz4_writer_typed.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

bool gAttachNumericIndexSegments = false;

#define WAVEPERF_INSTRUCTION_FEATURE(key, field, group) field,
constexpr const char* kInstructionFeatureFields[] = {
#include "../../QtViewer/WavePerfInstructionFeatures.def"
};
#undef WAVEPERF_INSTRUCTION_FEATURE
constexpr std::size_t kInstructionFeatureCount =
    sizeof(kInstructionFeatureFields) /
    sizeof(kInstructionFeatureFields[0]);

struct TempNode {
    std::string name;
    wvz4::NodeKind kind = wvz4::NodeKind::Object;
    std::uint32_t parent = 0;
    std::vector<std::uint32_t> children;
};

class LayoutBuilder {
public:
    LayoutBuilder() {
        TempNode root;
        root.name = "gpu";
        root.kind = wvz4::NodeKind::Root;
        nodes_.push_back(root);
    }

    std::uint32_t addSignal(const std::vector<std::string>& segments,
                            wvz4::ValueType type,
                            std::uint32_t bitWidth,
                            wvz4::Radix radix) {
        std::uint32_t parent = 0;
        for (std::size_t i = 0; i < segments.size(); ++i) {
            const bool leaf = i + 1u == segments.size();
            std::uint32_t child = findChild(parent, segments[i]);
            if (child == kMissing) {
                TempNode node;
                node.name = segments[i];
                node.parent = parent;
                node.kind = leaf ? wvz4::NodeKind::SignalLeaf
                                 : (segments[i].front() == '['
                                        ? wvz4::NodeKind::ArrayElem
                                        : wvz4::NodeKind::Object);
                child = static_cast<std::uint32_t>(nodes_.size());
                nodes_.push_back(node);
                nodes_[parent].children.push_back(child);
            }
            parent = child;
        }

        SignalInfo signal;
        signal.node = parent;
        signal.type = type;
        signal.bitWidth = bitWidth;
        signal.radix = radix;
        signals_.push_back(signal);
        return static_cast<std::uint32_t>(signals_.size());
    }

    wvz4::Layout finish() const {
        wvz4::Layout layout;
        for (std::uint32_t i = 0; i < nodes_.size(); ++i) {
            const TempNode& source = nodes_[i];
            const std::uint32_t id = i + 1u;
            wvz4::add_layout_name_blob_record(
                layout, id, source.name.data(),
                static_cast<std::uint32_t>(source.name.size()));

            wvz4::NodeRecord node;
            node.node_id = id;
            node.parent_id = i == 0 ? 0u : source.parent + 1u;
            node.name_id = id;
            node.kind = source.kind;
            node.first_child =
                source.children.empty() ? 0u : source.children.front() + 1u;
            if (i != 0) {
                const std::vector<std::uint32_t>& siblings =
                    nodes_[source.parent].children;
                for (std::size_t s = 0; s + 1u < siblings.size(); ++s) {
                    if (siblings[s] == i) {
                        node.next_sibling = siblings[s + 1u] + 1u;
                        break;
                    }
                }
            }
            layout.nodes.push_back(node);
        }

        for (std::uint32_t i = 0; i < signals_.size(); ++i) {
            const SignalInfo& source = signals_[i];
            wvz4::SignalDefinition signal;
            signal.signal_id = i + 1u;
            signal.node_id = source.node + 1u;
            signal.type = source.type;
            signal.bit_width = source.bitWidth;
            signal.radix = source.radix;
            layout.signals.push_back(signal);
        }
        return layout;
    }

private:
    struct SignalInfo {
        std::uint32_t node = 0;
        wvz4::ValueType type = wvz4::ValueType::Bool;
        std::uint32_t bitWidth = 1;
        wvz4::Radix radix = wvz4::Radix::Bin;
    };

    static constexpr std::uint32_t kMissing = ~std::uint32_t(0);

    std::uint32_t findChild(std::uint32_t parent,
                            const std::string& name) const {
        for (std::uint32_t child : nodes_[parent].children) {
            if (nodes_[child].name == name) return child;
        }
        return kMissing;
    }

    std::vector<TempNode> nodes_;
    std::vector<SignalInfo> signals_;
};

struct IssueSignals {
    std::uint32_t valid[2] = {};
    std::uint32_t issueType[2] = {};
    std::array<std::array<std::uint32_t,
                          kInstructionFeatureCount>, 2> features = {};
    std::uint32_t sgId[2] = {};
    std::uint32_t pc[2] = {};
};

struct SchedulerSignals {
    std::uint32_t valid = 0;
    std::uint32_t queueCount = 0;
    std::uint32_t stall = 0;
    std::uint32_t sleep = 0;
    std::uint32_t flow = 0;
    std::uint32_t barrier = 0;
    std::uint32_t setMaxTemp = 0;
    std::uint32_t inflightMemory = 0;
    std::uint32_t dependencies[7] = {};
    std::uint32_t queueHeadIndex = 0;
    std::uint32_t queueHeadPc = 0;
    std::uint32_t queueHeadIssueType = 0;
    std::uint32_t queueHeadThreadSubtype = 0;
    std::uint32_t queueHeadExeUnit = 0;
    std::array<std::uint32_t,
               kInstructionFeatureCount> queueHeadFeatures = {};
    std::uint32_t functionUnitPending[3] = {};
};

bool instructionFeatureValue(const char* field,
                             std::size_t qppu,
                             int slot,
                             bool latencyCase) {
    const bool barrier = qppu == 0u && slot == 1;
    const bool branch = qppu == 1u && slot == 0;
    const bool exit = qppu == 1u && slot == 1;
    const bool flowControl = branch || exit;
    const bool fence = qppu == 0u && slot == 0;
    const bool globalMemory =
        latencyCase && qppu == 0u && slot == 0;
    const bool ldmb = globalMemory;
    const bool stmb = qppu == 2u && slot == 0;
    const bool movrel = qppu == 3u && slot == 0;
    const std::string name(field);
    if (name == "MOVRELSrc1IsGR" || name == "isMOVREL") return movrel;
    if (name == "allThreadExit" || name == "isExit") return exit;
    if (name == "clause") return slot == 0;
    if (name == "ebb") return slot == 1;
    if (name == "isBarrier") return barrier;
    if (name == "isBranch" || name == "is_relative_pc") return branch;
    if (name == "isFlowCtrl" || name == "toFastFCU") return flowControl;
    if (name == "isFence") return fence;
    if (name == "isGLoad") return slot == 1;
    if (name == "isGlobalMem" || name == "isLdgStl" ||
        name == "isWaitDepCnt" || name == "rdChkDepCntVld" ||
        name == "wrChkDepCntVld") {
        return globalMemory;
    }
    if (name == "isLDMB") return ldmb;
    if (name == "isMemBarrier") return barrier || fence;
    if (name == "isSTMB") return stmb;
    if (name == "needFastFcuCheck") return barrier || flowControl;
    return false;
}

struct BandwidthSignals {
    std::uint32_t l1WriteValid = 0;
    std::uint32_t l1WriteTaken = 0;
    std::uint32_t l1StoreMask = 0;
    std::uint32_t l1AtomicMask = 0;
    std::uint32_t l1ReadValid = 0;
    std::uint32_t l1ReadReady = 0;
    std::uint32_t l1ReadMask = 0;
    std::uint32_t l2WriteValid = 0;
    std::uint32_t l2WriteMask = 0;
    std::uint32_t l2ReadValid = 0;
    std::uint32_t l2ReadSectorValid = 0;
    std::uint32_t l2ReadMask = 0;
};

struct ResourceSignals {
    std::uint32_t fifoOccupancy = 0;
    std::uint32_t fifoCapacity = 0;
    std::uint32_t fifoReads = 0;
    std::uint32_t fifoWrites = 0;
    std::uint32_t queueOccupancy = 0;
    std::uint32_t queueCapacity = 0;
    std::uint32_t availableCredit = 0;
    std::uint32_t cacheHit = 0;
    std::uint32_t cacheMiss = 0;
};

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin < path.size()) {
        const std::size_t end = path.find('.', begin);
        const std::string segment = path.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        const bool numericIndex =
            segment.size() >= 3u && segment.front() == '[' &&
            segment.back() == ']' &&
            segment.find_first_not_of("0123456789", 1u) ==
                segment.size() - 1u;
        if (gAttachNumericIndexSegments && numericIndex && !result.empty()) {
            result.back() += segment;
        } else {
            result.push_back(segment);
        }
        if (end == std::string::npos) break;
        begin = end + 1u;
    }
    return result;
}

IssueSignals addIssueSignals(LayoutBuilder& builder,
                             const std::string& qppuPath,
                             bool flattened) {
    IssueSignals result;
    const std::string issueArray =
        flattened ? "issue_inst_[size=2]" : "issue_inst_";
    for (int slot = 0; slot < 2; ++slot) {
        const std::string root = qppuPath + ".m_QPPUCtrl." + issueArray +
                                 ".[" + std::to_string(slot) + "]";
        result.valid[slot] = builder.addSignal(
            splitPath(root + ".vld"), wvz4::ValueType::Bool, 1u,
            wvz4::Radix::Bin);
        result.issueType[slot] = builder.addSignal(
            splitPath(root + ".preDecode.instIssueType"),
            wvz4::ValueType::U32, 32u, wvz4::Radix::Dec);
        auto addFeature = [&](const char* field) {
            return builder.addSignal(
                splitPath(root + ".preDecode." + field),
                wvz4::ValueType::Bool, 1u, wvz4::Radix::Bin);
        };
        for (std::size_t feature = 0;
             feature < kInstructionFeatureCount; ++feature) {
            result.features[slot][feature] =
                addFeature(kInstructionFeatureFields[feature]);
        }
        result.sgId[slot] = builder.addSignal(
            splitPath(root + ".sgId"),
            wvz4::ValueType::U32, 4u, wvz4::Radix::Dec);
        result.pc[slot] = builder.addSignal(
            splitPath(root + ".PC.pc_"),
            wvz4::ValueType::U32, 32u, wvz4::Radix::Hex);
    }
    return result;
}

SchedulerSignals addSchedulerSignals(LayoutBuilder& builder,
                                     const std::string& qppuPath,
                                     bool flattened) {
    SchedulerSignals result;
    const std::string sgArray =
        flattened ? "sg_table_[size=16].[0]"
                  : "sg_table_.[0]";
    const std::string queueArray =
        flattened ? "instr_queue_[size=16].[0]"
                  : "instr_queue_.[0]";
    const std::string vectorSuffix =
        flattened ? "[size=16].[0]" : ".[0]";
    const std::string root = qppuPath + ".m_QPPUCtrl.";
    auto addBool = [&](const std::string& path) {
        return builder.addSignal(splitPath(path), wvz4::ValueType::Bool,
                                 1u, wvz4::Radix::Bin);
    };
    auto addU32 = [&](const std::string& path, std::uint32_t width) {
        return builder.addSignal(splitPath(path), wvz4::ValueType::U32,
                                 width, wvz4::Radix::Dec);
    };
    result.valid = addBool(root + sgArray + ".valid");
    result.queueCount = addU32(root + queueArray + ".m_count", 4u);
    result.queueHeadIndex =
        addU32(root + queueArray + ".m_ri", 2u);
    const std::string queueData =
        flattened
            ? root +
                  "instr_queue_[size=16].[0].m_QData[size=4].[0].Data"
            : root + "instr_queue_.[0].m_QData.[0].Data";
    result.queueHeadPc =
        addU32(queueData + ".PC.pc_", 32u);
    result.queueHeadIssueType =
        addU32(queueData + ".preDecode.instIssueType", 4u);
    result.queueHeadThreadSubtype =
        addU32(queueData + ".preDecode.instType.subType.thread", 4u);
    result.queueHeadExeUnit =
        addU32(queueData + ".preDecode.exeThdUnit", 4u);
    for (std::size_t feature = 0;
         feature < kInstructionFeatureCount; ++feature) {
        result.queueHeadFeatures[feature] = addBool(
            queueData + ".preDecode." +
            kInstructionFeatureFields[feature]);
    }
    result.stall =
        addU32(root + "stall_cnt_vector_" + vectorSuffix, 4u);
    result.sleep =
        addU32(root + "sleep_cnt_vector_" + vectorSuffix, 4u);
    result.flow =
        addBool(root + "flow_ctrl_pend_wait_vector_" + vectorSuffix);
    result.barrier =
        addBool(root + "barrier_pend_wait_vector_" + vectorSuffix);
    result.setMaxTemp =
        addBool(root + "set_max_temp_pend_wait_vector_" + vectorSuffix);
    result.inflightMemory =
        addU32(root + "inflight_mem_cnt_" + vectorSuffix, 4u);
    for (int dependency = 0; dependency < 7; ++dependency) {
        const std::string path =
            flattened
                ? root +
                      "check_dep_cnt_vector_[size=16].[0][size=7].[" +
                      std::to_string(dependency) + "]"
                : root + "check_dep_cnt_vector_.[0].[" +
                      std::to_string(dependency) + "]";
        result.dependencies[dependency] = addU32(path, 4u);
    }
    for (int unit = 0; unit < 3; ++unit) {
        const std::string path =
            flattened
                ? root + "function_unit_pend_wait_vector_[size=3].[" +
                      std::to_string(unit) + "]"
                : root + "function_unit_pend_wait_vector_.[" +
                      std::to_string(unit) + "]";
        result.functionUnitPending[unit] = addU32(path, 4u);
    }
    return result;
}

ResourceSignals addResourceSignals(LayoutBuilder& builder,
                                   const std::string& qppuPath) {
    ResourceSignals result;
    const std::string root = qppuPath + ".m_QPPUCtrl.";
    auto addU32 = [&](const std::string& path, std::uint32_t width) {
        return builder.addSignal(splitPath(path), wvz4::ValueType::U32,
                                 width, wvz4::Radix::Dec);
    };
    result.fifoOccupancy =
        addU32(root + "test_fifo.m_num_readable", 4u);
    result.fifoCapacity =
        addU32(root + "test_fifo.m_size", 4u);
    result.fifoReads =
        addU32(root + "test_fifo.m_num_read", 4u);
    result.fifoWrites =
        addU32(root + "test_fifo.m_num_written", 4u);
    result.queueOccupancy =
        addU32(root + "test_queue.m_count", 4u);
    result.queueCapacity =
        addU32(root + "test_queue.m_size", 4u);
    result.availableCredit =
        addU32(root + "mma_ldMb_credit_cnt_", 4u);
    result.cacheHit =
        addU32(root + "test_cache.cache_hit", 1u);
    result.cacheMiss =
        addU32(root + "test_cache.cache_miss", 1u);
    return result;
}

BandwidthSignals addBandwidthSignals(LayoutBuilder& builder,
                                     bool flattened) {
    BandwidthSignals result;
    const std::string cluster =
        flattened ? "m_clusters[size=2].[0]."
                  : "m_clusters.[0].";
    const std::string l1Write =
        cluster +
        (flattened
             ? "chDls2L1lstxReq[size=5].[0][size=2].[0][size=2].[0]"
             : "chDls2L1lstxReq.[0].[0].[0]");
    const std::string l1Taken =
        cluster +
        (flattened
             ? "chDls2L1lstxTaken[size=5].[0][size=2].[0][size=2].[0]"
             : "chDls2L1lstxTaken.[0].[0].[0]");
    const std::string l1Read =
        cluster +
        (flattened
             ? "chL1lstx2DlsReq[size=5].[0][size=2].[0][size=2].[0]"
             : "chL1lstx2DlsReq.[0].[0].[0]");
    const std::string l1Ready =
        cluster +
        (flattened
             ? "chDls2L1lstxReqRdy[size=5].[0][size=2].[0][size=2].[0]"
             : "chDls2L1lstxReqRdy.[0].[0].[0]");
    const std::string l2Write =
        cluster +
        (flattened
             ? "chUscTxArb2L2CacheWrData[size=5].[0][size=2].[0]"
             : "chUscTxArb2L2CacheWrData.[0].[0]");
    const std::string l2Read =
        cluster +
        (flattened
             ? "chL2Cache2UscTxArbRtnDataIn[size=5].[0][size=2].[0]"
             : "chL2Cache2UscTxArbRtnDataIn.[0].[0]");

    auto addBool = [&](const std::string& path) {
        return builder.addSignal(splitPath(path), wvz4::ValueType::Bool,
                                 1u, wvz4::Radix::Bin);
    };
    auto addU32 = [&](const std::string& path, std::uint32_t width) {
        return builder.addSignal(splitPath(path), wvz4::ValueType::U32,
                                 width, wvz4::Radix::Hex);
    };

    result.l1WriteValid = addBool(l1Write + ".valid");
    result.l1WriteTaken = addBool(l1Taken);
    result.l1StoreMask =
        addU32(l1Write + ".req_packet.store_req.smask", 8u);
    result.l1AtomicMask =
        addU32(l1Write + ".req_packet.atomic_req.smask", 8u);
    result.l1ReadValid = addBool(l1Read + ".valid");
    result.l1ReadReady = addBool(l1Ready);
    result.l1ReadMask =
        addU32(l1Read + ".req_packet.load_rrb.smask", 8u);
    result.l2WriteValid =
        addU32(l2Write + ".vld[size=1].[0]", 8u);
    result.l2WriteMask =
        addU32(l2Write + ".wmask[size=1].[0]", 32u);
    result.l2ReadValid = addBool(l2Read + ".vld");
    result.l2ReadSectorValid =
        addBool(l2Read + ".sector[size=1].[0].vld");
    result.l2ReadMask =
        addU32(l2Read + ".sector[size=1].[0].mask", 8u);
    return result;
}

bool appendBool(wvz4::CycleSubmission& submission,
                std::uint32_t signalId,
                bool value) {
    const std::uint8_t raw = value ? 1u : 0u;
    return submission.append_grouped_raw(1u, signalId, &raw);
}

bool appendU32(wvz4::CycleSubmission& submission,
               std::uint32_t signalId,
               std::uint32_t value) {
    return submission.append_grouped_raw(4u, signalId, &value);
}

bool writeFile(const std::string& output,
               bool flattened,
               bool latencyCase,
               bool windowedCase) {
    LayoutBuilder builder;
    std::vector<IssueSignals> qppus;
    std::vector<SchedulerSignals> schedulers;
    std::vector<ResourceSignals> resources;
    if (flattened) {
        const std::string qppuPath =
            "m_clusters[size=2].[0].m_dppu[size=5].[0]."
            "m_ppu[size=2].[0].m_QPPUTOP[size=4].[0]";
        qppus.push_back(addIssueSignals(
            builder, qppuPath, true));
        schedulers.push_back(addSchedulerSignals(
            builder, qppuPath, true));
        resources.push_back(addResourceSignals(builder, qppuPath));
    } else {
        for (int qppu = 0; qppu < 4; ++qppu) {
            const std::string qppuPath =
                "m_clusters.[0].m_dppu.[0].m_ppu.[0].m_QPPUTOP.[" +
                std::to_string(qppu) + "]";
            qppus.push_back(addIssueSignals(
                builder, qppuPath, false));
            schedulers.push_back(addSchedulerSignals(
                builder, qppuPath, false));
            resources.push_back(addResourceSignals(builder, qppuPath));
        }
    }
    const BandwidthSignals bandwidth =
        addBandwidthSignals(builder, flattened);

    wvz4::WriterOptions options;
    options.compression = wvz4::Compression::Zstd;
    options.zstd_level = 1;
    options.enable_block_pipeline = false;
    options.enable_lod_tables = true;
    options.lod_bucket_cycle_scale = 10u;
    options.target_block_span = 256u;

    std::string error;
    wvz4::Writer writer;
    if (!writer.open(output, builder.finish(), options, error)) {
        std::cerr << error << '\n';
        return false;
    }

    // One business cycle is ten WVZ4 ticks.
    for (std::uint32_t cycle = 0; cycle <= 100u; ++cycle) {
        wvz4::CycleSubmission submission;
        submission.cycle = static_cast<wvz4::i64>(cycle) * 10;
        if (cycle == 0u) {
            if (!appendBool(submission, bandwidth.l1WriteValid, true) ||
                !appendBool(submission, bandwidth.l1WriteTaken, true) ||
                !appendU32(submission, bandwidth.l1StoreMask,
                           latencyCase ? 0u : 0x3u) ||
                !appendU32(submission, bandwidth.l1AtomicMask, 0u) ||
                !appendBool(submission, bandwidth.l1ReadValid, false) ||
                !appendBool(submission, bandwidth.l1ReadReady, true) ||
                !appendU32(submission, bandwidth.l1ReadMask, 0xfu) ||
                !appendU32(submission, bandwidth.l2WriteValid, 0xffu) ||
                !appendU32(submission, bandwidth.l2WriteMask, 0xffffu) ||
                !appendBool(submission, bandwidth.l2ReadValid, true) ||
                !appendBool(submission,
                            bandwidth.l2ReadSectorValid, false) ||
                !appendU32(submission, bandwidth.l2ReadMask,
                           0xffu)) {
                return false;
            }
        }
        if (cycle == (latencyCase ? 1u : 10u) &&
            !appendBool(submission, bandwidth.l1WriteValid, false)) {
            return false;
        }
        if (cycle == 20u) {
            if (!appendBool(submission, bandwidth.l1ReadValid, true) ||
                !appendU32(submission, bandwidth.l2WriteValid, 0u)) {
                return false;
            }
        }
        if (cycle == 30u &&
            !appendBool(submission,
                        bandwidth.l2ReadSectorValid, true)) {
            return false;
        }
        if (cycle == (latencyCase ? 21u : 40u) &&
            !appendBool(submission, bandwidth.l1ReadValid, false)) {
            return false;
        }
        if (cycle == 60u &&
            !appendBool(submission,
                        bandwidth.l2ReadSectorValid, false)) {
            return false;
        }
        for (std::size_t qppu = 0; qppu < qppus.size(); ++qppu) {
            const std::uint32_t slot0End[] = {
                latencyCase ? 1u : 80u,
                latencyCase ? 0u : 50u,
                latencyCase ? 0u : 100u,
                0u
            };
            const std::uint32_t slot1End[] = {
                latencyCase ? 0u : 40u, 0u,
                latencyCase ? 0u : 100u, 0u
            };
            const std::uint32_t ends[2] = {
                slot0End[qppu], slot1End[qppu]
            };
            for (int slot = 0; slot < 2; ++slot) {
                if (cycle == 0u) {
                    if (!appendBool(
                            submission, qppus[qppu].valid[slot],
                            windowedCase ? false : ends[slot] > 0u) ||
                        !appendU32(submission, qppus[qppu].issueType[slot],
                                   slot == 0 ? 2u : 4u) ||
                        !appendU32(submission, qppus[qppu].sgId[slot], 0u) ||
                        !appendU32(
                            submission, qppus[qppu].pc[slot],
                            0x100u + static_cast<std::uint32_t>(qppu) *
                                         0x100u +
                                static_cast<std::uint32_t>(slot) * 4u)) {
                        return false;
                    }
                    for (std::size_t feature = 0;
                         feature < kInstructionFeatureCount;
                         ++feature) {
                        if (!appendBool(
                                submission,
                                qppus[qppu].features[slot][feature],
                                instructionFeatureValue(
                                    kInstructionFeatureFields[feature],
                                    qppu, slot, latencyCase))) {
                            return false;
                        }
                    }
                } else if (windowedCase) {
                    const bool firstWindow =
                        qppu == 0u && slot == 0;
                    const bool secondWindow =
                        qppu == 1u && slot == 0;
                    if ((firstWindow && cycle == 10u) ||
                        (secondWindow && cycle == 50u)) {
                        if (!appendBool(
                                submission, qppus[qppu].valid[slot],
                                true)) {
                            return false;
                        }
                    } else if ((firstWindow && cycle == 30u) ||
                               (secondWindow && cycle == 80u)) {
                        if (!appendBool(
                                submission, qppus[qppu].valid[slot],
                                false)) {
                            return false;
                        }
                    }
                } else if (latencyCase && qppu == 0u && slot == 0 &&
                           cycle == 100u) {
                    // Keep the latency regression's request/response interval
                    // inside the global first-to-last issue analysis window.
                    if (!appendBool(
                            submission, qppus[qppu].valid[slot], true)) {
                        return false;
                    }
                } else if (cycle == ends[slot] && ends[slot] < 100u) {
                    if (!appendBool(submission, qppus[qppu].valid[slot],
                                    false)) {
                        return false;
                    }
                }
            }

            const SchedulerSignals& scheduler = schedulers[qppu];
            if (cycle == 0u) {
                const bool qppuParticipates =
                    !latencyCase || qppu == 0u;
                if (!appendBool(submission, scheduler.valid,
                                qppuParticipates) ||
                    !appendU32(submission, scheduler.queueCount,
                               qppuParticipates ? 1u : 0u) ||
                    !appendU32(submission, scheduler.queueHeadIndex, 0u) ||
                    !appendU32(
                        submission, scheduler.queueHeadPc,
                        0x100u + static_cast<std::uint32_t>(qppu) *
                                     0x100u) ||
                     !appendU32(submission,
                                scheduler.queueHeadIssueType, 2u) ||
                     !appendU32(submission,
                                scheduler.queueHeadThreadSubtype, 1u) ||
                     !appendU32(submission,
                                scheduler.queueHeadExeUnit, 0u) ||
                     !appendU32(submission, scheduler.stall, 0u) ||
                    !appendU32(submission, scheduler.sleep, 0u) ||
                    !appendBool(submission, scheduler.flow, false) ||
                    !appendBool(submission, scheduler.barrier, false) ||
                    !appendBool(submission, scheduler.setMaxTemp, false) ||
                    !appendU32(submission, scheduler.inflightMemory, 0u)) {
                    return false;
                }
                for (std::size_t feature = 0;
                     feature < kInstructionFeatureCount;
                     ++feature) {
                    if (!appendBool(
                            submission,
                            scheduler.queueHeadFeatures[feature],
                            instructionFeatureValue(
                                kInstructionFeatureFields[feature],
                                qppu, 0, latencyCase))) {
                        return false;
                    }
                }
                for (std::uint32_t dependency :
                     scheduler.dependencies) {
                    if (!appendU32(submission, dependency, 0u)) {
                        return false;
                    }
                }
                for (std::uint32_t pending :
                     scheduler.functionUnitPending) {
                    if (!appendU32(submission, pending, 0u)) {
                        return false;
                    }
                }
            }
            if (latencyCase && qppu == 0u &&
                cycle == 1u &&
                !appendU32(submission,
                           scheduler.dependencies[0], 1u)) {
                return false;
            }
            if (latencyCase && qppu == 0u &&
                cycle == 100u &&
                !appendU32(submission,
                           scheduler.dependencies[0], 0u)) {
                return false;
            }

            const ResourceSignals& resource = resources[qppu];
            const std::uint32_t fifoFullEnd[] = {50u, 0u, 80u, 20u};
            const std::uint32_t queueFullEnd[] = {30u, 0u, 70u, 0u};
            const std::uint32_t creditExhaustedEnd[] = {40u, 0u, 90u, 10u};
            const std::uint32_t fifoEnd =
                fifoFullEnd[flattened ? 0u : qppu];
            const std::uint32_t queueEnd =
                queueFullEnd[flattened ? 0u : qppu];
            const std::uint32_t creditEnd =
                creditExhaustedEnd[flattened ? 0u : qppu];
            if (cycle == 0u) {
                if (!appendU32(submission, resource.fifoOccupancy,
                               fifoEnd > 0u ? 4u : 2u) ||
                    !appendU32(submission, resource.fifoCapacity, 4u) ||
                    !appendU32(submission, resource.fifoReads, 0u) ||
                    !appendU32(submission, resource.fifoWrites, 0u) ||
                    !appendU32(submission, resource.queueOccupancy,
                               queueEnd > 0u ? 4u : 1u) ||
                    !appendU32(submission, resource.queueCapacity, 4u) ||
                    !appendU32(submission, resource.availableCredit,
                               creditEnd > 0u ? 0u : 2u) ||
                    !appendU32(submission, resource.cacheHit, 0u) ||
                    !appendU32(submission, resource.cacheMiss, 0u)) {
                    return false;
                }
            }
            if (cycle == 10u) {
                if (!appendU32(submission, resource.fifoReads, 1u) ||
                    !appendU32(submission, resource.fifoWrites, 2u) ||
                    !appendU32(submission, resource.cacheHit, 1u)) {
                    return false;
                }
            }
            if (cycle == 12u) {
                if (!appendU32(submission, resource.fifoReads, 0u) ||
                    !appendU32(submission, resource.fifoWrites, 0u) ||
                    !appendU32(submission, resource.cacheHit, 0u) ||
                    !appendU32(submission, resource.cacheMiss, 1u)) {
                    return false;
                }
            }
            if (cycle == 13u &&
                !appendU32(submission, resource.cacheMiss, 0u)) {
                return false;
            }
            if (fifoEnd > 0u && cycle == fifoEnd &&
                !appendU32(submission, resource.fifoOccupancy, 1u)) {
                return false;
            }
            if (queueEnd > 0u && cycle == queueEnd &&
                !appendU32(submission, resource.queueOccupancy, 1u)) {
                return false;
            }
            if (creditEnd > 0u && cycle == creditEnd &&
                !appendU32(submission, resource.availableCredit, 2u)) {
                return false;
            }

            const std::size_t blockedQppu =
                flattened ? 0u : 3u;
            const std::uint32_t blockStart =
                flattened ? 80u : 20u;
            const std::uint32_t blockEnd =
                flattened ? 90u : 40u;
            if (qppu == blockedQppu && cycle == blockStart &&
                !appendU32(submission, scheduler.stall, 1u)) {
                return false;
            }
            if (qppu == blockedQppu && cycle == blockEnd &&
                !appendU32(submission, scheduler.stall, 0u)) {
                return false;
            }
        }
        if (!writer.submit_cycle(submission, error)) {
            std::cerr << error << '\n';
            return false;
        }
    }
    if (!writer.close(error)) {
        std::cerr << error << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: waveperf_layout_modes_writer "
                     "<normal|normal-attached|flat|latency|windowed> "
                     "<output.wvz4>\n";
        return 2;
    }
    const std::string mode = argv[1];
    if (mode != "normal" && mode != "normal-attached" &&
        mode != "flat" &&
        mode != "latency" && mode != "windowed") {
        std::cerr
            << "mode must be normal, normal-attached, flat, latency "
               "or windowed\n";
        return 2;
    }
    gAttachNumericIndexSegments = mode == "normal-attached";
    if (!writeFile(argv[2], mode == "flat",
                   mode == "latency",
                   mode == "windowed")) return 3;
    std::cout << "generated=" << argv[2] << " mode=" << mode
              << " business_cycles=100\n";
    return 0;
}
