#include <wvz4_writer_typed.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

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
        root.name = "unrelated_root";
        root.kind = wvz4::NodeKind::Root;
        nodes_.push_back(root);
    }

    std::uint32_t addU32(const std::string& path) {
        std::uint32_t parent = 0;
        for (const std::string& segment : split(path)) {
            std::uint32_t child = findChild(parent, segment);
            if (child == kMissing) {
                TempNode node;
                node.name = segment;
                node.parent = parent;
                node.kind = segment.front() == '[' ? wvz4::NodeKind::ArrayElem
                                                   : wvz4::NodeKind::Object;
                child = static_cast<std::uint32_t>(nodes_.size());
                nodes_.push_back(node);
                nodes_[parent].children.push_back(child);
            }
            parent = child;
        }
        nodes_[parent].kind = wvz4::NodeKind::SignalLeaf;
        Signal signal;
        signal.node = parent;
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
            node.first_child = source.children.empty()
                                   ? 0u
                                   : source.children.front() + 1u;
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
            wvz4::SignalDefinition signal;
            signal.signal_id = i + 1u;
            signal.node_id = signals_[i].node + 1u;
            signal.type = wvz4::ValueType::U32;
            signal.bit_width = 32u;
            signal.radix = wvz4::Radix::Dec;
            layout.signals.push_back(signal);
        }
        return layout;
    }

private:
    struct Signal {
        std::uint32_t node = 0;
    };
    static constexpr std::uint32_t kMissing = ~std::uint32_t(0);

    static std::vector<std::string> split(const std::string& path) {
        std::vector<std::string> result;
        std::size_t begin = 0;
        while (begin < path.size()) {
            const std::size_t end = path.find('.', begin);
            result.push_back(path.substr(
                begin, end == std::string::npos ? std::string::npos
                                                : end - begin));
            if (end == std::string::npos) break;
            begin = end + 1u;
        }
        return result;
    }

    std::uint32_t findChild(std::uint32_t parent,
                            const std::string& name) const {
        for (std::uint32_t child : nodes_[parent].children) {
            if (nodes_[child].name == name) return child;
        }
        return kMissing;
    }

    std::vector<TempNode> nodes_;
    std::vector<Signal> signals_;
};

struct Signals {
    std::uint32_t readable = 0;
    std::uint32_t readableSize = 0;
    std::uint32_t readableRead = 0;
    std::uint32_t readableWritten = 0;
    std::uint32_t avail = 0;
    std::uint32_t availSize = 0;
    std::uint32_t availRi = 0;
    std::uint32_t availWi = 0;
    std::uint32_t count = 0;
    std::uint32_t countSize = 0;
    std::uint32_t countRi = 0;
    std::uint32_t countWi = 0;
    std::uint32_t missingSize = 0;
    std::uint32_t missingOccupancy = 0;
    std::vector<std::uint32_t> irrelevant;
};

bool appendU32(wvz4::CycleSubmission& submission,
               std::uint32_t id,
               std::uint32_t value) {
    return submission.append_grouped_raw(4u, id, &value);
}

bool writeWave(const std::string& output, bool flat) {
    LayoutBuilder builder;
    const std::string prefix = flat ? "blocks[size=8].[0]." : "";
    Signals ids;
    const std::string a = prefix + "alpha_pipe.stage7.object_a.";
    const std::string b = prefix + "beta_store.cell9.object_b.";
    const std::string c = prefix + "gamma_bucket.slot3.object_c.";
    ids.readable = builder.addU32(a + "m_num_readable");
    ids.readableSize = builder.addU32(a + "m_size");
    ids.readableRead = builder.addU32(a + "m_num_read");
    ids.readableWritten = builder.addU32(a + "m_num_written");
    ids.avail = builder.addU32(b + "m_numAvail");
    ids.availSize = builder.addU32(b + "m_size");
    ids.availRi = builder.addU32(b + "m_ri");
    ids.availWi = builder.addU32(b + "m_wi");
    ids.count = builder.addU32(c + "m_count");
    ids.countSize = builder.addU32(c + "m_size");
    ids.countRi = builder.addU32(c + "m_ri");
    ids.countWi = builder.addU32(c + "m_wi");
    ids.missingSize = builder.addU32(prefix + "noise.incomplete.m_num_readable");
    ids.missingOccupancy = builder.addU32(prefix + "noise.only_size.m_size");
    for (int i = 0; i < 32; ++i) {
        ids.irrelevant.push_back(builder.addU32(
            prefix + "payload_bank.lane" + std::to_string(i) +
            ".data_word"));
    }

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
    for (std::uint32_t cycle = 0; cycle <= 100u; ++cycle) {
        wvz4::CycleSubmission submission;
        submission.cycle = static_cast<wvz4::i64>(cycle) * 10;
        if (cycle == 0u) {
            const std::pair<std::uint32_t, std::uint32_t> initial[] = {
                {ids.readable, 4u}, {ids.readableSize, 4u},
                {ids.readableRead, 0u}, {ids.readableWritten, 0u},
                {ids.avail, 2u}, {ids.availSize, 8u}, {ids.availRi, 0u},
                {ids.availWi, 0u}, {ids.count, 0u}, {ids.countSize, 2u},
                {ids.countRi, 0u}, {ids.countWi, 0u},
                {ids.missingSize, 1u}, {ids.missingOccupancy, 4u}};
            for (const auto& value : initial) {
                if (!appendU32(submission, value.first, value.second))
                    return false;
            }
            for (std::uint32_t id : ids.irrelevant) {
                if (!appendU32(submission, id, id)) return false;
            }
        }
        if (cycle == 20u && !appendU32(submission, ids.avail, 8u))
            return false;
        if (cycle == 30u && !appendU32(submission, ids.readable, 3u))
            return false;
        if (cycle == 40u && !appendU32(submission, ids.count, 2u))
            return false;
        if (cycle == 70u && !appendU32(submission, ids.avail, 1u))
            return false;
        if (cycle == 100u && !appendU32(submission, ids.count, 0u))
            return false;
        if (cycle == 100u && !appendU32(submission, ids.readable, 2u))
            return false;
        if (cycle == 100u && !appendU32(submission, ids.avail, 0u))
            return false;
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
    if (argc != 3 || (std::string(argv[1]) != "normal" &&
                      std::string(argv[1]) != "flat")) {
        std::cerr << "usage: wavefifo_layout_modes_writer <normal|flat> <output.wvz4>\n";
        return 2;
    }
    if (!writeWave(argv[2], std::string(argv[1]) == "flat")) return 3;
    std::cout << "generated=" << argv[2] << " mode=" << argv[1]
              << " business_cycles=100\n";
    return 0;
}
