#include "WaveParser.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtGlobal>

namespace {
    constexpr char kWaveCompressedMagic[] = "WVZ1";

    ValueRadix parseRadixText(const QString& s) {
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

    QString radixToText(ValueRadix r) {
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

    bool parseStrictInt64String(const QJsonValue& value, qint64& out) {
        if (!value.isString()) return false;
        bool ok = false;
        const QString s = value.toString().trimmed();
        if (s.isEmpty()) return false;
        out = s.toLongLong(&ok, 10);
        return ok;
    }
}

bool WaveParser::loadFromJsonBytes(const QByteArray& data, WaveFile& outWave, QString& error) {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        error = QString("JSON 解析失败：%1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    if (!root.contains("signals") || !root.value("signals").isArray()) {
        error = "无效波形文件：缺少 signals 数组";
        return false;
    }

    outWave = WaveFile{};

    if (root.contains("meta") && root.value("meta").isObject()) {
        const QJsonObject metaObj = root.value("meta").toObject();
        outWave.meta.title = metaObj.value("title").toString("custom_wave");
        outWave.meta.timescale = metaObj.value("timescale").toString("1ns");
        qint64 start = 0;
        qint64 end = 100;
        if (!parseStrictInt64String(metaObj.value("start"), start)) {
            error = "meta.start 必须是字符串形式的 int64";
            return false;
        }
        if (!parseStrictInt64String(metaObj.value("end"), end)) {
            error = "meta.end 必须是字符串形式的 int64";
            return false;
        }
        outWave.meta.start = start;
        outWave.meta.end = end;
    }

    qint64 minT = std::numeric_limits<qint64>::max();
    qint64 maxT = std::numeric_limits<qint64>::min();
    const QJsonArray sigArray = root.value("signals").toArray();

    for (int i = 0; i < sigArray.size(); ++i) {
        if (!sigArray.at(i).isObject()) {
            error = QString("第 %1 个信号格式错误").arg(i + 1);
            return false;
        }

        const QJsonObject sigObj = sigArray.at(i).toObject();
        const QString name = sigObj.value("name").toString();
        if (name.isEmpty()) {
            error = QString("第 %1 个信号缺少 name").arg(i + 1);
            return false;
        }
        if (!sigObj.contains("samples") || !sigObj.value("samples").isArray()) {
            error = QString("信号 %1 缺少 samples 数组").arg(name);
            return false;
        }

        const QString kindText = sigObj.value("kind").toString("bit").trimmed().toLower();
        const SignalKind kind = (kindText == "bus") ? SignalKind::Bus : SignalKind::Bit;

        WaveSignal sig;
        sig.name = name;
        sig.kind = kind;
        sig.width = qMax(1, sigObj.value("width").toInt(1));
        sig.defaultRadix = parseRadixText(sigObj.value("radix").toString(kind == SignalKind::Bus ? "hex" : "bin"));
        sig.currentRadix = sig.defaultRadix;

        const QJsonArray samples = sigObj.value("samples").toArray();
        for (int k = 0; k < samples.size(); ++k) {
            if (!samples.at(k).isArray()) {
                error = QString("信号 %1 的 samples[%2] 不是数组").arg(name).arg(k);
                return false;
            }
            const QJsonArray item = samples.at(k).toArray();
            if (item.size() < 2) {
                error = QString("信号 %1 的 samples[%2] 长度不足").arg(name).arg(k);
                return false;
            }

            WaveSample ws;
            if (!parseStrictInt64String(item.at(0), ws.time)) {
                error = QString("信号 %1 的 samples[%2][0] 必须是字符串形式的 int64").arg(name).arg(k);
                return false;
            }
            if (item.at(1).isString()) ws.value = item.at(1).toString();
            else if (item.at(1).isDouble()) ws.value = QString::number(item.at(1).toVariant().toULongLong());
            else if (item.at(1).isBool()) ws.value = item.at(1).toBool() ? "1" : "0";
            else ws.value = "x";

            sig.samples.push_back(ws);
            if (ws.time < minT) minT = ws.time;
            if (ws.time > maxT) maxT = ws.time;
        }

        for (WaveSample& ws : sig.samples) hydrateWaveSampleRawFields(sig.kind, sig.width, ws);
        outWave.signalList.push_back(sig);
    }

    if (outWave.signalList.isEmpty()) {
        error = "signals 为空";
        return false;
    }

    if (outWave.meta.end <= outWave.meta.start) {
        if (minT != std::numeric_limits<qint64>::max() && maxT != std::numeric_limits<qint64>::min() && maxT > minT) {
            outWave.meta.start = minT;
            outWave.meta.end = maxT;
        }
        else {
            outWave.meta.start = 0;
            outWave.meta.end = 100;
        }
    }

    return true;
}

QByteArray WaveParser::toCompactJsonBytes(const WaveFile& wave) {
    QJsonObject root;
    QJsonObject meta;
    meta.insert("title", wave.meta.title);
    meta.insert("timescale", wave.meta.timescale);
    meta.insert("start", QString::number(wave.meta.start));
    meta.insert("end", QString::number(wave.meta.end));
    root.insert("meta", meta);

    QJsonArray sigArray;
    for (const WaveSignal& sig : wave.signalList) {
        QJsonObject sigObj;
        sigObj.insert("name", sig.name);
        sigObj.insert("kind", sig.kind == SignalKind::Bit ? "bit" : "bus");
        sigObj.insert("width", sig.width);
        sigObj.insert("radix", radixToText(sig.defaultRadix));

        QJsonArray samples;
        for (const WaveSample& s : sig.samples) {
            QJsonArray item;
            item.push_back(QString::number(s.time));
            item.push_back(waveSampleRawText(sig, s));
            samples.push_back(item);
        }
        sigObj.insert("samples", samples);
        sigArray.push_back(sigObj);
    }
    root.insert("signals", sigArray);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool WaveParser::loadFromFile(const QString& filePath, WaveFile& outWave, QString& error) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QString("无法打开文件：%1").arg(filePath);
        return false;
    }
    const QByteArray raw = file.readAll();
    file.close();

    if (raw.startsWith(kWaveCompressedMagic) || filePath.endsWith(".wvz", Qt::CaseInsensitive)) {
        QByteArray payload = raw;
        if (payload.startsWith(kWaveCompressedMagic)) payload.remove(0, 4);
        const QByteArray jsonBytes = qUncompress(payload);
        if (jsonBytes.isEmpty()) {
            error = "压缩波形文件解压失败";
            return false;
        }
        return loadFromJsonBytes(jsonBytes, outWave, error);
    }

    return loadFromJsonBytes(raw, outWave, error);
}

bool WaveParser::saveToCompressedFile(const QString& filePath, const WaveFile& wave, QString& error) {
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        error = QString("无法写入文件：%1").arg(filePath);
        return false;
    }

    const QByteArray jsonBytes = toCompactJsonBytes(wave);
    const QByteArray compressed = qCompress(jsonBytes, 9);
    QByteArray out;
    out.append(kWaveCompressedMagic, 4);
    out.append(compressed);

    if (file.write(out) != out.size()) {
        error = QString("写入压缩波形文件失败：%1").arg(filePath);
        file.close();
        return false;
    }
    file.close();
    return true;
}
