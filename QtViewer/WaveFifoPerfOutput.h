#pragma once

#include <QJsonObject>
#include <QString>

namespace wavefifo {

bool writeFifoPerformanceBundle(const QString& outputDirectory,
                                const QJsonObject& model,
                                QString& error);

}  // namespace wavefifo
