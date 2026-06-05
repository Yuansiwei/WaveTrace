#pragma once

#include "WaveTypes.h"
#include <QString>

class WaveParser {
public:
    static bool loadFromFile(const QString& filePath, WaveFile& outWave, QString& error);
    static bool saveToCompressedFile(const QString& filePath, const WaveFile& wave, QString& error);

private:
    static bool loadFromJsonBytes(const QByteArray& data, WaveFile& outWave, QString& error);
    static QByteArray toCompactJsonBytes(const WaveFile& wave);
};
