#include "WaveParser4.h"

#include <QCoreApplication>

#include <iostream>

static int fail(const QString& message) {
    std::cerr << message.toLocal8Bit().constData() << "\n";
    return 1;
}

static int findChild(const WaveTreeInfo& tree, int parentId, const QString& name) {
    if (parentId <= 0 || parentId >= tree.nodesById.size()) return 0;
    for (int child = tree.nodesById.at(parentId).firstChild;
         child != 0;
         child = tree.nodesById.at(child).nextSibling) {
        if (waveTreeNodeSegmentName(tree, child) == name) return child;
    }
    return 0;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2) return fail(QStringLiteral("missing WVZ4 path"));

    WaveParser4::LoadOptions options;
    options.includeAllSignalDefinitions = true;
    options.loadRawSamples = true;
    options.autoLoadFirstSignalCount = -1;

    WaveFile wave;
    QString error;
    if (!WaveParser4::loadFromFile(QString::fromLocal8Bit(argv[1]), wave, error, options)) {
        return fail(QStringLiteral("load failed: ") + error);
    }
    if (!wave.tree.valid || wave.tree.rootNodeIds.size() != 1) {
        return fail(QStringLiteral("missing waveform tree"));
    }

    const int top = wave.tree.rootNodeIds.front();
    const int first = findChild(wave.tree, top, QStringLiteral("first"));
    const int second = findChild(wave.tree, top, QStringLiteral("second"));
    const int peekFirst = findChild(wave.tree, top, QStringLiteral("peekFirst"));
    const int peekSecond = findChild(wave.tree, top, QStringLiteral("peekSecond"));
    if (first == 0 || second == 0 || peekFirst == 0 || peekSecond == 0) {
        return fail(QStringLiteral("missing canonical or reference mount"));
    }
    if (wave.tree.nodesById.at(second).kind != 7 ||
        wave.tree.nodesById.at(second).referenceTargetId != first ||
        wave.tree.nodesById.at(peekSecond).kind != 7 ||
        wave.tree.nodesById.at(peekSecond).referenceTargetId != peekFirst) {
        return fail(QStringLiteral("Dynamic/Peek reference targets are incorrect"));
    }

    const int firstCount = findChild(wave.tree, first, QStringLiteral("count"));
    const int secondCount = findChild(wave.tree, second, QStringLiteral("count"));
    const int peekFirstCount =
        findChild(wave.tree, peekFirst, QStringLiteral("count"));
    const int peekSecondCount =
        findChild(wave.tree, peekSecond, QStringLiteral("count"));
    if (firstCount == 0 || secondCount == 0 ||
        peekFirstCount == 0 || peekSecondCount == 0) {
        return fail(QStringLiteral("reference subtree was not expanded transparently"));
    }
    const WaveTreeNode& firstLeaf = wave.tree.nodesById.at(firstCount);
    const WaveTreeNode& secondLeaf = wave.tree.nodesById.at(secondCount);
    if (firstLeaf.signalIndex < 0 ||
        firstLeaf.signalIndex != secondLeaf.signalIndex ||
        firstLeaf.signalId != secondLeaf.signalId) {
        return fail(QStringLiteral("reference leaves do not reuse canonical signal/storage"));
    }
    if (wave.tree.nodesById.at(peekFirstCount).signalIndex !=
            wave.tree.nodesById.at(peekSecondCount).signalIndex ||
        wave.tree.nodesById.at(peekFirstCount).signalId !=
            wave.tree.nodesById.at(peekSecondCount).signalId) {
        return fail(QStringLiteral("Peek reference leaves do not reuse the canonical signal"));
    }
    if (wave.signalList.size() != 4) {
        return fail(QStringLiteral("logical signal table was duplicated"));
    }

    WaveParser4Reader reader;
    if (!reader.open(QString::fromLocal8Bit(argv[1]), error)) {
        return fail(QStringLiteral("indexed open failed: ") + error);
    }
    WaveTreeInfo indexedTree = reader.directoryWave().tree;
    const int indexedTop = indexedTree.rootNodeIds.isEmpty()
        ? 0 : indexedTree.rootNodeIds.front();
    const int indexedFirst = findChild(indexedTree, indexedTop, QStringLiteral("first"));
    const int indexedSecond = findChild(indexedTree, indexedTop, QStringLiteral("second"));
    const int indexedPeekFirst =
        findChild(indexedTree, indexedTop, QStringLiteral("peekFirst"));
    const int indexedPeekSecond =
        findChild(indexedTree, indexedTop, QStringLiteral("peekSecond"));
    const int indexedFirstCount =
        findChild(indexedTree, indexedFirst, QStringLiteral("count"));
    const int indexedSecondCountBeforeLoad =
        findChild(indexedTree, indexedSecond, QStringLiteral("count"));
    const int indexedPeekFirstCount =
        findChild(indexedTree, indexedPeekFirst, QStringLiteral("count"));
    const int indexedPeekSecondCountBeforeLoad =
        findChild(indexedTree, indexedPeekSecond, QStringLiteral("count"));
    if (indexedFirstCount == 0 || indexedPeekFirstCount == 0 ||
        indexedSecondCountBeforeLoad != 0 ||
        indexedPeekSecondCountBeforeLoad != 0) {
        return fail(QStringLiteral("indexed Viewer path eagerly materialized a shared subtree"));
    }

    WaveSubtreeReferencePatch dynamicPatch;
    WaveSubtreeReferencePatch peekPatch;
    if (!buildWaveSubtreeReferencePatch(indexedTree, indexedSecond,
                                        dynamicPatch, error) ||
        !applyWaveSubtreeReferencePatch(indexedTree, dynamicPatch, error) ||
        !buildWaveSubtreeReferencePatch(indexedTree, indexedPeekSecond,
                                        peekPatch, error) ||
        !applyWaveSubtreeReferencePatch(indexedTree, peekPatch, error)) {
        return fail(QStringLiteral("background reference patch failed: ") + error);
    }
    const int indexedSecondCount =
        findChild(indexedTree, indexedSecond, QStringLiteral("count"));
    const int indexedPeekSecondCount =
        findChild(indexedTree, indexedPeekSecond, QStringLiteral("count"));
    if (indexedSecondCount == 0 || indexedPeekSecondCount == 0 ||
        indexedTree.nodesById.at(indexedFirstCount).signalIndex !=
            indexedTree.nodesById.at(indexedSecondCount).signalIndex ||
        indexedTree.nodesById.at(indexedPeekFirstCount).signalIndex !=
            indexedTree.nodesById.at(indexedPeekSecondCount).signalIndex) {
        return fail(QStringLiteral("indexed Viewer path did not materialize the shared subtree"));
    }

    std::cout << "wvz4_subtree_reference_parser_ok signals="
              << wave.signalList.size()
              << " nodes=" << wave.tree.nodesById.size() - 1
              << " shared_signal_index=" << firstLeaf.signalIndex << "\n";
    return 0;
}
