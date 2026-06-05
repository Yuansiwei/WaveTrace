#include "WaveParser4.h"

#include <QCoreApplication>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::cerr << "usage: smoke_wvz4_parser <file.wvz4>\n";
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
    if (wave.signalList.size() < 2 ||
        wave.signalList.at(0).samples.size() < 2 ||
        wave.signalList.at(1).samples.size() < 2 ||
        wave.signalList.at(0).samples.last().rawBits != 1 ||
        wave.signalList.at(1).samples.last().rawBits != 1) {
        std::cerr << "full load did not fan out storage samples to both logical signals\n";
        return 4;
    }

    WaveFile one;
    WaveParser4::LoadOptions oneSignal;
    oneSignal.signalIds.push_back(2);
    oneSignal.includeAllSignalDefinitions = false;
    oneSignal.loadAllIfWindowEmpty = false;
    if (!WaveParser4::loadFromFile(QString::fromLocal8Bit(argv[1]), one, error, oneSignal)) {
        std::cerr << "on-demand load failed: " << error.toLocal8Bit().constData() << "\n";
        return 5;
    }
    if (one.signalList.size() != 1 ||
        one.signalList.at(0).signalId != 2 ||
        one.signalList.at(0).samples.size() < 2 ||
        one.signalList.at(0).samples.last().rawBits != 1) {
        std::cerr << "on-demand logical alias load did not decode its storage stream\n";
        return 6;
    }

    return 0;
}
