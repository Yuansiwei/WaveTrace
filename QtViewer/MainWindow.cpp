#include "MainWindow.h"
#include "ActiveSignalListWidget.h"
#include "WaveCanvas.h"
#include "WaveParser.h"
#include "WaveParser2.h"
#include "WaveParser3.h"
#include "WaveParser4.h"
#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QWindow>
#include <QCoreApplication>
#include <QDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QColor>
#include <QLabel>
#include <QLinearGradient>
#include <QPen>
#include <QLineEdit>
#include <QClipboard>
#include <QMessageBox>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QHeaderView>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QSpinBox>
#include <QStringList>
#include <QTreeView>
#include <QTreeWidget>
#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QByteArray>
#include <QBrush>
#include <QDataStream>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QIODevice>
#include <QSet>
#include <QTreeWidgetItem>
#include <QItemSelectionModel>
#include <QVBoxLayout>
#include <QTimer>
#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace {

constexpr const char* kMimeSignalIndexes = "application/x-waveviewer-signal-indexes";
constexpr const char* kMimeActiveRows = "application/x-waveviewer-active-rows";

constexpr int kTreeRoleSignalIndex = Qt::UserRole;
constexpr int kTreeRoleNodeId = Qt::UserRole + 100;
constexpr int kValueFindRoleFirstHit = Qt::UserRole;
constexpr int kValueFindRoleSignalIndex = Qt::UserRole + 1;
constexpr qint64 kLargeWvz4FullLoadLimitBytes = 1024ll * 1024ll * 1024ll;
constexpr quint64 kViewerOnDemandSampleBudget = 20ull * 1000ull * 1000ull;
constexpr quint64 kViewerFullLoadSampleBudget = 50ull * 1000ull * 1000ull;

QString formatLargeWvz4FullLoadError(qint64 fileSize, const QString& operation) {
    return QStringLiteral("WVZ4 %1 full-load is disabled for files over %2 MiB (%3 bytes). "
                          "Open the file normally and use LOD/on-demand viewing instead.")
        .arg(operation)
        .arg(kLargeWvz4FullLoadLimitBytes / (1024ll * 1024ll))
        .arg(fileSize);
}

bool viewerPerfLogEnabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("WV_VIEWER_PERF_LOG");
        return value && value[0] && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

bool viewerDisableLodEnabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("WV_VIEWER_DISABLE_LOD");
        return value && value[0] && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

void viewerPerfLog(const char* step, qint64 elapsedMs, int signalCount, int treeNodeCount, int activeRows = -1) {
    if (!viewerPerfLogEnabled()) return;
    std::fprintf(stderr,
                 "[qtviewer-perf] step=%s elapsed_ms=%lld signals=%d tree_nodes=%d active_rows=%d\n",
                 step,
                 static_cast<long long>(elapsedMs),
                 signalCount,
                 treeNodeCount,
                 activeRows);
    std::fflush(stderr);
    const char* filePath = std::getenv("WV_VIEWER_PERF_LOG_FILE");
    if (filePath && filePath[0]) {
        if (FILE* f = std::fopen(filePath, "ab")) {
            std::fprintf(f,
                         "[qtviewer-perf] step=%s elapsed_ms=%lld signals=%d tree_nodes=%d active_rows=%d\n",
                         step,
                         static_cast<long long>(elapsedMs),
                         signalCount,
                         treeNodeCount,
                         activeRows);
            std::fclose(f);
        }
    }
}

static inline uint32_t fnv1aStep(uint32_t h, unsigned char c) {
    return (h ^ uint32_t(c)) * 16777619u;
}

static inline uint32_t fnv1aHash(const char* data, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; ++i) h = fnv1aStep(h, static_cast<unsigned char>(data[i]));
    return h ? h : 1u;
}

template<class T, int N = 32>
class SmallVec32 {
public:
    SmallVec32() : m_size(0), m_capacity(N), m_data(m_local) {}
    ~SmallVec32() { if (m_data != m_local) delete[] m_data; }

    SmallVec32(const SmallVec32& other) : SmallVec32() {
        reserve(other.m_size);
        for (int i = 0; i < other.m_size; ++i) m_data[i] = other.m_data[i];
        m_size = other.m_size;
    }

    SmallVec32(SmallVec32&& other) noexcept : SmallVec32() {
        if (other.m_data == other.m_local) {
            for (int i = 0; i < other.m_size; ++i) m_local[i] = std::move(other.m_local[i]);
            m_size = other.m_size;
        } else {
            m_data = other.m_data;
            m_capacity = other.m_capacity;
            m_size = other.m_size;
            other.m_data = other.m_local;
            other.m_capacity = N;
            other.m_size = 0;
        }
    }

    SmallVec32& operator=(const SmallVec32& other) {
        if (this == &other) return *this;
        clear();
        reserve(other.m_size);
        for (int i = 0; i < other.m_size; ++i) m_data[i] = other.m_data[i];
        m_size = other.m_size;
        return *this;
    }

    SmallVec32& operator=(SmallVec32&& other) noexcept {
        if (this == &other) return *this;
        if (m_data != m_local) delete[] m_data;
        m_data = m_local;
        m_capacity = N;
        m_size = 0;

        if (other.m_data == other.m_local) {
            for (int i = 0; i < other.m_size; ++i) m_local[i] = std::move(other.m_local[i]);
            m_size = other.m_size;
        } else {
            m_data = other.m_data;
            m_capacity = other.m_capacity;
            m_size = other.m_size;
            other.m_data = other.m_local;
            other.m_capacity = N;
            other.m_size = 0;
        }
        return *this;
    }

    int size() const { return m_size; }
    bool empty() const { return m_size == 0; }
    T* data() { return m_data; }
    const T* data() const { return m_data; }

    T& operator[](int i) { return m_data[i]; }
    const T& operator[](int i) const { return m_data[i]; }

    T* begin() { return m_data; }
    T* end() { return m_data + m_size; }
    const T* begin() const { return m_data; }
    const T* end() const { return m_data + m_size; }

    void clear() { m_size = 0; }

    void reserve(int wanted) {
        if (wanted <= m_capacity) return;
        int newCap = m_capacity;
        while (newCap < wanted) newCap *= 2;
        T* p = new T[newCap];
        for (int i = 0; i < m_size; ++i) p[i] = std::move(m_data[i]);
        if (m_data != m_local) delete[] m_data;
        m_data = p;
        m_capacity = newCap;
    }

    void push_back(const T& v) {
        if (m_size >= m_capacity) reserve(m_size + 1);
        m_data[m_size++] = v;
    }

    void push_back(T&& v) {
        if (m_size >= m_capacity) reserve(m_size + 1);
        m_data[m_size++] = std::move(v);
    }

private:
    int m_size;
    int m_capacity;
    T* m_data;
    T m_local[N];
};

struct SmallStrView {
    const char* data = nullptr;
    int len = 0;
    uint32_t hash = 0;
};

struct SmallStr32 {
    SmallStr32() = default;

    SmallStr32(const char* p, int n, uint32_t h = 0) {
        assign(p, n, h);
    }

    SmallStr32(const SmallStr32& other) {
        assign(other.data(), other.size, other.hash);
    }

    SmallStr32(SmallStr32&& other) noexcept {
        moveFrom(std::move(other));
    }

    ~SmallStr32() {
        if (heap) delete[] heap;
    }

    SmallStr32& operator=(const SmallStr32& other) {
        if (this == &other) return *this;
        assign(other.data(), other.size, other.hash);
        return *this;
    }

    SmallStr32& operator=(SmallStr32&& other) noexcept {
        if (this == &other) return *this;
        if (heap) delete[] heap;
        heap = nullptr;
        size = 0;
        hash = 0;
        moveFrom(std::move(other));
        return *this;
    }

    void assign(const char* p, int n, uint32_t h = 0) {
        if (heap) {
            delete[] heap;
            heap = nullptr;
        }

        if (!p || n <= 0) {
            size = 0;
            hash = 1u;
            local[0] = '\0';
            return;
        }

        if (n > 65535) n = 65535;
        size = static_cast<uint16_t>(n);
        hash = h ? h : fnv1aHash(p, n);

        if (n <= int(sizeof(local))) {
            memcpy(local, p, size_t(n));
            if (n < int(sizeof(local))) local[n] = '\0';
        } else {
            heap = new char[size_t(n) + 1u];
            memcpy(heap, p, size_t(n));
            heap[n] = '\0';
        }
    }

    void moveFrom(SmallStr32&& other) {
        size = other.size;
        hash = other.hash;
        if (other.heap) {
            heap = other.heap;
            other.heap = nullptr;
        } else {
            memcpy(local, other.local, size_t(other.size));
            if (other.size < sizeof(local)) local[other.size] = '\0';
            heap = nullptr;
        }
        other.size = 0;
        other.hash = 1u;
        other.local[0] = '\0';
    }

    const char* data() const { return heap ? heap : local; }

    bool equals(const char* p, int n, uint32_t h) const {
        return int(size) == n && hash == h && memcmp(data(), p, size_t(n)) == 0;
    }

    QString toQString() const {
        return QString::fromUtf8(data(), int(size));
    }

    uint16_t size = 0;
    uint32_t hash = 1u;
    char* heap = nullptr;
    char local[32] = {};
};

struct FlatIntIntMap {
    struct Entry {
        int key = -1;
        int value = -1;
    };

    void clear() {
        table.clear();
        mask = 0;
        used = 0;
    }

    void reserve(int wanted) {
        int cap = 8;
        while (cap < wanted * 2) cap <<= 1;
        if (cap <= table.size()) return;

        QVector<Entry> old = std::move(table);
        table.clear();
        table.resize(cap);
        mask = cap - 1;
        used = 0;

        for (const Entry& e : old) {
            if (e.key >= 0) insert(e.key, e.value);
        }
    }

    bool find(int key, int& outValue) const {
        if (table.isEmpty()) return false;
        uint32_t h = uint32_t(key) * 2654435761u;
        int pos = int(h) & mask;
        for (;;) {
            const Entry& e = table[pos];
            if (e.key < 0) return false;
            if (e.key == key) {
                outValue = e.value;
                return true;
            }
            pos = (pos + 1) & mask;
        }
    }

    void insert(int key, int value) {
        if (table.isEmpty() || used * 2 >= table.size()) reserve(qMax(8, used + 1));
        uint32_t h = uint32_t(key) * 2654435761u;
        int pos = int(h) & mask;
        for (;;) {
            Entry& e = table[pos];
            if (e.key < 0) {
                e.key = key;
                e.value = value;
                ++used;
                return;
            }
            if (e.key == key) {
                e.value = value;
                return;
            }
            pos = (pos + 1) & mask;
        }
    }

    QVector<Entry> table;
    int mask = 0;
    int used = 0;
};

struct SmallStrPool {
    struct Entry {
        uint32_t hash = 0;
        int id = -1;
    };

    void reserve(int n) {
        strings.reserve(n);
        int cap = 8;
        while (cap < n * 2) cap <<= 1;

        if (!strings.isEmpty()) {
            rehash(cap);
            return;
        }

        table.clear();
        table.resize(cap);
        mask = cap - 1;
        used = 0;
    }

    int intern(const SmallStrView& v) {
        if (!v.data || v.len <= 0) return -1;
        if (table.isEmpty()) reserve(1024);
        if (used * 2 >= table.size()) rehash(table.size() * 2);

        int pos = int(v.hash) & mask;
        for (;;) {
            Entry& e = table[pos];
            if (e.id < 0) {
                const int id = strings.size();
                strings.push_back(SmallStr32(v.data, v.len, v.hash));
                e.hash = v.hash;
                e.id = id;
                ++used;
                return id;
            }
            if (e.hash == v.hash && strings[e.id].equals(v.data, v.len, v.hash)) {
                return e.id;
            }
            pos = (pos + 1) & mask;
        }
    }

    const SmallStr32& get(int id) const {
        return strings[id];
    }

    void rehash(int cap) {
        QVector<Entry> old = std::move(table);
        table.clear();
        table.resize(cap);
        mask = cap - 1;
        used = 0;
        for (const Entry& e : old) {
            if (e.id < 0) continue;
            int pos = int(e.hash) & mask;
            for (;;) {
                Entry& dst = table[pos];
                if (dst.id < 0) {
                    dst = e;
                    ++used;
                    break;
                }
                pos = (pos + 1) & mask;
            }
        }
    }

    QVector<SmallStr32> strings;
    QVector<Entry> table;
    int mask = 0;
    int used = 0;
};

using PathSegViewList = SmallVec32<SmallStrView, 32>;
using PathNameIdList = SmallVec32<int, 32>;

PathSegViewList splitPathToSmallVec(const QByteArray& utf8) {
    PathSegViewList out;
    const char* s = utf8.constData();
    const int n = utf8.size();

    int start = 0;
    uint32_t h = 2166136261u;
    for (int i = 0; i <= n; ++i) {
        if (i == n || s[i] == '.') {
            const int len = i - start;
            if (len > 0) {
                SmallStrView v;
                v.data = s + start;
                v.len = len;
                v.hash = h ? h : 1u;
                out.push_back(v);
            }
            start = i + 1;
            h = 2166136261u;
        } else {
            h = fnv1aStep(h, static_cast<unsigned char>(s[i]));
        }
    }
    return out;
}

struct SignalPathEntry {
    int signalIndex = -1;
    int signalId = -1;
    int width = 1;
    PathNameIdList nameIds;
};

struct LogicTreeNode {
    int nameId = -1;
    int parent = -1;
    int childListId = -1;
    int signalIndex = -1;
    int signalId = -1;
    int width = 1;
};

struct LogicChildList {
    SmallVec32<int, 32> children;
    FlatIntIntMap lookup;
    bool lookupReady = false;
};

} // namespace

struct SignalLogicTree {
    SmallStrPool names;
    QVector<SignalPathEntry> signalPaths;
    QVector<LogicTreeNode> nodes;
    QVector<LogicChildList> childLists;
    SmallVec32<int, 32> roots;
    FlatIntIntMap rootLookup;
    QVector<int> nodeIdBySignalIndex;

    void clear() {
        names = SmallStrPool();
        signalPaths.clear();
        nodes.clear();
        childLists.clear();
        roots.clear();
        rootLookup.clear();
        nodeIdBySignalIndex.clear();
    }

    bool hasChildren(int nodeId) const {
        if (nodeId < 0 || nodeId >= nodes.size()) return false;
        const int listId = nodes[nodeId].childListId;
        return listId >= 0 && listId < childLists.size() && !childLists[listId].children.empty();
    }

    const LogicChildList* childListForNode(int nodeId) const {
        if (nodeId < 0 || nodeId >= nodes.size()) return nullptr;
        const int listId = nodes[nodeId].childListId;
        if (listId < 0 || listId >= childLists.size()) return nullptr;
        return &childLists[listId];
    }

    LogicChildList& ensureChildList(int nodeId) {
        LogicTreeNode& node = nodes[nodeId];
        if (node.childListId < 0) {
            node.childListId = childLists.size();
            childLists.push_back(LogicChildList());
        }
        return childLists[node.childListId];
    }

    int findChildInList(const LogicChildList& list, int nameId) const {
        if (list.lookupReady) {
            int nodeId = -1;
            return list.lookup.find(nameId, nodeId) ? nodeId : -1;
        }
        for (int childNodeId : list.children) {
            if (nodes[childNodeId].nameId == nameId) return childNodeId;
        }
        return -1;
    }

    void maybeBuildLookup(LogicChildList& list) {
        if (list.lookupReady || list.children.size() <= 32) return;
        list.lookup.reserve(list.children.size() * 2);
        for (int childNodeId : list.children) {
            list.lookup.insert(nodes[childNodeId].nameId, childNodeId);
        }
        list.lookupReady = true;
    }

    int findOrCreateChildByNameId(int parentNodeId,
                                  int nameId,
                                  bool leaf,
                                  int signalIndex,
                                  int signalId,
                                  int width) {
        if (nameId < 0) return parentNodeId;

        if (parentNodeId < 0) {
            int existing = -1;
            if (rootLookup.find(nameId, existing)) {
                if (leaf) {
                    nodes[existing].signalIndex = signalIndex;
                    nodes[existing].signalId = signalId;
                    nodes[existing].width = width;
                }
                return existing;
            }

            LogicTreeNode node;
            node.nameId = nameId;
            node.parent = -1;
            node.signalIndex = leaf ? signalIndex : -1;
            node.signalId = leaf ? signalId : -1;
            node.width = width;
            const int nodeId = nodes.size();
            nodes.push_back(node);
            roots.push_back(nodeId);
            rootLookup.insert(nameId, nodeId);
            return nodeId;
        }

        LogicChildList& list = ensureChildList(parentNodeId);
        int existing = findChildInList(list, nameId);
        if (existing >= 0) {
            if (leaf) {
                nodes[existing].signalIndex = signalIndex;
                nodes[existing].signalId = signalId;
                nodes[existing].width = width;
            }
            return existing;
        }

        LogicTreeNode node;
        node.nameId = nameId;
        node.parent = parentNodeId;
        node.signalIndex = leaf ? signalIndex : -1;
        node.signalId = leaf ? signalId : -1;
        node.width = width;
        const int nodeId = nodes.size();
        nodes.push_back(node);
        list.children.push_back(nodeId);
        if (list.lookupReady) list.lookup.insert(nameId, nodeId);
        else maybeBuildLookup(list);
        return nodeId;
    }

    void buildFromSignalDefs(const QVector<WaveSignal>& signalDefs) {
        clear();
        nodeIdBySignalIndex.resize(signalDefs.size());
        std::fill(nodeIdBySignalIndex.begin(), nodeIdBySignalIndex.end(), -1);

        names.reserve(qMax(1024, signalDefs.size() * 2));
        signalPaths.reserve(signalDefs.size());
        nodes.reserve(signalDefs.size() * 2);

        for (int signalIndex = 0; signalIndex < signalDefs.size(); ++signalIndex) {
            const WaveSignal& sig = signalDefs.at(signalIndex);
            const QByteArray utf8 = sig.name.toUtf8();
            PathSegViewList segViews = splitPathToSmallVec(utf8);
            if (segViews.empty()) continue;

            SignalPathEntry path;
            path.signalIndex = signalIndex;
            path.signalId = sig.signalId;
            path.width = sig.width;

            int parent = -1;
            for (int i = 0; i < segViews.size(); ++i) {
                const bool isLeaf = (i == segViews.size() - 1);
                const int nameId = names.intern(segViews[i]);
                path.nameIds.push_back(nameId);
                parent = findOrCreateChildByNameId(parent, nameId, isLeaf,
                                                   isLeaf ? signalIndex : -1,
                                                   sig.signalId,
                                                   sig.width);
            }

            if (parent >= 0 && signalIndex < nodeIdBySignalIndex.size()) {
                nodeIdBySignalIndex[signalIndex] = parent;
            }
            signalPaths.push_back(std::move(path));
        }
    }

    void buildFromWaveTree(const WaveTreeInfo& tree, const QVector<WaveSignal>& signalDefs) {
        clear();

        nodeIdBySignalIndex.resize(signalDefs.size());
        std::fill(nodeIdBySignalIndex.begin(), nodeIdBySignalIndex.end(), -1);

        if (!tree.valid || tree.nodesById.isEmpty()) {
            buildFromSignalDefs(signalDefs);
            return;
        }

        const int fileNodeCount = tree.nodesById.size();
        names.reserve(qMax(1024, fileNodeCount));
        nodes.clear();
        nodes.resize(fileNodeCount); // WVZ4 path: logic node id == NODE table index.
        childLists.reserve(qMax(1, fileNodeCount / 2));

        // First pass: materialize each valid WVZ4 NODE into the same index in
        // SignalLogicTree::nodes. No remapping table is needed; node_id directly
        // indexes both the model node and QModelIndex::internalId().
        for (int nodeId = 1; nodeId < fileNodeCount; ++nodeId) {
            const WaveTreeNode& src = tree.nodesById.at(nodeId);
            if (!src.valid) continue;

            const QByteArray utf8 = src.name.toUtf8();
            SmallStrView view;
            view.data = utf8.constData();
            view.len = utf8.size();
            view.hash = fnv1aHash(view.data, view.len);

            LogicTreeNode& node = nodes[nodeId];
            node.nameId = names.intern(view);
            node.parent = (src.parentId > 0 && src.parentId < fileNodeCount) ? src.parentId : -1;
            node.signalIndex = src.signalIndex;
            node.signalId = src.signalId;
            node.width = 1;
            if (src.signalIndex >= 0 && src.signalIndex < signalDefs.size()) {
                node.width = signalDefs.at(src.signalIndex).width;
                if (src.signalIndex < nodeIdBySignalIndex.size()) {
                    nodeIdBySignalIndex[src.signalIndex] = nodeId;
                }
            }
        }

        // Roots are also stored as original WVZ4 node indices.
        for (int rootId : tree.rootNodeIds) {
            if (rootId <= 0 || rootId >= fileNodeCount) continue;
            const LogicTreeNode& rootNode = nodes.at(rootId);
            if (rootNode.nameId < 0) continue;
            roots.push_back(rootId);
            rootLookup.insert(rootNode.nameId, rootId);
            nodes[rootId].parent = -1;
        }

        // Defensive recovery for legacy tree metadata without rootNodeIds.
        // Current WVZ4 parser supplies rootNodeIds, so this path should not run.
        if (roots.empty()) {
            for (int nodeId = 1; nodeId < fileNodeCount; ++nodeId) {
                if (nodes.at(nodeId).nameId < 0) continue;
                if (tree.nodesById.at(nodeId).parentId != 0) continue;
                roots.push_back(nodeId);
                rootLookup.insert(nodes.at(nodeId).nameId, nodeId);
                nodes[nodeId].parent = -1;
            }
        }

        // Build child lists strictly from the WVZ4 first_child / next_sibling
        // chains. This is still direct-indexed: every child id is the original
        // WVZ4 node_id, not a newly assigned UI/model id.
        for (int parentNodeId = 1; parentNodeId < fileNodeCount; ++parentNodeId) {
            if (nodes.at(parentNodeId).nameId < 0) continue;

            int childNodeId = tree.nodesById.at(parentNodeId).firstChild;
            if (childNodeId == 0) continue;

            LogicChildList& list = ensureChildList(parentNodeId);
            int guard = 0;
            while (childNodeId != 0 && guard++ < fileNodeCount) {
                if (childNodeId <= 0 || childNodeId >= fileNodeCount) break;
                if (nodes.at(childNodeId).nameId >= 0) {
                    nodes[childNodeId].parent = parentNodeId;
                    list.children.push_back(childNodeId);
                }
                childNodeId = tree.nodesById.at(childNodeId).nextSibling;
            }
            maybeBuildLookup(list);
        }
    }

    QString fullPathForNodeId(int nodeId) const {
        if (nodeId < 0 || nodeId >= nodes.size()) return QString();

        QVector<QString> parts;
        int cur = nodeId;
        int guard = 0;
        while (cur >= 0 && cur < nodes.size() && guard++ < nodes.size()) {
            const int nameId = nodes.at(cur).nameId;
            if (nameId >= 0 && nameId < names.strings.size()) {
                parts.push_back(names.get(nameId).toQString());
            }
            cur = nodes.at(cur).parent;
        }
        std::reverse(parts.begin(), parts.end());

        QString out;
        for (int i = 0; i < parts.size(); ++i) {
            if (i > 0) out += QLatin1Char('.');
            out += parts.at(i);
        }
        return out;
    }

    QString fullPathForSignalIndex(int signalIndex) const {
        if (signalIndex < 0 || signalIndex >= nodeIdBySignalIndex.size()) return QString();
        return fullPathForNodeId(nodeIdBySignalIndex.at(signalIndex));
    }

    QString nodeNameString(int nodeId) const {
        if (nodeId < 0 || nodeId >= nodes.size()) return QString();
        const int nameId = nodes.at(nodeId).nameId;
        if (nameId < 0 || nameId >= names.strings.size()) return QString();
        return names.get(nameId).toQString();
    }

    static QStringList splitSearchPath(const QString& query) {
        QStringList parts;
        QString current;
        current.reserve(query.size());

        for (int i = 0; i < query.size(); ++i) {
            const QChar ch = query.at(i);
            if (ch == QLatin1Char('.')) {
                const QString trimmed = current.trimmed();
                if (!trimmed.isEmpty()) parts.push_back(trimmed);
                current.clear();
                continue;
            }
            current += ch;
        }

        const QString trimmed = current.trimmed();
        if (!trimmed.isEmpty()) parts.push_back(trimmed);
        return parts;
    }

    QString nodeSearchNameString(int nodeId) const {
        QString text = nodeNameString(nodeId);
        if (nodeId >= 0 && nodeId < nodes.size()) {
            const LogicTreeNode& node = nodes.at(nodeId);
            if (node.signalIndex >= 0 && node.width > 1) {
                text += QStringLiteral("[%1:0]").arg(node.width - 1);
            }
        }
        return text;
    }

    bool nodeNameContains(int nodeId, const QString& needle) const {
        return nodeSearchNameString(nodeId).contains(needle, Qt::CaseInsensitive);
    }

    bool nodeNameEquals(int nodeId, const QString& needle) const {
        const QString displayName = nodeSearchNameString(nodeId);
        const QString bareName = nodeNameString(nodeId);
        if (QString::compare(displayName, needle, Qt::CaseInsensitive) == 0 ||
            QString::compare(bareName, needle, Qt::CaseInsensitive) == 0) {
            return true;
        }

        // Compare-mode leaves are named "A <signal>" / "B <signal>" so A/B
        // pairs stay adjacent.  Still allow structural searches using the
        // original signal segment, e.g. "top.data[7:0]".
        if ((bareName.startsWith(QStringLiteral("A ")) || bareName.startsWith(QStringLiteral("B "))) && bareName.size() > 2) {
            const QString strippedBare = bareName.mid(2);
            QString strippedDisplay = displayName;
            if (strippedDisplay.startsWith(QStringLiteral("A ")) || strippedDisplay.startsWith(QStringLiteral("B "))) {
                strippedDisplay = strippedDisplay.mid(2);
            }
            return QString::compare(strippedDisplay, needle, Qt::CaseInsensitive) == 0 ||
                   QString::compare(strippedBare, needle, Qt::CaseInsensitive) == 0;
        }
        return false;
    }

    bool directPathMatchesFrom(int nodeId, const QStringList& parts, int partIndex) const {
        if (partIndex < 0 || partIndex >= parts.size()) return true;
        if (!nodeNameEquals(nodeId, parts.at(partIndex))) return false;
        if (partIndex == parts.size() - 1) return true;

        const LogicChildList* list = childListForNode(nodeId);
        if (!list) return false;

        const QString& next = parts.at(partIndex + 1);
        for (int childNodeId : list->children) {
            if (nodeNameEquals(childNodeId, next) &&
                directPathMatchesFrom(childNodeId, parts, partIndex + 1)) {
                return true;
            }
        }
        return false;
    }

    int directPathEndNodeFrom(int nodeId, const QStringList& parts, int partIndex) const {
        if (partIndex < 0 || partIndex >= parts.size()) return -1;
        if (!nodeNameEquals(nodeId, parts.at(partIndex))) return -1;
        if (partIndex == parts.size() - 1) return nodeId;

        const LogicChildList* list = childListForNode(nodeId);
        if (!list) return -1;

        const QString& next = parts.at(partIndex + 1);
        for (int childNodeId : list->children) {
            if (!nodeNameEquals(childNodeId, next)) continue;
            const int endNode = directPathEndNodeFrom(childNodeId, parts, partIndex + 1);
            if (endNode >= 0) return endNode;
        }
        return -1;
    }

    QVector<int> searchTreeQuery(const QString& query, int maxResults) const {
        QVector<int> result;
        if (maxResults <= 0) return result;

        const QString trimmed = query.trimmed();
        if (trimmed.isEmpty()) return result;

        const QStringList parts = splitSearchPath(trimmed);
        if (parts.isEmpty()) return result;

        result.reserve(qMin(maxResults, 1024));

        if (parts.size() == 1) {
            const QString& part = parts.first();

            // Single-segment search searches tree node names, not complete leaf paths.
            for (int nodeId = 0; nodeId < nodes.size(); ++nodeId) {
                if (nodeNameContains(nodeId, part)) {
                    result.push_back(nodeId);
                    if (result.size() >= maxResults) return result;
                }
            }
            return result;
        }

        // Multi-segment search is structural:
        // "a.b" means a node named "a" with a direct child path segment "b".
        // It can match anywhere in the tree, e.g. top.x.a.b.y will match at a.b.
        for (int nodeId = 0; nodeId < nodes.size(); ++nodeId) {
            if (!nodeNameEquals(nodeId, parts.first())) continue;
            const int endNode = directPathEndNodeFrom(nodeId, parts, 0);
            if (endNode >= 0) {
                result.push_back(endNode);
                if (result.size() >= maxResults) return result;
            }
        }
        return result;
    }
};

namespace {



QByteArray encodeIntList(const QList<int>& values) {
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_0);
    out << values;
    return bytes;
}

QList<int> decodeIntList(const QMimeData* mimeData, const char* format) {
    QList<int> values;
    if (!mimeData || !mimeData->hasFormat(format)) return values;

    const QByteArray bytes = mimeData->data(format);
    QDataStream in(bytes);
    in.setVersion(QDataStream::Qt_5_0);
    in >> values;
    return values;
}

QString fullSignalPathFromWave(const WaveFile& wave, int signalIndex) {
    if (signalIndex < 0 || signalIndex >= wave.signalList.size()) return QString();

    const WaveTreeInfo& tree = wave.tree;
    if (tree.valid && signalIndex < tree.signalIndexToNodeId.size()) {
        const int nodeId = tree.signalIndexToNodeId.at(signalIndex);
        if (nodeId > 0 && nodeId < tree.nodesById.size()) {
            QVector<QString> parts;
            int cur = nodeId;
            int guard = 0;
            while (cur > 0 && cur < tree.nodesById.size() && guard++ < tree.nodesById.size()) {
                const WaveTreeNode& node = tree.nodesById.at(cur);
                if (!node.valid) break;
                if (!node.name.isEmpty()) parts.push_back(node.name);
                cur = node.parentId;
            }
            if (!parts.isEmpty()) {
                std::reverse(parts.begin(), parts.end());
                QString joined;
                for (int i = 0; i < parts.size(); ++i) {
                    if (i > 0) joined += QLatin1Char('.');
                    joined += parts.at(i);
                }
                return joined;
            }
        }
    }

    return wave.signalList.at(signalIndex).name.trimmed();
}

bool loadWaveFileFullyForCompare(const QString& path, WaveFile& wave, QString& error) {
    error.clear();
    const bool isWvz4 = path.endsWith(QStringLiteral(".wvz4"), Qt::CaseInsensitive);
    const bool isWvz3 = path.endsWith(QStringLiteral(".wvz3"), Qt::CaseInsensitive);
    if (isWvz4) {
        const qint64 fileSize = QFileInfo(path).size();
        if (fileSize > kLargeWvz4FullLoadLimitBytes) {
            error = formatLargeWvz4FullLoadError(fileSize, QStringLiteral("compare"));
            return false;
        }

        WaveParser4::LoadOptions options;
        options.includeAllSignalDefinitions = true;
        options.autoLoadFirstSignalCount = -1;
        options.loadAllIfWindowEmpty = true;
        options.maxDecodedSamples = kViewerFullLoadSampleBudget;
        return WaveParser4::loadFromFile(path, wave, error, options);
    }
    if (isWvz3) {
        WaveParser3::LoadOptions options;
        options.includeAllSignalDefinitions = true;
        options.autoLoadFirstSignalCount = -1;
        options.loadAllIfWindowEmpty = true;
        return WaveParser3::loadFromFile(path, wave, error, options);
    }
    if (path.endsWith(QStringLiteral(".wvz2"), Qt::CaseInsensitive)) {
        return WaveParser2::loadFromFile(path, wave, error);
    }
    return WaveParser::loadFromFile(path, wave, error);
}

WaveSample makeAbsentCompareSample() {
    WaveSample s;
    s.time = 0;
    s.value = waveAbsentValue();
    s.isAbsent = true;
    s.isZ = false;
    s.rawBits = 0;
    s.rawFieldsReady = true;
    return s;
}

bool compareValueEquivalent(const WaveSample& a, const WaveSample& b) {
    return waveSamplesEquivalent(a, b);
}

void appendCompareDiffRegion(QVector<WaveDiffRegion>& regions, qint64 start, qint64 end, qint64 clipStart, qint64 clipEnd) {
    start = qMax(start, clipStart);
    end = qMin(end, clipEnd);
    if (end <= start) return;

    if (!regions.isEmpty() && regions.last().end >= start) {
        regions.last().end = qMax(regions.last().end, end);
        return;
    }

    WaveDiffRegion r;
    r.start = start;
    r.end = end;
    regions.push_back(r);
}

QVector<WaveDiffRegion> computeSignalDiffRegions(const WaveSignal& left,
                                                  const WaveSignal& right,
                                                  qint64 compareStart,
                                                  qint64 compareEnd,
                                                  qint64 leftEnd,
                                                  qint64 rightEnd) {
    QVector<WaveDiffRegion> regions;
    if (compareEnd <= compareStart) compareEnd = compareStart + 1;

    if (left.kind != right.kind || left.width != right.width) {
        appendCompareDiffRegion(regions, compareStart, compareEnd, compareStart, compareEnd);
        return regions;
    }

    const WaveSample absent = makeAbsentCompareSample();
    const WaveSample* curLeft = &absent;
    const WaveSample* curRight = &absent;
    int i = 0;
    int j = 0;
    bool leftEnded = (leftEnd <= compareStart);
    bool rightEnded = (rightEnd <= compareStart);

    const WaveSample* leftSamples = left.samples.constData();
    const WaveSample* rightSamples = right.samples.constData();
    const int leftCount = left.samples.size();
    const int rightCount = right.samples.size();

    while (i < leftCount || j < rightCount || !leftEnded || !rightEnded) {
        qint64 t = std::numeric_limits<qint64>::max();
        if (!leftEnded) t = qMin(t, leftEnd);
        if (!rightEnded) t = qMin(t, rightEnd);
        while (i < leftCount && leftEnded && leftSamples[i].time >= leftEnd) ++i;
        while (j < rightCount && rightEnded && rightSamples[j].time >= rightEnd) ++j;
        if (i < leftCount && (!leftEnded || leftSamples[i].time < leftEnd)) {
            t = qMin(t, leftSamples[i].time);
        }
        if (j < rightCount && (!rightEnded || rightSamples[j].time < rightEnd)) {
            t = qMin(t, rightSamples[j].time);
        }
        if (t == std::numeric_limits<qint64>::max()) break;

        if (!leftEnded && t >= leftEnd) {
            curLeft = &absent;
            leftEnded = true;
            while (i < leftCount && leftSamples[i].time <= t) ++i;
        } else {
            while (i < leftCount && leftSamples[i].time == t) {
                curLeft = &leftSamples[i++];
            }
        }

        if (!rightEnded && t >= rightEnd) {
            curRight = &absent;
            rightEnded = true;
            while (j < rightCount && rightSamples[j].time <= t) ++j;
        } else {
            while (j < rightCount && rightSamples[j].time == t) {
                curRight = &rightSamples[j++];
            }
        }

        qint64 nextT = compareEnd;
        if (!leftEnded) nextT = qMin(nextT, leftEnd);
        if (!rightEnded) nextT = qMin(nextT, rightEnd);
        if (i < leftCount && (!leftEnded || leftSamples[i].time < leftEnd)) {
            nextT = qMin(nextT, leftSamples[i].time);
        }
        if (j < rightCount && (!rightEnded || rightSamples[j].time < rightEnd)) {
            nextT = qMin(nextT, rightSamples[j].time);
        }
        if (nextT <= t) nextT = qMin(compareEnd, t + 1);

        if (!compareValueEquivalent(*curLeft, *curRight)) {
            appendCompareDiffRegion(regions, t, nextT, compareStart, compareEnd);
        }

        if (t >= compareEnd) break;
    }

    return regions;
}

void hydrateSignalSamplesForCompare(WaveSignal& sig) {
    rebuildWaveSignalDerivedCaches(sig);
}

QString comparedSideFullPath(const QString& matchedPath, const QString& sidePrefix) {
    const int dot = matchedPath.lastIndexOf(QLatin1Char('.'));
    if (dot < 0) return sidePrefix + QLatin1Char(' ') + matchedPath;
    const QString parent = matchedPath.left(dot);
    const QString leaf = matchedPath.mid(dot + 1);
    return parent + QLatin1Char('.') + sidePrefix + QLatin1Char(' ') + leaf;
}

WaveSignal makeComparedSideSignal(const QString& fullName,
                                  const WaveSignal& src,
                                  int signalId,
                                  const QVector<WaveDiffRegion>& diffRegions) {
    WaveSignal out = src;
    out.signalId = signalId;
    out.name = fullName;
    out.samplesLoaded = true;
    out.diffRegions = diffRegions;
    hydrateSignalSamplesForCompare(out);
    return out;
}

bool buildComparedWaveFile(const QString& leftPath,
                           const WaveFile& leftWave,
                           const QString& rightPath,
                           const WaveFile& rightWave,
                           WaveFile& outWave,
                           QString& error) {
    error.clear();
    outWave = WaveFile();

    QHash<QString, int> leftByPath;
    QVector<QString> leftPathOrder;
    leftByPath.reserve(leftWave.signalList.size() * 2 + 1);
    leftPathOrder.reserve(leftWave.signalList.size());
    for (int i = 0; i < leftWave.signalList.size(); ++i) {
        const QString path = fullSignalPathFromWave(leftWave, i);
        if (path.isEmpty() || leftByPath.contains(path)) continue;
        leftByPath.insert(path, i);
        leftPathOrder.push_back(path);
    }

    QHash<QString, int> rightByPath;
    rightByPath.reserve(rightWave.signalList.size() * 2 + 1);
    for (int i = 0; i < rightWave.signalList.size(); ++i) {
        const QString path = fullSignalPathFromWave(rightWave, i);
        if (path.isEmpty() || rightByPath.contains(path)) continue;
        rightByPath.insert(path, i);
    }

    outWave.meta.title = QStringLiteral("Compare_%1_vs_%2")
        .arg(QFileInfo(leftPath).completeBaseName(), QFileInfo(rightPath).completeBaseName());
    outWave.meta.timescale = (leftWave.meta.timescale == rightWave.meta.timescale)
        ? leftWave.meta.timescale
        : QStringLiteral("cycle");
    outWave.meta.start = qMin(leftWave.meta.start, rightWave.meta.start);
    outWave.meta.end = qMax(qMax(leftWave.meta.end, rightWave.meta.end), outWave.meta.start + 1);

    int commonPathCount = 0;
    int nextSignalId = 1;
    for (const QString& path : leftPathOrder) {
        const int li = leftByPath.value(path, -1);
        const int ri = rightByPath.value(path, -1);
        if (li < 0 || ri < 0) continue;
        ++commonPathCount;
        const WaveSignal& leftSig = leftWave.signalList.at(li);
        const WaveSignal& rightSig = rightWave.signalList.at(ri);
        const QVector<WaveDiffRegion> diffRegions = computeSignalDiffRegions(leftSig, rightSig,
                                                                               outWave.meta.start, outWave.meta.end,
                                                                               leftWave.meta.end, rightWave.meta.end);
        if (diffRegions.isEmpty()) continue;

        const QString leftName = comparedSideFullPath(path, QStringLiteral("A"));
        const QString rightName = comparedSideFullPath(path, QStringLiteral("B"));
        outWave.signalList.push_back(makeComparedSideSignal(leftName, leftSig, nextSignalId++, diffRegions));
        outWave.signalList.push_back(makeComparedSideSignal(rightName, rightSig, nextSignalId++, diffRegions));
    }

    if (commonPathCount == 0) {
        error = QString::fromUtf8("两个文件没有路径完全相同的信号。");
        return false;
    }
    if (outWave.signalList.isEmpty()) {
        error = QString::fromUtf8("没有发现路径相同且任意 cycle 数据不同的信号。");
        return false;
    }
    return true;
}

class ReadOnlyTextDelegate : public QStyledItemDelegate {
public:
    explicit ReadOnlyTextDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex&) const override {
        QLineEdit* editor = new QLineEdit(parent);
        editor->setReadOnly(true);
        editor->setFrame(false);
        editor->setStyleSheet(QStringLiteral("QLineEdit { background:#2E3540; color:#F7FAFF; selection-background-color:#5E8FD6; selection-color:#FFFFFF; padding:0 2px; border:none; }"));
        return editor;
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        QLineEdit* line = qobject_cast<QLineEdit*>(editor);
        if (!line) {
            QStyledItemDelegate::setEditorData(editor, index);
            return;
        }
        const QString text = index.data(Qt::DisplayRole).toString();
        line->setText(text);
        line->setCursorPosition(text.size());
        line->deselect();
    }

    void setModelData(QWidget*, QAbstractItemModel*, const QModelIndex&) const override {
        // Read-only delegate: this editor exists only for text selection/copy and cursor navigation.
    }

    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex&) const override {
        if (editor) editor->setGeometry(option.rect.adjusted(1, 1, -1, -1));
    }
};

class SignalTreeModel : public QAbstractItemModel {
public:
    explicit SignalTreeModel(QObject* parent = nullptr)
        : QAbstractItemModel(parent) {}

    void setLogicTree(SignalLogicTree* tree) {
        beginResetModel();
        m_tree = tree;
        m_searchMode = false;
        clearSearchStorage();
        rebuildRowCache();
        endResetModel();
    }

    void setSearchRoots(const QVector<int>& nodeIds) {
        beginResetModel();
        m_searchMode = true;
        clearSearchStorage();

        if (m_tree) {
            const int n = m_tree->nodes.size();
            m_searchVisible.resize(n);
            std::fill(m_searchVisible.begin(), m_searchVisible.end(), uchar(0));

            // Search mode must still display the original tree structure.  Mark
            // every matched node plus all of its ancestors as visible, then build
            // filtered child lists that preserve WVZ4 NODE order.
            for (int nodeId : nodeIds) {
                if (!isValidNode(nodeId)) continue;
                markNodeAndAncestorsVisible(nodeId);

                // If the matched item is a module/container node, expose its
                // filtered subtree as well.  Searching a module name should let
                // the user expand that module and see/select its members, not
                // leave a dead branch with no children.
                if (m_tree->nodes.at(nodeId).signalIndex < 0) {
                    markSubtreeVisible(nodeId);
                }
            }

            m_searchChildren.resize(n);
            m_searchRowByNodeId.resize(n);
            std::fill(m_searchRowByNodeId.begin(), m_searchRowByNodeId.end(), -1);

            for (int row = 0; row < m_tree->roots.size(); ++row) {
                const int rootNodeId = m_tree->roots[row];
                if (!isSearchVisibleNode(rootNodeId)) continue;
                const int visibleRow = m_searchRootRows.size();
                m_searchRootRows.push_back(rootNodeId);
                if (rootNodeId >= 0 && rootNodeId < m_searchRowByNodeId.size()) {
                    m_searchRowByNodeId[rootNodeId] = visibleRow;
                }
                buildSearchChildren(rootNodeId);
            }
        }

        endResetModel();
    }

    void clearSearch() {
        if (!m_searchMode) return;
        beginResetModel();
        m_searchMode = false;
        clearSearchStorage();
        endResetModel();
    }

    QModelIndex indexForNode(int nodeId, int column = 0) const {
        if (!isValidNode(nodeId) || column < 0 || column >= columnCount()) return QModelIndex();
        if (!isNodeVisibleInCurrentMode(nodeId)) return QModelIndex();

        int row = -1;
        if (m_searchMode) {
            if (nodeId >= 0 && nodeId < m_searchRowByNodeId.size()) row = m_searchRowByNodeId.at(nodeId);
        } else if (nodeId >= 0 && nodeId < m_rowByNodeId.size()) {
            row = m_rowByNodeId.at(nodeId);
        }
        if (row < 0) return QModelIndex();
        return createIndex(row, column, quintptr(nodeId));
    }

    int nodeIdFromIndex(const QModelIndex& index) const {
        if (!index.isValid() || index.model() != this) return -1;
        const int nodeId = int(index.internalId());
        return isValidNode(nodeId) ? nodeId : -1;
    }

    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override {
        if (!m_tree || row < 0 || column < 0 || column >= columnCount()) return QModelIndex();

        int nodeId = -1;
        if (!parent.isValid()) {
            const SmallVec32<int, 32>* rootRows = currentRootRows();
            if (!rootRows || row >= rootRows->size()) return QModelIndex();
            nodeId = (*rootRows)[row];
        } else {
            const int parentNodeId = nodeIdFromIndex(parent);
            const SmallVec32<int, 32>* rows = childRowsForNode(parentNodeId);
            if (!rows || row >= rows->size()) return QModelIndex();
            nodeId = (*rows)[row];
        }

        if (!isValidNode(nodeId)) return QModelIndex();
        return createIndex(row, column, quintptr(nodeId));
    }

    QModelIndex parent(const QModelIndex& child) const override {
        if (!m_tree || !child.isValid() || child.model() != this) return QModelIndex();
        const int nodeId = nodeIdFromIndex(child);
        if (!isValidNode(nodeId)) return QModelIndex();

        const int parentNodeId = m_tree->nodes.at(nodeId).parent;
        if (!isValidNode(parentNodeId)) return QModelIndex();
        if (m_searchMode && !isSearchVisibleNode(parentNodeId)) return QModelIndex();

        int parentRow = -1;
        if (m_searchMode) {
            parentRow = (parentNodeId >= 0 && parentNodeId < m_searchRowByNodeId.size()) ? m_searchRowByNodeId.at(parentNodeId) : -1;
        } else {
            parentRow = (parentNodeId >= 0 && parentNodeId < m_rowByNodeId.size()) ? m_rowByNodeId.at(parentNodeId) : -1;
        }
        return parentRow >= 0 ? createIndex(parentRow, 0, quintptr(parentNodeId)) : QModelIndex();
    }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override {
        if (!m_tree || parent.column() > 0) return 0;
        if (!parent.isValid()) {
            const SmallVec32<int, 32>* rootRows = currentRootRows();
            return rootRows ? rootRows->size() : 0;
        }
        const int parentNodeId = nodeIdFromIndex(parent);
        const SmallVec32<int, 32>* rows = childRowsForNode(parentNodeId);
        return rows ? rows->size() : 0;
    }

    int columnCount(const QModelIndex& = QModelIndex()) const override {
        return 1;
    }

    bool hasChildren(const QModelIndex& parent = QModelIndex()) const override {
        return rowCount(parent) > 0;
    }

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override {
        if (!m_tree || !index.isValid() || index.column() != 0) return QVariant();
        const int nodeId = nodeIdFromIndex(index);
        if (!isValidNode(nodeId)) return QVariant();

        const LogicTreeNode& node = m_tree->nodes.at(nodeId);
        if (role == kTreeRoleNodeId) return nodeId;
        if (role == kTreeRoleSignalIndex) {
            return node.signalIndex >= 0 ? QVariant(node.signalIndex) : QVariant();
        }

        if (role == Qt::DisplayRole) {
            QString text = m_tree->nodeNameString(nodeId);
            if (node.signalIndex >= 0 && node.width > 1) {
                text += QStringLiteral("[%1:0]").arg(node.width - 1);
            }
            return text;
        }
        if (role == Qt::ToolTipRole) {
            return m_tree->fullPathForNodeId(nodeId);
        }
        if (role == Qt::ForegroundRole) {
            if (node.signalIndex >= 0) return QBrush(QColor("#F2F4F7"));
            return QBrush(QColor("#9CC7FF"));
        }
        if (role == Qt::FontRole && node.signalIndex < 0) {
            QFont font;
            font.setBold(true);
            return font;
        }
        return QVariant();
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override {
        if (!index.isValid()) return Qt::NoItemFlags;
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsEditable;
    }

    QStringList mimeTypes() const override {
        QStringList types;
        types << QString::fromLatin1(kMimeSignalIndexes);
        return types;
    }

    QMimeData* mimeData(const QModelIndexList& indexes) const override {
        QMimeData* mime = new QMimeData();
        QSet<int> seenNodes;
        QSet<int> seenSignals;
        QList<int> signalIndexes;
        for (const QModelIndex& index : indexes) {
            if (!index.isValid() || index.column() != 0) continue;
            const int nodeId = nodeIdFromIndex(index);
            if (!isValidNode(nodeId) || seenNodes.contains(nodeId)) continue;
            seenNodes.insert(nodeId);
            collectSignalIndexes(nodeId, seenSignals, signalIndexes);
        }
        if (signalIndexes.isEmpty()) {
            delete mime;
            return nullptr;
        }
        mime->setData(kMimeSignalIndexes, encodeIntList(signalIndexes));
        return mime;
    }

    Qt::DropActions supportedDragActions() const override {
        return Qt::CopyAction;
    }

private:
    SignalLogicTree* m_tree = nullptr;
    QVector<int> m_rowByNodeId;
    bool m_searchMode = false;
    SmallVec32<int, 32> m_searchRootRows;
    QVector<SmallVec32<int, 32>> m_searchChildren;
    QVector<int> m_searchRowByNodeId;
    QVector<uchar> m_searchVisible;

    const SmallVec32<int, 32>* currentRootRows() const {
        if (!m_tree) return nullptr;
        return m_searchMode ? &m_searchRootRows : &m_tree->roots;
    }

    const SmallVec32<int, 32>* childRowsForNode(int nodeId) const {
        if (!isValidNode(nodeId)) return nullptr;
        if (m_searchMode) {
            if (nodeId < 0 || nodeId >= m_searchChildren.size()) return nullptr;
            return &m_searchChildren[nodeId];
        }
        const LogicChildList* list = m_tree->childListForNode(nodeId);
        return list ? &list->children : nullptr;
    }

    bool isValidNode(int nodeId) const {
        return m_tree && nodeId >= 0 && nodeId < m_tree->nodes.size() && m_tree->nodes.at(nodeId).nameId >= 0;
    }

    bool isSearchVisibleNode(int nodeId) const {
        return isValidNode(nodeId) && nodeId >= 0 && nodeId < m_searchVisible.size() && m_searchVisible.at(nodeId) != 0;
    }

    bool isNodeVisibleInCurrentMode(int nodeId) const {
        if (!isValidNode(nodeId)) return false;
        if (!m_searchMode) return true;
        return isSearchVisibleNode(nodeId);
    }

    void clearSearchStorage() {
        m_searchRootRows.clear();
        m_searchChildren.clear();
        m_searchRowByNodeId.clear();
        m_searchVisible.clear();
    }

    void markNodeAndAncestorsVisible(int nodeId) {
        if (!m_tree) return;
        const int n = m_tree->nodes.size();
        int cur = nodeId;
        int guard = 0;
        while (isValidNode(cur) && guard++ < n) {
            if (cur >= 0 && cur < m_searchVisible.size()) m_searchVisible[cur] = 1;
            cur = m_tree->nodes.at(cur).parent;
        }
    }

    void markSubtreeVisible(int nodeId) {
        if (!isValidNode(nodeId) || nodeId < 0 || nodeId >= m_searchVisible.size()) return;
        m_searchVisible[nodeId] = 1;
        const LogicChildList* list = m_tree->childListForNode(nodeId);
        if (!list) return;
        for (int childNodeId : list->children) {
            markSubtreeVisible(childNodeId);
        }
    }

    void buildSearchChildren(int nodeId) {
        if (!isSearchVisibleNode(nodeId) || nodeId < 0 || nodeId >= m_searchChildren.size()) return;
        const LogicChildList* list = m_tree->childListForNode(nodeId);
        if (!list) return;
        SmallVec32<int, 32>& rows = m_searchChildren[nodeId];
        for (int childNodeId : list->children) {
            if (!isSearchVisibleNode(childNodeId)) continue;
            const int row = rows.size();
            rows.push_back(childNodeId);
            if (childNodeId >= 0 && childNodeId < m_searchRowByNodeId.size()) {
                m_searchRowByNodeId[childNodeId] = row;
            }
            buildSearchChildren(childNodeId);
        }
    }

    void rebuildRowCache() {
        m_rowByNodeId.clear();
        if (!m_tree) return;
        m_rowByNodeId.resize(m_tree->nodes.size());
        std::fill(m_rowByNodeId.begin(), m_rowByNodeId.end(), -1);
        for (int row = 0; row < m_tree->roots.size(); ++row) {
            const int nodeId = m_tree->roots[row];
            if (nodeId >= 0 && nodeId < m_rowByNodeId.size()) m_rowByNodeId[nodeId] = row;
        }
        for (int parentNodeId = 0; parentNodeId < m_tree->nodes.size(); ++parentNodeId) {
            const LogicChildList* list = m_tree->childListForNode(parentNodeId);
            if (!list) continue;
            for (int row = 0; row < list->children.size(); ++row) {
                const int nodeId = list->children[row];
                if (nodeId >= 0 && nodeId < m_rowByNodeId.size()) m_rowByNodeId[nodeId] = row;
            }
        }
    }

    void collectSignalIndexes(int nodeId, QSet<int>& seen, QList<int>& output) const {
        if (!isValidNode(nodeId)) return;
        const LogicTreeNode& node = m_tree->nodes.at(nodeId);
        if (node.signalIndex >= 0) {
            if (!seen.contains(node.signalIndex)) {
                seen.insert(node.signalIndex);
                output.push_back(node.signalIndex);
            }
            return;
        }
        const LogicChildList* list = m_tree->childListForNode(nodeId);
        if (!list) return;
        for (int childNodeId : list->children) collectSignalIndexes(childNodeId, seen, output);
    }
};

static inline SignalTreeModel* signalTreeModelFrom(QAbstractItemModel* model) {
    return static_cast<SignalTreeModel*>(model);
}

class SignalTreeView : public QTreeView {
public:
    using ActiveRowsDroppedCallback = std::function<void(const QList<int>&)>;
    explicit SignalTreeView(QWidget* parent = nullptr)
        : QTreeView(parent) {}

    void setActiveRowsDroppedCallback(ActiveRowsDroppedCallback callback) {
        m_activeRowsDropped = std::move(callback);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QTreeView::paintEvent(event);
        if (!model()) return;

        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRect dirty = event ? event->rect() : viewport()->rect();
        drawVisibleDisclosureIndicators(painter, dirty);
    }

    void mousePressEvent(QMouseEvent* event) override {
        m_pressIndex = QModelIndex();
        m_pressPos = QPoint();
        m_pressOnDisclosure = false;
        m_pressWasSelected = false;
        if (event && event->button() == Qt::LeftButton) {
            const QModelIndex idx = indexAt(event->pos());
            if (idx.isValid()) {
                m_pressIndex = idx;
                m_pressPos = event->pos();
                m_pressWasSelected = selectionModel() && selectionModel()->isSelected(idx);
                m_pressOnDisclosure = isDisclosureArea(idx, event->pos());
                if (m_pressOnDisclosure) {
                    event->accept();
                    return;
                }
            }
        }
        QTreeView::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event && event->button() == Qt::LeftButton && m_pressOnDisclosure) {
            const QModelIndex releaseIndex = indexAt(event->pos());
            const bool sameDisclosure = releaseIndex.isValid() &&
                                        m_pressIndex.isValid() &&
                                        releaseIndex == m_pressIndex &&
                                        (event->pos() - m_pressPos).manhattanLength() <= QApplication::startDragDistance() &&
                                        isDisclosureArea(releaseIndex, event->pos());
            if (sameDisclosure && model() && model()->hasChildren(releaseIndex)) {
                setExpanded(releaseIndex, !isExpanded(releaseIndex));
                viewport()->update();
            }
            m_pressIndex = QModelIndex();
            m_pressPos = QPoint();
            m_pressOnDisclosure = false;
            event->accept();
            return;
        }

        const QModelIndex pressIndex = m_pressIndex;
        const QPoint pressPos = m_pressPos;
        const bool pressWasSelected = m_pressWasSelected;
        QTreeView::mouseReleaseEvent(event);

        if (event && event->button() == Qt::LeftButton &&
            event->modifiers() == Qt::NoModifier &&
            pressIndex.isValid() && indexAt(event->pos()) == pressIndex &&
            (event->pos() - pressPos).manhattanLength() <= QApplication::startDragDistance()) {
            maybeOpenTextEditorBySlowSecondClick(pressIndex, pressWasSelected);
        }

        m_pressIndex = QModelIndex();
        m_pressPos = QPoint();
        m_pressOnDisclosure = false;
        m_pressWasSelected = false;
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        m_lastTextClickNodeId = -1;
        m_lastTextClickMs = 0;
        QTreeView::mouseDoubleClickEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event && event->matches(QKeySequence::Copy)) {
            QStringList lines;
            const QModelIndexList picked = selectionModel() ? selectionModel()->selectedRows(0) : QModelIndexList();
            if (!picked.isEmpty()) {
                for (const QModelIndex& idx : picked) {
                    if (idx.isValid()) lines << idx.data(Qt::DisplayRole).toString();
                }
            } else if (currentIndex().isValid()) {
                lines << currentIndex().data(Qt::DisplayRole).toString();
            }
            if (!lines.isEmpty() && QGuiApplication::clipboard()) {
                QGuiApplication::clipboard()->setText(lines.join(QStringLiteral("\n")));
                event->accept();
                return;
            }
        }
        QTreeView::keyPressEvent(event);
    }

    void dragEnterEvent(QDragEnterEvent* event) override {
        if (!event) return;
        const QMimeData* mime = event->mimeData();
        if (mime && mime->hasFormat(kMimeActiveRows)) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        if (mime && mime->hasFormat(kMimeSignalIndexes)) {
            event->ignore();
            return;
        }
        QTreeView::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent* event) override {
        if (!event) return;
        const QMimeData* mime = event->mimeData();
        if (mime && mime->hasFormat(kMimeActiveRows)) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        if (mime && mime->hasFormat(kMimeSignalIndexes)) {
            event->ignore();
            return;
        }
        QTreeView::dragMoveEvent(event);
    }

    void dropEvent(QDropEvent* event) override {
        if (!event) return;
        const QMimeData* mime = event->mimeData();
        if (mime && mime->hasFormat(kMimeActiveRows)) {
            const QList<int> rows = decodeIntList(mime, kMimeActiveRows);
            if (!rows.isEmpty() && m_activeRowsDropped) {
                m_activeRowsDropped(rows);
                event->setDropAction(Qt::CopyAction);
                event->accept();
                return;
            }
        }
        if (mime && mime->hasFormat(kMimeSignalIndexes)) {
            event->ignore();
            return;
        }
        QTreeView::dropEvent(event);
    }

private:
    void maybeOpenTextEditorBySlowSecondClick(const QModelIndex& index, bool pressWasSelected) {
        if (!index.isValid() || index.column() != 0) return;
        if (!pressWasSelected) {
            rememberTextClick(index);
            return;
        }
        const int nodeId = index.data(kTreeRoleNodeId).toInt();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 elapsed = (nodeId == m_lastTextClickNodeId) ? (now - m_lastTextClickMs) : 0;
        const int fastDoubleClickMs = QApplication::doubleClickInterval();
        if (nodeId == m_lastTextClickNodeId && elapsed > fastDoubleClickMs && elapsed < 2000) {
            edit(index);
            m_lastTextClickNodeId = -1;
            m_lastTextClickMs = 0;
            return;
        }
        rememberTextClick(index);
    }

    void rememberTextClick(const QModelIndex& index) {
        m_lastTextClickNodeId = index.isValid() ? index.data(kTreeRoleNodeId).toInt() : -1;
        m_lastTextClickMs = QDateTime::currentMSecsSinceEpoch();
    }

    QRect disclosureRectForIndex(const QModelIndex& index) const {
        if (!index.isValid() || !model() || !model()->hasChildren(index)) return QRect();

        const QRect itemRect = visualRect(index);
        if (!itemRect.isValid()) return QRect();

        QStyleOptionViewItem option;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        initViewItemOption(&option);
#else
        option = viewOptions();
#endif
        option.rect = itemRect;
        option.state |= QStyle::State_Children;
        if (isExpanded(index)) option.state |= QStyle::State_Open;

        QRect rect = style()->subElementRect(QStyle::SE_TreeViewDisclosureItem, &option, this);

        // Some style-sheet combinations make SE_TreeViewDisclosureItem empty or
        // place it over the item body. Fall back to the indentation strip just
        // before the text area; this keeps module rows visibly expandable.
        if (!rect.isValid() || rect.width() < 6 || rect.height() < 6 || rect.left() >= itemRect.left()) {
            const int box = qBound(10, indentation() - 6, 14);
            const int x = qMax(2, itemRect.left() - indentation() + (indentation() - box) / 2);
            const int y = itemRect.top() + (itemRect.height() - box) / 2;
            rect = QRect(x, y, box, box);
        }

        const int box = qBound(10, qMin(rect.width(), rect.height()), 14);
        return QRect(rect.center().x() - box / 2,
                     itemRect.top() + (itemRect.height() - box) / 2,
                     box, box);
    }

    bool isDisclosureArea(const QModelIndex& index, const QPoint& pos) const {
        const QRect rect = disclosureRectForIndex(index);
        return rect.isValid() && rect.adjusted(-3, -3, 3, 3).contains(pos);
    }

    void drawOneDisclosureIndicator(QPainter& painter, const QModelIndex& index) const {
        if (!index.isValid() || !model() || !model()->hasChildren(index)) return;
        const QRect box = disclosureRectForIndex(index);
        if (!box.isValid()) return;

        painter.setPen(QPen(QColor("#E8EDF3"), 1));
        painter.setBrush(QColor("#46515C"));
        painter.drawRoundedRect(box.adjusted(0, 0, -1, -1), 2, 2);

        const int cx = box.center().x();
        const int cy = box.center().y();
        const int half = qMax(3, box.width() / 3);
        painter.setPen(QPen(QColor("#FFFFFF"), 1.4, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(cx - half, cy, cx + half, cy);
        if (!isExpanded(index)) painter.drawLine(cx, cy - half, cx, cy + half);
    }

    void drawVisibleDisclosureIndicators(QPainter& painter, const QRect& dirty) const {
        if (!model() || !viewport()) return;
        const int step = qMax(1, sizeHintForRow(0));
        int y = qMax(0, dirty.top());
        const int maxY = qMin(viewport()->height() - 1, dirty.bottom());

        while (y <= maxY) {
            QModelIndex index = indexAt(QPoint(qMax(0, viewport()->width() / 2), y));
            if (!index.isValid()) index = indexAt(QPoint(qMax(0, indentation() + 4), y));
            if (!index.isValid()) index = indexAt(QPoint(0, y));

            if (!index.isValid()) {
                y += step;
                continue;
            }

            const QRect itemRect = visualRect(index);
            if (!itemRect.isValid()) {
                y += step;
                continue;
            }

            drawOneDisclosureIndicator(painter, index);
            y = itemRect.bottom() + 1;
        }
    }

private:
    ActiveRowsDroppedCallback m_activeRowsDropped;
    QModelIndex m_pressIndex;
    QPoint m_pressPos;
    bool m_pressOnDisclosure = false;
    bool m_pressWasSelected = false;
    int m_lastTextClickNodeId = -1;
    qint64 m_lastTextClickMs = 0;
};

    ValueRadix textToFormat(const QString& s) {
        const QString t = s.trimmed().toLower();
        if (t == "hex") return ValueRadix::Hex;
        if (t == "dec") return ValueRadix::Dec;
        if (t == "int") return ValueRadix::Int;
        if (t == "uint" || t == "unsigned") return ValueRadix::UInt;
        if (t == "float") return ValueRadix::Float;
        if (t == "int64") return ValueRadix::Int64;
        if (t == "uint64" || t == "unsigned64") return ValueRadix::UInt64;
        if (t == "double") return ValueRadix::Double;
        return ValueRadix::Bin;
    }

    QString formatToText(ValueRadix r) {
        switch (r) {
        case ValueRadix::Bin: return "bin";
        case ValueRadix::Hex: return "hex";
        case ValueRadix::Dec: return "dec";
        case ValueRadix::Int: return "int";
        case ValueRadix::UInt: return "uint";
        case ValueRadix::Float: return "float";
        case ValueRadix::Int64: return "int64";
        case ValueRadix::UInt64: return "uint64";
        case ValueRadix::Double: return "double";
        }
        return "bin";
    }

    QSet<int> selectedTopLevelIndexes(QTreeWidget* tree) {
        QSet<int> indexes;
        if (!tree) return indexes;
        const QList<QTreeWidgetItem*> picked = tree->selectedItems();
        for (QTreeWidgetItem* item : picked) {
            const int row = tree->indexOfTopLevelItem(item);
            if (row >= 0) indexes.insert(row);
        }
        return indexes;
    }

    struct ParsedValueFindTarget {
        quint64 bits = 0;
        bool negativeDecimal = false;
    };

    bool parseValueFindTargetText(const QString& text, ParsedValueFindTarget& target) {
        const QString raw = text.trimmed();
        if (raw.isEmpty()) return false;

        bool ok = false;
        if (raw.startsWith(QLatin1Char('-'))) {
            const QString body = raw.mid(1);
            if (!waveIsDecimalDigitsText(body)) return false;
            const qlonglong signedValue = raw.toLongLong(&ok, 10);
            if (!ok) return false;
            target.bits = static_cast<quint64>(signedValue);
            target.negativeDecimal = true;
            return true;
        }

        if (raw.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
            const QString body = raw.mid(2);
            if (!waveIsHexDigitsText(body)) return false;
            const quint64 value = body.toULongLong(&ok, 16);
            if (!ok) return false;
            target.bits = value;
            return true;
        }

        if (raw.startsWith(QStringLiteral("0b"), Qt::CaseInsensitive)) {
            const QString body = raw.mid(2);
            if (!waveIsBinaryDigitsText(body)) return false;
            const quint64 value = body.toULongLong(&ok, 2);
            if (!ok) return false;
            target.bits = value;
            return true;
        }

        if (waveIsDecimalDigitsText(raw)) {
            const quint64 value = raw.toULongLong(&ok, 10);
            if (!ok) return false;
            target.bits = value;
            return true;
        }

        if (waveIsHexDigitsText(raw)) {
            const quint64 value = raw.toULongLong(&ok, 16);
            if (!ok) return false;
            target.bits = value;
            return true;
        }

        return false;
    }

    bool valueFindTargetForSignal(const ParsedValueFindTarget& target, int width, quint64& maskedBits) {
        if (width <= 0) return false;
        const quint64 mask = waveBitMaskForWidth(width);
        if (!target.negativeDecimal && width < 64 && target.bits > mask) return false;
        maskedBits = target.bits & mask;
        return true;
    }

    QString stripDisplayRangeSuffix(QString name) {
        name = name.trimmed();
        if (!name.endsWith(QLatin1Char(']'))) return name;
        const int open = name.lastIndexOf(QLatin1Char('['));
        if (open <= 0) return name;

        const QString body = name.mid(open + 1, name.size() - open - 2).trimmed();
        if (body.isEmpty()) return name;
        bool ok = true;
        bool sawDigit = false;
        for (int i = 0; i < body.size(); ++i) {
            const QChar ch = body.at(i);
            if (ch.isDigit()) {
                sawDigit = true;
                continue;
            }
            if (ch == QLatin1Char(':') || ch.isSpace()) continue;
            ok = false;
            break;
        }
        return (ok && sawDigit) ? name.left(open).trimmed() : name;
    }

    int literalWidthForValue(quint64 value) {
        int width = 1;
        while (width < 64 && (value >> width) != 0) ++width;
        return width;
    }

    struct DerivedExprNode {
        enum Kind {
            Literal,
            Signal,
            Unary,
            Binary
        };

        Kind kind = Literal;
        QString op;
        quint64 literal = 0;
        int signalIndex = -1;
        int signalSlot = -1;
        int left = -1;
        int right = -1;
        int width = 1;
    };

    struct DerivedExpressionProgram {
        QVector<DerivedExprNode> nodes;
        QVector<int> dependencyIndexes;
        int root = -1;
        int inferredWidth = 1;
    };

    struct DerivedEvalValue {
        bool known = false;
        quint64 bits = 0;
    };

    class DerivedExpressionParser {
    public:
        using SignalResolver = std::function<bool(const QString&, int&, int&, QString&)>;

        DerivedExpressionParser(const QString& expression, SignalResolver resolver)
            : m_expression(expression), m_resolver(std::move(resolver)) {
            nextToken();
        }

        bool parse(DerivedExpressionProgram& out, QString& error) {
            m_error.clear();
            const int root = parseExpression(1);
            if (root < 0) {
                error = m_error.isEmpty() ? QStringLiteral("Invalid expression.") : m_error;
                return false;
            }
            if (m_token.type != End) {
                error = QStringLiteral("Unexpected token near '%1'.").arg(m_token.text);
                return false;
            }

            out.nodes = std::move(m_nodes);
            out.dependencyIndexes = std::move(m_dependencyIndexes);
            out.root = root;
            out.inferredWidth = qBound(1, out.nodes.at(root).width, 64);
            return true;
        }

    private:
        enum TokenType {
            End,
            Number,
            Identifier,
            Operator,
            LParen,
            RParen
        };

        struct Token {
            TokenType type = End;
            QString text;
            quint64 value = 0;
            int width = 1;
        };

        static bool isIdentStart(QChar ch) {
            return ch.isLetter() || ch == QLatin1Char('_') || ch == QLatin1Char('$');
        }

        static bool isIdentChar(QChar ch) {
            return ch.isLetterOrNumber() ||
                   ch == QLatin1Char('_') ||
                   ch == QLatin1Char('$') ||
                   ch == QLatin1Char('.') ||
                   ch == QLatin1Char('[') ||
                   ch == QLatin1Char(']') ||
                   ch == QLatin1Char(':');
        }

        void nextToken() {
            while (m_pos < m_expression.size() && m_expression.at(m_pos).isSpace()) ++m_pos;
            m_token = Token();
            if (m_pos >= m_expression.size()) {
                m_token.type = End;
                return;
            }

            const QChar ch = m_expression.at(m_pos);
            if (ch == QLatin1Char('(')) {
                ++m_pos;
                m_token.type = LParen;
                m_token.text = QStringLiteral("(");
                return;
            }
            if (ch == QLatin1Char(')')) {
                ++m_pos;
                m_token.type = RParen;
                m_token.text = QStringLiteral(")");
                return;
            }
            if (ch == QLatin1Char('`')) {
                const int start = ++m_pos;
                while (m_pos < m_expression.size() && m_expression.at(m_pos) != QLatin1Char('`')) ++m_pos;
                if (m_pos >= m_expression.size()) {
                    m_error = QStringLiteral("Missing closing backtick in signal name.");
                    m_token.type = End;
                    return;
                }
                m_token.type = Identifier;
                m_token.text = m_expression.mid(start, m_pos - start).trimmed();
                ++m_pos;
                return;
            }
            if (ch.isDigit()) {
                const int start = m_pos;
                if (ch == QLatin1Char('0') &&
                    m_pos + 1 < m_expression.size() &&
                    (m_expression.at(m_pos + 1) == QLatin1Char('x') || m_expression.at(m_pos + 1) == QLatin1Char('X'))) {
                    m_pos += 2;
                    const int bodyStart = m_pos;
                    while (m_pos < m_expression.size() && m_expression.at(m_pos).isLetterOrNumber()) ++m_pos;
                    const QString body = m_expression.mid(bodyStart, m_pos - bodyStart);
                    bool ok = false;
                    const quint64 value = body.toULongLong(&ok, 16);
                    if (!ok || !waveIsHexDigitsText(body)) {
                        m_error = QStringLiteral("Invalid hex literal near '%1'.").arg(m_expression.mid(start, m_pos - start));
                    }
                    m_token.type = Number;
                    m_token.text = m_expression.mid(start, m_pos - start);
                    m_token.value = value;
                    m_token.width = qBound(1, body.size() * 4, 64);
                    return;
                }
                if (ch == QLatin1Char('0') &&
                    m_pos + 1 < m_expression.size() &&
                    (m_expression.at(m_pos + 1) == QLatin1Char('b') || m_expression.at(m_pos + 1) == QLatin1Char('B'))) {
                    m_pos += 2;
                    const int bodyStart = m_pos;
                    while (m_pos < m_expression.size() && (m_expression.at(m_pos) == QLatin1Char('0') || m_expression.at(m_pos) == QLatin1Char('1'))) ++m_pos;
                    const QString body = m_expression.mid(bodyStart, m_pos - bodyStart);
                    bool ok = false;
                    const quint64 value = body.toULongLong(&ok, 2);
                    if (!ok || !waveIsBinaryDigitsText(body)) {
                        m_error = QStringLiteral("Invalid binary literal near '%1'.").arg(m_expression.mid(start, m_pos - start));
                    }
                    m_token.type = Number;
                    m_token.text = m_expression.mid(start, m_pos - start);
                    m_token.value = value;
                    m_token.width = qBound(1, body.size(), 64);
                    return;
                }

                while (m_pos < m_expression.size() && m_expression.at(m_pos).isDigit()) ++m_pos;
                const QString raw = m_expression.mid(start, m_pos - start);
                bool ok = false;
                const quint64 value = raw.toULongLong(&ok, 10);
                if (!ok) m_error = QStringLiteral("Invalid decimal literal near '%1'.").arg(raw);
                m_token.type = Number;
                m_token.text = raw;
                m_token.value = value;
                m_token.width = literalWidthForValue(value);
                return;
            }
            if (isIdentStart(ch)) {
                const int start = m_pos++;
                while (m_pos < m_expression.size() && isIdentChar(m_expression.at(m_pos))) ++m_pos;
                m_token.type = Identifier;
                m_token.text = m_expression.mid(start, m_pos - start).trimmed();
                return;
            }

            const QString two = (m_pos + 1 < m_expression.size()) ? m_expression.mid(m_pos, 2) : QString();
            if (two == QStringLiteral("&&") || two == QStringLiteral("||") ||
                two == QStringLiteral("==") || two == QStringLiteral("!=") ||
                two == QStringLiteral("<=") || two == QStringLiteral(">=") ||
                two == QStringLiteral("<<") || two == QStringLiteral(">>")) {
                m_pos += 2;
                m_token.type = Operator;
                m_token.text = two;
                return;
            }
            if (QStringLiteral("+-*/%&|^~!<>").contains(ch)) {
                ++m_pos;
                m_token.type = Operator;
                m_token.text = QString(ch);
                return;
            }

            m_error = QStringLiteral("Unsupported character '%1'.").arg(ch);
            ++m_pos;
            m_token.type = End;
        }

        int precedence(const QString& op) const {
            if (op == QStringLiteral("||")) return 1;
            if (op == QStringLiteral("&&")) return 2;
            if (op == QStringLiteral("|")) return 3;
            if (op == QStringLiteral("^")) return 4;
            if (op == QStringLiteral("&")) return 5;
            if (op == QStringLiteral("==") || op == QStringLiteral("!=")) return 6;
            if (op == QStringLiteral("<") || op == QStringLiteral("<=") ||
                op == QStringLiteral(">") || op == QStringLiteral(">=")) return 7;
            if (op == QStringLiteral("<<") || op == QStringLiteral(">>")) return 8;
            if (op == QStringLiteral("+") || op == QStringLiteral("-")) return 9;
            if (op == QStringLiteral("*") || op == QStringLiteral("/") || op == QStringLiteral("%")) return 10;
            return 0;
        }

        int addNode(DerivedExprNode node) {
            node.width = qBound(1, node.width, 64);
            const int index = m_nodes.size();
            m_nodes.push_back(std::move(node));
            return index;
        }

        int parseExpression(int minPrecedence) {
            int lhs = parseUnary();
            if (lhs < 0) return -1;

            while (m_token.type == Operator) {
                const QString op = m_token.text;
                const int prec = precedence(op);
                if (prec < minPrecedence) break;
                nextToken();
                const int rhs = parseExpression(prec + 1);
                if (rhs < 0) return -1;

                DerivedExprNode node;
                node.kind = DerivedExprNode::Binary;
                node.op = op;
                node.left = lhs;
                node.right = rhs;
                if (op == QStringLiteral("&&") || op == QStringLiteral("||") ||
                    op == QStringLiteral("==") || op == QStringLiteral("!=") ||
                    op == QStringLiteral("<") || op == QStringLiteral("<=") ||
                    op == QStringLiteral(">") || op == QStringLiteral(">=")) {
                    node.width = 1;
                } else if (op == QStringLiteral("<<") || op == QStringLiteral(">>")) {
                    node.width = m_nodes.at(lhs).width;
                } else {
                    node.width = qMax(m_nodes.at(lhs).width, m_nodes.at(rhs).width);
                }
                lhs = addNode(std::move(node));
            }
            return lhs;
        }

        int parseUnary() {
            if (m_token.type == Operator &&
                (m_token.text == QStringLiteral("+") ||
                 m_token.text == QStringLiteral("-") ||
                 m_token.text == QStringLiteral("!") ||
                 m_token.text == QStringLiteral("~"))) {
                const QString op = m_token.text;
                nextToken();
                const int child = parseUnary();
                if (child < 0) return -1;

                DerivedExprNode node;
                node.kind = DerivedExprNode::Unary;
                node.op = op;
                node.left = child;
                node.width = (op == QStringLiteral("!")) ? 1 : m_nodes.at(child).width;
                return addNode(std::move(node));
            }
            return parsePrimary();
        }

        int parsePrimary() {
            if (!m_error.isEmpty()) return -1;

            if (m_token.type == Number) {
                DerivedExprNode node;
                node.kind = DerivedExprNode::Literal;
                node.literal = m_token.value;
                node.width = m_token.width;
                nextToken();
                return addNode(std::move(node));
            }

            if (m_token.type == Identifier) {
                const QString name = m_token.text;
                int signalIndex = -1;
                int width = 1;
                QString resolveError;
                if (!m_resolver(name, signalIndex, width, resolveError)) {
                    m_error = resolveError.isEmpty() ? QStringLiteral("Unknown signal '%1'.").arg(name) : resolveError;
                    return -1;
                }

                int slot = m_depSlotBySignalIndex.value(signalIndex, -1);
                if (slot < 0) {
                    slot = m_dependencyIndexes.size();
                    m_dependencyIndexes.push_back(signalIndex);
                    m_depSlotBySignalIndex.insert(signalIndex, slot);
                }

                DerivedExprNode node;
                node.kind = DerivedExprNode::Signal;
                node.signalIndex = signalIndex;
                node.signalSlot = slot;
                node.width = width;
                nextToken();
                return addNode(std::move(node));
            }

            if (m_token.type == LParen) {
                nextToken();
                const int node = parseExpression(1);
                if (node < 0) return -1;
                if (m_token.type != RParen) {
                    m_error = QStringLiteral("Missing closing parenthesis.");
                    return -1;
                }
                nextToken();
                return node;
            }

            m_error = QStringLiteral("Expected a number, signal name, or parenthesized expression.");
            return -1;
        }

        QString m_expression;
        SignalResolver m_resolver;
        int m_pos = 0;
        Token m_token;
        QString m_error;
        QVector<DerivedExprNode> m_nodes;
        QVector<int> m_dependencyIndexes;
        QHash<int, int> m_depSlotBySignalIndex;
    };

    DerivedEvalValue evalDerivedExpressionNode(const DerivedExpressionProgram& program,
                                               int nodeIndex,
                                               const QVector<WaveSample>& currentSamples) {
        if (nodeIndex < 0 || nodeIndex >= program.nodes.size()) return DerivedEvalValue();

        const DerivedExprNode& node = program.nodes.at(nodeIndex);
        const quint64 mask = waveBitMaskForWidth(node.width);

        if (node.kind == DerivedExprNode::Literal) {
            DerivedEvalValue out;
            out.known = true;
            out.bits = node.literal & mask;
            return out;
        }

        if (node.kind == DerivedExprNode::Signal) {
            if (node.signalSlot < 0 || node.signalSlot >= currentSamples.size()) return DerivedEvalValue();
            WaveSample sample = currentSamples.at(node.signalSlot);
            if (!sample.rawFieldsReady) {
                hydrateWaveSampleRawFields(node.width == 1 ? SignalKind::Bit : SignalKind::Bus, node.width, sample);
            }
            if (sample.isAbsent || sample.isZ) return DerivedEvalValue();

            DerivedEvalValue out;
            out.known = true;
            out.bits = sample.rawBits & mask;
            return out;
        }

        if (node.kind == DerivedExprNode::Unary) {
            const DerivedEvalValue v = evalDerivedExpressionNode(program, node.left, currentSamples);
            if (!v.known) return DerivedEvalValue();

            quint64 bits = v.bits;
            if (node.op == QStringLiteral("-")) bits = quint64(0) - bits;
            else if (node.op == QStringLiteral("~")) bits = ~bits;
            else if (node.op == QStringLiteral("!")) bits = bits ? 0ull : 1ull;

            DerivedEvalValue out;
            out.known = true;
            out.bits = bits & mask;
            return out;
        }

        const DerivedEvalValue lhs = evalDerivedExpressionNode(program, node.left, currentSamples);
        const DerivedEvalValue rhs = evalDerivedExpressionNode(program, node.right, currentSamples);
        if (!lhs.known || !rhs.known) return DerivedEvalValue();

        quint64 bits = 0;
        const QString& op = node.op;
        if (op == QStringLiteral("+")) bits = lhs.bits + rhs.bits;
        else if (op == QStringLiteral("-")) bits = lhs.bits - rhs.bits;
        else if (op == QStringLiteral("*")) bits = lhs.bits * rhs.bits;
        else if (op == QStringLiteral("/")) {
            if (rhs.bits == 0) return DerivedEvalValue();
            bits = lhs.bits / rhs.bits;
        }
        else if (op == QStringLiteral("%")) {
            if (rhs.bits == 0) return DerivedEvalValue();
            bits = lhs.bits % rhs.bits;
        }
        else if (op == QStringLiteral("&")) bits = lhs.bits & rhs.bits;
        else if (op == QStringLiteral("|")) bits = lhs.bits | rhs.bits;
        else if (op == QStringLiteral("^")) bits = lhs.bits ^ rhs.bits;
        else if (op == QStringLiteral("<<")) bits = (rhs.bits >= 64) ? 0ull : (lhs.bits << int(rhs.bits));
        else if (op == QStringLiteral(">>")) bits = (rhs.bits >= 64) ? 0ull : (lhs.bits >> int(rhs.bits));
        else if (op == QStringLiteral("&&")) bits = (lhs.bits != 0 && rhs.bits != 0) ? 1ull : 0ull;
        else if (op == QStringLiteral("||")) bits = (lhs.bits != 0 || rhs.bits != 0) ? 1ull : 0ull;
        else if (op == QStringLiteral("==")) bits = (lhs.bits == rhs.bits) ? 1ull : 0ull;
        else if (op == QStringLiteral("!=")) bits = (lhs.bits != rhs.bits) ? 1ull : 0ull;
        else if (op == QStringLiteral("<")) bits = (lhs.bits < rhs.bits) ? 1ull : 0ull;
        else if (op == QStringLiteral("<=")) bits = (lhs.bits <= rhs.bits) ? 1ull : 0ull;
        else if (op == QStringLiteral(">")) bits = (lhs.bits > rhs.bits) ? 1ull : 0ull;
        else if (op == QStringLiteral(">=")) bits = (lhs.bits >= rhs.bits) ? 1ull : 0ull;

        DerivedEvalValue out;
        out.known = true;
        out.bits = bits & mask;
        return out;
    }

    bool sameDerivedSampleValue(const WaveSample& a, const WaveSample& b, int width) {
        if (a.isAbsent != b.isAbsent || a.isZ != b.isZ) return false;
        if (a.isAbsent || a.isZ) return true;
        return (a.rawBits & waveBitMaskForWidth(width)) == (b.rawBits & waveBitMaskForWidth(width));
    }

    WaveSample makeDerivedSample(qint64 time, const DerivedEvalValue& value, int width, ValueRadix radix) {
        WaveSample sample;
        sample.time = time;
        sample.rawFieldsReady = true;
        if (!value.known) {
            sample.isZ = true;
            sample.value = QStringLiteral("Z");
            return sample;
        }

        const SignalKind kind = (width <= 1) ? SignalKind::Bit : SignalKind::Bus;
        sample.rawBits = value.bits & waveBitMaskForWidth(width);
        sample.value = waveSampleRawText(kind, width, radix, sample);
        return sample;
    }

    QString formatInternalDisplayTime(qint64 internalTime) {
        return QString::number(internalTime);
    }

    QColor iconColor(const QString& name) {
        if (name == "open") return QColor("#43C59E");
        if (name == "compare") return QColor("#B86BFF");
        if (name == "derive") return QColor("#2EBAC6");
        if (name == "save") return QColor("#5CA8FF");
        if (name == "zoom_in" || name == "zoom_out") return QColor("#F0A43A");
        if (name == "left" || name == "right") return QColor("#55B4FF");
        if (name == "prev_change" || name == "next_change") return QColor("#62C462");
        if (name == "reset") return QColor("#FF7D57");
        return QColor("#7A91AA");
    }

    QIcon makeColorfulToolbarIcon(const QString& kind) {
        QPixmap pm(28, 28);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QColor base = iconColor(kind);
        const QColor light = base.lighter(155);
        const QColor mid = base.lighter(115);
        const QColor dark = base.darker(150);

        auto fillGradient = [&](const QRectF& r) {
            QLinearGradient g(r.topLeft(), r.bottomLeft());
            g.setColorAt(0.0, light);
            g.setColorAt(0.45, mid);
            g.setColorAt(1.0, dark);
            return g;
        };

        auto strokePen = QPen(dark, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);

        if (kind == "open") {
            QRectF body(4, 8, 20, 13);
            p.setPen(strokePen);
            p.setBrush(fillGradient(body));
            p.drawRoundedRect(body, 3, 3);
            p.setPen(QPen(QColor(255, 255, 255, 160), 1.2));
            p.drawLine(QPointF(7, 9), QPointF(18, 9));
            p.setPen(QPen(dark, 1.8));
            p.drawLine(7, 8, 10, 5); p.drawLine(10, 5, 16, 5); p.drawLine(16, 5, 19, 8);
        }
        else if (kind == "compare") {
            QRectF leftDoc(4.5, 5.0, 10.5, 16.0);
            QRectF rightDoc(13.0, 7.0, 10.5, 16.0);
            p.setPen(strokePen);
            p.setBrush(fillGradient(leftDoc));
            p.drawRoundedRect(leftDoc, 2.5, 2.5);
            p.setBrush(fillGradient(rightDoc));
            p.drawRoundedRect(rightDoc, 2.5, 2.5);
            p.setPen(QPen(QColor(255, 255, 255, 185), 1.1, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(7, 10, 12, 10);
            p.drawLine(7, 14, 12, 14);
            p.drawLine(16, 12, 21, 12);
            p.drawLine(16, 16, 21, 16);
            p.setPen(QPen(QColor("#FF4D4F"), 2.0, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(11, 22, 17, 4);
        }
        else if (kind == "derive") {
            p.setPen(QPen(dark, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(fillGradient(QRectF(5, 5, 18, 18)));
            p.drawRoundedRect(QRectF(5, 5, 18, 18), 3, 3);
            p.setPen(QPen(QColor(255, 255, 255, 190), 1.7, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(9, 14, 13, 10);
            p.drawLine(9, 14, 13, 18);
            p.drawLine(15, 10, 19, 14);
            p.drawLine(15, 18, 19, 14);
            p.setPen(QPen(dark, 1.6, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(9, 8, 19, 8);
            p.drawLine(9, 20, 19, 20);
        }
        else if (kind == "save") {
            QRectF body(5, 4, 18, 20);
            p.setPen(strokePen);
            p.setBrush(fillGradient(body));
            p.drawRoundedRect(body, 3, 3);
            p.setPen(QPen(QColor(255, 255, 255, 170), 1.2));
            p.drawLine(8, 7, 20, 7);
            p.setPen(QPen(dark, 1.8));
            p.drawLine(9, 8, 18, 8); p.drawLine(9, 12, 18, 12); p.drawLine(11, 17, 17, 17);
        }
        else if (kind == "zoom_in" || kind == "zoom_out") {
            QRectF lens(4, 4, 13, 13);
            p.setPen(QPen(dark, 1.8));
            p.setBrush(fillGradient(lens));
            p.drawEllipse(lens);
            p.setPen(QPen(QColor(255, 255, 255, 170), 1.0));
            p.drawArc(QRectF(5.5, 5.5, 8, 6), 20 * 16, 120 * 16);
            p.setPen(QPen(dark, 2.1, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(16, 16, 23, 23);
            if (kind == "zoom_in") p.drawLine(10.5, 7, 10.5, 13);
            p.drawLine(7.5, 10, 13.5, 10);
        }
        else if (kind == "left" || kind == "right") {
            QPainterPath path;
            if (kind == "left") {
                path.moveTo(9, 14); path.lineTo(17, 7); path.lineTo(17, 11); path.lineTo(24, 11); path.lineTo(24, 17); path.lineTo(17, 17); path.lineTo(17, 21); path.closeSubpath();
            }
            else {
                path.moveTo(19, 14); path.lineTo(11, 7); path.lineTo(11, 11); path.lineTo(4, 11); path.lineTo(4, 17); path.lineTo(11, 17); path.lineTo(11, 21); path.closeSubpath();
            }
            p.setPen(strokePen);
            QRectF br = path.boundingRect();
            p.setBrush(fillGradient(br));
            p.drawPath(path);
            p.setPen(QPen(QColor(255, 255, 255, 130), 1.0));
            p.drawLine(QPointF(br.left() + 2, br.top() + 2), QPointF(br.right() - 2, br.top() + 2));
        }
        else if (kind == "prev_change" || kind == "next_change") {
            p.setPen(QPen(dark, 1.8, Qt::SolidLine, Qt::RoundCap));
            if (kind == "prev_change") {
                p.drawLine(7, 6, 7, 22);
                QPainterPath path; path.moveTo(21, 8); path.lineTo(12, 14); path.lineTo(21, 20); path.closeSubpath();
                QRectF br = path.boundingRect();
                p.setBrush(fillGradient(br));
                p.drawPath(path);
            }
            else {
                p.drawLine(21, 6, 21, 22);
                QPainterPath path; path.moveTo(7, 8); path.lineTo(16, 14); path.lineTo(7, 20); path.closeSubpath();
                QRectF br = path.boundingRect();
                p.setBrush(fillGradient(br));
                p.drawPath(path);
            }
        }
        else if (kind == "reset") {
            p.setPen(QPen(dark, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawArc(QRectF(5, 5, 18, 18), 35 * 16, 285 * 16);
            QPainterPath path; path.moveTo(18, 5); path.lineTo(24, 6); path.lineTo(22, 12); path.closeSubpath();
            QRectF br = path.boundingRect();
            p.setBrush(fillGradient(br));
            p.drawPath(path);
        }
        return QIcon(pm);
    }

    QIcon makeAppIcon() {
        QPixmap pm(64, 64);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#20354E"));
        p.drawRoundedRect(QRectF(2, 2, 60, 60), 12, 12);
        p.setBrush(QColor("#30C56C"));
        p.drawRoundedRect(QRectF(10, 36, 18, 10), 3, 3);
        p.drawRoundedRect(QRectF(28, 20, 24, 10), 3, 3);
        p.setPen(QPen(QColor("#FFFFFF"), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(10, 41, 18, 41);
        p.drawLine(28, 25, 52, 25);
        p.drawLine(28, 25, 28, 41);
        p.drawLine(28, 41, 42, 41);
        p.drawLine(42, 41, 42, 14);
        p.drawLine(42, 14, 52, 14);
        return QIcon(pm);
    }

    QIcon loadImageIconOrFallback(const QString& baseName, const QString& fallbackKind) {
        const QStringList candidates{
            QCoreApplication::applicationDirPath() + "/icons/" + baseName + ".png",
            QFileInfo(QCoreApplication::applicationDirPath() + "/../icons/" + baseName + ".png").absoluteFilePath()
        };
        for (const QString& path : candidates) {
            if (QFileInfo::exists(path)) {
                QPixmap pm(path);
                if (!pm.isNull()) return QIcon(pm);
            }
        }
        return makeColorfulToolbarIcon(fallbackKind);
    }

    QString sanitizeNoZTextValue(const QString& raw, const bool isBit) {
        static const QString kAbsentValueText = QStringLiteral("__WVZ_ABSENT__");
        if (raw == kAbsentValueText) return raw;

        const QString trimmed = raw.trimmed();
        if (isBit) {
            return trimmed == QStringLiteral("1") ? QStringLiteral("1") : QStringLiteral("0");
        }

        QString out = trimmed;
        if (out.isEmpty()) return QStringLiteral("0");
        for (QChar& ch : out) {
            if (ch == QLatin1Char('z') || ch == QLatin1Char('Z') ||
                ch == QLatin1Char('x') || ch == QLatin1Char('X')) {
                ch = QLatin1Char('0');
            }
        }
        return out;
    }

    WaveFile buildExportWaveNoZ(const WaveFile& src) {
        WaveFile dst = src;
        for (WaveSignal& sig : dst.signalList) {
            sig.supportsZState = false;
            const bool isBit = (sig.kind == SignalKind::Bit);
            for (WaveSample& sample : sig.samples) {
                if (sample.isAbsent) continue;
                sample.isZ = false;
                if (isBit) sample.rawBits = (sample.rawBits & 1ull);
                sample.value = sanitizeNoZTextValue(waveSampleRawText(sig, sample), isBit);
                hydrateWaveSampleRawFields(sig.kind, sig.width, sample);
            }
        }
        return dst;
    }

}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowIcon(loadImageIconOrFallback("app", "app"));
    if (windowIcon().isNull()) setWindowIcon(makeAppIcon());
    buildUi();
    applyTheme();
    loadDemoWave();
}

MainWindow::~MainWindow() = default;

void MainWindow::setupToolbarButton(QPushButton* button, const QIcon& icon, const QString& objectName, const QString& tip) {
    button->setObjectName(objectName);
    button->setIcon(icon.isNull() ? makeColorfulToolbarIcon("open") : icon);
    button->setIconSize(QSize(18, 18));
    button->setText(QString());
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    button->setAccessibleDescription(tip);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setFixedSize(28, 28);
}

void MainWindow::buildUi() {
    setWindowTitle("Wave Viewer - Qt/C++");
    setMinimumSize(1120, 700);
    resize(1860, 980);

    m_central = new QWidget(this);
    setCentralWidget(m_central);

    auto* root = new QVBoxLayout(m_central);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    m_splitter = new QSplitter(this);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(4);
    m_splitter->setOpaqueResize(false);
    root->addWidget(m_splitter, 1);

    auto* leftPane = new QFrame();
    leftPane->setObjectName("darkPanel");
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(8, 8, 8, 8);
    leftLayout->setSpacing(6);
    auto* leftTitle = new QLabel(QString::fromUtf8("可选信号"), leftPane);
    leftTitle->setObjectName("panelTitle");
    leftLayout->addWidget(leftTitle);

    m_treeSearchEdit = new QLineEdit(leftPane);
    m_treeSearchEdit->setPlaceholderText(QString::fromUtf8("搜索层级名 / 信号名"));
    leftLayout->addWidget(m_treeSearchEdit);

    auto* signalTree = new SignalTreeView(leftPane);
    signalTree->setActiveRowsDroppedCallback([this](const QList<int>& rows) {
        removeActiveRows(rows);
    });
    m_tree = signalTree;
    m_treeModel = new SignalTreeModel(m_tree);
    m_tree->setModel(m_treeModel);
    m_tree->setItemDelegate(new ReadOnlyTextDelegate(m_tree));
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setIndentation(18);
    m_tree->setAnimated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setSortingEnabled(false);
    m_tree->setAutoExpandDelay(-1);
    m_tree->setExpandsOnDoubleClick(false);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setDragEnabled(true);
    m_tree->setAcceptDrops(true);
    m_tree->setDropIndicatorShown(true);
    m_tree->setDefaultDropAction(Qt::CopyAction);
    m_tree->setDragDropMode(QAbstractItemView::DragDrop);
    leftLayout->addWidget(m_tree, 1);


    auto* middlePane = new QFrame();
    middlePane->setObjectName("darkPanel");
    auto* middleLayout = new QVBoxLayout(middlePane);
    middleLayout->setContentsMargins(8, 8, 8, 8);
    middleLayout->setSpacing(6);

    auto* middleTitle = new QLabel(QString::fromUtf8("选中信号"), middlePane);
    middleTitle->setObjectName("panelTitle");
    middleLayout->addWidget(middleTitle);

    m_activeSearchEdit = new QLineEdit(middlePane);
    m_activeSearchEdit->setPlaceholderText(QString::fromUtf8("搜索选中信号"));
    middleLayout->addWidget(m_activeSearchEdit);

    m_activeList = new ActiveSignalListWidget(middlePane);
    middleLayout->addWidget(m_activeList, 1);

    auto* rightPane = new QFrame();
    rightPane->setObjectName("wavePanel");
    auto* rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 6);
    rightLayout->setSpacing(4);

    auto* topBarCard = new QFrame(rightPane);
    topBarCard->setObjectName("toolbarCard");
    auto* topBarLayout = new QHBoxLayout(topBarCard);
    topBarCard->setFixedHeight(41);
    topBarLayout->setContentsMargins(6, 5, 6, 5);
    topBarLayout->setSpacing(4);

    auto* btnOpen = new QPushButton(topBarCard);
    auto* btnCompare = new QPushButton(topBarCard);
    auto* btnDerivedSignal = new QPushButton(topBarCard);
    auto* btnExportCompressed = new QPushButton(topBarCard);
    auto* btnZoomIn = new QPushButton(topBarCard);
    auto* btnZoomOut = new QPushButton(topBarCard);
    auto* btnPanLeft = new QPushButton(topBarCard);
    auto* btnPanRight = new QPushButton(topBarCard);
    auto* btnPrevChange = new QPushButton(topBarCard);
    auto* btnNextChange = new QPushButton(topBarCard);
    auto* btnReset = new QPushButton(topBarCard);

    setupToolbarButton(btnOpen, loadImageIconOrFallback("open", "open"), QStringLiteral("toolbarOpenButton"), QString::fromUtf8("打开波形文件"));
    setupToolbarButton(btnCompare, loadImageIconOrFallback("compare", "compare"), QStringLiteral("toolbarCompareButton"), QString::fromUtf8("比较两个波形文件"));
    setupToolbarButton(btnDerivedSignal, loadImageIconOrFallback("derive", "derive"), QStringLiteral("toolbarDerivedSignalButton"), QStringLiteral("Create temporary signal"));
    setupToolbarButton(btnExportCompressed, loadImageIconOrFallback("save", "save"), QStringLiteral("toolbarExportButton"), QString::fromUtf8("导出波形文件"));
    setupToolbarButton(btnZoomIn, loadImageIconOrFallback("zoom_in", "zoom_in"), QStringLiteral("toolbarZoomInButton"), QString::fromUtf8("放大"));
    setupToolbarButton(btnZoomOut, loadImageIconOrFallback("zoom_out", "zoom_out"), QStringLiteral("toolbarZoomOutButton"), QString::fromUtf8("缩小"));
    setupToolbarButton(btnPanLeft, loadImageIconOrFallback("left", "left"), QStringLiteral("toolbarPanLeftButton"), QString::fromUtf8("左移"));
    setupToolbarButton(btnPanRight, loadImageIconOrFallback("right", "right"), QStringLiteral("toolbarPanRightButton"), QString::fromUtf8("右移"));
    setupToolbarButton(btnPrevChange, loadImageIconOrFallback("prev_change", "prev_change"), QStringLiteral("toolbarPrevChangeButton"), QString::fromUtf8("跳到选中信号上一次变化"));
    setupToolbarButton(btnNextChange, loadImageIconOrFallback("next_change", "next_change"), QStringLiteral("toolbarNextChangeButton"), QString::fromUtf8("跳到选中信号下一次变化"));
    setupToolbarButton(btnReset, loadImageIconOrFallback("reset", "reset"), QStringLiteral("toolbarResetButton"), QString::fromUtf8("重置视图"));

    m_jumpTimeEdit = new QLineEdit(topBarCard);
    m_jumpTimeEdit->setObjectName("jumpTimeEdit");
    m_jumpTimeEdit->setFixedWidth(118);
    m_jumpTimeEdit->setPlaceholderText(QString::fromUtf8("跳转时间"));
    m_jumpTimeEdit->setToolTip(QString::fromUtf8("输入时间轴显示值后按 Enter 跳转"));

    m_metaLabel = new QLabel("-", topBarCard);
    m_metaLabel->setObjectName("metaLabel");
    m_metaLabel->setMinimumWidth(90);

    topBarLayout->addWidget(btnOpen);
    topBarLayout->addWidget(btnCompare);
    topBarLayout->addWidget(btnDerivedSignal);
    topBarLayout->addWidget(btnExportCompressed);
    topBarLayout->addSpacing(4);
    topBarLayout->addWidget(btnZoomIn);
    topBarLayout->addWidget(btnZoomOut);
    topBarLayout->addWidget(btnPanLeft);
    topBarLayout->addWidget(btnPanRight);
    topBarLayout->addWidget(btnPrevChange);
    topBarLayout->addWidget(btnNextChange);
    topBarLayout->addWidget(btnReset);
    topBarLayout->addSpacing(8);
    topBarLayout->addWidget(m_jumpTimeEdit);
    topBarLayout->addSpacing(8);
    topBarLayout->addWidget(m_metaLabel);
    topBarLayout->addStretch(1);
    rightLayout->addWidget(topBarCard);

    m_canvas = new WaveCanvas(rightPane);
    rightLayout->addWidget(m_canvas, 1);

    m_splitter->addWidget(leftPane);
    m_splitter->addWidget(middlePane);
    m_splitter->addWidget(rightPane);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 0);
    m_splitter->setStretchFactor(2, 1);
    m_splitter->setSizes(QList<int>() << 330 << 420 << 1110);

    connect(btnOpen, &QPushButton::clicked, this, &MainWindow::openWaveFile);
    connect(btnCompare, &QPushButton::clicked, this, &MainWindow::compareWaveFiles);
    connect(btnDerivedSignal, &QPushButton::clicked, this, &MainWindow::openDerivedSignalDialog);
    connect(btnExportCompressed, &QPushButton::clicked, this, &MainWindow::exportCompressedWaveFile);
    connect(btnZoomIn, &QPushButton::clicked, this, &MainWindow::zoomIn);
    connect(btnZoomOut, &QPushButton::clicked, this, &MainWindow::zoomOut);
    connect(btnPanLeft, &QPushButton::clicked, this, &MainWindow::panLeft);
    connect(btnPanRight, &QPushButton::clicked, this, &MainWindow::panRight);
    connect(btnPrevChange, &QPushButton::clicked, this, &MainWindow::jumpToPrevChange);
    connect(btnNextChange, &QPushButton::clicked, this, &MainWindow::jumpToNextChange);
    connect(btnReset, &QPushButton::clicked, this, &MainWindow::resetView);
    connect(m_jumpTimeEdit, &QLineEdit::returnPressed, this, &MainWindow::jumpToTime);

    auto* findValueShortcut = new QShortcut(QKeySequence::Find, this);
    findValueShortcut->setContext(Qt::WindowShortcut);
    connect(findValueShortcut, &QShortcut::activated, this, &MainWindow::openValueFindDialog);

    auto* derivedSignalShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+E")), this);
    derivedSignalShortcut->setContext(Qt::WindowShortcut);
    connect(derivedSignalShortcut, &QShortcut::activated, this, &MainWindow::openDerivedSignalDialog);

    connect(m_tree, &QTreeView::doubleClicked, this, &MainWindow::onTreeIndexDoubleClicked);
    connect(m_treeSearchEdit, &QLineEdit::returnPressed, this, [this]() {
        onTreeSearchTextChanged(m_treeSearchEdit ? m_treeSearchEdit->text() : QString());
    });

    connect(m_activeSearchEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (!m_activeList) return;
        const QString q = text.trimmed();
        m_activeList->clearSelection();
        QTreeWidgetItem* firstMatch = nullptr;
        for (int i = 0; i < m_activeList->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = m_activeList->topLevelItem(i);
            const bool match = q.isEmpty() || item->text(0).contains(q, Qt::CaseInsensitive);
            item->setHidden(false);
            if (!q.isEmpty() && match) {
                item->setSelected(true);
                if (!firstMatch) firstMatch = item;
            }
        }
        if (firstMatch) {
            m_activeList->setCurrentItem(firstMatch, 0, QItemSelectionModel::NoUpdate);
            m_activeList->scrollToItem(firstMatch);
        }
        m_canvas->setSelectedEntryIndexes(selectedTopLevelIndexes(m_activeList));
        syncActiveScrollToCanvas();
        });
    connect(m_canvas, &WaveCanvas::cursorMoved, this, &MainWindow::onCursorMoved);
    connect(m_canvas, &WaveCanvas::hoverMoved, this, &MainWindow::onHoverMoved);
    connect(m_canvas, &WaveCanvas::viewportChanged, this, &MainWindow::onViewportChanged);

    connect(m_canvas, &WaveCanvas::entryClicked, this, [this](int row, bool ctrlHeld) {
        if (row < 0 || row >= m_activeList->topLevelItemCount()) return;
        if (QTreeWidgetItem* item = m_activeList->topLevelItem(row)) {
            if (ctrlHeld) {
                const bool nextSelected = !item->isSelected();
                item->setSelected(nextSelected);
                m_activeList->setCurrentItem(item);
            }
            else {
                m_activeList->clearSelection();
                item->setSelected(true);
                m_activeList->setCurrentItem(item);
            }
            m_activeList->scrollToItem(item);
            m_canvas->setSelectedEntryIndexes(selectedTopLevelIndexes(m_activeList));
        }
        });

    connect(m_activeList->model(), &QAbstractItemModel::rowsMoved, this, [this]() {
        rebuildVisibleSignals();
        refreshActiveValueLabels();
        onActiveCurrentItemChanged(m_activeList->currentItem(), nullptr);
        });
    connect(m_activeList, &QTreeWidget::currentItemChanged, this, &MainWindow::onActiveCurrentItemChanged);
    connect(m_activeList, &QTreeWidget::itemSelectionChanged, this, [this]() {
        m_canvas->setSelectedEntryIndexes(selectedTopLevelIndexes(m_activeList));
        });
    connect(m_activeList->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        syncActiveScrollToCanvas();
        scheduleRefreshActiveValueLabels(0);
    });
    connect(m_activeList, &ActiveSignalListWidget::deleteRequested, this, &MainWindow::removeActiveItem);
    connect(m_activeList, &ActiveSignalListWidget::formatRequested, this, &MainWindow::setActiveItemFormat);
    connect(m_activeList, &ActiveSignalListWidget::signalIndexesDropped, this, &MainWindow::addSignalIndexesToActive);
    connect(m_activeList, &ActiveSignalListWidget::activeRowsReordered, this, [this]() {
        rebuildVisibleSignals();
        refreshActiveValueLabels();
        onActiveCurrentItemChanged(m_activeList->currentItem(), nullptr);
    });

    m_activeValueRefreshTimer = new QTimer(this);
    m_activeValueRefreshTimer->setSingleShot(true);
    connect(m_activeValueRefreshTimer, &QTimer::timeout, this, [this]() {
        refreshActiveValueLabels();
    });

    clampWindowToAvailableScreen();
}

void MainWindow::applyTheme() {
    setStyleSheet(R"(
        QWidget {
            background: #64707C;
            color: #F4F7FA;
            font-family: "Segoe UI", "Microsoft YaHei";
            font-size: 10.2pt;
        }
        QMainWindow {
            background: #5B6773;
        }
        #toolbarCard {
            background: #D7DADF;
            border: none;
            border-top-left-radius: 10px;
            border-top-right-radius: 10px;
            border-bottom-left-radius: 0px;
            border-bottom-right-radius: 0px;
        }
        #darkPanel {
            background: #76818C;
            border: 2px solid #FFFFFF;
            border-radius: 10px;
        }
        #wavePanel {
            background: #0A0A0A;
            border: 1px solid #FFFFFF;
            border-radius: 10px;
        }
        #metaLabel {
            color: #3F4650;
            font-weight: 600;
            padding-left: 2px;
            background: transparent;
        }
        #panelTitle {
            font-size: 10.8pt;
            font-weight: 700;
            color: #FFFFFF;
            background: transparent;
        }
        #statChip {
            padding: 4px 10px;
            border-radius: 8px;
            background: #111827;
            border: 1px solid #B8C7D8;
            color: #F8FAFC;
        }
        QPushButton {
            background: #ECEEF1;
            border: 1px solid #B5BAC1;
            border-radius: 8px;
            padding: 5px 10px;
            color: #2E3945;
            font-weight: 600;
        }
        QPushButton:hover {
            border-color: #D7DBE1;
            background: #F4F5F7;
        }
        QPushButton:pressed {
            background: #DCDDDF;
        }
        QLineEdit {
            background: #57616B;
            border: 1px solid #FFFFFF;
            border-radius: 8px;
            padding: 7px 10px;
            color: #F8FAFC;
            selection-background-color: #6D8FB2;
        }
        QTreeWidget, QTreeView {
            background: #5C6670;
            border: 1px solid #FFFFFF;
            border-radius: 8px;
            outline: none;
            padding: 4px;
            alternate-background-color: #66717B;
            color: #F2F4F7;
        }
        QTreeWidget::item, QTreeView::item {
            background: transparent;
            padding: 4px 6px;
            border-radius: 0px;
            margin: 0px;
            border-bottom: 1px solid #8C98A4;
        }
        QTreeWidget::item:selected, QTreeView::item:selected {
            background: #688BB4;
            color: #FFFFFF;
        }
        QHeaderView::section {
            background: #6A7580;
            color: #FFFFFF;
            border: none;
            border-bottom: 1px solid #FFFFFF;
            border-right: 1px solid #7A8694;
            padding: 6px 8px;
            font-weight: 600;
        }
        QSplitter::handle {
            background: #C8D0D8;
            margin: 1px 0;
        }
        QScrollBar:vertical, QScrollBar:horizontal {
            background: #111111;
            border: none;
            margin: 0px;
        }
        QScrollBar:vertical { width: 12px; }
        QScrollBar:horizontal { height: 12px; }
        QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
            background: #FFFFFF;
            border-radius: 5px;
            min-height: 28px;
            min-width: 28px;
        }
        QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page {
            background: transparent;
            border: none;
        }
        QTreeWidget::branch, QTreeView::branch {
            background: transparent;
        }
    )");
}

QString MainWindow::signalDisplayName(int signalIndex) const {
    if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) return QString();

    if (m_signalTreeModel) {
        const QString full = m_signalTreeModel->fullPathForSignalIndex(signalIndex);
        if (!full.isEmpty()) return full;
    }

    return m_wave.signalList.at(signalIndex).name;
}

QString MainWindow::formatNameWithRange(int signalIndex) const {
    if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) return QString();

    const WaveSignal& sig = m_wave.signalList.at(signalIndex);
    const QString name = signalDisplayName(signalIndex);
    if (sig.width <= 1) return name;
    return QString("%1[%2:0]").arg(name).arg(qMax(0, sig.width - 1));
}

QString MainWindow::formatNameWithRange(const WaveSignal& sig) const {
    if (sig.width <= 1) return sig.name;
    return QString("%1[%2:0]").arg(sig.name).arg(qMax(0, sig.width - 1));
}

WaveFile MainWindow::materializeWaveSignalNames(const WaveFile& src) const {
    WaveFile dst = src;
    if (!m_signalTreeModel) return dst;

    const int n = qMin(dst.signalList.size(), m_wave.signalList.size());
    for (int i = 0; i < n; ++i) {
        const QString full = m_signalTreeModel->fullPathForSignalIndex(i);
        if (!full.isEmpty()) dst.signalList[i].name = full;
    }
    return dst;
}

void MainWindow::loadDemoWave() {
    WaveFile wave;
    wave.meta.title = "demo_soc";
    wave.meta.timescale = "1ns";
    wave.meta.start = 0;
    wave.meta.end = 120;

    auto bitS = [](qint64 t, const char* v) { WaveSample s; s.time = t; s.value = QString::fromLatin1(v); hydrateWaveSampleRawFields(SignalKind::Bit, 1, s); return s; };
    auto busS = [](qint64 t, const char* v, int width) { WaveSample s; s.time = t; s.value = QString::fromLatin1(v); hydrateWaveSampleRawFields(SignalKind::Bus, width, s); return s; };

    auto addSig = [&](const QString& name, SignalKind kind, int width, ValueRadix radix, const QVector<WaveSample>& samples) {
        WaveSignal s;
        s.name = name;
        s.kind = kind;
        s.width = width;
        s.defaultRadix = radix;
        s.currentRadix = radix;
        s.samples = samples;
        wave.signalList.push_back(s);
    };

    addSig("clk", SignalKind::Bit, 1, ValueRadix::Bin, {
        bitS(0,"0"),bitS(5,"1"),bitS(10,"0"),bitS(15,"1"),bitS(20,"0"),bitS(25,"1"),
        bitS(30,"0"),bitS(35,"1"),bitS(40,"0"),bitS(45,"1"),bitS(50,"0"),bitS(55,"1"),
        bitS(60,"0"),bitS(65,"1"),bitS(70,"0"),bitS(75,"1"),bitS(80,"0"),bitS(85,"1"),
        bitS(90,"0"),bitS(95,"1"),bitS(100,"0"),bitS(105,"1"),bitS(110,"0"),bitS(115,"1")
        });

    addSig("rst_n", SignalKind::Bit, 1, ValueRadix::Bin, { bitS(0,"0"), bitS(18,"1") });

    addSig("top.cpu.ifu.pc", SignalKind::Bus, 32, ValueRadix::Hex, {
        busS(0,"0x00000000",32),
        busS(20,"0x00000004",32),
        busS(40,"0x00000008",32),
        busS(60,"0x0000000C",32),
        busS(80,"0x00000010",32),
        busS(100,"0x00000014",32)
        });

    addSig("top.cpu.ctrl.state", SignalKind::Bus, 3, ValueRadix::Bin, {
        busS(0,"0b000",3),
        busS(20,"0b001",3),
        busS(35,"0b010",3),
        busS(60,"0b011",3),
        busS(85,"0b100",3)
        });

    addSig("top.cpu.exe.valid", SignalKind::Bit, 1, ValueRadix::Bin, {
        bitS(0,"0"),bitS(30,"1"),bitS(52,"0"),bitS(76,"1"),bitS(98,"0")
        });

    addSig("top.cpu.exe.ready", SignalKind::Bit, 1, ValueRadix::Bin, {
        bitS(0,"1"),bitS(46,"0"),bitS(68,"1")
        });

    addSig("top.uart.tx_busy", SignalKind::Bit, 1, ValueRadix::Bin, {
        bitS(0,"0"),bitS(72,"1"),bitS(104,"0")
        });

    addSig("top.uart.tx_data", SignalKind::Bus, 8, ValueRadix::Hex, {
        busS(0,"0x00",32),
        busS(72,"0x55",32),
        busS(88,"0xA3",32),
        busS(104,"0x1C",32)
        });

    applyWave(std::move(wave));
}

void MainWindow::applyWave(WaveFile&& wave) {
    const bool perf = viewerPerfLogEnabled();
    QElapsedTimer totalTimer;
    QElapsedTimer stepTimer;
    if (perf) {
        totalTimer.start();
        stepTimer.start();
    }

    m_wave = std::move(wave);
    m_valueFindHits.clear();
    m_valueFindSignalIndexes.clear();
    m_valueFindCurrentHit = -1;
    m_valueFindSummaryBase = QStringLiteral("No value search has been run for this wave.");
    if (m_valueFindResults) m_valueFindResults->clear();
    updateValueFindNavigationState();
    if (perf) {
        viewerPerfLog("apply.move_assign", stepTimer.restart(),
                      m_wave.signalList.size(), m_wave.tree.nodesById.size());
    }

    m_signalIndexBySignalId.clear();
    for (int i = 0; i < m_wave.signalList.size(); ++i) {
        if (m_wave.signalList[i].signalId < 0) m_wave.signalList[i].signalId = i;
        const int sid = m_wave.signalList.at(i).signalId;
        if (sid >= 0 && sid <= 100000000) {
            if (m_signalIndexBySignalId.size() <= sid) m_signalIndexBySignalId.resize(sid + 1);
            m_signalIndexBySignalId[sid] = i + 1; // store +1 so zero means missing
        }
        if ((m_wave.signalList.at(i).samplesLoaded || !m_wave.signalList.at(i).samples.isEmpty()) &&
            !m_wave.signalList.at(i).changeTimesReady) {
            rebuildWaveSignalDerivedCaches(m_wave.signalList[i]);
        }
    }
    if (perf) {
        viewerPerfLog("apply.signal_index", stepTimer.restart(),
                      m_wave.signalList.size(), m_wave.tree.nodesById.size());
    }

    updateMetaLabel();
    if (perf) {
        viewerPerfLog("apply.meta", stepTimer.restart(),
                      m_wave.signalList.size(), m_wave.tree.nodesById.size());
    }

    rebuildTree();
    if (perf) {
        viewerPerfLog("apply.rebuild_tree", stepTimer.restart(),
                      m_wave.signalList.size(), m_wave.tree.nodesById.size());
    }

    m_activeList->clear();
    m_canvas->setWave(&m_wave);
    if (perf) {
        viewerPerfLog("apply.canvas_bind", stepTimer.restart(),
                      m_wave.signalList.size(), m_wave.tree.nodesById.size(), m_activeList->topLevelItemCount());
    }

    for (int i = 0; i < qMin(6, m_wave.signalList.size()); ++i) addSignalToActive(i);
    if (perf) {
        viewerPerfLog("apply.first_active", stepTimer.restart(),
                      m_wave.signalList.size(), m_wave.tree.nodesById.size(), m_activeList->topLevelItemCount());
    }

    rebuildVisibleSignals();
    refreshActiveValueLabels();
    if (perf) {
        viewerPerfLog("apply.final_refresh", stepTimer.restart(),
                      m_wave.signalList.size(), m_wave.tree.nodesById.size(), m_activeList->topLevelItemCount());
        viewerPerfLog("apply.total", totalTimer.elapsed(),
                      m_wave.signalList.size(), m_wave.tree.nodesById.size(), m_activeList->topLevelItemCount());
    }
}

void MainWindow::updateMetaLabel() {
    auto displayTimeText = [](qint64 internalTime) -> QString {
        return QString::number(internalTime);
    };

    const QString rangeText = QStringLiteral("%1 ~ %2")
        .arg(displayTimeText(m_wave.meta.start), displayTimeText(m_wave.meta.end));

    if (m_metaLabel) {
        m_metaLabel->setText(rangeText);
        m_metaLabel->setToolTip(QString::fromUtf8("时间范围：%1").arg(rangeText));
    }
    if (m_jumpTimeEdit) {
        m_jumpTimeEdit->setPlaceholderText(QString::fromUtf8("跳转时间"));
        m_jumpTimeEdit->setToolTip(QString::fromUtf8("输入显示时间后按 Enter 跳转。合理范围：%1").arg(rangeText));
    }
}



bool MainWindow::openWaveFilePath(const QString& path, bool showError) {
    if (path.isEmpty()) return false;

    const bool perf = viewerPerfLogEnabled();
    QElapsedTimer totalTimer;
    QElapsedTimer stepTimer;
    if (perf) {
        totalTimer.start();
        stepTimer.start();
    }

    WaveFile wave;
    QString error;
    bool ok = false;
    const bool isWvz3 = path.endsWith(".wvz3", Qt::CaseInsensitive);
    const bool isWvz4 = path.endsWith(".wvz4", Qt::CaseInsensitive);
    if (isWvz4) {
        WaveParser4::LoadOptions loadOptions;
        const bool rawOnly = viewerDisableLodEnabled();
        loadOptions.includeAllSignalDefinitions = true;
        loadOptions.autoLoadFirstSignalCount = rawOnly ? 6 : 0;
        loadOptions.autoLoadFirstSignalLodCount = rawOnly ? 0 : 6;
        loadOptions.loadAllIfWindowEmpty = false;
        ok = WaveParser4::loadFromFile(path, wave, error, loadOptions);
    }
    else if (isWvz3) {
        WaveParser3::LoadOptions loadOptions;
        loadOptions.includeAllSignalDefinitions = true;
        loadOptions.autoLoadFirstSignalCount = 6;
        loadOptions.loadAllIfWindowEmpty = false;
        ok = WaveParser3::loadFromFile(path, wave, error, loadOptions);
    }
    else if (path.endsWith(".wvz2", Qt::CaseInsensitive)) {
        ok = WaveParser2::loadFromFile(path, wave, error);
    }
    else {
        ok = WaveParser::loadFromFile(path, wave, error);
    }
    if (perf) {
        viewerPerfLog("open.load", stepTimer.restart(),
                      wave.signalList.size(), wave.tree.nodesById.size());
    }
    if (!ok) {
        if (!showError) return false;
        QMessageBox::critical(this, QString::fromUtf8("打开失败"), error);
        return false;
    }

    m_currentWaveFilePath = (isWvz3 || isWvz4) ? path : QString();
    m_currentWaveSupportsOnDemand = (isWvz3 || isWvz4);
    applyWave(std::move(wave));
    if (perf) {
        viewerPerfLog("open.apply", stepTimer.restart(),
                      m_wave.signalList.size(), m_wave.tree.nodesById.size());
        viewerPerfLog("open.total", totalTimer.elapsed(),
                      m_wave.signalList.size(), m_wave.tree.nodesById.size());
    }
    return true;
}

void MainWindow::openWaveFile() {
    const QString path = QFileDialog::getOpenFileName(
        this,
        QString::fromUtf8("打开波形文件"),
        QString(),
        "Wave Files (*.wvjson *.json *.wvz *.wvz2 *.wvz3 *.wvz4)");
    if (path.isEmpty()) return;

    openWaveFilePath(path);
}

void MainWindow::compareWaveFiles() {
    const QStringList paths = QFileDialog::getOpenFileNames(
        this,
        QString::fromUtf8("选择两个波形文件进行比较"),
        QString(),
        QStringLiteral("Wave Files (*.wvjson *.json *.wvz *.wvz2 *.wvz3 *.wvz4)"));
    if (paths.isEmpty()) return;
    if (paths.size() != 2) {
        QMessageBox::warning(this,
            QString::fromUtf8("比较失败"),
            QString::fromUtf8("请一次选择且只选择两个波形文件。"));
        return;
    }

    WaveFile leftWave;
    WaveFile rightWave;
    QString error;
    if (!loadWaveFileFullyForCompare(paths.at(0), leftWave, error)) {
        QMessageBox::critical(this,
            QString::fromUtf8("比较失败"),
            QString::fromUtf8("第一个文件解析失败：\n%1").arg(error));
        return;
    }
    if (!loadWaveFileFullyForCompare(paths.at(1), rightWave, error)) {
        QMessageBox::critical(this,
            QString::fromUtf8("比较失败"),
            QString::fromUtf8("第二个文件解析失败：\n%1").arg(error));
        return;
    }

    rebuildWaveFileDerivedCaches(leftWave);
    rebuildWaveFileDerivedCaches(rightWave);

    WaveFile comparedWave;
    if (!buildComparedWaveFile(paths.at(0), leftWave, paths.at(1), rightWave, comparedWave, error)) {
        QMessageBox::information(this,
            QString::fromUtf8("比较完成"),
            error.isEmpty() ? QString::fromUtf8("没有发现路径相同且数据不同的信号。") : error);
        return;
    }

    m_currentWaveFilePath.clear();
    m_currentWaveSupportsOnDemand = false;
    m_signalIndexBySignalId.clear();
    applyWave(std::move(comparedWave));
}

void MainWindow::exportCompressedWaveFile() {
    QString selectedFilter = QStringLiteral("WVZ3 Wave (*.wvz3)");
    QString path = QFileDialog::getSaveFileName(
        this,
        QString::fromUtf8("导出波形文件"),
        m_wave.meta.title + ".wvz3",
        QStringLiteral("WVZ3 Wave (*.wvz3);;WVZ2 Wave (*.wvz2);;Compressed Wave (*.wvz)"),
        &selectedFilter);
    if (path.isEmpty()) return;

    if (m_currentWaveSupportsOnDemand) {
        if (m_currentWaveFilePath.endsWith(".wvz4", Qt::CaseInsensitive)) {
            const qint64 fileSize = QFileInfo(m_currentWaveFilePath).size();
            if (fileSize > kLargeWvz4FullLoadLimitBytes) {
                QMessageBox::warning(this,
                    QStringLiteral("Export disabled"),
                    formatLargeWvz4FullLoadError(fileSize, QStringLiteral("export")));
                return;
            }
        }

        QList<int> allSignalIndexes;
        allSignalIndexes.reserve(m_wave.signalList.size());
        for (int i = 0; i < m_wave.signalList.size(); ++i) {
            allSignalIndexes.push_back(i);
        }
        if (!ensureSignalSamplesLoaded(allSignalIndexes, false)) {
            return;
        }
    }

    const WaveFile namedWave = materializeWaveSignalNames(m_wave);
    const WaveFile exportWave = buildExportWaveNoZ(namedWave);

    auto ensureSuffix = [](QString value, const QString& suffix) {
        if (!value.endsWith(suffix, Qt::CaseInsensitive)) value += suffix;
        return value;
    };

    if (QFileInfo(path).suffix().isEmpty()) {
        if (selectedFilter.contains("*.wvz3")) path = ensureSuffix(path, ".wvz3");
        else if (selectedFilter.contains("*.wvz2")) path = ensureSuffix(path, ".wvz2");
        else path = ensureSuffix(path, ".wvz");
    }

    QString error;
    bool ok = false;
    if (path.endsWith(".wvz3", Qt::CaseInsensitive)) {
        WaveParser3::SaveOptions options;
        options.timescalePs = 1000;
        options.targetBlockSpan = 100000;
        options.compression = wvz3::Comp_Zstd;
        options.enableChecksum = false;
        options.enableClockDeltaOptimization = true;
        options.clockHalfPeriodTolerance = 0;
        options.minClockToggleCount = 6;
        options.forceDurableCommit = true;
        options.enableSharedTimeTable = true;
        ok = WaveParser3::saveToFile(path, exportWave, error, options);
    }
    else if (path.endsWith(".wvz2", Qt::CaseInsensitive)) {
        WaveParser2::SaveOptions options;
        options.timescalePs = 1000;
        options.targetBlockSpan = 256;
        options.compression = wvz2::Comp_Zlib;
        options.enableClockDeltaOptimization = true;
        options.clockHalfPeriodTolerance = 0;
        options.minClockToggleCount = 6;
        ok = WaveParser2::saveToFile(path, exportWave, error, options);
    }
    else {
        ok = WaveParser::saveToCompressedFile(path, exportWave, error);
    }

    if (!ok) {
        QMessageBox::critical(this, QString::fromUtf8("导出失败"), error);
        return;
    }
    QMessageBox::information(this, QString::fromUtf8("导出成功"), QString::fromUtf8("已导出波形文件： %1").arg(path));
}


void MainWindow::zoomIn() { m_canvas->zoomByFactor(0.70); }
void MainWindow::zoomOut() { m_canvas->zoomByFactor(1.35); }
void MainWindow::panLeft() { const qint64 s = m_canvas->viewEnd() - m_canvas->viewStart(); m_canvas->panBy(static_cast<qint64>(std::llround(-double(s) * 0.18))); }
void MainWindow::panRight() { const qint64 s = m_canvas->viewEnd() - m_canvas->viewStart(); m_canvas->panBy(static_cast<qint64>(std::llround(double(s) * 0.18))); }

QList<int> MainWindow::selectedActiveSignalIndexesForJump() const {
    QList<int> signalIndexes;
    QSet<int> seen;
    if (!m_activeList) return signalIndexes;

    QList<QTreeWidgetItem*> picked = m_activeList->selectedItems();
    if (picked.isEmpty()) {
        if (QTreeWidgetItem* item = m_activeList->currentItem()) picked.push_back(item);
    }

    std::sort(picked.begin(), picked.end(), [this](QTreeWidgetItem* a, QTreeWidgetItem* b) {
        return m_activeList->indexOfTopLevelItem(a) < m_activeList->indexOfTopLevelItem(b);
    });

    for (QTreeWidgetItem* item : picked) {
        const int signalIndex = signalIndexFromActiveItem(item);
        if (signalIndex >= 0 && !seen.contains(signalIndex)) {
            seen.insert(signalIndex);
            signalIndexes.push_back(signalIndex);
        }
    }
    return signalIndexes;
}

QList<int> MainWindow::selectedActiveSignalIndexesForFind() const {
    return selectedActiveSignalIndexesForJump();
}

void MainWindow::openValueFindDialog() {
    if (!m_valueFindDialog) {
        m_valueFindDialog = new QDialog(this);
        m_valueFindDialog->setWindowTitle(QStringLiteral("Find value"));
        m_valueFindDialog->resize(560, 430);

        auto* root = new QVBoxLayout(m_valueFindDialog);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        auto* inputRow = new QHBoxLayout();
        auto* inputLabel = new QLabel(QStringLiteral("Target"), m_valueFindDialog);
        m_valueFindEdit = new QLineEdit(m_valueFindDialog);
        m_valueFindEdit->setPlaceholderText(QStringLiteral("10, 0xA, 0b1010, -1"));
        auto* findButton = new QPushButton(QStringLiteral("Find"), m_valueFindDialog);
        inputRow->addWidget(inputLabel);
        inputRow->addWidget(m_valueFindEdit, 1);
        inputRow->addWidget(findButton);
        root->addLayout(inputRow);

        m_valueFindSummaryLabel = new QLabel(QStringLiteral("Select highlighted active signals and enter a value."), m_valueFindDialog);
        m_valueFindSummaryLabel->setWordWrap(true);
        root->addWidget(m_valueFindSummaryLabel);

        m_valueFindResults = new QTreeWidget(m_valueFindDialog);
        m_valueFindResults->setColumnCount(3);
        m_valueFindResults->setHeaderLabels(QStringList() << QStringLiteral("Signal")
                                                          << QStringLiteral("Count")
                                                          << QStringLiteral("First time"));
        m_valueFindResults->setRootIsDecorated(false);
        m_valueFindResults->setSelectionMode(QAbstractItemView::SingleSelection);
        m_valueFindResults->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_valueFindResults->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_valueFindResults->header()->setStretchLastSection(false);
        m_valueFindResults->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_valueFindResults->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_valueFindResults->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        root->addWidget(m_valueFindResults, 1);

        auto* buttonRow = new QHBoxLayout();
        m_valueFindPrevButton = new QPushButton(QStringLiteral("Previous"), m_valueFindDialog);
        m_valueFindNextButton = new QPushButton(QStringLiteral("Next"), m_valueFindDialog);
        auto* closeButton = new QPushButton(QStringLiteral("Close"), m_valueFindDialog);
        buttonRow->addWidget(m_valueFindPrevButton);
        buttonRow->addWidget(m_valueFindNextButton);
        buttonRow->addStretch(1);
        buttonRow->addWidget(closeButton);
        root->addLayout(buttonRow);

        connect(findButton, &QPushButton::clicked, this, &MainWindow::runValueFind);
        connect(m_valueFindEdit, &QLineEdit::returnPressed, this, &MainWindow::runValueFind);
        connect(m_valueFindPrevButton, &QPushButton::clicked, this, &MainWindow::jumpToPreviousValueFindHit);
        connect(m_valueFindNextButton, &QPushButton::clicked, this, &MainWindow::jumpToNextValueFindHit);
        connect(closeButton, &QPushButton::clicked, m_valueFindDialog, &QDialog::hide);
        connect(m_valueFindResults, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
            if (!item) return;
            const int hitIndex = item->data(0, kValueFindRoleFirstHit).toInt();
            if (hitIndex >= 0) jumpToValueFindHit(hitIndex);
        });
        connect(m_valueFindResults, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
            if (!current) return;
            const int hitIndex = current->data(0, kValueFindRoleFirstHit).toInt();
            if (hitIndex >= 0) {
                m_valueFindCurrentHit = hitIndex;
                updateValueFindNavigationState();
            }
        });
    }

    if (m_valueFindHits.isEmpty()) {
        const int selectedCount = selectedActiveSignalIndexesForFind().size();
        m_valueFindSummaryBase = selectedCount > 0
            ? QStringLiteral("%1 highlighted active signal(s) selected.").arg(selectedCount)
            : QStringLiteral("No highlighted active signal is selected.");
        updateValueFindNavigationState();
    }

    m_valueFindDialog->show();
    m_valueFindDialog->raise();
    m_valueFindDialog->activateWindow();
    if (m_valueFindEdit) {
        m_valueFindEdit->setFocus();
        m_valueFindEdit->selectAll();
    }
}

void MainWindow::runValueFind() {
    if (!m_valueFindEdit) return;

    ParsedValueFindTarget target;
    const QString targetText = m_valueFindEdit->text().trimmed();
    if (!parseValueFindTargetText(targetText, target)) {
        QMessageBox::warning(this,
            QStringLiteral("Find value"),
            QStringLiteral("Enter a numeric target, for example 10, 0xA, 0b1010, or -1."));
        m_valueFindEdit->setFocus();
        m_valueFindEdit->selectAll();
        return;
    }

    const QList<int> signalIndexes = selectedActiveSignalIndexesForFind();
    if (signalIndexes.isEmpty()) {
        QMessageBox::information(this,
            QStringLiteral("Find value"),
            QStringLiteral("Highlight one or more active signals before searching."));
        return;
    }

    if (!ensureSignalSamplesLoaded(signalIndexes, false)) return;

    QElapsedTimer timer;
    timer.start();

    m_valueFindHits.clear();
    m_valueFindSignalIndexes = signalIndexes;
    m_valueFindCurrentHit = -1;

    int widthSkippedSignals = 0;
    for (int signalIndex : signalIndexes) {
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;

        const WaveSignal& sig = m_wave.signalList.at(signalIndex);
        quint64 targetBits = 0;
        if (!valueFindTargetForSignal(target, sig.width, targetBits)) {
            ++widthSkippedSignals;
            continue;
        }

        const quint64 mask = waveBitMaskForWidth(sig.width);
        bool previousMatched = false;
        for (int sampleIndex = 0; sampleIndex < sig.samples.size(); ++sampleIndex) {
            const WaveSample& sample = sig.samples.at(sampleIndex);
            bool matched = false;

            if (!sample.isAbsent && !sample.isZ) {
                if (sample.rawFieldsReady) {
                    matched = ((sample.rawBits & mask) == targetBits);
                } else {
                    WaveSample hydrated = sample;
                    hydrateWaveSampleRawFields(sig.kind, sig.width, hydrated);
                    matched = !hydrated.isAbsent && !hydrated.isZ && ((hydrated.rawBits & mask) == targetBits);
                }
            }

            if (matched && !previousMatched) {
                ValueFindHit hit;
                hit.signalIndex = signalIndex;
                hit.sampleIndex = sampleIndex;
                hit.time = sample.time;
                m_valueFindHits.push_back(hit);
            }
            previousMatched = matched;
        }
    }

    std::stable_sort(m_valueFindHits.begin(), m_valueFindHits.end(), [](const ValueFindHit& a, const ValueFindHit& b) {
        if (a.time != b.time) return a.time < b.time;
        if (a.signalIndex != b.signalIndex) return a.signalIndex < b.signalIndex;
        return a.sampleIndex < b.sampleIndex;
    });

    QSet<int> matchedSignals;
    for (const ValueFindHit& hit : m_valueFindHits) matchedSignals.insert(hit.signalIndex);

    m_valueFindSummaryBase = QStringLiteral("%1 target segment(s) in %2/%3 highlighted signal(s), scanned in %4 ms.")
        .arg(m_valueFindHits.size())
        .arg(matchedSignals.size())
        .arg(signalIndexes.size())
        .arg(timer.elapsed());
    if (widthSkippedSignals > 0) {
        m_valueFindSummaryBase += QStringLiteral(" %1 signal(s) skipped because the positive target exceeds their width.")
            .arg(widthSkippedSignals);
    }

    rebuildValueFindResults();
    if (!m_valueFindHits.isEmpty()) {
        jumpToValueFindHit(0);
    } else {
        updateValueFindNavigationState();
    }
}

void MainWindow::rebuildValueFindResults() {
    if (!m_valueFindResults) return;

    QHash<int, int> countBySignal;
    QHash<int, int> firstHitBySignal;
    for (int i = 0; i < m_valueFindHits.size(); ++i) {
        const ValueFindHit& hit = m_valueFindHits.at(i);
        countBySignal[hit.signalIndex] = countBySignal.value(hit.signalIndex) + 1;
        if (!firstHitBySignal.contains(hit.signalIndex)) firstHitBySignal.insert(hit.signalIndex, i);
    }

    m_valueFindResults->clear();
    for (int signalIndex : m_valueFindSignalIndexes) {
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;

        const int count = countBySignal.value(signalIndex, 0);
        const int firstHit = firstHitBySignal.value(signalIndex, -1);
        auto* item = new QTreeWidgetItem(m_valueFindResults);
        item->setText(0, signalDisplayName(signalIndex));
        item->setText(1, QString::number(count));
        item->setText(2, firstHit >= 0 ? formatInternalDisplayTime(m_valueFindHits.at(firstHit).time) : QStringLiteral("-"));
        item->setData(0, kValueFindRoleFirstHit, firstHit);
        item->setData(0, kValueFindRoleSignalIndex, signalIndex);
        if (count <= 0) {
            item->setForeground(1, QBrush(QColor("#AAB3BC")));
            item->setForeground(2, QBrush(QColor("#AAB3BC")));
        }
    }
}

void MainWindow::updateValueFindNavigationState() {
    const bool hasHits = !m_valueFindHits.isEmpty();
    if (m_valueFindPrevButton) m_valueFindPrevButton->setEnabled(hasHits);
    if (m_valueFindNextButton) m_valueFindNextButton->setEnabled(hasHits);

    if (!m_valueFindSummaryLabel) return;

    QString summary = m_valueFindSummaryBase;
    if (m_valueFindCurrentHit >= 0 && m_valueFindCurrentHit < m_valueFindHits.size()) {
        const ValueFindHit& hit = m_valueFindHits.at(m_valueFindCurrentHit);
        summary += QStringLiteral(" Current: %1 @ %2 (%3/%4).")
            .arg(signalDisplayName(hit.signalIndex))
            .arg(formatInternalDisplayTime(hit.time))
            .arg(m_valueFindCurrentHit + 1)
            .arg(m_valueFindHits.size());
    }
    m_valueFindSummaryLabel->setText(summary);
}

void MainWindow::jumpToValueFindHit(int hitIndex) {
    if (hitIndex < 0 || hitIndex >= m_valueFindHits.size() || !m_canvas) return;

    const ValueFindHit hit = m_valueFindHits.at(hitIndex);
    if (!m_canvas->jumpToTime(hit.time)) return;

    m_valueFindCurrentHit = hitIndex;

    if (m_activeList) {
        for (int row = 0; row < m_activeList->topLevelItemCount(); ++row) {
            QTreeWidgetItem* item = m_activeList->topLevelItem(row);
            if (!item || signalIndexFromActiveItem(item) != hit.signalIndex) continue;
            m_activeList->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
            m_activeList->scrollToItem(item);
            break;
        }
        m_canvas->setSelectedEntryIndexes(selectedTopLevelIndexes(m_activeList));
    }

    if (m_valueFindResults) {
        QSignalBlocker blocker(m_valueFindResults);
        for (int row = 0; row < m_valueFindResults->topLevelItemCount(); ++row) {
            QTreeWidgetItem* item = m_valueFindResults->topLevelItem(row);
            if (!item || item->data(0, kValueFindRoleSignalIndex).toInt() != hit.signalIndex) continue;
            m_valueFindResults->setCurrentItem(item);
            m_valueFindResults->scrollToItem(item);
            break;
        }
    }

    refreshActiveValueLabels();
    updateValueFindNavigationState();
}

void MainWindow::jumpToAdjacentValueFindHit(bool forward) {
    if (m_valueFindHits.isEmpty()) return;

    int targetIndex = -1;
    if (m_valueFindCurrentHit >= 0 && m_valueFindCurrentHit < m_valueFindHits.size()) {
        targetIndex = m_valueFindCurrentHit + (forward ? 1 : -1);
        if (targetIndex < 0) targetIndex = m_valueFindHits.size() - 1;
        if (targetIndex >= m_valueFindHits.size()) targetIndex = 0;
    } else {
        const qint64 cursor = m_canvas ? m_canvas->cursorTime() : -1;
        if (cursor >= 0) {
            if (forward) {
                for (int i = 0; i < m_valueFindHits.size(); ++i) {
                    if (m_valueFindHits.at(i).time > cursor) {
                        targetIndex = i;
                        break;
                    }
                }
            } else {
                for (int i = m_valueFindHits.size() - 1; i >= 0; --i) {
                    if (m_valueFindHits.at(i).time < cursor) {
                        targetIndex = i;
                        break;
                    }
                }
            }
        }
        if (targetIndex < 0) targetIndex = forward ? 0 : (m_valueFindHits.size() - 1);
    }

    jumpToValueFindHit(targetIndex);
}

void MainWindow::jumpToPreviousValueFindHit() {
    jumpToAdjacentValueFindHit(false);
}

void MainWindow::jumpToNextValueFindHit() {
    jumpToAdjacentValueFindHit(true);
}

void MainWindow::jumpToPrevChange() {
    const QList<int> signalIndexes = selectedActiveSignalIndexesForJump();
    if (signalIndexes.isEmpty()) return;
    const bool diffOnly = (QApplication::keyboardModifiers() & Qt::ControlModifier) != 0;
    if (m_canvas->jumpToNearestChangeForSignals(signalIndexes, false, diffOnly)) {
        refreshActiveValueLabels();
    }
}

void MainWindow::jumpToNextChange() {
    const QList<int> signalIndexes = selectedActiveSignalIndexesForJump();
    if (signalIndexes.isEmpty()) return;
    const bool diffOnly = (QApplication::keyboardModifiers() & Qt::ControlModifier) != 0;
    if (m_canvas->jumpToNearestChangeForSignals(signalIndexes, true, diffOnly)) {
        refreshActiveValueLabels();
    }
}

void MainWindow::jumpToTime() {
    if (!m_canvas) return;

    const qint64 rangeStart = m_canvas->fullStartTime();
    const qint64 rangeEnd = m_canvas->fullEndTime();

    auto displayTimeText = [](qint64 internalTime) -> QString {
        return QString::number(internalTime);
    };

    const QString minText = displayTimeText(rangeStart);
    const QString maxText = displayTimeText(rangeEnd);
    const QString rangeText = QStringLiteral("%1 ~ %2").arg(minText, maxText);

    if (rangeEnd < rangeStart) {
        QMessageBox::warning(this,
            QString::fromUtf8("无法跳转"),
            QString::fromUtf8("当前波形时间范围无效。"));
        return;
    }

    QString input = m_jumpTimeEdit ? m_jumpTimeEdit->text().trimmed() : QString();
    if (input.isEmpty()) {
        QMessageBox::warning(this,
            QString::fromUtf8("时间为空"),
            QString::fromUtf8("请输入时间轴显示值。\\n合理范围：%1").arg(rangeText));
        if (m_jumpTimeEdit) m_jumpTimeEdit->setFocus();
        return;
    }

    bool parsed = false;
    const qint64 internalTime = input.toLongLong(&parsed, 10);
    if (!parsed) {
        QMessageBox::warning(this,
            QString::fromUtf8("时间格式不正确"),
            QString::fromUtf8("请输入数字时间。\\n合理范围：%1").arg(rangeText));
        if (m_jumpTimeEdit) m_jumpTimeEdit->selectAll();
        return;
    }

    if (internalTime < rangeStart || internalTime > rangeEnd) {
        QMessageBox::warning(this,
            QString::fromUtf8("时间超出范围"),
            QString::fromUtf8("输入时间不在合理范围内。\\n合理范围：%1").arg(rangeText));
        if (m_jumpTimeEdit) m_jumpTimeEdit->selectAll();
        return;
    }

    if (internalTime < rangeStart || internalTime > rangeEnd) {
        QMessageBox::warning(this,
            QString::fromUtf8("时间超出范围"),
            QString::fromUtf8("输入时间换算为内部整数时间后超出范围。\\n合理范围：%1").arg(rangeText));
        if (m_jumpTimeEdit) m_jumpTimeEdit->selectAll();
        return;
    }

    if (!m_canvas->jumpToTime(internalTime)) {
        QMessageBox::warning(this,
            QString::fromUtf8("无法跳转"),
            QString::fromUtf8("无法跳转到该时间。\\n合理范围：%1").arg(rangeText));
        if (m_jumpTimeEdit) m_jumpTimeEdit->selectAll();
        return;
    }

    if (m_jumpTimeEdit) {
        m_jumpTimeEdit->setText(displayTimeText(internalTime));
        m_jumpTimeEdit->selectAll();
    }
    refreshActiveValueLabels();
}

void MainWindow::openDerivedSignalDialog() {
    if (m_wave.signalList.isEmpty()) {
        QMessageBox::information(this,
            QStringLiteral("Create temporary signal"),
            QStringLiteral("Open a waveform before creating a temporary signal."));
        return;
    }

    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModal(false);
    dialog->setWindowTitle(QStringLiteral("Create temporary signal"));
    dialog->resize(680, 230);

    auto* root = new QVBoxLayout(dialog);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto* nameEdit = new QLineEdit(dialog);
    nameEdit->setText(QStringLiteral("tmp_expr"));
    nameEdit->setPlaceholderText(QStringLiteral("tmp_expr"));
    form->addRow(QStringLiteral("Name"), nameEdit);

    auto* exprEdit = new QLineEdit(dialog);
    exprEdit->setPlaceholderText(QStringLiteral("`top.a` & `top.b`, (`valid` && `ready`), `bus` + 4"));

    auto* exprRow = new QHBoxLayout();
    exprRow->setSpacing(6);
    auto* insertSelectedButton = new QPushButton(QStringLiteral("Insert selected"), dialog);
    exprRow->addWidget(exprEdit, 1);
    exprRow->addWidget(insertSelectedButton);
    form->addRow(QStringLiteral("Expression"), exprRow);

    auto* widthSpin = new QSpinBox(dialog);
    widthSpin->setRange(0, 64);
    widthSpin->setValue(0);
    widthSpin->setSpecialValueText(QStringLiteral("auto"));
    form->addRow(QStringLiteral("Width"), widthSpin);
    root->addLayout(form);

    auto* hint = new QLabel(QStringLiteral("This window is non-modal: select/copy signal names from the main viewer while it stays open. Supported operators: + - * / % & | ^ ~ << >> && || ! == != < <= > >=."), dialog);
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto* buttonRow = new QHBoxLayout();
    auto* createButton = new QPushButton(QStringLiteral("Create"), dialog);
    auto* closeButton = new QPushButton(QStringLiteral("Close"), dialog);
    buttonRow->addStretch(1);
    buttonRow->addWidget(createButton);
    buttonRow->addWidget(closeButton);
    root->addLayout(buttonRow);

    auto selectedSignalIndex = [this]() -> int {
        if (m_activeList) {
            if (QTreeWidgetItem* item = m_activeList->currentItem()) {
                const int signalIndex = signalIndexFromActiveItem(item);
                if (signalIndex >= 0 && signalIndex < m_wave.signalList.size()) return signalIndex;
            }
            const QList<QTreeWidgetItem*> picked = m_activeList->selectedItems();
            for (QTreeWidgetItem* item : picked) {
                const int signalIndex = signalIndexFromActiveItem(item);
                if (signalIndex >= 0 && signalIndex < m_wave.signalList.size()) return signalIndex;
            }
        }

        if (m_tree && m_tree->selectionModel()) {
            const QModelIndex current = m_tree->currentIndex();
            const QVariant currentSignal = current.data(kTreeRoleSignalIndex);
            if (currentSignal.isValid()) {
                const int signalIndex = currentSignal.toInt();
                if (signalIndex >= 0 && signalIndex < m_wave.signalList.size()) return signalIndex;
            }

            const QModelIndexList picked = m_tree->selectionModel()->selectedRows(0);
            for (const QModelIndex& index : picked) {
                const QVariant signal = index.data(kTreeRoleSignalIndex);
                if (!signal.isValid()) continue;
                const int signalIndex = signal.toInt();
                if (signalIndex >= 0 && signalIndex < m_wave.signalList.size()) return signalIndex;
            }
        }

        return -1;
    };

    auto quoteSignalName = [](QString name) {
        name.replace(QLatin1Char('`'), QLatin1Char('_'));
        return QStringLiteral("`") + name + QStringLiteral("`");
    };

    connect(insertSelectedButton, &QPushButton::clicked, this, [this, exprEdit, selectedSignalIndex, quoteSignalName]() {
        const int signalIndex = selectedSignalIndex();
        if (signalIndex < 0) {
            QMessageBox::information(this,
                QStringLiteral("Create temporary signal"),
                QStringLiteral("Select a signal in the active list or signal tree first."));
            return;
        }

        const QString text = quoteSignalName(signalDisplayName(signalIndex));
        if (!exprEdit->text().isEmpty()) exprEdit->insert(QStringLiteral(" "));
        exprEdit->insert(text);
        exprEdit->setFocus();
    });

    connect(createButton, &QPushButton::clicked, this, [this, dialog, nameEdit, exprEdit, widthSpin]() {
        if (createDerivedSignal(nameEdit->text(), exprEdit->text(), widthSpin->value())) {
            dialog->close();
        }
    });
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::close);

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    exprEdit->setFocus();
}

bool MainWindow::createDerivedSignal(const QString& name, const QString& expression, int widthOverride) {
    const QString signalName = name.trimmed();
    const QString expr = expression.trimmed();
    if (signalName.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Create temporary signal"), QStringLiteral("Enter a signal name."));
        return false;
    }
    if (expr.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Create temporary signal"), QStringLiteral("Enter an expression."));
        return false;
    }

    QHash<QString, int> exactByName;
    QHash<QString, int> leafByName;
    QSet<QString> duplicateExact;
    QSet<QString> duplicateLeaf;

    auto addExact = [&](const QString& rawName, int signalIndex) {
        const QString key = stripDisplayRangeSuffix(rawName);
        if (key.isEmpty()) return;
        if (exactByName.contains(key) && exactByName.value(key) != signalIndex) {
            duplicateExact.insert(key);
        } else {
            exactByName.insert(key, signalIndex);
        }
    };

    auto addLeaf = [&](const QString& rawName, int signalIndex) {
        const QString key = stripDisplayRangeSuffix(rawName);
        if (key.isEmpty()) return;
        const int dot = key.lastIndexOf(QLatin1Char('.'));
        const QString leaf = (dot >= 0) ? key.mid(dot + 1) : key;
        if (leaf.isEmpty()) return;
        if (leafByName.contains(leaf) && leafByName.value(leaf) != signalIndex) {
            duplicateLeaf.insert(leaf);
        } else {
            leafByName.insert(leaf, signalIndex);
        }
    };

    for (int i = 0; i < m_wave.signalList.size(); ++i) {
        const QString fullName = signalDisplayName(i);
        const QString rawName = m_wave.signalList.at(i).name;
        addExact(fullName, i);
        addExact(rawName, i);
        addLeaf(fullName, i);
        addLeaf(rawName, i);
    }

    if (exactByName.contains(stripDisplayRangeSuffix(signalName))) {
        QMessageBox::warning(this,
            QStringLiteral("Create temporary signal"),
            QStringLiteral("Signal name already exists. Choose another name."));
        return false;
    }

    auto resolveSignal = [&](const QString& rawName, int& signalIndex, int& width, QString& error) -> bool {
        const QString key = stripDisplayRangeSuffix(rawName);
        if (key.isEmpty()) {
            error = QStringLiteral("Empty signal name in expression.");
            return false;
        }
        if (duplicateExact.contains(key)) {
            error = QStringLiteral("Signal name '%1' is ambiguous; use the full path in backticks.").arg(rawName);
            return false;
        }
        if (exactByName.contains(key)) {
            signalIndex = exactByName.value(key);
            if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) return false;
            width = qBound(1, m_wave.signalList.at(signalIndex).width, 64);
            return true;
        }
        if (duplicateLeaf.contains(key)) {
            error = QStringLiteral("Leaf signal name '%1' is ambiguous; use the full path in backticks.").arg(rawName);
            return false;
        }
        if (leafByName.contains(key)) {
            signalIndex = leafByName.value(key);
            if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) return false;
            width = qBound(1, m_wave.signalList.at(signalIndex).width, 64);
            return true;
        }

        error = QStringLiteral("Unknown signal '%1'.").arg(rawName);
        return false;
    };

    DerivedExpressionProgram program;
    QString parseError;
    DerivedExpressionParser parser(expr, resolveSignal);
    if (!parser.parse(program, parseError)) {
        QMessageBox::warning(this, QStringLiteral("Create temporary signal"), parseError);
        return false;
    }

    if (!ensureSignalSamplesLoaded(program.dependencyIndexes, false)) {
        return false;
    }

    const int outputWidth = qBound(1, widthOverride > 0 ? widthOverride : program.inferredWidth, 64);
    const SignalKind outputKind = (outputWidth <= 1) ? SignalKind::Bit : SignalKind::Bus;
    const ValueRadix outputRadix = (outputWidth <= 1) ? ValueRadix::Bin : ValueRadix::Hex;

    WaveSample absent;
    absent.time = m_wave.meta.start;
    absent.value = waveAbsentValue();
    absent.isAbsent = true;
    absent.rawFieldsReady = true;

    QVector<int> samplePositions(program.dependencyIndexes.size(), 0);
    QVector<WaveSample> currentSamples(program.dependencyIndexes.size(), absent);
    QVector<WaveSample> outputSamples;
    outputSamples.reserve(qMin(1024, qMax(1, program.dependencyIndexes.size() * 16)));

    auto consumeSamplesAtOrBefore = [&](qint64 t) {
        for (int slot = 0; slot < program.dependencyIndexes.size(); ++slot) {
            const int signalIndex = program.dependencyIndexes.at(slot);
            if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
            const WaveSignal& sig = m_wave.signalList.at(signalIndex);
            int& pos = samplePositions[slot];
            while (pos < sig.samples.size() && sig.samples.at(pos).time <= t) {
                WaveSample sample = sig.samples.at(pos++);
                if (!sample.rawFieldsReady) {
                    hydrateWaveSampleRawFields(sig.kind, sig.width, sample);
                }
                currentSamples[slot] = sample;
            }
        }
    };

    auto appendResultAt = [&](qint64 t) -> bool {
        DerivedEvalValue value = evalDerivedExpressionNode(program, program.root, currentSamples);
        if (value.known) value.bits &= waveBitMaskForWidth(outputWidth);
        WaveSample sample = makeDerivedSample(t, value, outputWidth, outputRadix);
        if (outputSamples.isEmpty() || !sameDerivedSampleValue(outputSamples.last(), sample, outputWidth)) {
            outputSamples.push_back(sample);
            if (outputSamples.size() > int(kViewerOnDemandSampleBudget)) {
                return false;
            }
        }
        return true;
    };

    const qint64 startTime = m_wave.meta.start;
    consumeSamplesAtOrBefore(startTime);
    if (!appendResultAt(startTime)) {
        QMessageBox::warning(this,
            QStringLiteral("Create temporary signal"),
            QStringLiteral("The derived signal generated too many transitions."));
        return false;
    }

    for (;;) {
        qint64 nextTime = std::numeric_limits<qint64>::max();
        for (int slot = 0; slot < program.dependencyIndexes.size(); ++slot) {
            const int signalIndex = program.dependencyIndexes.at(slot);
            if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
            const WaveSignal& sig = m_wave.signalList.at(signalIndex);
            const int pos = samplePositions.at(slot);
            if (pos < sig.samples.size()) nextTime = qMin(nextTime, sig.samples.at(pos).time);
        }
        if (nextTime == std::numeric_limits<qint64>::max()) break;
        consumeSamplesAtOrBefore(nextTime);
        if (!appendResultAt(nextTime)) {
            QMessageBox::warning(this,
                QStringLiteral("Create temporary signal"),
                QStringLiteral("The derived signal generated too many transitions."));
            return false;
        }
    }

    if (outputSamples.isEmpty()) {
        QMessageBox::warning(this,
            QStringLiteral("Create temporary signal"),
            QStringLiteral("Expression produced no samples."));
        return false;
    }

    int maxSignalId = -1;
    for (const WaveSignal& sig : m_wave.signalList) {
        maxSignalId = qMax(maxSignalId, sig.signalId);
    }

    WaveSignal derived;
    derived.signalId = maxSignalId + 1;
    derived.storageId = -1;
    derived.name = signalName;
    derived.kind = outputKind;
    derived.width = outputWidth;
    derived.defaultRadix = outputRadix;
    derived.currentRadix = outputRadix;
    derived.supportsZState = true;
    derived.samplesLoaded = true;
    derived.samples = std::move(outputSamples);
    rebuildWaveSignalDerivedCaches(derived);

    const int newSignalIndex = m_wave.signalList.size();
    m_wave.signalList.push_back(std::move(derived));
    if (m_signalIndexBySignalId.size() <= maxSignalId + 1) {
        m_signalIndexBySignalId.resize(maxSignalId + 2);
    }
    m_signalIndexBySignalId[maxSignalId + 1] = newSignalIndex + 1;

    addSignalToActive(newSignalIndex);
    if (m_canvas) m_canvas->update();

    QMessageBox::information(this,
        QStringLiteral("Create temporary signal"),
        QStringLiteral("Temporary signal '%1' created with %2 transition sample(s).")
            .arg(signalName)
            .arg(m_wave.signalList.at(newSignalIndex).samples.size()));
    return true;
}

void MainWindow::resetView() { m_canvas->resetView(); refreshActiveValueLabels(); }

void MainWindow::insertSignalIntoTree(const QString& fullName, int signalIndex) {
    Q_UNUSED(fullName);
    if (!m_tree || !m_treeModel || !m_signalTreeModel) return;
    if (signalIndex < 0 || signalIndex >= m_signalTreeModel->nodeIdBySignalIndex.size()) return;

    const int nodeId = m_signalTreeModel->nodeIdBySignalIndex.at(signalIndex);
    SignalTreeModel* model = signalTreeModelFrom(m_treeModel);
    if (!model) return;
    const QModelIndex index = model->indexForNode(nodeId);
    if (!index.isValid()) return;
    m_tree->scrollTo(index, QAbstractItemView::PositionAtCenter);
    if (m_tree->selectionModel()) {
        m_tree->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_tree->setCurrentIndex(index);
    }
}

void MainWindow::resetTreeViewModel() {
    if (!m_tree) return;
    if (!m_treeModel) {
        m_treeModel = new SignalTreeModel(m_tree);
        m_tree->setModel(m_treeModel);
    }
    SignalTreeModel* model = signalTreeModelFrom(m_treeModel);
    if (!model) return;
    model->setLogicTree(m_signalTreeModel.get());
    m_tree->collapseAll();
    m_tree->viewport()->update();
}

void MainWindow::rebuildTree() {
    if (!m_signalTreeModel) {
        m_signalTreeModel.reset(new SignalLogicTree);
    }

    if (m_wave.tree.valid) {
        m_signalTreeModel->buildFromWaveTree(m_wave.tree, m_wave.signalList);
    } else {
        m_signalTreeModel->buildFromSignalDefs(m_wave.signalList);
    }
    resetTreeViewModel();
}

void MainWindow::collectSignalIndexesFromLogicNode(int nodeId, QSet<int>& seen, QList<int>& output) const {
    if (!m_signalTreeModel || nodeId < 0 || nodeId >= m_signalTreeModel->nodes.size()) return;

    const LogicTreeNode& node = m_signalTreeModel->nodes.at(nodeId);
    if (node.signalIndex >= 0) {
        if (!seen.contains(node.signalIndex)) {
            seen.insert(node.signalIndex);
            output.push_back(node.signalIndex);
        }
        return;
    }

    const LogicChildList* list = m_signalTreeModel->childListForNode(nodeId);
    if (!list) return;

    for (int childNodeId : list->children) {
        collectSignalIndexesFromLogicNode(childNodeId, seen, output);
    }
}

void MainWindow::showTreeSearchResults(const QString& query) {
    if (!m_treeModel || !m_signalTreeModel) return;
    SignalTreeModel* model = signalTreeModelFrom(m_treeModel);
    if (!model) return;

    const QString q = query.trimmed();
    if (q.isEmpty()) {
        model->clearSearch();
        m_tree->collapseAll();
        return;
    }

    const int maxResults = 5000;
    const QVector<int> matchedNodeIds = m_signalTreeModel->searchTreeQuery(q, maxResults);
    model->setSearchRoots(matchedNodeIds);

    // In search mode keep the real tree skeleton.  Expand only ancestor paths
    // needed to reveal matched nodes; do not expand every matched module subtree,
    // otherwise a broad module-name search can expand thousands of descendants
    // and stall the UI.  A matched module remains collapsible/expandable by the
    // user through its +/- indicator.
    auto expandAncestorsTo = [this, model](int nodeId) {
        if (!m_signalTreeModel) return;
        QVector<int> chain;
        int cur = nodeId;
        int guard = 0;
        while (cur >= 0 && cur < m_signalTreeModel->nodes.size() && guard++ < m_signalTreeModel->nodes.size()) {
            chain.push_back(cur);
            cur = m_signalTreeModel->nodes.at(cur).parent;
        }
        for (int i = chain.size() - 1; i >= 1; --i) {
            const QModelIndex idx = model->indexForNode(chain.at(i));
            if (idx.isValid()) m_tree->expand(idx);
        }
    };

    const int expandLimit = qMin(matchedNodeIds.size(), 512);
    for (int i = 0; i < expandLimit; ++i) expandAncestorsTo(matchedNodeIds.at(i));

    if (!matchedNodeIds.isEmpty()) {
        const QModelIndex first = model->indexForNode(matchedNodeIds.first());
        if (first.isValid()) m_tree->scrollTo(first, QAbstractItemView::PositionAtTop);
    }
}

void MainWindow::onTreeSearchTextChanged(const QString& text) {
    showTreeSearchResults(text);
}

void MainWindow::onTreeIndexDoubleClicked(const QModelIndex& index) {
    if (!index.isValid()) return;
    const QVariant data = index.data(kTreeRoleSignalIndex);
    if (!data.isValid()) return;
    addSignalToActive(data.toInt());
}


int MainWindow::signalIndexFromActiveItem(QTreeWidgetItem* item) const {
    return item ? item->data(0, RoleSignalIndex).toInt() : -1;
}

ValueRadix MainWindow::formatFromActiveItem(QTreeWidgetItem* item) const {
    if (!item) return ValueRadix::Bin;
    return textToFormat(item->data(0, RoleCurrentFormat).toString());
}

void MainWindow::setActiveItemFormat(QTreeWidgetItem* item, const QString& text) {
    if (!item) return;

    QList<QTreeWidgetItem*> picked = m_activeList->selectedItems();
    if (picked.isEmpty() || !picked.contains(item)) {
        picked.clear();
        picked.push_back(item);
    }

    bool changed = false;
    const ValueRadix fmt = textToFormat(text);
    for (QTreeWidgetItem* one : picked) {
        const int signalIndex = signalIndexFromActiveItem(one);
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;

        const WaveSignal& sig = m_wave.signalList.at(signalIndex);
        if (sig.width != 32 && (fmt == ValueRadix::Int || fmt == ValueRadix::UInt || fmt == ValueRadix::Float)) {
            continue;
        }
        one->setData(0, RoleCurrentFormat, text.toLower());
        changed = true;
    }

    if (changed) {
        rebuildVisibleSignals();
        refreshActiveValueLabels();
    }
}

bool MainWindow::canDeferSamplesWithLod(const WaveSignal& sig) const {
    if (viewerDisableLodEnabled()) return false;
    if (sig.lodLevels.isEmpty() || !m_canvas) return false;
    const int plotWidth = qMax(1, m_canvas->width() - 20);
    const qint64 span = qMax<qint64>(1, m_canvas->viewEnd() - m_canvas->viewStart());
    const double cyclesPerPixel = double(span) / double(plotWidth);
    return cyclesPerPixel >= 128.0;
}

bool MainWindow::ensureSignalSamplesLoaded(const QList<int>& signalIndexes, bool allowLodDefer) {
    if (!m_currentWaveSupportsOnDemand || m_currentWaveFilePath.isEmpty()) return true;
    if (signalIndexes.isEmpty()) return true;

    QVector<int> signalIdsToLoad;
    QSet<int> seenIds;
    QList<int> validIndexes;

    for (int signalIndex : signalIndexes) {
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
        const WaveSignal& sig = m_wave.signalList.at(signalIndex);
        if (sig.samplesLoaded) continue;
        if (allowLodDefer && canDeferSamplesWithLod(sig)) continue;
        if (sig.signalId < 0) continue;
        if (seenIds.contains(sig.signalId)) continue;
        seenIds.insert(sig.signalId);
        signalIdsToLoad.push_back(sig.signalId);
        validIndexes.push_back(signalIndex);
    }

    if (signalIdsToLoad.isEmpty()) return true;

    WaveFile loadedWave;
    QString error;
    bool loadOk = false;
    if (m_currentWaveFilePath.endsWith(".wvz4", Qt::CaseInsensitive)) {
        WaveParser4::LoadOptions loadOptions;
        loadOptions.signalIds = signalIdsToLoad;
        loadOptions.includeAllSignalDefinitions = false;
        loadOptions.loadAllIfWindowEmpty = false;
        loadOptions.maxDecodedSamples = kViewerOnDemandSampleBudget;
        loadOk = WaveParser4::loadFromFile(m_currentWaveFilePath, loadedWave, error, loadOptions);
    } else {
        WaveParser3::LoadOptions loadOptions;
        loadOptions.signalIds = signalIdsToLoad;
        loadOptions.includeAllSignalDefinitions = false;
        loadOptions.loadAllIfWindowEmpty = false;
        loadOk = WaveParser3::loadFromFile(m_currentWaveFilePath, loadedWave, error, loadOptions);
    }

    if (!loadOk) {
        QMessageBox::warning(this,
            QString::fromUtf8("加载信号失败"),
            error.isEmpty() ? QString::fromUtf8("无法按需加载所选信号。") : error);
        return false;
    }

    QSet<int> returnedIds;
    for (WaveSignal& loadedSig : loadedWave.signalList) {
        if (loadedSig.signalId < 0) continue;
        returnedIds.insert(loadedSig.signalId);
        int targetIndex = -1;
        const int sid = loadedSig.signalId;
        if (sid >= 0 && sid < m_signalIndexBySignalId.size()) {
            targetIndex = m_signalIndexBySignalId.at(sid) - 1;
        }
        if (targetIndex < 0 || targetIndex >= m_wave.signalList.size()) continue;

        WaveSignal& target = m_wave.signalList[targetIndex];
        target.samples = std::move(loadedSig.samples);
        target.samplesLoaded = true;
        target.supportsZState = loadedSig.supportsZState;
        target.defaultRadix = loadedSig.defaultRadix;
        rebuildWaveSignalDerivedCaches(target);
    }

    // A selected signal may be constant and therefore produce no wave records.
    // Mark it as loaded anyway to avoid repeatedly scanning the file for it.
    for (int signalIndex : validIndexes) {
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
        WaveSignal& sig = m_wave.signalList[signalIndex];
        if (seenIds.contains(sig.signalId) && !returnedIds.contains(sig.signalId)) {
            sig.samples.clear();
            sig.samplesLoaded = true;
            rebuildWaveSignalDerivedCaches(sig);
        }
    }

    if (m_canvas) m_canvas->update();
    return true;
}

void MainWindow::addSignalToActive(int signalIndex) {
    addSignalIndexesToActive(QList<int>() << signalIndex);
}

void MainWindow::addSignalIndexesToActive(const QList<int>& signalIndexes) {
    if (signalIndexes.isEmpty()) return;
    if (!ensureSignalSamplesLoaded(signalIndexes, false)) return;

    QList<QTreeWidgetItem*> addedItems;
    addedItems.reserve(signalIndexes.size());

    for (int signalIndex : signalIndexes) {
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;

        const WaveSignal& sig = m_wave.signalList.at(signalIndex);
        auto* item = new QTreeWidgetItem();
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setText(0, formatNameWithRange(signalIndex));
        item->setText(1, "-");
        item->setData(0, RoleSignalIndex, signalIndex);
        item->setData(0, RoleSignalWidth, sig.width);
        item->setData(0, RoleCurrentFormat, formatToText(sig.defaultRadix));
        item->setSizeHint(0, QSize(0, 40));
        item->setSizeHint(1, QSize(0, 40));
        m_activeList->addTopLevelItem(item);
        addedItems.push_back(item);
    }

    if (addedItems.isEmpty()) return;

    m_activeList->clearSelection();
    for (QTreeWidgetItem* item : addedItems) {
        item->setSelected(true);
    }
    m_activeList->setCurrentItem(addedItems.last(), 0, QItemSelectionModel::NoUpdate);
    m_activeList->scrollToItem(addedItems.last());

    rebuildVisibleSignals();
    refreshActiveValueLabels();
}

void MainWindow::removeActiveItem(QTreeWidgetItem* item) {
    if (!item) return;

    QList<QTreeWidgetItem*> picked = m_activeList->selectedItems();
    if (picked.isEmpty() || !picked.contains(item)) {
        picked.clear();
        picked.push_back(item);
    }

    QList<int> rows;
    rows.reserve(picked.size());
    for (QTreeWidgetItem* one : picked) {
        const int row = m_activeList->indexOfTopLevelItem(one);
        if (row >= 0) rows.push_back(row);
    }
    removeActiveRows(rows);
}

void MainWindow::removeActiveRows(const QList<int>& inputRows) {
    if (inputRows.isEmpty()) return;

    QList<int> rows = inputRows;
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

    for (int row : rows) {
        if (row < 0 || row >= m_activeList->topLevelItemCount()) continue;
        delete m_activeList->takeTopLevelItem(row);
    }

    rebuildVisibleSignals();
    refreshActiveValueLabels();
}

void MainWindow::rebuildActiveListRows() {
    for (int i = 0; i < m_activeList->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_activeList->topLevelItem(i);
        const int signalIndex = signalIndexFromActiveItem(item);
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
        const WaveSignal& sig = m_wave.signalList.at(signalIndex);
        item->setText(0, formatNameWithRange(signalIndex));
    }
}

void MainWindow::rebuildVisibleSignals() {
    QVector<ActiveSignalRef> entries;
    for (int i = 0; i < m_activeList->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_activeList->topLevelItem(i);
        const int signalIndex = signalIndexFromActiveItem(item);
        if (signalIndex >= 0) {
            ActiveSignalRef ref;
            ref.signalIndex = signalIndex;
            ref.format = formatFromActiveItem(item);
            entries.push_back(ref);
        }
    }
    m_canvas->setVisibleEntries(entries);
    syncActiveScrollToCanvas();
    rebuildActiveListRows();
}

void MainWindow::syncActiveScrollToCanvas() {
    if (!m_canvas || !m_activeList) return;

    const int total = m_activeList->topLevelItemCount();
    if (total <= 0) {
        m_canvas->setVisibleEntryWindow(0, 0);
        return;
    }

    QScrollBar* bar = m_activeList->verticalScrollBar();
    int firstVisible = bar ? bar->value() : 0;
    firstVisible = qBound(0, firstVisible, total - 1);

    const int viewportH = m_activeList->viewport() ? m_activeList->viewport()->height() : 0;
    int rowH = m_activeList->sizeHintForRow(firstVisible);
    if (rowH <= 0) rowH = 40;

    // ActiveSignalListWidget uses uniform row heights and ScrollPerItem.  Walking
    // every top-level item and calling visualItemRect() on scroll/selection is O(N)
    // and becomes a visible stall when thousands of signals are active.  The
    // scrollbar value is the first visible row in this mode; draw one extra row for
    // the partially visible bottom edge.
    const int visibleCount = qMin(total - firstVisible, qMax(1, (viewportH + rowH - 1) / rowH + 1));
    m_canvas->setVisibleEntryWindow(firstVisible, visibleCount);
}

void MainWindow::clampWindowToAvailableScreen() {
    QScreen* screen = nullptr;
    if (QWindow* handle = windowHandle()) screen = handle->screen();
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;

    const QRect available = screen->availableGeometry();
    const int margin = 24;
    const int maxW = qMax(320, available.width() - margin);
    const int maxH = qMax(240, available.height() - margin);

    // Keep the historical preferred size on normal screens, but avoid an
    // impossible minimum size on smaller displays.
    setMinimumSize(qMin(1120, maxW), qMin(700, maxH));

    const int targetW = qMin(width(), maxW);
    const int targetH = qMin(height(), maxH);
    resize(targetW, targetH);

    const int maxX = available.right() - width() + 1;
    const int maxY = available.bottom() - height() + 1;
    const int centeredX = available.left() + (available.width() - width()) / 2;
    const int centeredY = available.top() + (available.height() - height()) / 2;
    const int x = (maxX < available.left()) ? available.left() : qBound(available.left(), centeredX, maxX);
    const int y = (maxY < available.top()) ? available.top() : qBound(available.top(), centeredY, maxY);
    move(x, y);
}

void MainWindow::scheduleRefreshActiveValueLabels(int delayMs) {
    if (!m_activeValueRefreshTimer) {
        refreshActiveValueLabels();
        return;
    }
    m_activeValueRefreshTimer->start(qMax(0, delayMs));
}

void MainWindow::refreshActiveValueLabels() {
    if (!m_activeList || !m_canvas) return;
    const int total = m_activeList->topLevelItemCount();
    if (total <= 0) return;

    QScrollBar* bar = m_activeList->verticalScrollBar();
    int first = bar ? bar->value() : 0;
    first = qBound(0, first, total - 1);

    const int viewportH = m_activeList->viewport() ? m_activeList->viewport()->height() : 0;
    int rowH = m_activeList->sizeHintForRow(first);
    if (rowH <= 0) rowH = 40;
    const int count = qMin(total - first, qMax(1, (viewportH + rowH - 1) / rowH + 1));
    const int end = qMin(total, first + count);

    for (int i = first; i < end; ++i) {
        QTreeWidgetItem* item = m_activeList->topLevelItem(i);
        if (!item) continue;
        ActiveSignalRef ref;
        ref.signalIndex = signalIndexFromActiveItem(item);
        ref.format = formatFromActiveItem(item);
        item->setText(1, m_canvas->formattedValueAtCursor(ref));
    }
}

void MainWindow::onAddSelectedFromTree() {
    QSet<int> seen;
    QList<int> signalIndexes;
    if (!m_tree || !m_tree->selectionModel()) {
        addSignalIndexesToActive(signalIndexes);
        return;
    }

    const QModelIndexList picked = m_tree->selectionModel()->selectedRows(0);
    for (const QModelIndex& index : picked) {
        const QVariant nodeValue = index.data(kTreeRoleNodeId);
        if (!nodeValue.isValid()) continue;
        collectSignalIndexesFromLogicNode(nodeValue.toInt(), seen, signalIndexes);
    }
    addSignalIndexesToActive(signalIndexes);
}

void MainWindow::onRemoveSelectedActive() {
    QList<QTreeWidgetItem*> picked = m_activeList->selectedItems();
    if (picked.isEmpty()) {
        if (QTreeWidgetItem* item = m_activeList->currentItem()) picked.push_back(item);
    }
    QList<int> rows;
    rows.reserve(picked.size());
    for (QTreeWidgetItem* item : picked) {
        const int row = m_activeList->indexOfTopLevelItem(item);
        if (row >= 0) rows.push_back(row);
    }
    removeActiveRows(rows);
}

void MainWindow::onClearActive() {
    m_activeList->clear();
    m_canvas->setSelectedEntryIndex(-1);
    rebuildVisibleSignals();
    refreshActiveValueLabels();
}

void MainWindow::onActiveCurrentItemChanged(QTreeWidgetItem* current, QTreeWidgetItem*) {
    const int row = current ? m_activeList->indexOfTopLevelItem(current) : -1;
    if (row >= 0) {
        QSet<int> indexes = selectedTopLevelIndexes(m_activeList);
        if (indexes.isEmpty()) indexes.insert(row);
        m_canvas->setSelectedEntryIndexes(indexes);
    }
    else {
        m_canvas->setSelectedEntryIndexes(QSet<int>());
    }
}


void MainWindow::onCursorMoved(qint64) {
    scheduleRefreshActiveValueLabels(35);
}

void MainWindow::onHoverMoved(qint64) {
}

void MainWindow::onViewportChanged(qint64, qint64) {
    if (m_currentWaveSupportsOnDemand && m_activeList) {
        QList<int> needRawSignals;
        for (int i = 0; i < m_activeList->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = m_activeList->topLevelItem(i);
            const int signalIndex = signalIndexFromActiveItem(item);
            if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
            const WaveSignal& sig = m_wave.signalList.at(signalIndex);
            if (sig.samplesLoaded) continue;
            if (canDeferSamplesWithLod(sig)) continue;
            needRawSignals.push_back(signalIndex);
        }
        if (!needRawSignals.isEmpty() && ensureSignalSamplesLoaded(needRawSignals, false)) {
            rebuildVisibleSignals();
        }
    }
    scheduleRefreshActiveValueLabels(35);
}
