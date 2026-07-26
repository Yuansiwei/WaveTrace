#pragma once

#include <QJsonObject>
#include <QString>

namespace waveperf {

bool writePerformanceBundle(const QString& outputDirectory,
                            QJsonObject& model,
                            QString& error);

QString buildPerformanceConsoleSummary(const QJsonObject& model,
                                       const QString& outputDirectory);

bool performanceOutputSelfTest(QString& error);

}  // namespace waveperf
