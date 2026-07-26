#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace waveperf {

QJsonArray buildQppuConclusions(const QJsonObject& model);
QJsonObject buildWorkloadProfile(
    const QJsonObject& model,
    const QJsonArray& qppuConclusions);
QJsonArray buildPerformanceFindings(const QJsonObject& model);
QString buildPerformanceConclusion(const QJsonObject& model,
                                   const QJsonArray& findings);
bool performanceDiagnosisSelfTest(QString& error);

}  // namespace waveperf
