#include "AtlasImporter.h"

#include "TextNormalizer.h"
#include "AppPaths.h"
#include "AtlasReport.h"
#include "Utf8Streams.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QLocale>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>
#include <QUuid>
#include <QVariant>
#include <QXmlStreamReader>
#include <QFileInfo>
#include <algorithm>

namespace {
constexpr qsizetype MaximumLineLength = 500;
constexpr qint64 ProgressInterval = 10000;
constexpr qint64 PreprocessReportInterval = 100000;
constexpr qint64 CommitInterval = 50000;
constexpr qint64 MaximumQualityWarningsPerImport = 5000;
constexpr qint64 RejectedSummarySnapshotInterval = 100000;
constexpr qsizetype MaximumPunctuationSplitSegmentWords = 8;

struct PunctuationSplitResult {
    bool shouldReport = false;
    QString skipReason;
    QList<QPair<QString, QString>> segments;
};

bool isSameNormalizedText(const QString &sourceText, const QString &targetText);
qsizetype normalizedWordCount(const QString &text);
bool looksLikeDesktopMetadata(const QString &text);
bool hasSameTextFlag(const QStringList &flags);
bool hasAcceptedSameTextFlag(const QStringList &flags);
bool hasSameProperNameFlag(const QStringList &flags);

struct OpusPreprocessDecision {
    QString sourceClean;
    QString targetClean;
    QStringList flags;
    qsizetype sourceWords = 0;
    qsizetype targetWords = 0;
    int qualityScore = 100;
    QString importBucket;
};

struct OpusAlignmentLink {
    QString fromDoc;
    QString toDoc;
    QString alignment;
    bool valid = false;
};

struct OpusIdsRow {
    QString raw;
    QString fromDoc;
    QString toDoc;
    QString sourceId;
    QString targetId;
    bool valid = false;
};

struct OpusPreprocessRejectExample {
    QString reason;
    qint64 line = 0;
    QString sourceText;
    QString targetText;
    QString sourceClean;
    QString targetClean;
    QString flags;
    QString bucket;
    int qualityScore = 0;
    QString xmlFromDoc;
    QString xmlToDoc;
    QString xmlAlignment;
    QString idsRaw;
    QString idsFromDoc;
    QString idsToDoc;
    QString idsSourceId;
    QString idsTargetId;
};

struct MediaWikiPreprocessRejectExample {
    QString reason;
    qint64 page = 0;
    QString title;
    QString sourceLanguage;
    QString targetLanguage;
    QString line;
    QString translation;
    QString gloss;
};

class OpusAlignmentReader
{
public:
    explicit OpusAlignmentReader(const QString &xmlFilePath)
        : m_file(xmlFilePath)
        , m_xml(&m_file)
    {
        if (xmlFilePath.isEmpty() || !QFileInfo::exists(xmlFilePath)) {
            return;
        }
        m_available = m_file.open(QIODevice::ReadOnly | QIODevice::Text);
    }

    bool isAvailable() const
    {
        return m_available;
    }

    QString errorString() const
    {
        return m_error;
    }

    bool next(OpusAlignmentLink &link)
    {
        link = OpusAlignmentLink();
        if (!m_available) {
            return false;
        }

        while (!m_xml.atEnd()) {
            m_xml.readNext();
            if (!m_xml.isStartElement()) {
                continue;
            }

            const QStringView name = m_xml.name();
            if (name == QStringLiteral("linkGrp")) {
                const QXmlStreamAttributes attributes = m_xml.attributes();
                m_currentFromDoc = attributes.value(QStringLiteral("fromDoc")).toString();
                m_currentToDoc = attributes.value(QStringLiteral("toDoc")).toString();
            } else if (name == QStringLiteral("link")) {
                link.fromDoc = m_currentFromDoc;
                link.toDoc = m_currentToDoc;
                link.alignment = m_xml.attributes().value(QStringLiteral("xtargets")).toString();
                link.valid = !link.alignment.trimmed().isEmpty();
                return true;
            }
        }

        if (m_xml.hasError()) {
            m_error = m_xml.errorString();
        }
        return false;
    }

private:
    QFile m_file;
    QXmlStreamReader m_xml;
    QString m_currentFromDoc;
    QString m_currentToDoc;
    QString m_error;
    bool m_available = false;
};

OpusIdsRow parseOpusIdsRow(const QString &line)
{
    OpusIdsRow row;
    row.raw = line.trimmed();
    const QStringList columns = line.split(QLatin1Char('\t'));
    if (columns.size() >= 4) {
        row.fromDoc = columns.at(0).trimmed();
        row.toDoc = columns.at(1).trimmed();
        row.sourceId = columns.at(2).trimmed();
        row.targetId = columns.at(3).trimmed();
        row.valid = !row.fromDoc.isEmpty() && !row.toDoc.isEmpty()
            && !row.sourceId.isEmpty() && !row.targetId.isEmpty();
    } else if (!row.raw.isEmpty()) {
        row.valid = true;
    }
    return row;
}

QString opusAlignmentSourceId(const QString &alignment)
{
    return alignment.section(QLatin1Char(';'), 0, 0).trimmed();
}

QString opusAlignmentTargetId(const QString &alignment)
{
    return alignment.section(QLatin1Char(';'), 1, 1).trimmed();
}

QString safeTsvField(QString text)
{
    text.replace(QLatin1Char('\t'), QLatin1Char(' '));
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    text.replace(QRegularExpression(QStringLiteral(R"(\s+)")), QStringLiteral(" "));
    return text.trimmed();
}

QString discardReasonLabel(const QString &reason)
{
    if (reason == QStringLiteral("url/contact")) {
        return QStringLiteral("Ignorado: URL/contato");
    }
    if (reason == QStringLiteral("contact/url row")
        || reason == QStringLiteral("contact/phone row")
        || reason == QStringLiteral("address/contact row")) {
        return QStringLiteral("Ignorado: URL/contato");
    }
    if (reason == QStringLiteral("email/contact")) {
        return QStringLiteral("Ignorado: e-mail/contato");
    }
    if (reason == QStringLiteral("contact/email row")) {
        return QStringLiteral("Ignorado: e-mail/contato");
    }
    if (reason == QStringLiteral("metadata")
        || reason == QStringLiteral("metadata row")
        || reason == QStringLiteral("copyright row")
        || reason == QStringLiteral("administrative content")
        || reason == QStringLiteral("OPUS possible metadata quarantine")) {
        return QStringLiteral("Ignorado: metadado");
    }
    if (reason == QStringLiteral("same source target")
        || reason == QStringLiteral("same text row")
        || reason == QStringLiteral("OPUS same normalized text skipped")) {
        return QStringLiteral("Ignorado: texto igual origem/destino");
    }
    if (reason == QStringLiteral("OPUS same text review")) {
        return QStringLiteral("Ignorado: texto igual enviado para revisao");
    }
    if (reason == QStringLiteral("OPUS same proper name frequency only")) {
        return QStringLiteral("Ignorado: texto igual origem/destino (nome proprio contado como frequencia)");
    }
    if (reason == QStringLiteral("invalid language")) {
        return QStringLiteral("Ignorado: idioma fora do par esperado");
    }
    if (reason == QStringLiteral("broken record")) {
        return QStringLiteral("Ignorado: registro quebrado");
    }
    if (reason == QStringLiteral("invalid format")) {
        return QStringLiteral("Ignorado: formato invalido");
    }
    if (reason == QStringLiteral("empty source text")) {
        return QStringLiteral("Ignorado: texto de origem vazio");
    }
    if (reason == QStringLiteral("empty translation")) {
        return QStringLiteral("Ignorado: traducao vazia");
    }
    if (reason == QStringLiteral("OPUS reject_review bucket")) {
        return QStringLiteral("Ignorado: revisao/rejeicao do pre-processamento");
    }
    if (reason.contains(QStringLiteral("exceeds 8 words"))) {
        return QStringLiteral("Ignorado: acima do limite de 8 palavras");
    }
    if (reason == QStringLiteral("long phrase not segmentable")) {
        return QStringLiteral("Ignorado: frase longa nao segmentavel");
    }
    if (reason == QStringLiteral("OPUS segment rejected by quality filter")) {
        return QStringLiteral("Ignorado: segmento rejeitado pelo filtro de qualidade");
    }
    return reason;
}

QString sidecarPathForPreprocessedOutput(const QString &outputFilePath, const QString &suffix)
{
    const QFileInfo outputInfo(outputFilePath);
    return outputInfo.dir().filePath(QStringLiteral("%1%2").arg(outputInfo.completeBaseName(), suffix));
}

QString hiddenPreprocessProgressPath(const QString &identityPath, const QString &visibleName)
{
    QDir().mkpath(AppPaths::preprocessProgressPath());
    QString token = QFileInfo(visibleName).completeBaseName();
    if (token.isEmpty()) {
        token = QFileInfo(identityPath).completeBaseName();
    }
    token.replace(QRegularExpression(QStringLiteral(R"([^A-Za-z0-9_.-]+)")), QStringLiteral("_"));
    if (token.size() > 80) {
        token = token.left(80);
    }

    const QString identity = QFileInfo(identityPath).absoluteFilePath();
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
    return QDir::cleanPath(QDir(AppPaths::preprocessProgressPath()).filePath(QStringLiteral("%1_%2.progress").arg(token, hash)));
}

QString terminalElapsedSuffix(QElapsedTimer &timer)
{
    QString elapsed = QStringLiteral("%1s")
                          .arg(static_cast<double>(timer.nsecsElapsed()) / 1000000000.0, 0, 'f', 2);
    elapsed.replace(QLatin1Char('.'), QLatin1Char(','));
    timer.restart();
    return QStringLiteral(" | %1").arg(elapsed);
}

QString terminalCount(qint64 value)
{
    return QLocale(QLocale::Portuguese, QLocale::Brazil).toString(value);
}

QString terminalShortPath(const QString &path)
{
    const QFileInfo info(path);
    const QString parent = info.dir().dirName();
    if (parent.isEmpty()) {
        return info.fileName();
    }
    return QStringLiteral("%1/%2").arg(parent, info.fileName());
}

bool readOpusPreprocessProgress(const QString &progressFilePath,
                                qint64 &processedLines,
                                qint64 &rowsWritten,
                                bool &completed)
{
    processedLines = 0;
    rowsWritten = 0;
    completed = false;

    QFile file(progressFilePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream stream(&file);
    configureUtf8Stream(stream);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        const int separator = line.indexOf(QLatin1Char('='));
        if (separator <= 0) {
            continue;
        }
        const QString key = line.left(separator);
        const QString value = line.mid(separator + 1);
        if (key == QStringLiteral("processed_lines")) {
            processedLines = value.toLongLong();
        } else if (key == QStringLiteral("rows_written")) {
            rowsWritten = value.toLongLong();
        } else if (key == QStringLiteral("completed")) {
            completed = value == QStringLiteral("1");
        }
    }
    return processedLines > 0 || completed;
}

bool writeOpusPreprocessProgress(const QString &progressFilePath,
                                 qint64 processedLines,
                                 qint64 rowsWritten,
                                 bool completed,
                                 QString *error)
{
    QFile file(progressFilePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (error != nullptr) {
            *error = QStringLiteral("Nao foi possivel gravar arquivo de progresso OPUS: %1").arg(progressFilePath);
        }
        return false;
    }

    QTextStream stream(&file);
    configureUtf8Stream(stream);
    stream << "processed_lines=" << processedLines << Qt::endl;
    stream << "rows_written=" << rowsWritten << Qt::endl;
    stream << "completed=" << (completed ? 1 : 0) << Qt::endl;
    stream.flush();
    return true;
}

bool readPreprocessProgress(const QString &progressFilePath,
                            qint64 &processedItems,
                            qint64 &rowsWritten,
                            bool &completed)
{
    return readOpusPreprocessProgress(progressFilePath, processedItems, rowsWritten, completed);
}

bool writePreprocessProgress(const QString &progressFilePath,
                             const QString &label,
                             qint64 processedItems,
                             qint64 rowsWritten,
                             bool completed,
                             QString *error)
{
    QFile file(progressFilePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (error != nullptr) {
            *error = QStringLiteral("Nao foi possivel gravar arquivo de progresso %1: %2").arg(label, progressFilePath);
        }
        return false;
    }

    QTextStream stream(&file);
    configureUtf8Stream(stream);
    stream << "processed_lines=" << processedItems << Qt::endl;
    stream << "rows_written=" << rowsWritten << Qt::endl;
    stream << "completed=" << (completed ? 1 : 0) << Qt::endl;
    stream.flush();
    return true;
}

bool skipOpusLines(QTextStream &stream, qint64 count)
{
    for (qint64 index = 0; index < count; ++index) {
        if (stream.atEnd()) {
            return false;
        }
        stream.readLine();
    }
    return true;
}

QString cleanOpusMosesText(QString text)
{
    text.replace(QRegularExpression(QStringLiteral(R"(&\s*([A-Za-z][A-Za-z0-9_-]{1,48})\s*;+)")),
                 QStringLiteral("\\1"));
    text.replace(QRegularExpression(QStringLiteral(R"(([A-Za-zÀ-ÖØ-öø-ÿ0-9])-\s+([A-Za-zÀ-ÖØ-öø-ÿ0-9]))")),
                 QStringLiteral("\\1-\\2"));
    text.replace(QRegularExpression(QStringLiteral(R"(\s+([,.;:!?]))")), QStringLiteral("\\1"));
    text.replace(QRegularExpression(QStringLiteral(R"(([({\[])\s+)")), QStringLiteral("\\1"));
    text.replace(QRegularExpression(QStringLiteral(R"(\s+([)}\]]))")), QStringLiteral("\\1"));
    text.replace(QRegularExpression(QStringLiteral(R"(\s+)")), QStringLiteral(" "));
    return text.trimmed();
}

QStringList sharedOpusPlaceholders(const QString &sourceText, const QString &targetText)
{
    static const QRegularExpression placeholderExpression(
        QStringLiteral(R"((?:%\d+|%[A-Za-z_]+|\$\{?[A-Za-z_][A-Za-z0-9_]*\}?|\{\d+\}|@[A-Za-z_]+@|__[A-Za-z0-9_]+__))"));

    QSet<QString> sourceTokens;
    QRegularExpressionMatchIterator sourceMatches = placeholderExpression.globalMatch(sourceText);
    while (sourceMatches.hasNext()) {
        sourceTokens.insert(sourceMatches.next().captured(0));
    }

    QStringList shared;
    QRegularExpressionMatchIterator targetMatches = placeholderExpression.globalMatch(targetText);
    while (targetMatches.hasNext()) {
        const QString token = targetMatches.next().captured(0);
        if (sourceTokens.contains(token) && !shared.contains(token)) {
            shared.append(token);
        }
    }
    return shared;
}

void removeSharedOpusPlaceholders(QString &sourceText, QString &targetText)
{
    for (const QString &token : sharedOpusPlaceholders(sourceText, targetText)) {
        sourceText.replace(token, QString());
        targetText.replace(token, QString());
    }
    sourceText = cleanOpusMosesText(sourceText);
    targetText = cleanOpusMosesText(targetText);
}

bool isSameProperNameCandidate(const QString &sourceText, const QString &targetText)
{
    if (!isSameNormalizedText(sourceText, targetText)) {
        return false;
    }

    static const QRegularExpression rejectedNameExpression(
        QStringLiteral(R"(^(?:copyright|genericname|name|comment)$)"),
        QRegularExpression::CaseInsensitiveOption);
    if (rejectedNameExpression.match(sourceText.trimmed()).hasMatch()
        || rejectedNameExpression.match(targetText.trimmed()).hasMatch()) {
        return false;
    }

    const QStringList words = sourceText.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (words.isEmpty() || words.size() > 4) {
        return false;
    }

    for (const QString &word : words) {
        if (word.size() < 2 || word.at(0).isLower() || word.contains(QRegularExpression(QStringLiteral(R"(\d|[_/@])")))) {
            return false;
        }
    }
    return true;
}

bool hasSameTextFlag(const QStringList &flags)
{
    return flags.contains(QStringLiteral("texto_igual"));
}

bool hasAcceptedSameTextFlag(const QStringList &flags)
{
    return flags.contains(QStringLiteral("texto_igual_aceito"));
}

bool hasSameProperNameFlag(const QStringList &flags)
{
    return flags.contains(QStringLiteral("nome_proprio_igual"));
}

bool isSuspiciousSameTextCandidate(const QString &text)
{
    const QString trimmed = text.trimmed();
    static const QRegularExpression rejectedKeyExpression(
        QStringLiteral(R"(^(?:copyright|genericname|name|comment)$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression contactExpression(
        QStringLiteral(R"((?:https?|ftp)\s*:\s*/\s*/|www\s*\.|[A-Z0-9._%+\-]+\s*@\s*[A-Z0-9.\-]+\s*\.\s*[A-Z]{2,}|(?:tel|fax)\s*[:.])"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression metadataExpression(
        QStringLiteral(R"((?:copyright|all rights reserved|genericname|comment|translator|author|license|european medicines agency))"),
        QRegularExpression::CaseInsensitiveOption);

    return rejectedKeyExpression.match(trimmed).hasMatch()
        || contactExpression.match(trimmed).hasMatch()
        || metadataExpression.match(trimmed).hasMatch()
        || looksLikeDesktopMetadata(trimmed);
}

bool isAcceptableSameTextCandidate(const QString &sourceText, const QString &targetText)
{
    if (!isSameNormalizedText(sourceText, targetText)) {
        return false;
    }
    const QString text = sourceText.trimmed();
    if (text.isEmpty()
        || isSuspiciousSameTextCandidate(sourceText)
        || isSuspiciousSameTextCandidate(targetText)) {
        return false;
    }

    const qsizetype words = qMax(normalizedWordCount(sourceText), normalizedWordCount(targetText));
    if (words > 3 || text.size() > 40) {
        return false;
    }

    static const QRegularExpression technicalExpression(
        QStringLiteral(R"(^(?:c\+\+|c#|f#|\.net|node\.js|javascript|typescript|python|java|html|css|sql|linux|windows|android|ios)$)"),
        QRegularExpression::CaseInsensitiveOption);
    if (technicalExpression.match(text).hasMatch()) {
        return true;
    }

    bool hasLetterOrNumber = false;
    for (const QChar character : text) {
        if (character.isLetterOrNumber()) {
            hasLetterOrNumber = true;
            break;
        }
    }
    return hasLetterOrNumber;
}

QString opusPreprocessSkipReason(const QString &sourceText,
                                 const QString &targetText,
                                 const QStringList &flags)
{
    const QString combined = QStringLiteral("%1 %2").arg(sourceText, targetText).trimmed();

    static const QRegularExpression spacedUrlExpression(
        QStringLiteral(R"((?:https?|ftp)\s*:\s*(?:/\s*){2}|www\s*\.)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression emailExpression(
        QStringLiteral(R"([A-Z0-9._%+\-]+\s*@\s*[A-Z0-9.\-]+\s*\.\s*[A-Z]{2,})"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression phoneExpression(
        QStringLiteral(R"((?:\b(?:tel|telephone|phone|fax|e-mail|email)\b\s*[:.]?\s*)?(?:\+?\d[\d\s().-]{6,}\d))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression copyrightExpression(
        QStringLiteral(R"((?:©|\(c\)|copyright|all rights reserved|european medicines agency))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression addressExpression(
        QStringLiteral(R"(\b(?:circus|canary\s+wharf|london|avenue|street|road|postcode|zip|fax|tel\.?|e-mail)\b)"),
        QRegularExpression::CaseInsensitiveOption);

    if (sourceText.trimmed().isEmpty() || targetText.trimmed().isEmpty()) {
        return QStringLiteral("empty row");
    }
    if (spacedUrlExpression.match(combined).hasMatch()) {
        return QStringLiteral("contact/url row");
    }
    if (emailExpression.match(combined).hasMatch()) {
        return QStringLiteral("contact/email row");
    }
    if (phoneExpression.match(combined).hasMatch()) {
        return QStringLiteral("contact/phone row");
    }
    if (copyrightExpression.match(combined).hasMatch()) {
        return QStringLiteral("copyright row");
    }
    if (addressExpression.match(combined).hasMatch() && normalizedWordCount(combined) >= 6) {
        return QStringLiteral("address/contact row");
    }
    if (flags.contains(QStringLiteral("possible_metadata"))) {
        return QStringLiteral("metadata row");
    }
    if (hasSameTextFlag(flags)
        && !hasAcceptedSameTextFlag(flags)
        && !hasSameProperNameFlag(flags)) {
        return QStringLiteral("same text row");
    }

    return QString();
}

QStringList opusQualityFlags(const QString &sourceText,
                             const QString &targetText,
                             bool xmlAvailable,
                             const OpusAlignmentLink &alignment)
{
    QStringList flags;
    const QString source = sourceText.trimmed();
    const QString target = targetText.trimmed();

    if (!xmlAvailable) {
        flags.append(QStringLiteral("missing_xml"));
    } else if (!alignment.valid) {
        flags.append(QStringLiteral("missing_alignment"));
    }
    if (source.isEmpty()) {
        flags.append(QStringLiteral("empty_source"));
    }
    if (target.isEmpty()) {
        flags.append(QStringLiteral("empty_target"));
    }
    if (!source.isEmpty() && !target.isEmpty() && isSameNormalizedText(source, target)) {
        flags.append(QStringLiteral("texto_igual"));
        if (isAcceptableSameTextCandidate(source, target)) {
            flags.append(QStringLiteral("texto_igual_aceito"));
        } else if (isSuspiciousSameTextCandidate(source) || isSuspiciousSameTextCandidate(target)) {
            flags.append(QStringLiteral("texto_igual_suspeito"));
            flags.append(QStringLiteral("possible_metadata"));
        } else {
            flags.append(QStringLiteral("texto_igual_longo"));
        }
    }
    if (source.size() > MaximumLineLength) {
        flags.append(QStringLiteral("long_source"));
    }
    if (target.size() > MaximumLineLength) {
        flags.append(QStringLiteral("long_target"));
    }

    static const QRegularExpression translatorMetadataExpression(
        QStringLiteral(R"((?:ROLES_OF_TRANSLATORS|EMAIL_OF_TRANSLATORS|CREDIT_FOR_TRANSLATORS|\b(?:translator|tradutor(?:a|es)?|tradu[cç][aã]o)\s*:|@[A-Za-z0-9_.-]+|\b\d{1,2}[-/]\d{1,2}[-/]\d{2,4}\b))"),
        QRegularExpression::CaseInsensitiveOption);
    if (translatorMetadataExpression.match(source).hasMatch()
        || translatorMetadataExpression.match(target).hasMatch()) {
        flags.append(QStringLiteral("possible_metadata"));
    }

    return flags;
}

OpusPreprocessDecision classifyOpusRow(const QString &sourceText,
                                       const QString &targetText,
                                       bool xmlAvailable,
                                       const OpusAlignmentLink &alignment)
{
    OpusPreprocessDecision decision;
    decision.sourceClean = cleanOpusMosesText(sourceText);
    decision.targetClean = cleanOpusMosesText(targetText);
    removeSharedOpusPlaceholders(decision.sourceClean, decision.targetClean);
    decision.flags = opusQualityFlags(decision.sourceClean, decision.targetClean, xmlAvailable, alignment);

    if (isSameProperNameCandidate(decision.sourceClean, decision.targetClean)
        && !decision.flags.contains(QStringLiteral("nome_proprio_igual"))) {
        decision.flags.append(QStringLiteral("nome_proprio_igual"));
    }

    decision.sourceWords = normalizedWordCount(decision.sourceClean);
    decision.targetWords = normalizedWordCount(decision.targetClean);

    for (const QString &flag : decision.flags) {
        if (flag == QStringLiteral("missing_xml") || flag == QStringLiteral("missing_alignment")) {
            decision.qualityScore -= 5;
        } else if (flag == QStringLiteral("long_source") || flag == QStringLiteral("long_target")) {
            decision.qualityScore -= 10;
        } else if (flag == QStringLiteral("nome_proprio_igual")) {
            decision.qualityScore -= 15;
        } else if (flag == QStringLiteral("texto_igual_aceito")) {
            decision.qualityScore -= 20;
        } else if (flag == QStringLiteral("texto_igual")) {
            decision.qualityScore -= 30;
        } else if (flag == QStringLiteral("texto_igual_longo")) {
            decision.qualityScore -= 35;
        } else if (flag == QStringLiteral("texto_igual_suspeito")) {
            decision.qualityScore -= 45;
        } else if (flag == QStringLiteral("possible_metadata")) {
            decision.qualityScore -= 45;
        } else {
            decision.qualityScore -= 25;
        }
    }
    decision.qualityScore = qBound(0, decision.qualityScore, 100);

    const qsizetype maxWords = qMax(decision.sourceWords, decision.targetWords);
    if (decision.flags.contains(QStringLiteral("empty_source"))
        || decision.flags.contains(QStringLiteral("empty_target"))
        || decision.flags.contains(QStringLiteral("possible_metadata"))
        || (hasSameTextFlag(decision.flags)
            && !hasAcceptedSameTextFlag(decision.flags)
            && !hasSameProperNameFlag(decision.flags))) {
        decision.importBucket = QStringLiteral("reject_review");
    } else if (maxWords <= MaximumPunctuationSplitSegmentWords) {
        decision.importBucket = QStringLiteral("lexical");
    } else if (maxWords <= 16) {
        decision.importBucket = QStringLiteral("phrase");
    } else {
        decision.importBucket = QStringLiteral("context");
    }

    return decision;
}

bool isSameNormalizedText(const QString &sourceText, const QString &targetText)
{
    static const TextNormalizer normalizer;
    const QString normalizedSource = normalizer.normalizeForLookup(sourceText);
    const QString normalizedTarget = normalizer.normalizeForLookup(targetText);
    return !normalizedSource.isEmpty() && normalizedSource == normalizedTarget;
}

QString databaseLanguageToken(const QString &languageCode)
{
    const LanguageNormalizer normalizer;
    const QString normalized = normalizer.normalize(languageCode);
    if (normalized == QStringLiteral("pt_BR")) {
        return QStringLiteral("por");
    }
    if (normalized == QStringLiteral("en")) {
        return QStringLiteral("en");
    }
    if (normalized == QStringLiteral("fr")) {
        return QStringLiteral("fr");
    }
    if (normalized == QStringLiteral("es")) {
        return QStringLiteral("es");
    }

    QString token = normalized.toLower();
    token.replace(QLatin1Char('_'), QLatin1Char('-'));
    return token;
}

int databaseLanguagePriority(const QString &token)
{
    if (token == QStringLiteral("por")) {
        return 0;
    }
    if (token == QStringLiteral("fr")) {
        return 1;
    }
    if (token == QStringLiteral("es")) {
        return 2;
    }
    if (token == QStringLiteral("en")) {
        return 3;
    }
    return 10;
}

QStringList splitTranslationAlternatives(const QString &text)
{
    QString cleaned = text;
    cleaned.remove(QRegularExpression(QStringLiteral(R"(\([^)]*\))")));
    cleaned.remove(QRegularExpression(QStringLiteral(R"(\[[^\]]*\])")));
    cleaned.replace(QRegularExpression(QStringLiteral(R"(\s*(?:;|,)\s*)")), QStringLiteral("\n"));

    QStringList alternatives;
    for (const QString &part : cleaned.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QString alternative = part.trimmed();
        if (!alternative.isEmpty() && !alternatives.contains(alternative)) {
            alternatives.append(alternative);
        }
    }
    return alternatives;
}

bool canSplitSlashAlternatives(const QString &text)
{
    if (!text.contains(QLatin1Char('/'))) {
        return false;
    }

    static const QRegularExpression unsafeSlashExpression(
        QStringLiteral("(?:https?|ftp)://|www\\.|</?\\w+|\\{\\{[^}]*/|[A-Za-z]:[\\\\/]|(?:^|[\\s])(?:/[\\w.-]+)+|/{2,}|\\b\\d{1,4}/\\d{1,2}(?:/\\d{1,4})?\\b|\\b(?:km|m|cm|mm|kg|g|mg|s|ms|h|kb|mb|gb|tb)/(?:h|s|ms|m|cm|mm|kg|g|mg|kb|mb|gb|tb)\\b|\\binput/output\\b"),
        QRegularExpression::CaseInsensitiveOption);
    if (unsafeSlashExpression.match(text).hasMatch()) {
        return false;
    }

    for (qsizetype index = 0; index < text.size(); ++index) {
        if (text.at(index) != QLatin1Char('/')) {
            continue;
        }
        if (index == 0 || (index + 1) >= text.size()) {
            return false;
        }
        const QChar before = text.at(index - 1);
        const QChar after = text.at(index + 1);
        if (before.isSpace() || after.isSpace()) {
            return false;
        }
        if (!before.isLetterOrNumber() || !after.isLetterOrNumber()) {
            return false;
        }
    }

    return true;
}

QStringList slashTranslationAlternatives(const QString &text)
{
    if (!canSplitSlashAlternatives(text)) {
        return QStringList{text};
    }

    QStringList alternatives;
    for (const QString &part : text.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        const QString alternative = part.trimmed();
        if (!alternative.isEmpty() && !alternatives.contains(alternative)) {
            alternatives.append(alternative);
        }
    }

    return alternatives.isEmpty() ? QStringList{text} : alternatives;
}

bool hasStructuralPunctuation(const QString &text)
{
    static const QString structural = QStringLiteral(".,!?;:");
    for (const QChar character : text) {
        if (structural.contains(character)) {
            return true;
        }
    }
    return false;
}

bool hasBalancedWrappers(const QString &text)
{
    int parentheses = 0;
    int brackets = 0;
    bool doubleQuoteOpen = false;
    bool singleQuoteOpen = false;

    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar character = text.at(index);
        if (character == QLatin1Char('(')) {
            ++parentheses;
        } else if (character == QLatin1Char(')')) {
            --parentheses;
        } else if (character == QLatin1Char('[')) {
            ++brackets;
        } else if (character == QLatin1Char(']')) {
            --brackets;
        } else if (character == QLatin1Char('"')) {
            doubleQuoteOpen = !doubleQuoteOpen;
        } else if (character == QLatin1Char('\'')
                   && !(index > 0 && (index + 1) < text.size()
                        && text.at(index - 1).isLetterOrNumber()
                        && text.at(index + 1).isLetterOrNumber())) {
            singleQuoteOpen = !singleQuoteOpen;
        }

        if (parentheses < 0 || brackets < 0) {
            return false;
        }
    }

    return parentheses == 0 && brackets == 0 && !doubleQuoteOpen && !singleQuoteOpen;
}

QString punctuationSignature(const QString &text, const QString &allowedMarks)
{
    QString signature;
    for (const QChar character : text) {
        if (allowedMarks.contains(character)) {
            signature.append(character);
        }
    }
    return signature;
}

QStringList splitByStructuralMarks(const QString &text, const QString &allowedMarks)
{
    QStringList segments;
    QString current;
    for (const QChar character : text) {
        current.append(character);
        if (allowedMarks.contains(character)) {
            const QString segment = current.trimmed();
            if (!segment.isEmpty()) {
                segments.append(segment);
            }
            current.clear();
        }
    }

    const QString trailing = current.trimmed();
    if (!trailing.isEmpty()) {
        segments.append(trailing);
    }
    return segments;
}

qsizetype normalizedWordCount(const QString &text)
{
    static const TextNormalizer normalizer;
    return normalizer.words(text).size();
}

bool isWithinPunctuationSplitWordLimit(const QString &sourceText, const QString &targetText)
{
    return normalizedWordCount(sourceText) <= MaximumPunctuationSplitSegmentWords
        && normalizedWordCount(targetText) <= MaximumPunctuationSplitSegmentWords;
}

PunctuationSplitResult splitByCompatibleStructuralPunctuation(const QString &sourceText,
                                                             const QString &targetText)
{
    PunctuationSplitResult result;
    if (!hasStructuralPunctuation(sourceText) && !hasStructuralPunctuation(targetText)) {
        return result;
    }

    result.shouldReport = true;
    if (!hasBalancedWrappers(sourceText) || !hasBalancedWrappers(targetText)) {
        result.skipReason = QStringLiteral("unbalanced parentheses, brackets, or quotes");
        return result;
    }

    const QString fullMarks = QStringLiteral(".,!?;:");
    const QString sourceFullSignature = punctuationSignature(sourceText, fullMarks);
    const QString targetFullSignature = punctuationSignature(targetText, fullMarks);
    QString marksToUse;

    if (!sourceFullSignature.isEmpty() && sourceFullSignature == targetFullSignature) {
        marksToUse = fullMarks;
    } else {
        const QString periodOnly = QStringLiteral(".");
        const QString sourcePeriodSignature = punctuationSignature(sourceText, periodOnly);
        const QString targetPeriodSignature = punctuationSignature(targetText, periodOnly);
        if (!sourcePeriodSignature.isEmpty() && sourcePeriodSignature == targetPeriodSignature) {
            marksToUse = periodOnly;
        } else {
            result.skipReason = QStringLiteral("punctuation signature mismatch");
            return result;
        }
    }

    const QStringList sourceSegments = splitByStructuralMarks(sourceText, marksToUse);
    const QStringList targetSegments = splitByStructuralMarks(targetText, marksToUse);
    if (sourceSegments.size() < 2 || sourceSegments.size() != targetSegments.size()) {
        result.skipReason = QStringLiteral("not enough aligned punctuation segments");
        return result;
    }

    for (qsizetype index = 0; index < sourceSegments.size(); ++index) {
        if (sourceSegments.at(index).isEmpty() || targetSegments.at(index).isEmpty()) {
            result.segments.clear();
            result.skipReason = QStringLiteral("empty segment after punctuation split");
            return result;
        }
        result.segments.append(qMakePair(sourceSegments.at(index), targetSegments.at(index)));
    }

    result.skipReason.clear();
    return result;
}

QString cleanWikiTranslationText(QString text)
{
    text.replace(QRegularExpression(QStringLiteral(R"(\[\[[^|\]]+\|([^\]]+)\]\])")), QStringLiteral("\\1"));
    text.replace(QRegularExpression(QStringLiteral(R"(\[\[([^\]]+)\]\])")), QStringLiteral("\\1"));
    text.replace(QStringLiteral("[["), QString());
    text.replace(QStringLiteral("]]"), QString());
    text.remove(QRegularExpression(QStringLiteral(R"(\{\{[^}]*$)")));
    text.remove(QRegularExpression(QStringLiteral(R"([\{\}]+$)")));
    text.remove(QRegularExpression(QStringLiteral(R"(<[^>]+>)")));
    text.remove(QRegularExpression(QStringLiteral(R"(&lt;[^&]+&gt;)")));
    return text.trimmed();
}

bool isAdministrativeMediaWikiTitle(const QString &title)
{
    static const QStringList administrativePrefixes = {
        QStringLiteral("Wiktionary:"), QStringLiteral("Wiktionary talk:"),
        QStringLiteral("User:"), QStringLiteral("User talk:"),
        QStringLiteral("Template:"), QStringLiteral("Template talk:"),
        QStringLiteral("Help:"), QStringLiteral("Help talk:"),
        QStringLiteral("Category:"), QStringLiteral("Category talk:"),
        QStringLiteral("File:"), QStringLiteral("File talk:"),
        QStringLiteral("Module:"), QStringLiteral("Module talk:"),
        QStringLiteral("Appendix:"), QStringLiteral("Appendix talk:"),
        QStringLiteral("Rhymes:"), QStringLiteral("Rhymes talk:"),
        QStringLiteral("Thesaurus:"), QStringLiteral("Thesaurus talk:"),
        QStringLiteral("Citations:"), QStringLiteral("Citations talk:"),
        QStringLiteral("Reconstruction:"), QStringLiteral("Reconstruction talk:"),
        QStringLiteral("Topic:"), QStringLiteral("Special:"),
        QStringLiteral("MediaWiki:"), QStringLiteral("Talk:")
    };

    for (const QString &prefix : administrativePrefixes) {
        if (title.startsWith(prefix, Qt::CaseInsensitive)) {
            return true;
        }
    }

    return title.contains(QLatin1Char(':'));
}

bool looksLikeDesktopMetadata(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }

    static const QRegularExpression desktopSectionExpression(
        QStringLiteral(R"(^\s*\[\s*Desktop\s+Entry\s*\]\s*$|\bDesktop\s+Entry\b)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression desktopKeyExpression(
        QStringLiteral(R"(^\s*(?:Type|Version|Name|GenericName|NoDisplay|Comment|Exec|TryExec|Icon|Hidden|OnlyShowIn|NotShowIn|DBusActivatable|Path|Terminal|Actions|MimeType|Categories|Implements|Keywords|StartupNotify|StartupWMClass|URL|X-[A-Za-z0-9_.-]+)(?:\[[A-Za-z0-9_@.-]+\])?\s*=)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression gluedDesktopKeyExpression(
        QStringLiteral(R"((?:GenericName|NoDisplay|StartupNotify|StartupWMClass|Desktop\s+Entry))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression bareDesktopKeyExpression(
        QStringLiteral(R"(^\s*(?:GenericName|NoDisplay|StartupNotify|StartupWMClass|Desktop\s+Entry)\s*$)"),
        QRegularExpression::CaseInsensitiveOption);

    return desktopSectionExpression.match(trimmed).hasMatch()
        || desktopKeyExpression.match(trimmed).hasMatch()
        || bareDesktopKeyExpression.match(trimmed).hasMatch()
        || gluedDesktopKeyExpression.match(trimmed).hasMatch();
}

bool isTextLetterNumberOrMarkCategory(QChar::Category category)
{
    return category == QChar::Letter_Uppercase
        || category == QChar::Letter_Lowercase
        || category == QChar::Letter_Titlecase
        || category == QChar::Letter_Modifier
        || category == QChar::Letter_Other
        || category == QChar::Number_DecimalDigit
        || category == QChar::Number_Letter
        || category == QChar::Number_Other
        || category == QChar::Mark_NonSpacing
        || category == QChar::Mark_SpacingCombining
        || category == QChar::Mark_Enclosing;
}

bool isLetterNumberOrMarkAt(const QString &text, qsizetype index, qsizetype *advance = nullptr)
{
    if (advance != nullptr) {
        *advance = 1;
    }

    const QChar character = text.at(index);
    if (character.isHighSurrogate()
        && (index + 1) < text.size()
        && text.at(index + 1).isLowSurrogate()) {
        if (advance != nullptr) {
            *advance = 2;
        }
        const char32_t codepoint = QChar::surrogateToUcs4(character, text.at(index + 1));
        return isTextLetterNumberOrMarkCategory(QChar::category(codepoint));
    }

    return isTextLetterNumberOrMarkCategory(character.category());
}

bool parseWikiHeading(const QString &line, int &level, QString &heading)
{
    static const QRegularExpression headingExpression(QStringLiteral(R"(^(={2,6})\s*([^=]+?)\s*\1\s*$)"));
    const QRegularExpressionMatch match = headingExpression.match(line.trimmed());
    if (!match.hasMatch()) {
        return false;
    }

    level = match.captured(1).size();
    heading = match.captured(2).trimmed();
    return true;
}

bool isMediaWikiTranslationHeading(int level, const QString &heading)
{
    return level >= 4
        && level <= 5
        && heading.compare(QStringLiteral("Translations"), Qt::CaseInsensitive) == 0;
}

bool isIgnoredMediaWikiSection(const QString &heading)
{
    static const QSet<QString> ignoredSections = {
        QStringLiteral("etymology"), QStringLiteral("pronunciation"),
        QStringLiteral("noun"), QStringLiteral("verb"), QStringLiteral("adjective"), QStringLiteral("adverb"),
        QStringLiteral("alternative forms"), QStringLiteral("derived terms"), QStringLiteral("related terms"),
        QStringLiteral("synonyms"), QStringLiteral("antonyms"), QStringLiteral("hyponyms"), QStringLiteral("hypernyms"),
        QStringLiteral("descendants"), QStringLiteral("declension"), QStringLiteral("conjugation"),
        QStringLiteral("usage notes"), QStringLiteral("references"), QStringLiteral("further reading"),
        QStringLiteral("see also"), QStringLiteral("quotations"), QStringLiteral("examples"), QStringLiteral("anagrams"),
        QStringLiteral("categories"), QStringLiteral("images"), QStringLiteral("audio"), QStringLiteral("ipa"),
        QStringLiteral("rhymes"), QStringLiteral("hyphenation"), QStringLiteral("inflection")
    };

    QString normalizedHeading = heading.toLower().trimmed();
    normalizedHeading.remove(QRegularExpression(QStringLiteral(R"(\s*\d+$)")));
    return ignoredSections.contains(normalizedHeading);
}

bool isIgnoredMediaWikiTranslationControlLine(const QString &line)
{
    static const QRegularExpression ignoredTemplateExpression(QStringLiteral(
        R"(\{\{\s*(?:trans-top|trans-mid|trans-bottom|t-needed|qualifier|q|gloss|lb|syn|ant|desc|desctree|cog|der|bor|inh|m|l|ux|quote-[^|}]+|RQ:[^|}]+|audio|IPA|wp|pedia|see translation subpage|trans-see)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return ignoredTemplateExpression.match(line).hasMatch();
}

bool parseMediaWikiTranslationGlossLine(const QString &line, QString &gloss)
{
    static const QRegularExpression transTopExpression(
        QStringLiteral(R"(\{\{\s*trans-top(?:-also)?\s*(?:\|\s*(?:1\s*=\s*)?([^|}]+))?)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = transTopExpression.match(line);
    if (!match.hasMatch()) {
        return false;
    }

    gloss = cleanWikiTranslationText(match.captured(1)).trimmed();
    return true;
}

QString safeMediaWikiFileToken(QString text)
{
    text.replace(QRegularExpression(QStringLiteral(R"([^A-Za-z0-9._-]+)")), QStringLiteral("_"));
    text = text.trimmed();
    return text.isEmpty() ? QStringLiteral("dataset") : text.left(120);
}

QHash<QString, QString> mediaWikiHeadingLanguageMap()
{
    return {
        {QStringLiteral("Portuguese"), QStringLiteral("pt_BR")},
        {QStringLiteral("English"), QStringLiteral("en")},
        {QStringLiteral("Spanish"), QStringLiteral("es")},
        {QStringLiteral("French"), QStringLiteral("fr")},
        {QStringLiteral("German"), QStringLiteral("de")},
        {QStringLiteral("Italian"), QStringLiteral("it")},
        {QStringLiteral("Dutch"), QStringLiteral("nl")},
        {QStringLiteral("Russian"), QStringLiteral("ru")},
        {QStringLiteral("Ukrainian"), QStringLiteral("uk")},
        {QStringLiteral("Polish"), QStringLiteral("pl")},
        {QStringLiteral("Czech"), QStringLiteral("cs")},
        {QStringLiteral("Swedish"), QStringLiteral("sv")},
        {QStringLiteral("Norwegian"), QStringLiteral("no")},
        {QStringLiteral("Norwegian Bokmål"), QStringLiteral("nb")},
        {QStringLiteral("Norwegian Nynorsk"), QStringLiteral("nn")},
        {QStringLiteral("Danish"), QStringLiteral("da")},
        {QStringLiteral("Finnish"), QStringLiteral("fi")},
        {QStringLiteral("Greek"), QStringLiteral("el")},
        {QStringLiteral("Turkish"), QStringLiteral("tr")},
        {QStringLiteral("Arabic"), QStringLiteral("ar")},
        {QStringLiteral("Hebrew"), QStringLiteral("he")},
        {QStringLiteral("Persian"), QStringLiteral("fa")},
        {QStringLiteral("Hindi"), QStringLiteral("hi")},
        {QStringLiteral("Urdu"), QStringLiteral("ur")},
        {QStringLiteral("Bengali"), QStringLiteral("bn")},
        {QStringLiteral("Punjabi"), QStringLiteral("pa")},
        {QStringLiteral("Panjabi"), QStringLiteral("pa")},
        {QStringLiteral("Tamil"), QStringLiteral("ta")},
        {QStringLiteral("Telugu"), QStringLiteral("te")},
        {QStringLiteral("Marathi"), QStringLiteral("mr")},
        {QStringLiteral("Gujarati"), QStringLiteral("gu")},
        {QStringLiteral("Mandarin"), QStringLiteral("cmn")},
        {QStringLiteral("Chinese"), QStringLiteral("zh")},
        {QStringLiteral("Cantonese"), QStringLiteral("yue")},
        {QStringLiteral("Wu"), QStringLiteral("wuu")},
        {QStringLiteral("Min Nan"), QStringLiteral("nan")},
        {QStringLiteral("Hakka"), QStringLiteral("hak")},
        {QStringLiteral("Gan"), QStringLiteral("gan")},
        {QStringLiteral("Xiang"), QStringLiteral("hsn")},
        {QStringLiteral("Japanese"), QStringLiteral("ja")},
        {QStringLiteral("Korean"), QStringLiteral("ko")},
        {QStringLiteral("Vietnamese"), QStringLiteral("vi")},
        {QStringLiteral("Thai"), QStringLiteral("th")},
        {QStringLiteral("Indonesian"), QStringLiteral("id")},
        {QStringLiteral("Malay"), QStringLiteral("ms")},
        {QStringLiteral("Tagalog"), QStringLiteral("tl")},
        {QStringLiteral("Filipino"), QStringLiteral("fil")},
        {QStringLiteral("Swahili"), QStringLiteral("sw")},
        {QStringLiteral("Afrikaans"), QStringLiteral("af")},
        {QStringLiteral("Zulu"), QStringLiteral("zu")},
        {QStringLiteral("Xhosa"), QStringLiteral("xh")},
        {QStringLiteral("Yoruba"), QStringLiteral("yo")},
        {QStringLiteral("Hausa"), QStringLiteral("ha")},
        {QStringLiteral("Amharic"), QStringLiteral("am")},
        {QStringLiteral("Pashto"), QStringLiteral("ps")},
        {QStringLiteral("Lahnda"), QStringLiteral("lah")}
    };
}

QHash<QString, QString> mediaWikiTemplateLanguageMap()
{
    return {
        {QStringLiteral("pt"), QStringLiteral("pt_BR")},
        {QStringLiteral("por"), QStringLiteral("pt_BR")},
        {QStringLiteral("en"), QStringLiteral("en")},
        {QStringLiteral("eng"), QStringLiteral("en")},
        {QStringLiteral("es"), QStringLiteral("es")},
        {QStringLiteral("spa"), QStringLiteral("es")},
        {QStringLiteral("fr"), QStringLiteral("fr")},
        {QStringLiteral("fra"), QStringLiteral("fr")},
        {QStringLiteral("fre"), QStringLiteral("fr")},
        {QStringLiteral("de"), QStringLiteral("de")},
        {QStringLiteral("deu"), QStringLiteral("de")},
        {QStringLiteral("ger"), QStringLiteral("de")},
        {QStringLiteral("it"), QStringLiteral("it")},
        {QStringLiteral("ita"), QStringLiteral("it")},
        {QStringLiteral("nl"), QStringLiteral("nl")},
        {QStringLiteral("nld"), QStringLiteral("nl")},
        {QStringLiteral("dut"), QStringLiteral("nl")},
        {QStringLiteral("ru"), QStringLiteral("ru")},
        {QStringLiteral("rus"), QStringLiteral("ru")},
        {QStringLiteral("uk"), QStringLiteral("uk")},
        {QStringLiteral("ukr"), QStringLiteral("uk")},
        {QStringLiteral("pl"), QStringLiteral("pl")},
        {QStringLiteral("pol"), QStringLiteral("pl")},
        {QStringLiteral("cs"), QStringLiteral("cs")},
        {QStringLiteral("ces"), QStringLiteral("cs")},
        {QStringLiteral("cze"), QStringLiteral("cs")},
        {QStringLiteral("sv"), QStringLiteral("sv")},
        {QStringLiteral("swe"), QStringLiteral("sv")},
        {QStringLiteral("no"), QStringLiteral("no")},
        {QStringLiteral("nor"), QStringLiteral("no")},
        {QStringLiteral("nb"), QStringLiteral("nb")},
        {QStringLiteral("nn"), QStringLiteral("nn")},
        {QStringLiteral("da"), QStringLiteral("da")},
        {QStringLiteral("dan"), QStringLiteral("da")},
        {QStringLiteral("fi"), QStringLiteral("fi")},
        {QStringLiteral("fin"), QStringLiteral("fi")},
        {QStringLiteral("el"), QStringLiteral("el")},
        {QStringLiteral("grc"), QStringLiteral("grc")},
        {QStringLiteral("tr"), QStringLiteral("tr")},
        {QStringLiteral("tur"), QStringLiteral("tr")},
        {QStringLiteral("ar"), QStringLiteral("ar")},
        {QStringLiteral("ara"), QStringLiteral("ar")},
        {QStringLiteral("he"), QStringLiteral("he")},
        {QStringLiteral("heb"), QStringLiteral("he")},
        {QStringLiteral("fa"), QStringLiteral("fa")},
        {QStringLiteral("per"), QStringLiteral("fa")},
        {QStringLiteral("fas"), QStringLiteral("fa")},
        {QStringLiteral("hi"), QStringLiteral("hi")},
        {QStringLiteral("hin"), QStringLiteral("hi")},
        {QStringLiteral("ur"), QStringLiteral("ur")},
        {QStringLiteral("urd"), QStringLiteral("ur")},
        {QStringLiteral("bn"), QStringLiteral("bn")},
        {QStringLiteral("ben"), QStringLiteral("bn")},
        {QStringLiteral("pa"), QStringLiteral("pa")},
        {QStringLiteral("pan"), QStringLiteral("pa")},
        {QStringLiteral("ta"), QStringLiteral("ta")},
        {QStringLiteral("tam"), QStringLiteral("ta")},
        {QStringLiteral("te"), QStringLiteral("te")},
        {QStringLiteral("tel"), QStringLiteral("te")},
        {QStringLiteral("mr"), QStringLiteral("mr")},
        {QStringLiteral("mar"), QStringLiteral("mr")},
        {QStringLiteral("gu"), QStringLiteral("gu")},
        {QStringLiteral("guj"), QStringLiteral("gu")},
        {QStringLiteral("zh"), QStringLiteral("zh")},
        {QStringLiteral("cmn"), QStringLiteral("cmn")},
        {QStringLiteral("yue"), QStringLiteral("yue")},
        {QStringLiteral("wuu"), QStringLiteral("wuu")},
        {QStringLiteral("nan"), QStringLiteral("nan")},
        {QStringLiteral("hak"), QStringLiteral("hak")},
        {QStringLiteral("gan"), QStringLiteral("gan")},
        {QStringLiteral("hsn"), QStringLiteral("hsn")},
        {QStringLiteral("ja"), QStringLiteral("ja")},
        {QStringLiteral("jpn"), QStringLiteral("ja")},
        {QStringLiteral("ko"), QStringLiteral("ko")},
        {QStringLiteral("kor"), QStringLiteral("ko")},
        {QStringLiteral("vi"), QStringLiteral("vi")},
        {QStringLiteral("vie"), QStringLiteral("vi")},
        {QStringLiteral("th"), QStringLiteral("th")},
        {QStringLiteral("tha"), QStringLiteral("th")},
        {QStringLiteral("id"), QStringLiteral("id")},
        {QStringLiteral("ind"), QStringLiteral("id")},
        {QStringLiteral("ms"), QStringLiteral("ms")},
        {QStringLiteral("msa"), QStringLiteral("ms")},
        {QStringLiteral("tl"), QStringLiteral("tl")},
        {QStringLiteral("tgl"), QStringLiteral("tl")},
        {QStringLiteral("fil"), QStringLiteral("fil")},
        {QStringLiteral("sw"), QStringLiteral("sw")},
        {QStringLiteral("swa"), QStringLiteral("sw")},
        {QStringLiteral("af"), QStringLiteral("af")},
        {QStringLiteral("afr"), QStringLiteral("af")},
        {QStringLiteral("zu"), QStringLiteral("zu")},
        {QStringLiteral("zul"), QStringLiteral("zu")},
        {QStringLiteral("xh"), QStringLiteral("xh")},
        {QStringLiteral("xho"), QStringLiteral("xh")},
        {QStringLiteral("yo"), QStringLiteral("yo")},
        {QStringLiteral("yor"), QStringLiteral("yo")},
        {QStringLiteral("ha"), QStringLiteral("ha")},
        {QStringLiteral("hau"), QStringLiteral("ha")},
        {QStringLiteral("am"), QStringLiteral("am")},
        {QStringLiteral("amh"), QStringLiteral("am")},
        {QStringLiteral("ps"), QStringLiteral("ps")},
        {QStringLiteral("pus"), QStringLiteral("ps")},
        {QStringLiteral("lah"), QStringLiteral("lah")}
    };
}

QString fallbackMediaWikiLanguageToken(QString text)
{
    text = text.trimmed().toLower();
    text.replace(QRegularExpression(QStringLiteral(R"([\s-]+)")), QStringLiteral("_"));
    text.remove(QRegularExpression(QStringLiteral(R"([^a-z0-9_])")));
    text.replace(QRegularExpression(QStringLiteral(R"(_+)")), QStringLiteral("_"));
    text = text.trimmed();
    while (text.startsWith(QLatin1Char('_'))) {
        text.remove(0, 1);
    }
    while (text.endsWith(QLatin1Char('_'))) {
        text.chop(1);
    }

    if (text.isEmpty() || text.at(0).isDigit()) {
        return QString();
    }
    return text.left(48);
}

QString mediaWikiHeadingLanguageCode(const QString &heading)
{
    const LanguageNormalizer normalizer;
    static const QHash<QString, QString> knownHeadings = mediaWikiHeadingLanguageMap();
    const QString knownCode = knownHeadings.value(heading.trimmed());
    if (!knownCode.isEmpty()) {
        const QString normalizedCode = normalizer.normalize(knownCode);
        return normalizer.isSupported(normalizedCode) ? normalizedCode : QString();
    }

    const QString fallbackCode = normalizer.normalize(fallbackMediaWikiLanguageToken(heading));
    return normalizer.isSupported(fallbackCode) ? fallbackCode : QString();
}

QString mediaWikiTemplateLanguageCode(const QString &languageCode)
{
    const LanguageNormalizer normalizer;
    static const QHash<QString, QString> knownCodes = mediaWikiTemplateLanguageMap();
    const QString rawCode = languageCode.trimmed().toLower();
    const QString knownCode = knownCodes.value(rawCode);
    if (!knownCode.isEmpty()) {
        const QString normalizedCode = normalizer.normalize(knownCode);
        return normalizer.isSupported(normalizedCode) ? normalizedCode : QString();
    }

    static const QRegularExpression plausibleCodeExpression(
        QStringLiteral(R"(^[a-z][a-z0-9]{1,15}(?:-[a-z0-9]{1,12}){0,3}$)"));
    if (!plausibleCodeExpression.match(rawCode).hasMatch()) {
        return QString();
    }

    const QString normalizedCode = normalizer.normalize(rawCode);
    return normalizer.isSupported(normalizedCode) ? normalizedCode : QString();
}
}

AtlasImporter::AtlasImporter(const QString &databasePath)
    : m_databasePath(databasePath),
      m_connectionName(QStringLiteral("atlas_importer_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
    if (m_databasePath.isEmpty()) {
        m_databasePath = QStringLiteral(":memory:");
    }

    if (m_databasePath != QStringLiteral(":memory:")) {
        m_databasePath = QDir::cleanPath(QFileInfo(m_databasePath).absoluteFilePath());
    }
}

AtlasImporter::~AtlasImporter()
{
    if (m_database.isValid()) {
        m_database.close();
    }

    m_database = QSqlDatabase();

    if (!m_connectionName.isEmpty()) {
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

QString AtlasImporter::databasePathForLanguagePair(const QString &sourceLang, const QString &targetLang)
{
    QString first = databaseLanguageToken(sourceLang);
    QString second = databaseLanguageToken(targetLang);

    if (databaseLanguagePriority(second) < databaseLanguagePriority(first)
        || (databaseLanguagePriority(second) == databaseLanguagePriority(first) && second < first)) {
        std::swap(first, second);
    }

    const QString fileName = QStringLiteral("Atlas_%1-%2_%2-%1.db").arg(first, second);
    return QDir::cleanPath(QDir(AppPaths::databasePath()).filePath(fileName));
}

bool AtlasImporter::importMosesDataset(const QString &sourceFilePath,
                                       const QString &targetFilePath,
                                       const QString &sourceLang,
                                       const QString &targetLang)
{
    m_stats = ImportStats();
    m_cleanupStats = CleanupStats();
    m_lastError.clear();
    m_currentImportSourceFile = sourceFilePath;
    m_rejectedDetailsBuffer.clear();
    m_bufferedPairs = 0;
    m_bufferSourceTexts.clear();
    m_bufferTranslatedTexts.clear();
    m_bufferSourceLangs.clear();
    m_bufferTargetLangs.clear();
    m_bufferSenseGlosses.clear();
    m_rejectedExamples.clear();
    m_lastRejectedSummarySnapshotProcessed = -1;
    QElapsedTimer totalTimer;
    totalTimer.start();
    const QString startedAt = AtlasReport::timestamp();

    const QString normalizedSourceLang = m_languageNormalizer.normalize(sourceLang);
    const QString normalizedTargetLang = m_languageNormalizer.normalize(targetLang);
    if (!m_languageNormalizer.isSupported(normalizedSourceLang)
        || !m_languageNormalizer.isSupported(normalizedTargetLang)) {
        m_lastError = QStringLiteral("Par de idiomas fora da lista suportada e ignorado: %1 -> %2")
                          .arg(sourceLang, targetLang);
        return false;
    }
    const QString currentImportKey = importKey(sourceFilePath, targetFilePath, normalizedSourceLang, normalizedTargetLang);

    QFile sourceFile(sourceFilePath);
    QFile targetFile(targetFilePath);

    if (!sourceFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = QStringLiteral("Nao foi possivel abrir arquivo de origem: %1").arg(sourceFilePath);
        return false;
    }

    if (!targetFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = QStringLiteral("Nao foi possivel abrir arquivo de destino: %1").arg(targetFilePath);
        return false;
    }

    if (!openDatabase() || !ensureSchema() || !ensureImportBufferTable()) {
        return false;
    }

    QTextStream sourceStream(&sourceFile);
    QTextStream targetStream(&targetFile);
    configureUtf8Stream(sourceStream);
    configureUtf8Stream(targetStream);

    QTextStream console(stdout);
    configureUtf8Stream(console);
    console << "Importando..." << Qt::endl;
    console << "Caminho do banco: " << m_databasePath << Qt::endl;
    console << "SQLite aberto: " << (m_database.isOpen() ? QStringLiteral("sim") : QStringLiteral("nao")) << Qt::endl;
    console << "Idiomas: " << normalizedSourceLang << " -> " << normalizedTargetLang << Qt::endl;

    qint64 resumeFromLine = 0;
    bool completed = false;
    if (!loadProgress(currentImportKey, resumeFromLine, completed)) {
        return false;
    }

    if (completed) {
        console << "Dataset ja importado anteriormente. Pulando reprocessamento." << Qt::endl;
        m_stats.totalTimeNs = totalTimer.nsecsElapsed();
        writeRejectedDetailsSummary(sourceFilePath, normalizedSourceLang, normalizedTargetLang, startedAt);
        writeImportReport(QStringLiteral("OPUS"), sourceFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, true);
        return true;
    }

    if (resumeFromLine > 0) {
        console << "Retomando importacao a partir da linha: " << resumeFromLine << Qt::endl;
        if (!skipLines(sourceStream, targetStream, resumeFromLine)) {
            return false;
        }
        m_stats.resumedLines = resumeFromLine;
        m_stats.processedLines = resumeFromLine;
    }

    maybeWriteRejectedDetailsSummary(sourceFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, true);

    QSqlQuery insertQuery(m_database);
    QSqlQuery updateQuery(m_database);
    if (!prepareInsertStatement(insertQuery) || !prepareUpdateFrequencyStatement(updateQuery)) {
        return false;
    }

    if (!m_database.transaction()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    bool success = true;
    qint64 linesSinceCommit = 0;
    qint64 lastProgressPrinted = 0;
    while (!sourceStream.atEnd() && !targetStream.atEnd()) {
        QElapsedTimer parsingTimer;
        parsingTimer.start();
        const QString sourceText = cleanText(sourceStream.readLine());
        const QString targetText = cleanText(targetStream.readLine());
        m_stats.parsingTimeNs += parsingTimer.nsecsElapsed();
        ++m_stats.processedLines;
        ++linesSinceCommit;

        QElapsedTimer sqliteTimer;
        sqliteTimer.start();
        if (!insertPreparedPair(insertQuery,
                                updateQuery,
                                sourceText,
                                targetText,
                                normalizedSourceLang,
                                normalizedTargetLang)) {
            m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
            if (!m_lastError.isEmpty()) {
                success = false;
                break;
            }
        }
        m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();

        if ((m_stats.processedLines % ProgressInterval) == 0
            && m_stats.processedLines != lastProgressPrinted) {
            printProgress();
            maybeWriteRejectedDetailsSummary(sourceFilePath, normalizedSourceLang, normalizedTargetLang, startedAt);
            lastProgressPrinted = m_stats.processedLines;
        }

        if (linesSinceCommit >= CommitInterval) {
            QElapsedTimer sqliteTimer;
            sqliteTimer.start();
            if (!mergeBufferedPairs()) {
                m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
                success = false;
                break;
            }
            m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
            if (!saveProgress(currentImportKey,
                              sourceFilePath,
                              targetFilePath,
                              normalizedSourceLang,
                              normalizedTargetLang,
                              m_stats.processedLines,
                              false)) {
                success = false;
                break;
            }

            if (!m_database.commit()) {
                m_lastError = m_database.lastError().text();
                success = false;
                break;
            }

            if (!m_database.transaction()) {
                m_lastError = m_database.lastError().text();
                success = false;
                break;
            }
            linesSinceCommit = 0;
        }
    }

    if (success && (sourceStream.atEnd() != targetStream.atEnd())) {
        m_lastError = QStringLiteral("Dataset desalinhado: os arquivos tem quantidades de linhas diferentes.");
        success = false;
    }

    if (success) {
        QElapsedTimer sqliteTimer;
        sqliteTimer.start();
        success = mergeBufferedPairs();
        m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
    }

    if (success) {
        success = saveProgress(currentImportKey,
                               sourceFilePath,
                               targetFilePath,
                               normalizedSourceLang,
                               normalizedTargetLang,
                               m_stats.processedLines,
                               true);
    }

    if (success && !m_database.commit()) {
        m_lastError = m_database.lastError().text();
        success = false;
    } else if (!success) {
        m_database.rollback();
    }

    QElapsedTimer indexTimer;
    indexTimer.start();
    if (success && !ensureIndexes()) {
        m_stats.indexTimeNs += indexTimer.nsecsElapsed();
        return false;
    }
    m_stats.indexTimeNs += indexTimer.nsecsElapsed();

    QElapsedTimer cleanupTimer;
    cleanupTimer.start();
    if (success && !optimizeDatabase(false)) {
        m_stats.cleanupTimeNs += cleanupTimer.nsecsElapsed();
        return false;
    }
    m_stats.cleanupTimeNs += cleanupTimer.nsecsElapsed();
    m_cleanupStats.finalTranslationsCount = translationCount();

    printProgress();
    console << "Resumo final:" << Qt::endl;
    console << "Linhas retomadas: " << m_stats.resumedLines << Qt::endl;
    console << "Linhas processadas: " << m_stats.processedLines << Qt::endl;
    console << "Novos pares: " << m_stats.insertedLines << Qt::endl;
    console << "Frequencias atualizadas: " << m_stats.updatedFrequencyLines << Qt::endl;
    console << "Ignorados: " << m_stats.ignoredLines << Qt::endl;
    console << "Duplicados: " << m_stats.duplicateLines << Qt::endl;
    console << "Avisos de qualidade registrados: " << m_stats.qualityWarningsLogged << Qt::endl;
    console << "Manutencao do banco concluida" << Qt::endl;
    console << "Total final de traducoes: " << m_cleanupStats.finalTranslationsCount << Qt::endl;

    m_stats.totalTimeNs = totalTimer.nsecsElapsed();
    flushRejectedDetails();
    maybeWriteRejectedDetailsSummary(sourceFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, true);
    writeImportReport(QStringLiteral("OPUS"), sourceFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, success);
    return success;
}

bool AtlasImporter::importMosesDatasetBidirectional(const QString &sourceFilePath,
                                                    const QString &targetFilePath,
                                                    const QString &sourceLang,
                                                    const QString &targetLang)
{
    if (!importMosesDataset(sourceFilePath, targetFilePath, sourceLang, targetLang)) {
        return false;
    }

    return importMosesDataset(targetFilePath, sourceFilePath, targetLang, sourceLang);
}

bool AtlasImporter::preprocessMosesDataset(const QString &sourceFilePath,
                                           const QString &targetFilePath,
                                           const QString &sourceLang,
                                           const QString &targetLang,
                                           const QString &corpusName,
                                           const QString &outputFilePath,
                                           QTextStream *progress)
{
    m_lastError.clear();
    QElapsedTimer totalTimer;
    totalTimer.start();
    const QString startedAt = AtlasReport::timestamp();

    QFile sourceFile(sourceFilePath);
    QFile targetFile(targetFilePath);
    if (!sourceFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = QStringLiteral("Nao foi possivel abrir arquivo OPUS de origem: %1").arg(sourceFilePath);
        return false;
    }
    if (!targetFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = QStringLiteral("Nao foi possivel abrir arquivo OPUS de destino: %1").arg(targetFilePath);
        return false;
    }
    const QString idsFilePath = QFileInfo(sourceFilePath).dir().filePath(
        QStringLiteral("%1.ids").arg(QFileInfo(sourceFilePath).completeBaseName()));
    QFile idsFile(idsFilePath);
    const bool idsAvailable = idsFile.exists() && idsFile.open(QIODevice::ReadOnly | QIODevice::Text);

    const QFileInfo outputInfo(outputFilePath);
    if (!QDir().mkpath(outputInfo.dir().absolutePath())) {
        m_lastError = QStringLiteral("Nao foi possivel criar diretorio de saida OPUS pre-processada: %1").arg(outputInfo.dir().absolutePath());
        return false;
    }

    const QString normalizedSourceLang = m_languageNormalizer.normalize(sourceLang);
    const QString normalizedTargetLang = m_languageNormalizer.normalize(targetLang);
    if (!m_languageNormalizer.isSupported(normalizedSourceLang)
        || !m_languageNormalizer.isSupported(normalizedTargetLang)) {
        m_lastError = QStringLiteral("Par de idiomas OPUS fora da lista suportada e ignorado: %1 -> %2")
                          .arg(sourceLang, targetLang);
        return false;
    }
    const QString xmlFilePath = QFileInfo(sourceFilePath).dir().filePath(
        QStringLiteral("%1.xml").arg(QFileInfo(sourceFilePath).completeBaseName()));
    OpusAlignmentReader alignmentReader(xmlFilePath);
    const bool xmlAvailable = alignmentReader.isAvailable();
    const QString progressFilePath = hiddenPreprocessProgressPath(outputFilePath, QFileInfo(outputFilePath).fileName());

    qint64 resumeLines = 0;
    qint64 resumeRows = 0;
    bool resumeCompleted = false;
    const bool hasProgress = readOpusPreprocessProgress(progressFilePath, resumeLines, resumeRows, resumeCompleted);
    const bool canResume = hasProgress
        && resumeLines > 0
        && QFileInfo::exists(outputFilePath);

    if (resumeCompleted && QFileInfo::exists(outputFilePath)) {
        if (progress != nullptr) {
            *progress << QStringLiteral("[OPUS] ja concluido: %1").arg(terminalShortPath(outputFilePath)) << Qt::endl;
        }
        writePreprocessReport(QStringLiteral("OPUS/Moses"),
                              sourceFilePath,
                              targetFilePath,
                              normalizedSourceLang,
                              normalizedTargetLang,
                              QFileInfo(outputFilePath).absoluteFilePath(),
                              startedAt,
                              true,
                              QStringLiteral("Pulado porque o progresso existente esta concluido.\nArquivo de progresso: %1\nLinhas ja processadas: %2\nLinhas ja gravadas: %3")
                                  .arg(QFileInfo(progressFilePath).absoluteFilePath())
                                  .arg(resumeLines)
                                  .arg(resumeRows));
        return true;
    }

    const bool resumeMode = canResume && !resumeCompleted;

    QFile output(outputFilePath);
    if (!output.open((resumeMode ? QIODevice::Append : QIODevice::Truncate)
                     | QIODevice::WriteOnly | QIODevice::Text)) {
        m_lastError = QStringLiteral("Nao foi possivel criar arquivo OPUS pre-processado: %1").arg(outputFilePath);
        return false;
    }

    QTextStream sourceStream(&sourceFile);
    QTextStream targetStream(&targetFile);
    QTextStream idsStream(&idsFile);
    QTextStream writer(&output);
    configureUtf8Stream(sourceStream);
    configureUtf8Stream(targetStream);
    if (idsAvailable) {
        configureUtf8Stream(idsStream);
    }
    if (resumeMode) {
        configureUtf8Stream(writer);
    } else {
        configureUtf8OutputStreamWithBom(writer);
    }

    QElapsedTimer terminalTimer;
    terminalTimer.start();
    auto print = [&](const QString &message) {
        if (progress != nullptr) {
            *progress << message << terminalElapsedSuffix(terminalTimer) << Qt::endl;
        }
    };

    print(QStringLiteral("[OPUS] %1 XML: %2 IDS: %3")
              .arg(corpusName,
                   xmlAvailable ? QStringLiteral("sim") : QStringLiteral("nao"),
                   idsAvailable ? QStringLiteral("sim") : QStringLiteral("nao")));
    print(QStringLiteral("[OPUS] saida: %1").arg(terminalShortPath(outputFilePath)));

    if (resumeMode) {
        print(QStringLiteral("[OPUS] retomando da linha %1").arg(terminalCount(resumeLines)));
        if (!skipOpusLines(sourceStream, resumeLines) || !skipOpusLines(targetStream, resumeLines)) {
            m_lastError = QStringLiteral("Nao foi possivel retomar OPUS; origem/destino terminou antes do checkpoint.");
            return false;
        }
        if (idsAvailable && !skipOpusLines(idsStream, resumeLines)) {
            m_lastError = QStringLiteral("Nao foi possivel retomar OPUS; IDS terminou antes do checkpoint.");
            return false;
        }
        if (xmlAvailable) {
            OpusAlignmentLink ignoredAlignment;
            for (qint64 index = 0; index < resumeLines; ++index) {
                if (!alignmentReader.next(ignoredAlignment)) {
                    m_lastError = QStringLiteral("Nao foi possivel retomar OPUS; alinhamento XML terminou antes do checkpoint.");
                    return false;
                }
            }
        }
    }

    if (!resumeMode) {
        writer << "bucket_importacao\tpontuacao_qualidade\torigem_limpa\tdestino_limpo" << Qt::endl;
    }

    qint64 linesRead = resumeMode ? resumeLines : 0;
    qint64 rowsWritten = resumeMode ? resumeRows : 0;
    qint64 sourceOnlyLines = 0;
    qint64 targetOnlyLines = 0;
    qint64 missingAlignmentLines = 0;
    qint64 idsRowsRead = 0;
    qint64 missingIdsRows = 0;
    qint64 idsMismatchRows = 0;
    QHash<QString, qint64> qualityCounts;
    QHash<QString, qint64> bucketCounts;
    QHash<QString, qint64> skippedPreprocessCounts;
    QHash<QString, OpusPreprocessRejectExample> skippedPreprocessExamples;
    QSet<QString> writtenCleanPairs;
    qint64 duplicateCleanRows = 0;
    qint64 qualityScoreTotal = 0;
    qint64 qualityScoreRows = 0;

    auto seedWrittenPairsFromExistingOutput = [&]() {
        if (!resumeMode) {
            return;
        }
        QFile existingOutput(outputFilePath);
        if (!existingOutput.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return;
        }
        QTextStream existingStream(&existingOutput);
        configureUtf8Stream(existingStream);
        while (!existingStream.atEnd()) {
            QString line = existingStream.readLine();
            if (line.startsWith(QChar(0xFEFF))) {
                line.remove(0, 1);
            }
            if (line.isEmpty()
                || line.startsWith(QLatin1Char('#'))
                || line.startsWith(QStringLiteral("bucket_importacao\tpontuacao_qualidade\t"))) {
                continue;
            }
            const QStringList columns = line.split(QLatin1Char('\t'));
            if (columns.size() == 4) {
                writtenCleanPairs.insert(QStringLiteral("%1\t%2\t%3\t%4")
                                             .arg(normalizedSourceLang,
                                                  normalizedTargetLang,
                                                  columns.value(2),
                                                  columns.value(3)));
            }
        }
    };

    auto writeDetailedPreprocessReport = [&](bool finalSnapshot, qint64 extraIdsRowsSnapshot) {
        QStringList currentSkipReasons = skippedPreprocessCounts.keys();
        std::sort(currentSkipReasons.begin(), currentSkipReasons.end());

        QFile detailedReport(outputInfo.dir().filePath(QStringLiteral("OPUS_Preprocess_Detalhado_Report.txt")));
        if (!detailedReport.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
            return;
        }

        QTextStream detailedStream(&detailedReport);
        configureUtf8Stream(detailedStream);
        detailedStream << "[OPUS PRE-PROCESSAMENTO DETALHADO - "
                       << (finalSnapshot ? QStringLiteral("FINAL") : QStringLiteral("PARCIAL"))
                       << "]" << Qt::endl;
        detailedStream << "Corpus: " << corpusName << Qt::endl;
        detailedStream << "Arquivo de origem: " << QFileInfo(sourceFilePath).absoluteFilePath() << Qt::endl;
        detailedStream << "Arquivo de destino: " << QFileInfo(targetFilePath).absoluteFilePath() << Qt::endl;
        detailedStream << "XML auxiliar original usado: " << (xmlAvailable ? QFileInfo(xmlFilePath).absoluteFilePath() : QStringLiteral("nao encontrado")) << Qt::endl;
        detailedStream << "IDS auxiliar original usado: " << (idsAvailable ? QFileInfo(idsFilePath).absoluteFilePath() : QStringLiteral("nao encontrado")) << Qt::endl;
        detailedStream << "TSV gerado: " << QFileInfo(outputFilePath).absoluteFilePath() << Qt::endl;
        detailedStream << "Progresso: " << QFileInfo(progressFilePath).absoluteFilePath() << Qt::endl;
        detailedStream << "Par: " << normalizedSourceLang << " -> " << normalizedTargetLang << Qt::endl;
        detailedStream << "Linhas lidas ate agora: " << linesRead << Qt::endl;
        detailedStream << "Linhas gravadas ate agora: " << rowsWritten << Qt::endl;
        detailedStream << "Duplicados limpos ignorados: " << duplicateCleanRows << Qt::endl;
        detailedStream << "Linhas somente na origem: " << sourceOnlyLines << Qt::endl;
        detailedStream << "Linhas somente no destino: " << targetOnlyLines << Qt::endl;
        detailedStream << "Linhas sem alinhamento: " << missingAlignmentLines << Qt::endl;
        detailedStream << "Linhas IDS lidas: " << idsRowsRead << Qt::endl;
        detailedStream << "Linhas IDS ausentes: " << missingIdsRows << Qt::endl;
        detailedStream << "Linhas IDS extras: " << extraIdsRowsSnapshot << Qt::endl;
        detailedStream << "Linhas com divergencia IDS/XML: " << idsMismatchRows << Qt::endl;
        detailedStream << Qt::endl << "Exemplo por motivo que desqualificou traducao:" << Qt::endl;
        if (currentSkipReasons.isEmpty()) {
            detailedStream << "* nenhum motivo registrado ate agora" << Qt::endl;
        } else {
            for (const QString &reason : currentSkipReasons) {
                const OpusPreprocessRejectExample example = skippedPreprocessExamples.value(reason);
                detailedStream << "------------------------------------------------------------" << Qt::endl;
                detailedStream << "Motivo: " << discardReasonLabel(reason) << Qt::endl;
                detailedStream << "Motivo interno: " << reason << Qt::endl;
                detailedStream << "Quantidade no arquivo: " << skippedPreprocessCounts.value(reason) << Qt::endl;
                detailedStream << "Linha original aproximada: " << example.line << Qt::endl;
                detailedStream << "Source bruto: " << example.sourceText << Qt::endl;
                detailedStream << "Target bruto: " << example.targetText << Qt::endl;
                detailedStream << "Source limpo: " << example.sourceClean << Qt::endl;
                detailedStream << "Target limpo: " << example.targetClean << Qt::endl;
                detailedStream << "Flags: " << example.flags << Qt::endl;
                detailedStream << "Bucket: " << example.bucket << Qt::endl;
                detailedStream << "Pontuacao de qualidade: " << example.qualityScore << Qt::endl;
                detailedStream << "XML fromDoc: " << example.xmlFromDoc << Qt::endl;
                detailedStream << "XML toDoc: " << example.xmlToDoc << Qt::endl;
                detailedStream << "XML alignment: " << example.xmlAlignment << Qt::endl;
                detailedStream << "IDS bruto: " << example.idsRaw << Qt::endl;
                detailedStream << "IDS fromDoc: " << example.idsFromDoc << Qt::endl;
                detailedStream << "IDS toDoc: " << example.idsToDoc << Qt::endl;
                detailedStream << "IDS sourceId: " << example.idsSourceId << Qt::endl;
                detailedStream << "IDS targetId: " << example.idsTargetId << Qt::endl;
            }
        }
        detailedStream << Qt::endl;
    };

    seedWrittenPairsFromExistingOutput();

    while (!sourceStream.atEnd() || !targetStream.atEnd()) {
        const bool hasSourceLine = !sourceStream.atEnd();
        const bool hasTargetLine = !targetStream.atEnd();
        const QString sourceText = hasSourceLine ? sourceStream.readLine() : QString();
        const QString targetText = hasTargetLine ? targetStream.readLine() : QString();
        ++linesRead;

        if (!hasSourceLine) {
            ++targetOnlyLines;
        }
        if (!hasTargetLine) {
            ++sourceOnlyLines;
        }

        OpusAlignmentLink alignment;
        if (xmlAvailable && !alignmentReader.next(alignment)) {
            ++missingAlignmentLines;
        }

        OpusIdsRow idsRow;
        if (idsAvailable) {
            if (!idsStream.atEnd()) {
                idsRow = parseOpusIdsRow(idsStream.readLine());
                ++idsRowsRead;
            } else {
                ++missingIdsRows;
            }
        }

        OpusPreprocessDecision decision = classifyOpusRow(sourceText, targetText, xmlAvailable, alignment);
        if (!hasSourceLine) {
            decision.flags.append(QStringLiteral("target_without_source_line"));
            decision.importBucket = QStringLiteral("reject_review");
        }
        if (!hasTargetLine) {
            decision.flags.append(QStringLiteral("source_without_target_line"));
            decision.importBucket = QStringLiteral("reject_review");
        }
        if (idsAvailable && !idsRow.valid) {
            decision.flags.append(QStringLiteral("missing_ids"));
            decision.importBucket = QStringLiteral("reject_review");
        }
        if (idsAvailable && idsRow.valid && alignment.valid) {
            const bool sameDocs = idsRow.fromDoc == alignment.fromDoc && idsRow.toDoc == alignment.toDoc;
            const bool sameIds = idsRow.sourceId == opusAlignmentSourceId(alignment.alignment)
                && idsRow.targetId == opusAlignmentTargetId(alignment.alignment);
            if (!sameDocs || !sameIds) {
                ++idsMismatchRows;
                decision.flags.append(QStringLiteral("ids_alignment_mismatch"));
                decision.importBucket = QStringLiteral("reject_review");
            }
        }

        const QString skipReason = opusPreprocessSkipReason(decision.sourceClean,
                                                            decision.targetClean,
                                                            decision.flags);
        if (!skipReason.isEmpty()) {
            ++skippedPreprocessCounts[skipReason];
            if (!skippedPreprocessExamples.contains(skipReason)) {
                skippedPreprocessExamples.insert(skipReason,
                                                 OpusPreprocessRejectExample{
                                                     skipReason,
                                                     linesRead,
                                                     sourceText.left(500),
                                                     targetText.left(500),
                                                     decision.sourceClean.left(500),
                                                     decision.targetClean.left(500),
                                                     decision.flags.join(QLatin1Char('|')),
                                                     decision.importBucket,
                                                     decision.qualityScore,
                                                     alignment.fromDoc,
                                                     alignment.toDoc,
                                                     alignment.alignment,
                                                     idsRow.raw,
                                                     idsRow.fromDoc,
                                                     idsRow.toDoc,
                                                     idsRow.sourceId,
                                                     idsRow.targetId});
            }
            if ((linesRead % ProgressInterval) == 0) {
                writer.flush();
                QString progressError;
                if (!writeOpusPreprocessProgress(progressFilePath, linesRead, rowsWritten, false, &progressError)) {
                    m_lastError = progressError;
                    return false;
                }
                if ((linesRead % PreprocessReportInterval) == 0) {
                    writeDetailedPreprocessReport(false, 0);
                }
                print(QStringLiteral("[OPUS] Lidas=%1 Gravadas=%2 Duplicadas=%3")
                          .arg(terminalCount(linesRead),
                               terminalCount(rowsWritten),
                               terminalCount(duplicateCleanRows)));
            }
            continue;
        }

        for (const QString &flag : decision.flags) {
            ++qualityCounts[flag];
        }
        ++bucketCounts[decision.importBucket];
        qualityScoreTotal += decision.qualityScore;
        ++qualityScoreRows;

        const QString cleanPairKey = QStringLiteral("%1\t%2\t%3\t%4")
                                         .arg(normalizedSourceLang,
                                              normalizedTargetLang,
                                              decision.sourceClean,
                                              decision.targetClean);
        if (writtenCleanPairs.contains(cleanPairKey)) {
            ++duplicateCleanRows;
            ++skippedPreprocessCounts[QStringLiteral("linha duplicada limpa")];
            if (!skippedPreprocessExamples.contains(QStringLiteral("linha duplicada limpa"))) {
                skippedPreprocessExamples.insert(QStringLiteral("linha duplicada limpa"),
                                                 OpusPreprocessRejectExample{
                                                     QStringLiteral("linha duplicada limpa"),
                                                     linesRead,
                                                     sourceText.left(500),
                                                     targetText.left(500),
                                                     decision.sourceClean.left(500),
                                                     decision.targetClean.left(500),
                                                     decision.flags.join(QLatin1Char('|')),
                                                     decision.importBucket,
                                                     decision.qualityScore,
                                                     alignment.fromDoc,
                                                     alignment.toDoc,
                                                     alignment.alignment,
                                                     idsRow.raw,
                                                     idsRow.fromDoc,
                                                     idsRow.toDoc,
                                                     idsRow.sourceId,
                                                     idsRow.targetId});
            }
            if ((linesRead % ProgressInterval) == 0) {
                writer.flush();
                QString progressError;
                if (!writeOpusPreprocessProgress(progressFilePath, linesRead, rowsWritten, false, &progressError)) {
                    m_lastError = progressError;
                    return false;
                }
                if ((linesRead % PreprocessReportInterval) == 0) {
                    writeDetailedPreprocessReport(false, 0);
                }
                print(QStringLiteral("[OPUS] Lidas=%1 Gravadas=%2 Duplicadas=%3")
                          .arg(terminalCount(linesRead),
                               terminalCount(rowsWritten),
                               terminalCount(duplicateCleanRows)));
            }
            continue;
        }
        writtenCleanPairs.insert(cleanPairKey);

        writer << decision.importBucket << '\t'
               << decision.qualityScore << '\t'
               << safeTsvField(decision.sourceClean) << '\t'
               << safeTsvField(decision.targetClean) << Qt::endl;

        ++rowsWritten;

        if ((linesRead % ProgressInterval) == 0) {
            writer.flush();
            QString progressError;
            if (!writeOpusPreprocessProgress(progressFilePath, linesRead, rowsWritten, false, &progressError)) {
                m_lastError = progressError;
                return false;
            }
            if ((linesRead % PreprocessReportInterval) == 0) {
                writeDetailedPreprocessReport(false, 0);
            }
            print(QStringLiteral("[OPUS] Lidas=%1 Gravadas=%2 Duplicadas=%3")
                      .arg(terminalCount(linesRead),
                           terminalCount(rowsWritten),
                           terminalCount(duplicateCleanRows)));
        }
    }

    if (!alignmentReader.errorString().isEmpty()) {
        m_lastError = QStringLiteral("Erro ao ler XML de alinhamento OPUS: %1").arg(alignmentReader.errorString());
        return false;
    }
    qint64 extraIdsRows = 0;
    if (idsAvailable) {
        while (!idsStream.atEnd()) {
            idsStream.readLine();
            ++extraIdsRows;
        }
    }

    output.flush();
    QString progressError;
    if (!writeOpusPreprocessProgress(progressFilePath, linesRead, rowsWritten, true, &progressError)) {
        m_lastError = progressError;
        return false;
    }

    QStringList buckets = bucketCounts.keys();
    std::sort(buckets.begin(), buckets.end());
    QStringList skipReasons = skippedPreprocessCounts.keys();
    std::sort(skipReasons.begin(), skipReasons.end());
    QStringList flags = qualityCounts.keys();
    std::sort(flags.begin(), flags.end());

    QFile report(outputInfo.dir().filePath(QStringLiteral("OPUS_Preprocess_Report.txt")));
    if (report.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
        QTextStream reportStream(&report);
        configureUtf8Stream(reportStream);
        reportStream << "Corpus: " << corpusName << Qt::endl;
        reportStream << "Origem: " << QFileInfo(sourceFilePath).absoluteFilePath() << Qt::endl;
        reportStream << "Destino: " << QFileInfo(targetFilePath).absoluteFilePath() << Qt::endl;
        reportStream << "XML: " << (xmlAvailable ? QFileInfo(xmlFilePath).absoluteFilePath() : QStringLiteral("nao encontrado")) << Qt::endl;
        reportStream << "IDS: " << (idsAvailable ? QFileInfo(idsFilePath).absoluteFilePath() : QStringLiteral("nao encontrado")) << Qt::endl;
        reportStream << "Saida: " << QFileInfo(outputFilePath).absoluteFilePath() << Qt::endl;
        reportStream << "Progresso: " << QFileInfo(progressFilePath).absoluteFilePath() << Qt::endl;
        reportStream << "Retomado de checkpoint: " << (resumeMode ? QStringLiteral("sim") : QStringLiteral("nao")) << Qt::endl;
        reportStream << "Par: " << normalizedSourceLang << " -> " << normalizedTargetLang << Qt::endl;
        reportStream << "Linhas lidas: " << linesRead << Qt::endl;
        reportStream << "Linhas gravadas: " << rowsWritten << Qt::endl;
        reportStream << "Duplicados limpos ignorados: " << duplicateCleanRows << Qt::endl;
        reportStream << "Linhas somente na origem: " << sourceOnlyLines << Qt::endl;
        reportStream << "Linhas somente no destino: " << targetOnlyLines << Qt::endl;
        reportStream << "Linhas sem alinhamento: " << missingAlignmentLines << Qt::endl;
        reportStream << "Linhas IDS lidas: " << idsRowsRead << Qt::endl;
        reportStream << "Linhas IDS ausentes: " << missingIdsRows << Qt::endl;
        reportStream << "Linhas IDS extras: " << extraIdsRows << Qt::endl;
        reportStream << "Linhas com divergencia IDS/XML: " << idsMismatchRows << Qt::endl;
        reportStream << "Pontuacao media de qualidade: "
                     << (qualityScoreRows > 0 ? QString::number(static_cast<double>(qualityScoreTotal) / static_cast<double>(qualityScoreRows), 'f', 2) : QStringLiteral("0.00"))
                     << Qt::endl;
        for (const QString &bucket : buckets) {
            reportStream << "Bucket de importacao " << bucket << ": " << bucketCounts.value(bucket) << Qt::endl;
        }
        for (const QString &reason : skipReasons) {
            reportStream << "Pre-processamento pulou " << reason << ": " << skippedPreprocessCounts.value(reason) << Qt::endl;
        }
        for (const QString &flag : flags) {
            reportStream << "Flag de qualidade " << flag << ": " << qualityCounts.value(flag) << Qt::endl;
        }
        reportStream << Qt::endl;
    }

    writeDetailedPreprocessReport(true, extraIdsRows);

    print(QStringLiteral("[OPUS] final Lidas=%1 Gravadas=%2 Duplicadas=%3 Ignoradas=%4")
              .arg(terminalCount(linesRead),
                   terminalCount(rowsWritten),
                   terminalCount(duplicateCleanRows),
                   terminalCount(linesRead - rowsWritten - duplicateCleanRows)));

    QString details;
    QTextStream detailsStream(&details);
    detailsStream << "Corpus: " << corpusName << Qt::endl;
    detailsStream << "Alinhamento XML: " << (xmlAvailable ? QFileInfo(xmlFilePath).absoluteFilePath() : QStringLiteral("nao encontrado")) << Qt::endl;
    detailsStream << "Alinhamento IDS: " << (idsAvailable ? QFileInfo(idsFilePath).absoluteFilePath() : QStringLiteral("nao encontrado")) << Qt::endl;
    detailsStream << "Arquivo de progresso: " << QFileInfo(progressFilePath).absoluteFilePath() << Qt::endl;
    detailsStream << "Retomado de checkpoint: " << (resumeMode ? QStringLiteral("sim") : QStringLiteral("nao")) << Qt::endl;
    detailsStream << "Linhas lidas: " << linesRead << Qt::endl;
    detailsStream << "Linhas gravadas: " << rowsWritten << Qt::endl;
    detailsStream << "Linhas somente na origem: " << sourceOnlyLines << Qt::endl;
    detailsStream << "Linhas somente no destino: " << targetOnlyLines << Qt::endl;
    detailsStream << "Linhas sem alinhamento: " << missingAlignmentLines << Qt::endl;
    detailsStream << "Linhas IDS lidas: " << idsRowsRead << Qt::endl;
    detailsStream << "Linhas IDS ausentes: " << missingIdsRows << Qt::endl;
    detailsStream << "Linhas IDS extras: " << extraIdsRows << Qt::endl;
    detailsStream << "Linhas com divergencia IDS/XML: " << idsMismatchRows << Qt::endl;
    detailsStream << "Pontuacao media de qualidade: "
                  << (qualityScoreRows > 0 ? QString::number(static_cast<double>(qualityScoreTotal) / static_cast<double>(qualityScoreRows), 'f', 2) : QStringLiteral("0.00"))
                  << Qt::endl;
    detailsStream << "Tempo total: " << AtlasReport::formatDuration(totalTimer.nsecsElapsed()) << Qt::endl;
    detailsStream << Qt::endl << "Buckets de importacao:" << Qt::endl;
    for (const QString &bucket : buckets) {
        detailsStream << "* " << bucket << ": " << bucketCounts.value(bucket) << Qt::endl;
    }
    detailsStream << Qt::endl << "Linhas puladas no pre-processamento:" << Qt::endl;
    if (skipReasons.isEmpty()) {
        detailsStream << "* nenhum: 0" << Qt::endl;
    } else {
        for (const QString &reason : skipReasons) {
            detailsStream << "* " << reason << ": " << skippedPreprocessCounts.value(reason) << Qt::endl;
        }
    }
    detailsStream << Qt::endl << "Flags de qualidade:" << Qt::endl;
    if (flags.isEmpty()) {
        detailsStream << "* nenhum: 0" << Qt::endl;
    } else {
        for (const QString &flag : flags) {
            detailsStream << "* " << flag << ": " << qualityCounts.value(flag) << Qt::endl;
        }
    }
    writePreprocessReport(QStringLiteral("OPUS/Moses"),
                          sourceFilePath,
                          targetFilePath,
                          normalizedSourceLang,
                          normalizedTargetLang,
                          QFileInfo(outputFilePath).absoluteFilePath(),
                          startedAt,
                          true,
                          details);
    return true;
}

bool AtlasImporter::importMosesPreprocessedDataset(const QString &preprocessedFilePath,
                                                   const QString &sourceLang,
                                                   const QString &targetLang)
{
    m_stats = ImportStats();
    m_cleanupStats = CleanupStats();
    m_lastError.clear();
    m_currentImportSourceFile = preprocessedFilePath;
    m_rejectedDetailsBuffer.clear();
    m_bufferedPairs = 0;
    m_bufferSourceTexts.clear();
    m_bufferTranslatedTexts.clear();
    m_bufferSourceLangs.clear();
    m_bufferTargetLangs.clear();
    m_bufferSenseGlosses.clear();
    m_rejectedExamples.clear();
    m_lastRejectedSummarySnapshotProcessed = -1;

    QElapsedTimer totalTimer;
    totalTimer.start();
    const QString startedAt = AtlasReport::timestamp();
    const QString normalizedSourceLang = m_languageNormalizer.normalize(sourceLang);
    const QString normalizedTargetLang = m_languageNormalizer.normalize(targetLang);
    if (!m_languageNormalizer.isSupported(normalizedSourceLang)
        || !m_languageNormalizer.isSupported(normalizedTargetLang)) {
        m_lastError = QStringLiteral("Par de idiomas OPUS pre-processado fora da lista suportada e ignorado: %1 -> %2")
                          .arg(sourceLang, targetLang);
        return false;
    }

    QFile file(preprocessedFilePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = QStringLiteral("Nao foi possivel abrir arquivo OPUS pre-processado: %1").arg(preprocessedFilePath);
        return false;
    }

    QTextStream stream(&file);
    configureUtf8Stream(stream);

    if (!openDatabase() || !ensureSchema() || !ensureImportBufferTable()) {
        return false;
    }

    const QString currentImportKey = importKey(preprocessedFilePath,
                                               QStringLiteral("OPUS:preprocessed"),
                                               normalizedSourceLang,
                                               normalizedTargetLang);
    qint64 resumeFromLine = 0;
    bool completed = false;
    if (!loadProgress(currentImportKey, resumeFromLine, completed)) {
        return false;
    }
    if (completed) {
        m_stats.totalTimeNs = totalTimer.nsecsElapsed();
        writeRejectedDetailsSummary(preprocessedFilePath, normalizedSourceLang, normalizedTargetLang, startedAt);
        writeImportReport(QStringLiteral("OPUS preprocessed"), preprocessedFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, true);
        return true;
    }
    m_stats.resumedLines = resumeFromLine;
    maybeWriteRejectedDetailsSummary(preprocessedFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, true);

    QSqlQuery insertQuery(m_database);
    QSqlQuery updateQuery(m_database);
    if (!prepareInsertStatement(insertQuery) || !prepareUpdateFrequencyStatement(updateQuery)) {
        return false;
    }

    if (!m_database.transaction()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    bool success = true;
    qint64 pairsSinceCommit = 0;
    qint64 lastProgressPrinted = 0;
    qint64 acceptedRows = 0;
    qint64 importedShortRows = 0;
    qint64 longRowsSkipped = 0;
    qint64 longRowsSplit = 0;
    qint64 splitSegmentsImported = 0;
    qint64 sameNormalizedSkippedRows = 0;
    qint64 rejectReviewRows = 0;

    auto importPair = [&](const QString &source, const QString &target, bool allowSameText = false) {
        QElapsedTimer sqliteTimer;
        sqliteTimer.start();
        const bool ok = insertPreparedPair(insertQuery, updateQuery, source, target, normalizedSourceLang, normalizedTargetLang, QString(), allowSameText)
            && insertPreparedPair(insertQuery, updateQuery, target, source, normalizedTargetLang, normalizedSourceLang, QString(), allowSameText);
        m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
        if (ok) {
            pairsSinceCommit += 2;
        }
        return ok;
    };

    while (!stream.atEnd()) {
        QString line = stream.readLine();
        if (m_stats.processedLines == 0 && line.startsWith(QChar(0xFEFF))) {
            line.remove(0, 1);
        }
        ++m_stats.processedLines;

        if (m_stats.processedLines <= resumeFromLine) {
            continue;
        }
        if (line.isEmpty()
            || line.startsWith(QLatin1Char('#'))
            || line.startsWith(QStringLiteral("bucket_importacao\tpontuacao_qualidade\t"))) {
            continue;
        }

        const QStringList columns = line.split(QLatin1Char('\t'));
        if (columns.size() != 4) {
            recordDiscard(QStringLiteral("broken record"),
                          line,
                          QString(),
                          normalizedSourceLang,
                          normalizedTargetLang);
            continue;
        }

        const int bucketColumn = 0;
        const int scoreColumn = 1;
        const int sourceCleanColumn = 2;
        const int targetCleanColumn = 3;

        const QString sourceText = cleanText(columns.value(sourceCleanColumn));
        const QString targetText = cleanText(columns.value(targetCleanColumn));
        const QString importBucket = columns.value(bucketColumn).trimmed();
        const int qualityScore = columns.value(scoreColumn).toInt();
        ++acceptedRows;

        const bool sameCleanText = isSameNormalizedText(sourceText, targetText);
        const bool allowSameTextImport = sameCleanText
            && importBucket != QStringLiteral("reject_review")
            && qualityScore <= 60;
        if (sameCleanText && !allowSameTextImport) {
            ++sameNormalizedSkippedRows;
            recordDiscard(QStringLiteral("OPUS same text review"),
                          sourceText,
                          targetText,
                          normalizedSourceLang,
                          normalizedTargetLang);
            continue;
        }
        if (importBucket == QStringLiteral("reject_review")) {
            ++rejectReviewRows;
            recordDiscard(QStringLiteral("OPUS reject_review bucket"),
                          sourceText,
                          targetText,
                          normalizedSourceLang,
                          normalizedTargetLang);
            continue;
        }

        if (isWithinPunctuationSplitWordLimit(sourceText, targetText)) {
            if (!importPair(sourceText, targetText, allowSameTextImport)) {
                success = false;
                break;
            }
            ++importedShortRows;
        } else {
            const PunctuationSplitResult splitResult = splitByCompatibleStructuralPunctuation(sourceText, targetText);
            qsizetype importedSegments = 0;
            for (const auto &segment : splitResult.segments) {
                if (!isWithinPunctuationSplitWordLimit(segment.first, segment.second)) {
                    recordDiscard(QStringLiteral("OPUS segment exceeds 8 words"),
                                  segment.first,
                                  segment.second,
                                  normalizedSourceLang,
                                  normalizedTargetLang);
                    continue;
                }
                if (!discardReasonForPair(segment.first, segment.second).isEmpty()) {
                    recordDiscard(QStringLiteral("OPUS segment rejected by quality filter"),
                                  segment.first,
                                  segment.second,
                                  normalizedSourceLang,
                                  normalizedTargetLang);
                    continue;
                }
                if (!importPair(segment.first, segment.second)) {
                    success = false;
                    break;
                }
                ++importedSegments;
                ++splitSegmentsImported;
            }
            if (!success) {
                break;
            }
            if (importedSegments > 0) {
                ++longRowsSplit;
            } else {
                ++longRowsSkipped;
                recordDiscard(QStringLiteral("OPUS row exceeds 8 words"),
                              sourceText,
                              targetText,
                              normalizedSourceLang,
                              normalizedTargetLang);
            }
        }

        if (pairsSinceCommit >= CommitInterval) {
            QElapsedTimer commitTimer;
            commitTimer.start();
            if (!mergeBufferedPairs()) {
                m_stats.sqliteTimeNs += commitTimer.nsecsElapsed();
                success = false;
                break;
            }
            m_stats.sqliteTimeNs += commitTimer.nsecsElapsed();
            if (!saveProgress(currentImportKey,
                              preprocessedFilePath,
                              QStringLiteral("OPUS:preprocessed"),
                              normalizedSourceLang,
                              normalizedTargetLang,
                              m_stats.processedLines,
                              false)) {
                success = false;
                break;
            }
            if (!m_database.commit()) {
                m_lastError = m_database.lastError().text();
                ++m_stats.errorCount;
                success = false;
                break;
            }
            if (!m_database.transaction()) {
                m_lastError = m_database.lastError().text();
                ++m_stats.errorCount;
                success = false;
                break;
            }
            pairsSinceCommit = 0;
        }

        if ((m_stats.processedLines % ProgressInterval) == 0
            && m_stats.processedLines != lastProgressPrinted) {
            printProgress();
            maybeWriteRejectedDetailsSummary(preprocessedFilePath, normalizedSourceLang, normalizedTargetLang, startedAt);
            lastProgressPrinted = m_stats.processedLines;
        }
    }

    if (success) {
        QElapsedTimer sqliteTimer;
        sqliteTimer.start();
        success = mergeBufferedPairs();
        m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
    }

    if (success) {
        success = saveProgress(currentImportKey,
                               preprocessedFilePath,
                               QStringLiteral("OPUS:preprocessed"),
                               normalizedSourceLang,
                               normalizedTargetLang,
                               m_stats.processedLines,
                               true);
    }

    if (success && !m_database.commit()) {
        m_lastError = m_database.lastError().text();
        ++m_stats.errorCount;
        success = false;
    } else if (!success) {
        m_database.rollback();
    }

    QElapsedTimer indexTimer;
    indexTimer.start();
    if (success && !ensureIndexes()) {
        m_stats.indexTimeNs += indexTimer.nsecsElapsed();
        return false;
    }
    m_stats.indexTimeNs += indexTimer.nsecsElapsed();

    QElapsedTimer cleanupTimer;
    cleanupTimer.start();
    if (success && !optimizeDatabase(false)) {
        m_stats.cleanupTimeNs += cleanupTimer.nsecsElapsed();
        return false;
    }
    m_stats.cleanupTimeNs += cleanupTimer.nsecsElapsed();
    m_cleanupStats.finalTranslationsCount = translationCount();
    m_stats.totalTimeNs = totalTimer.nsecsElapsed();

    QTextStream console(stdout);
    configureUtf8Stream(console);
    console << "[OPUS pre-processado] resumo final" << Qt::endl;
    console << "Linhas lidas: " << m_stats.processedLines << Qt::endl;
    console << "Linhas aceitas: " << acceptedRows << Qt::endl;
    console << "Linhas curtas importadas: " << importedShortRows << Qt::endl;
    console << "Linhas longas segmentadas: " << longRowsSplit << Qt::endl;
    console << "Segmentos importados: " << splitSegmentsImported << Qt::endl;
    console << "Linhas longas ignoradas: " << longRowsSkipped << Qt::endl;
    console << "Linhas iguais normalizadas ignoradas: " << sameNormalizedSkippedRows << Qt::endl;
    console << "Linhas enviadas para revisao/rejeicao: " << rejectReviewRows << Qt::endl;
    flushRejectedDetails();
    maybeWriteRejectedDetailsSummary(preprocessedFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, true);
    writeImportReport(QStringLiteral("OPUS preprocessed"), preprocessedFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, success);
    return success;
}

bool AtlasImporter::importFreeDictTeiDataset(const QString &teiFilePath,
                                             const QString &sourceLang,
                                             const QString &targetLang)
{
    m_stats = ImportStats();
    m_cleanupStats = CleanupStats();
    m_lastError.clear();
    m_currentImportSourceFile = teiFilePath;
    m_rejectedDetailsBuffer.clear();
    m_bufferedPairs = 0;
    m_bufferSourceTexts.clear();
    m_bufferTranslatedTexts.clear();
    m_bufferSourceLangs.clear();
    m_bufferTargetLangs.clear();
    m_bufferSenseGlosses.clear();
    m_rejectedExamples.clear();
    m_lastRejectedSummarySnapshotProcessed = -1;
    QElapsedTimer totalTimer;
    totalTimer.start();
    const QString startedAt = AtlasReport::timestamp();

    const QString normalizedSourceLang = m_languageNormalizer.normalize(sourceLang);
    const QString normalizedTargetLang = m_languageNormalizer.normalize(targetLang);
    if (!m_languageNormalizer.isSupported(normalizedSourceLang)
        || !m_languageNormalizer.isSupported(normalizedTargetLang)) {
        m_lastError = QStringLiteral("Par de idiomas FreeDict fora da lista suportada e ignorado: %1 -> %2")
                          .arg(sourceLang, targetLang);
        return false;
    }

    QFile file(teiFilePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = QStringLiteral("Nao foi possivel abrir arquivo FreeDict: %1").arg(teiFilePath);
        return false;
    }

    if (!openDatabase() || !ensureSchema() || !ensureImportBufferTable()) {
        return false;
    }

    const QString currentImportKey = importKey(teiFilePath,
                                               QStringLiteral("FreeDict:tei"),
                                               normalizedSourceLang,
                                               normalizedTargetLang);
    qint64 resumeFromEntry = 0;
    bool completed = false;
    if (!loadProgress(currentImportKey, resumeFromEntry, completed)) {
        return false;
    }
    if (completed) {
        m_stats.totalTimeNs = totalTimer.nsecsElapsed();
        writeRejectedDetailsSummary(teiFilePath, normalizedSourceLang, normalizedTargetLang, startedAt);
        writeImportReport(QStringLiteral("FreeDict"), teiFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, true);
        return true;
    }
    m_stats.resumedLines = resumeFromEntry;

    maybeWriteRejectedDetailsSummary(teiFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, true);

    QSqlQuery insertQuery(m_database);
    QSqlQuery updateQuery(m_database);
    if (!prepareInsertStatement(insertQuery) || !prepareUpdateFrequencyStatement(updateQuery)) {
        return false;
    }

    if (!m_database.transaction()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    QXmlStreamReader xml(&file);
    QString currentHeadword;
    bool insideEntry = false;
    bool insideForm = false;
    bool insideTranslationCitation = false;
    qint64 pairsSinceCommit = 0;
    qint64 lastProgressPrinted = 0;
    bool success = true;

    while (!xml.atEnd()) {
        QElapsedTimer parsingTimer;
        parsingTimer.start();
        xml.readNext();
        m_stats.parsingTimeNs += parsingTimer.nsecsElapsed();

        if (xml.isStartElement()) {
            const QStringView name = xml.name();
            if (name == QStringLiteral("entry")) {
                insideEntry = true;
                insideForm = false;
                insideTranslationCitation = false;
                currentHeadword.clear();
                ++m_stats.processedLines;
            } else if (insideEntry && name == QStringLiteral("form")) {
                insideForm = true;
            } else if (insideEntry && insideForm && name == QStringLiteral("orth") && currentHeadword.isEmpty()) {
                currentHeadword = cleanText(xml.readElementText(QXmlStreamReader::SkipChildElements));
            } else if (insideEntry && name == QStringLiteral("cit")) {
                insideTranslationCitation = xml.attributes().value(QStringLiteral("type")) == QStringLiteral("trans");
            } else if (insideEntry && insideTranslationCitation && name == QStringLiteral("quote")) {
                if (m_stats.processedLines <= resumeFromEntry) {
                    continue;
                }
                const QString quote = cleanText(xml.readElementText(QXmlStreamReader::SkipChildElements));
                for (const QString &targetText : splitTranslationAlternatives(quote)) {
                    QElapsedTimer sqliteTimer;
                    sqliteTimer.start();
                    if (!insertPreparedPair(insertQuery, updateQuery, currentHeadword, targetText, normalizedSourceLang, normalizedTargetLang)
                        || !insertPreparedPair(insertQuery, updateQuery, targetText, currentHeadword, normalizedTargetLang, normalizedSourceLang)) {
                        m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
                        success = false;
                        break;
                    }
                    m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
                    pairsSinceCommit += 2;
                }
                if (!success) {
                    break;
                }
            }
        } else if (xml.isEndElement()) {
            const QStringView name = xml.name();
            if (name == QStringLiteral("entry")) {
                insideEntry = false;
                insideForm = false;
                insideTranslationCitation = false;
            } else if (name == QStringLiteral("form")) {
                insideForm = false;
            } else if (name == QStringLiteral("cit")) {
                insideTranslationCitation = false;
            }
        }

        if (success && pairsSinceCommit >= CommitInterval) {
            QElapsedTimer sqliteTimer;
            sqliteTimer.start();
            if (!mergeBufferedPairs()) {
                m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
                success = false;
                break;
            }
            m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
            if (!saveProgress(currentImportKey,
                              teiFilePath,
                              QStringLiteral("FreeDict:tei"),
                              normalizedSourceLang,
                              normalizedTargetLang,
                              m_stats.processedLines,
                              false)) {
                success = false;
                break;
            }
            if (!m_database.commit()) {
                m_lastError = m_database.lastError().text();
                ++m_stats.errorCount;
                success = false;
                break;
            }
            if (!m_database.transaction()) {
                m_lastError = m_database.lastError().text();
                ++m_stats.errorCount;
                success = false;
                break;
            }
            pairsSinceCommit = 0;
        }

        if ((m_stats.processedLines % ProgressInterval) == 0
            && m_stats.processedLines > 0
            && m_stats.processedLines != lastProgressPrinted) {
            printProgress();
            maybeWriteRejectedDetailsSummary(teiFilePath, normalizedSourceLang, normalizedTargetLang, startedAt);
            lastProgressPrinted = m_stats.processedLines;
        }
    }

    if (success && xml.hasError()) {
        m_lastError = QStringLiteral("Erro ao ler FreeDict TEI: %1").arg(xml.errorString());
        ++m_stats.errorCount;
        success = false;
    }

    if (success) {
        QElapsedTimer sqliteTimer;
        sqliteTimer.start();
        success = mergeBufferedPairs();
        m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
    }

    if (success) {
        success = saveProgress(currentImportKey,
                               teiFilePath,
                               QStringLiteral("FreeDict:tei"),
                               normalizedSourceLang,
                               normalizedTargetLang,
                               m_stats.processedLines,
                               true);
    }

    if (success && !m_database.commit()) {
        m_lastError = m_database.lastError().text();
        ++m_stats.errorCount;
        success = false;
    } else if (!success) {
        m_database.rollback();
    }

    QElapsedTimer cleanupTimer;
    cleanupTimer.start();
    if (success && !optimizeDatabase(false)) {
        m_stats.cleanupTimeNs += cleanupTimer.nsecsElapsed();
        return false;
    }
    m_stats.cleanupTimeNs += cleanupTimer.nsecsElapsed();
    m_cleanupStats.finalTranslationsCount = translationCount();

    m_stats.totalTimeNs = totalTimer.nsecsElapsed();
    flushRejectedDetails();
    maybeWriteRejectedDetailsSummary(teiFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, true);
    writeImportReport(QStringLiteral("FreeDict"), teiFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, success);
    return success;
}

bool AtlasImporter::importMediaWikiDataset(const QString &mediaWikiFilePath,
                                           const QString &sourceLang,
                                           const QString &targetLang)
{
    m_stats = ImportStats();
    m_cleanupStats = CleanupStats();
    m_lastError.clear();
    m_currentImportSourceFile = mediaWikiFilePath;
    m_rejectedDetailsBuffer.clear();
    m_bufferedPairs = 0;
    m_bufferSourceTexts.clear();
    m_bufferTranslatedTexts.clear();
    m_bufferSourceLangs.clear();
    m_bufferTargetLangs.clear();
    m_bufferSenseGlosses.clear();
    m_rejectedExamples.clear();
    m_lastRejectedSummarySnapshotProcessed = -1;
    QElapsedTimer totalTimer;
    totalTimer.start();
    const QString startedAt = AtlasReport::timestamp();
    const QString normalizedSourceLang = m_languageNormalizer.normalize(sourceLang);
    const QString normalizedTargetLang = m_languageNormalizer.normalize(targetLang);
    if (!m_languageNormalizer.isSupported(normalizedSourceLang)
        || !m_languageNormalizer.isSupported(normalizedTargetLang)) {
        m_lastError = QStringLiteral("Par de idiomas MediaWiki fora da lista suportada e ignorado: %1 -> %2")
                          .arg(sourceLang, targetLang);
        return false;
    }

    QFile file(mediaWikiFilePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = QStringLiteral("Nao foi possivel abrir arquivo MediaWiki: %1").arg(mediaWikiFilePath);
        return false;
    }

    if (!openDatabase() || !ensureSchema() || !ensureImportBufferTable()) {
        return false;
    }

    const QString currentImportKey = importKey(mediaWikiFilePath,
                                               QStringLiteral("MediaWiki:xml"),
                                               normalizedSourceLang,
                                               normalizedTargetLang);
    qint64 resumeFromPage = 0;
    bool completed = false;
    if (!loadProgress(currentImportKey, resumeFromPage, completed)) {
        return false;
    }
    if (completed) {
        m_stats.totalTimeNs = totalTimer.nsecsElapsed();
        writeRejectedDetailsSummary(mediaWikiFilePath, normalizedSourceLang, normalizedTargetLang, startedAt);
        writeImportReport(QStringLiteral("MediaWiki"), mediaWikiFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, true);
        return true;
    }
    m_stats.resumedLines = resumeFromPage;

    maybeWriteRejectedDetailsSummary(mediaWikiFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, true);

    QSqlQuery insertQuery(m_database);
    QSqlQuery updateQuery(m_database);
    if (!prepareInsertStatement(insertQuery) || !prepareUpdateFrequencyStatement(updateQuery)) {
        return false;
    }

    if (!m_database.transaction()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    static const QRegularExpression translationTemplateExpression(QStringLiteral(R"(\{\{\s*(t(?:\+check|\+|-check|-simple)?)\s*\|\s*([^|}]+)\s*\|\s*([^|}]+))"),
                                                                  QRegularExpression::CaseInsensitiveOption);

    QXmlStreamReader xml(&file);
    QString title;
    QString ns;
    QString text;
    bool inPage = false;
    bool success = true;
    qint64 pairsSinceCommit = 0;
    qint64 lastProgressPrinted = 0;
    qint64 skippedAdministrativePages = 0;
    qint64 acceptedMainPages = 0;
    qint64 sourceLanguageSections = 0;
    qint64 ignoredSections = 0;
    qint64 translationBlocks = 0;
    qint64 translationLines = 0;
    qint64 acceptedTemplates = 0;
    qint64 ignoredTemplates = 0;
    qint64 unsupportedSourceLanguageSections = 0;
    qint64 unsupportedTargetLanguageTemplates = 0;
    QTextStream console(stdout);
    configureUtf8Stream(console);
    console << "[MediaWiki pre-processamento] Iniciando limpeza estrutural do XML antes da importacao SQLite" << Qt::endl;
    console << "[MediaWiki pre-processamento] Conteudo permitido: pagina principal + secao "
            << normalizedSourceLang << " + blocos Translations + templates t/t+/t-check/t-simple" << Qt::endl;

    while (!xml.atEnd()) {
        QElapsedTimer parsingTimer;
        parsingTimer.start();
        xml.readNext();
        m_stats.parsingTimeNs += parsingTimer.nsecsElapsed();
        if (xml.isStartElement()) {
            const QStringView name = xml.name();
            if (name == QStringLiteral("page")) {
                inPage = true;
                title.clear();
                ns.clear();
                text.clear();
            } else if (inPage && name == QStringLiteral("title")) {
                title = xml.readElementText(QXmlStreamReader::SkipChildElements);
            } else if (inPage && name == QStringLiteral("ns")) {
                ns = xml.readElementText(QXmlStreamReader::SkipChildElements);
            } else if (inPage && name == QStringLiteral("text")) {
                text = xml.readElementText(QXmlStreamReader::IncludeChildElements);
            }
        } else if (xml.isEndElement() && xml.name() == QStringLiteral("page")) {
            inPage = false;
            ++m_stats.processedLines;

            if (m_stats.processedLines <= resumeFromPage) {
                continue;
            }

            if (ns != QStringLiteral("0") || isAdministrativeMediaWikiTitle(title) || text.isEmpty()) {
                ++skippedAdministrativePages;
                if ((skippedAdministrativePages % ProgressInterval) == 0) {
                    console << "[MediaWiki pre-processamento] paginas administrativas puladas: "
                            << skippedAdministrativePages << Qt::endl;
                }
                continue;
            }

            ++acceptedMainPages;
            QString currentSourceLang;
            bool inTranslationsSection = false;
            bool insideSourceLanguage = false;
            QString currentTranslationGloss;
            const QStringList lines = text.split(QLatin1Char('\n'));
            for (const QString &line : lines) {
                const QString trimmedLine = line.trimmed();
                int headingLevel = 0;
                QString headingText;
                if (parseWikiHeading(trimmedLine, headingLevel, headingText)) {
                    if (headingLevel == 2) {
                        currentSourceLang = mediaWikiHeadingLanguageCode(headingText);
                        insideSourceLanguage = currentSourceLang == normalizedSourceLang;
                        if (insideSourceLanguage) {
                            ++sourceLanguageSections;
                        } else if (currentSourceLang.isEmpty()) {
                            ++unsupportedSourceLanguageSections;
                        }
                        inTranslationsSection = false;
                        continue;
                    }

                    if (!insideSourceLanguage) {
                        continue;
                    }

                    if (isMediaWikiTranslationHeading(headingLevel, headingText)) {
                        inTranslationsSection = true;
                        currentTranslationGloss.clear();
                        ++translationBlocks;
                        continue;
                    }

                    if (isIgnoredMediaWikiSection(headingText)) {
                        ++ignoredSections;
                    }
                    inTranslationsSection = false;
                    continue;
                }

                if (!insideSourceLanguage || currentSourceLang.isEmpty()) {
                    continue;
                }

                if (!inTranslationsSection) {
                    continue;
                }

                QString parsedGloss;
                if (parseMediaWikiTranslationGlossLine(trimmedLine, parsedGloss)) {
                    currentTranslationGloss = cleanText(parsedGloss);
                    continue;
                }

                if (isIgnoredMediaWikiTranslationControlLine(trimmedLine)) {
                    ++ignoredTemplates;
                    continue;
                }

                if (!trimmedLine.startsWith(QLatin1Char('*'))) {
                    continue;
                }

                QRegularExpressionMatchIterator matches = translationTemplateExpression.globalMatch(trimmedLine);
                bool foundExplicitTranslationTemplate = false;
                while (matches.hasNext()) {
                    foundExplicitTranslationTemplate = true;
                    const QRegularExpressionMatch match = matches.next();
                    const QString acceptedTemplateName = match.captured(1).trimmed().toLower();
                    const QString rawTargetLanguage = match.captured(2).trimmed().toLower();
                    const QString targetLang = mediaWikiTemplateLanguageCode(rawTargetLanguage);
                    const QString targetText = cleanText(cleanWikiTranslationText(match.captured(3)));
                    if (targetLang.isEmpty()) {
                        ++unsupportedTargetLanguageTemplates;
                    }
                    if ((acceptedTemplateName == QStringLiteral("t-check") || acceptedTemplateName == QStringLiteral("t-simple"))
                        && targetText.isEmpty()) {
                        ++ignoredTemplates;
                        continue;
                    }
                    if (targetLang.isEmpty()) {
                        ++ignoredTemplates;
                        continue;
                    }
                    if (targetLang != normalizedTargetLang || targetLang == currentSourceLang) {
                        ++ignoredTemplates;
                        continue;
                    }

                    ++translationLines;
                    ++acceptedTemplates;
                    QElapsedTimer sqliteTimer;
                    sqliteTimer.start();
                    if (!insertPreparedPair(insertQuery, updateQuery, title, targetText, currentSourceLang, targetLang, currentTranslationGloss)
                        || !insertPreparedPair(insertQuery, updateQuery, targetText, title, targetLang, currentSourceLang, currentTranslationGloss)) {
                        m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
                        success = false;
                        break;
                    }
                    m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
                    pairsSinceCommit += 2;
                }
                if (!success) {
                    break;
                }
                if (!foundExplicitTranslationTemplate
                    && (trimmedLine.contains(QStringLiteral("{{"))
                        || trimmedLine.contains(QLatin1Char(':')))) {
                    ++ignoredTemplates;
                }

                if (pairsSinceCommit >= CommitInterval) {
                    QElapsedTimer sqliteTimer;
                    sqliteTimer.start();
                    if (!mergeBufferedPairs()) {
                        m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
                        success = false;
                        break;
                    }
                    m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
                    if (!saveProgress(currentImportKey,
                                      mediaWikiFilePath,
                                      QStringLiteral("MediaWiki:xml"),
                                      normalizedSourceLang,
                                      normalizedTargetLang,
                                      m_stats.processedLines,
                                      false)) {
                        success = false;
                        break;
                    }
                    if (!m_database.commit()) {
                        m_lastError = m_database.lastError().text();
                        ++m_stats.errorCount;
                        success = false;
                        break;
                    }
                    if (!m_database.transaction()) {
                        m_lastError = m_database.lastError().text();
                        ++m_stats.errorCount;
                        success = false;
                        break;
                    }
                    pairsSinceCommit = 0;
                }
            }

            if (!success) {
                break;
            }

            if ((m_stats.processedLines % ProgressInterval) == 0
                && m_stats.processedLines != lastProgressPrinted) {
                printProgress();
                console << "[MediaWiki pre-processamento] paginas_lidas: " << m_stats.processedLines
                        << " | administrativas_puladas: " << skippedAdministrativePages
                        << " | verbetes_aceitos: " << acceptedMainPages
                        << " | secoes_idioma_origem: " << sourceLanguageSections
                        << " | secoes_ignoradas: " << ignoredSections
                        << " | blocos_Translations: " << translationBlocks
                        << " | linhas_reais_traducao: " << translationLines
                        << " | templates_aceitos: " << acceptedTemplates
                        << " | templates_ignorados: " << ignoredTemplates
                        << Qt::endl;
                maybeWriteRejectedDetailsSummary(mediaWikiFilePath, normalizedSourceLang, normalizedTargetLang, startedAt);
                lastProgressPrinted = m_stats.processedLines;
            }
        }
    }

    if (success && xml.hasError()) {
        m_lastError = QStringLiteral("Erro ao ler XML MediaWiki: %1").arg(xml.errorString());
        ++m_stats.errorCount;
        success = false;
    }

    if (success) {
        QElapsedTimer sqliteTimer;
        sqliteTimer.start();
        success = mergeBufferedPairs();
        m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
    }

    if (success) {
        success = saveProgress(currentImportKey,
                               mediaWikiFilePath,
                               QStringLiteral("MediaWiki:xml"),
                               normalizedSourceLang,
                               normalizedTargetLang,
                               m_stats.processedLines,
                               true);
    }

    if (success && !m_database.commit()) {
        m_lastError = m_database.lastError().text();
        ++m_stats.errorCount;
        success = false;
    } else if (!success) {
        m_database.rollback();
    }

    QElapsedTimer indexTimer;
    indexTimer.start();
    if (success && !ensureIndexes()) {
        m_stats.indexTimeNs += indexTimer.nsecsElapsed();
        return false;
    }
    m_stats.indexTimeNs += indexTimer.nsecsElapsed();

    QElapsedTimer cleanupTimer;
    cleanupTimer.start();
    if (success && !optimizeDatabase(false)) {
        m_stats.cleanupTimeNs += cleanupTimer.nsecsElapsed();
        return false;
    }
    m_stats.cleanupTimeNs += cleanupTimer.nsecsElapsed();
    m_cleanupStats.finalTranslationsCount = translationCount();

    m_stats.totalTimeNs = totalTimer.nsecsElapsed();
    console << "[MediaWiki pre-processamento] resumo final" << Qt::endl;
    console << "Paginas lidas: " << m_stats.processedLines << Qt::endl;
    console << "Paginas administrativas puladas: " << skippedAdministrativePages << Qt::endl;
    console << "Verbetes aceitos: " << acceptedMainPages << Qt::endl;
    console << "Secoes de idioma de origem encontradas: " << sourceLanguageSections << Qt::endl;
    console << "Secoes ignoradas: " << ignoredSections << Qt::endl;
    console << "Blocos Translations aceitos: " << translationBlocks << Qt::endl;
    console << "Linhas reais de traducao: " << translationLines << Qt::endl;
    console << "Templates diretos aceitos: " << acceptedTemplates << Qt::endl;
    console << "Templates/linhas ignorados: " << ignoredTemplates << Qt::endl;
    console << "Secoes de idioma fonte fora da lista ignoradas: " << unsupportedSourceLanguageSections << Qt::endl;
    console << "Templates com idioma alvo fora da lista ignorados: " << unsupportedTargetLanguageTemplates << Qt::endl;
    flushRejectedDetails();
    maybeWriteRejectedDetailsSummary(mediaWikiFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, true);
    writeImportReport(QStringLiteral("MediaWiki"), mediaWikiFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, success);
    return success;
}

bool AtlasImporter::preprocessMediaWikiDataset(const QString &mediaWikiFilePath,
                                               const QString &sourceLang,
                                               const QString &targetLang,
                                               const QString &outputFilePath,
                                               QTextStream *progress)
{
    m_lastError.clear();
    QElapsedTimer totalTimer;
    totalTimer.start();
    const QString startedAt = AtlasReport::timestamp();

    QFile input(mediaWikiFilePath);
    if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = QStringLiteral("Nao foi possivel abrir arquivo MediaWiki: %1").arg(mediaWikiFilePath);
        return false;
    }

    const QFileInfo outputInfo(outputFilePath);
    if (!QDir().mkpath(outputInfo.dir().absolutePath())) {
        m_lastError = QStringLiteral("Nao foi possivel criar diretorio de saida pre-processada: %1").arg(outputInfo.dir().absolutePath());
        return false;
    }

    const QString normalizedSourceLang = m_languageNormalizer.normalize(sourceLang);
    const QString normalizedTargetLang = m_languageNormalizer.normalize(targetLang);
    if (!m_languageNormalizer.isSupported(normalizedSourceLang)
        || !m_languageNormalizer.isSupported(normalizedTargetLang)) {
        m_lastError = QStringLiteral("Par de idiomas MediaWiki fora da lista suportada e ignorado: %1 -> %2")
                          .arg(sourceLang, targetLang);
        return false;
    }

    const QString progressFilePath = hiddenPreprocessProgressPath(outputFilePath, QFileInfo(outputFilePath).fileName());
    qint64 resumePages = 0;
    qint64 resumeRows = 0;
    bool resumeCompleted = false;
    const bool hasProgress = readPreprocessProgress(progressFilePath, resumePages, resumeRows, resumeCompleted);
    if (resumeCompleted && QFileInfo::exists(outputFilePath)) {
        if (progress != nullptr) {
            *progress << QStringLiteral("[MediaWiki] ja concluido: %1").arg(terminalShortPath(outputFilePath)) << Qt::endl;
        }
        writePreprocessReport(QStringLiteral("MediaWiki"),
                              mediaWikiFilePath,
                              QString(),
                              normalizedSourceLang,
                              normalizedTargetLang,
                              QFileInfo(outputFilePath).absoluteFilePath(),
                              startedAt,
                              true,
                              QStringLiteral("Pulado porque o progresso existente esta concluido.\nArquivo de progresso: %1\nPaginas ja processadas: %2\nLinhas ja gravadas: %3")
                                  .arg(QFileInfo(progressFilePath).absoluteFilePath())
                                  .arg(resumePages)
                                  .arg(resumeRows));
        return true;
    }
    const bool resumeMode = hasProgress && resumePages > 0 && QFileInfo::exists(outputFilePath);

    QFile output(outputFilePath);
    const QIODevice::OpenMode outputMode = QIODevice::WriteOnly | QIODevice::Text
        | (resumeMode ? QIODevice::Append : QIODevice::Truncate);
    if (!output.open(outputMode)) {
        m_lastError = QStringLiteral("Nao foi possivel criar arquivo pre-processado: %1").arg(outputFilePath);
        return false;
    }

    static const QRegularExpression translationTemplateExpression(QStringLiteral(R"(\{\{\s*(t(?:\+check|\+|-check|-simple)?)\s*\|\s*([^|}]+)\s*\|\s*([^|}]+))"),
                                                                  QRegularExpression::CaseInsensitiveOption);

    QTextStream writer(&output);
    if (!resumeMode) {
        configureUtf8OutputStreamWithBom(writer);
        writer << "origem_limpa\tdestino_limpo\tgloss" << Qt::endl;
    } else {
        configureUtf8Stream(writer);
    }

    QElapsedTimer terminalTimer;
    terminalTimer.start();
    auto print = [&](const QString &message) {
        if (progress != nullptr) {
            *progress << message << terminalElapsedSuffix(terminalTimer) << Qt::endl;
        }
    };

    print(QStringLiteral("[MediaWiki] lendo: %1").arg(QFileInfo(mediaWikiFilePath).fileName()));
    print(QStringLiteral("[MediaWiki] saida: %1").arg(terminalShortPath(outputFilePath)));
    if (resumeMode) {
        print(QStringLiteral("[MediaWiki] retomando depois da pagina %1").arg(terminalCount(resumePages)));
    }

    QXmlStreamReader xml(&input);
    QString title;
    QString ns;
    QString text;
    bool inPage = false;
    qint64 pages = 0;
    qint64 skippedAdministrativePages = 0;
    qint64 acceptedMainPages = 0;
    qint64 sourceLanguageSections = 0;
    qint64 ignoredSections = 0;
    qint64 translationBlocks = 0;
    qint64 acceptedTemplates = resumeMode ? resumeRows : 0;
    qint64 ignoredTemplates = 0;
    qint64 unsupportedSourceLanguageSections = 0;
    qint64 unsupportedTargetLanguageTemplates = 0;
    QHash<QString, qint64> mediaWikiSkipCounts;
    QHash<QString, MediaWikiPreprocessRejectExample> mediaWikiSkipExamples;

    auto recordMediaWikiSkip = [&](const QString &reason,
                                   qint64 page,
                                   const QString &pageTitle,
                                   const QString &sourceCode,
                                   const QString &targetCode,
                                   const QString &lineText,
                                   const QString &translation,
                                   const QString &gloss) {
        ++mediaWikiSkipCounts[reason];
        if (!mediaWikiSkipExamples.contains(reason)) {
            mediaWikiSkipExamples.insert(reason,
                                        MediaWikiPreprocessRejectExample{
                                            reason,
                                            page,
                                            pageTitle.left(500),
                                            sourceCode,
                                            targetCode,
                                            lineText.left(500),
                                            translation.left(500),
                                            gloss.left(500)});
        }
    };

    auto mediaWikiDetails = [&](bool finalSnapshot) {
        QString details;
        QTextStream detailsStream(&details);
        detailsStream << "Snapshot: " << (finalSnapshot ? QStringLiteral("final") : QStringLiteral("parcial")) << Qt::endl;
        detailsStream << "Paginas lidas: " << pages << Qt::endl;
        detailsStream << "Paginas administrativas puladas: " << skippedAdministrativePages << Qt::endl;
        detailsStream << "Verbetes aceitos: " << acceptedMainPages << Qt::endl;
        detailsStream << "Secoes de idioma suportado encontradas: " << sourceLanguageSections << Qt::endl;
        detailsStream << "Secoes ignoradas: " << ignoredSections << Qt::endl;
        detailsStream << "Blocos Translations aceitos: " << translationBlocks << Qt::endl;
        detailsStream << "Templates diretos salvos: " << acceptedTemplates << Qt::endl;
        detailsStream << "Templates/linhas ignorados: " << ignoredTemplates << Qt::endl;
        detailsStream << "Secoes de idioma fonte fora da lista ignoradas: " << unsupportedSourceLanguageSections << Qt::endl;
        detailsStream << "Templates com idioma alvo fora da lista ignorados: " << unsupportedTargetLanguageTemplates << Qt::endl;
        detailsStream << "Arquivo de progresso: " << QFileInfo(progressFilePath).absoluteFilePath() << Qt::endl;
        detailsStream << "Retomado de checkpoint: " << (resumeMode ? QStringLiteral("sim") : QStringLiteral("nao")) << Qt::endl;
        detailsStream << "Tempo acumulado: " << AtlasReport::formatDuration(totalTimer.nsecsElapsed()) << Qt::endl;
        detailsStream << Qt::endl << "Motivos de descarte:" << Qt::endl;
        QStringList reasons = mediaWikiSkipCounts.keys();
        std::sort(reasons.begin(), reasons.end());
        if (reasons.isEmpty()) {
            detailsStream << "* nenhum: 0" << Qt::endl;
        } else {
            for (const QString &reason : reasons) {
                detailsStream << "* " << reason << ": " << mediaWikiSkipCounts.value(reason) << Qt::endl;
            }
        }
        return details;
    };

    auto writeMediaWikiDetailedReport = [&](bool finalSnapshot) {
        QFile detailedReport(outputInfo.dir().filePath(QStringLiteral("MediaWiki_Preprocess_Detalhado_Report.txt")));
        if (!detailedReport.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
            return;
        }
        QTextStream detailedStream(&detailedReport);
        configureUtf8Stream(detailedStream);
        detailedStream << "[MEDIAWIKI PRE-PROCESSAMENTO DETALHADO - "
                       << (finalSnapshot ? QStringLiteral("FINAL") : QStringLiteral("PARCIAL"))
                       << "]" << Qt::endl;
        detailedStream << "Arquivo: " << QFileInfo(mediaWikiFilePath).absoluteFilePath() << Qt::endl;
        detailedStream << "Saida: " << QFileInfo(outputFilePath).absoluteFilePath() << Qt::endl;
        detailedStream << "Paginas lidas ate agora: " << pages << Qt::endl;
        detailedStream << "Linhas gravadas ate agora: " << acceptedTemplates << Qt::endl;
        detailedStream << Qt::endl << "Exemplo por motivo que desqualificou traducao:" << Qt::endl;
        QStringList reasons = mediaWikiSkipCounts.keys();
        std::sort(reasons.begin(), reasons.end());
        if (reasons.isEmpty()) {
            detailedStream << "* nenhum motivo registrado ate agora" << Qt::endl;
        } else {
            for (const QString &reason : reasons) {
                const MediaWikiPreprocessRejectExample example = mediaWikiSkipExamples.value(reason);
                detailedStream << "------------------------------------------------------------" << Qt::endl;
                detailedStream << "Motivo: " << reason << Qt::endl;
                detailedStream << "Quantidade no arquivo: " << mediaWikiSkipCounts.value(reason) << Qt::endl;
                detailedStream << "Pagina aproximada: " << example.page << Qt::endl;
                detailedStream << "Titulo: " << example.title << Qt::endl;
                detailedStream << "Idioma origem: " << example.sourceLanguage << Qt::endl;
                detailedStream << "Idioma destino: " << example.targetLanguage << Qt::endl;
                detailedStream << "Linha wiki: " << example.line << Qt::endl;
                detailedStream << "Traducao capturada: " << example.translation << Qt::endl;
                detailedStream << "Gloss: " << example.gloss << Qt::endl;
            }
        }
        detailedStream << Qt::endl;
    };

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QStringView name = xml.name();
            if (name == QStringLiteral("page")) {
                inPage = true;
                title.clear();
                ns.clear();
                text.clear();
            } else if (inPage && name == QStringLiteral("title")) {
                title = xml.readElementText(QXmlStreamReader::SkipChildElements);
            } else if (inPage && name == QStringLiteral("ns")) {
                ns = xml.readElementText(QXmlStreamReader::SkipChildElements);
            } else if (inPage && name == QStringLiteral("text")) {
                text = xml.readElementText(QXmlStreamReader::IncludeChildElements);
            }
        } else if (xml.isEndElement() && xml.name() == QStringLiteral("page")) {
            inPage = false;
            ++pages;
            if (pages <= resumePages) {
                continue;
            }

            if (ns != QStringLiteral("0") || isAdministrativeMediaWikiTitle(title) || text.isEmpty()) {
                ++skippedAdministrativePages;
                recordMediaWikiSkip(QStringLiteral("pagina administrativa/sem texto"),
                                    pages,
                                    title,
                                    normalizedSourceLang,
                                    normalizedTargetLang,
                                    QString(),
                                    QString(),
                                    QString());
                continue;
            }

            ++acceptedMainPages;
            QString currentSourceLang;
            bool insideSourceLanguage = false;
            bool inTranslationsSection = false;
            QString currentTranslationGloss;
            const QStringList lines = text.split(QLatin1Char('\n'));
            for (const QString &line : lines) {
                const QString trimmedLine = line.trimmed();
                int headingLevel = 0;
                QString headingText;
                if (parseWikiHeading(trimmedLine, headingLevel, headingText)) {
                    if (headingLevel == 2) {
                        currentSourceLang = mediaWikiHeadingLanguageCode(headingText);
                        insideSourceLanguage = currentSourceLang == normalizedSourceLang;
                        if (insideSourceLanguage) {
                            ++sourceLanguageSections;
                        } else if (currentSourceLang.isEmpty()) {
                            ++unsupportedSourceLanguageSections;
                            recordMediaWikiSkip(QStringLiteral("secao de idioma fora da lista suportada"),
                                                pages,
                                                title,
                                                headingText,
                                                normalizedTargetLang,
                                                trimmedLine,
                                                QString(),
                                                currentTranslationGloss);
                        }
                        inTranslationsSection = false;
                        continue;
                    }
                    if (!insideSourceLanguage) {
                        continue;
                    }
                    if (isMediaWikiTranslationHeading(headingLevel, headingText)) {
                        inTranslationsSection = true;
                        currentTranslationGloss.clear();
                        ++translationBlocks;
                        continue;
                    }
                    if (isIgnoredMediaWikiSection(headingText)) {
                        ++ignoredSections;
                        recordMediaWikiSkip(QStringLiteral("secao ignorada"),
                                            pages,
                                            title,
                                            currentSourceLang,
                                            normalizedTargetLang,
                                            trimmedLine,
                                            QString(),
                                            currentTranslationGloss);
                    }
                    inTranslationsSection = false;
                    continue;
                }

                if (!insideSourceLanguage || !inTranslationsSection) {
                    continue;
                }
                QString parsedGloss;
                if (parseMediaWikiTranslationGlossLine(trimmedLine, parsedGloss)) {
                    currentTranslationGloss = cleanText(parsedGloss);
                    continue;
                }
                if (isIgnoredMediaWikiTranslationControlLine(trimmedLine)) {
                    ++ignoredTemplates;
                    recordMediaWikiSkip(QStringLiteral("linha de controle ignorada"),
                                        pages,
                                        title,
                                        currentSourceLang,
                                        normalizedTargetLang,
                                        trimmedLine,
                                        QString(),
                                        currentTranslationGloss);
                    continue;
                }
                if (!trimmedLine.startsWith(QLatin1Char('*'))) {
                    recordMediaWikiSkip(QStringLiteral("linha fora de lista de traducao"),
                                        pages,
                                        title,
                                        currentSourceLang,
                                        normalizedTargetLang,
                                        trimmedLine,
                                        QString(),
                                        currentTranslationGloss);
                    continue;
                }

                QRegularExpressionMatchIterator matches = translationTemplateExpression.globalMatch(trimmedLine);
                while (matches.hasNext()) {
                    const QRegularExpressionMatch match = matches.next();
                    const QString templateName = match.captured(1).trimmed().toLower();
                    const QString targetCode = mediaWikiTemplateLanguageCode(match.captured(2));
                    const QString targetText = cleanText(cleanWikiTranslationText(match.captured(3)));
                    if (targetCode.isEmpty()) {
                        ++unsupportedTargetLanguageTemplates;
                        recordMediaWikiSkip(QStringLiteral("template com idioma alvo fora da lista suportada"),
                                            pages,
                                            title,
                                            currentSourceLang,
                                            match.captured(2).trimmed(),
                                            trimmedLine,
                                            targetText,
                                            currentTranslationGloss);
                    }
                    if ((templateName == QStringLiteral("t-check") || templateName == QStringLiteral("t-simple")) && targetText.isEmpty()) {
                        ++ignoredTemplates;
                        recordMediaWikiSkip(QStringLiteral("template sem traducao"),
                                            pages,
                                            title,
                                            currentSourceLang,
                                            targetCode,
                                            trimmedLine,
                                            targetText,
                                            currentTranslationGloss);
                        continue;
                    }
                    if (targetCode != normalizedTargetLang || targetCode == currentSourceLang || targetText.isEmpty()) {
                        ++ignoredTemplates;
                        recordMediaWikiSkip(QStringLiteral("template fora do par solicitado"),
                                            pages,
                                            title,
                                            currentSourceLang,
                                            targetCode,
                                            trimmedLine,
                                            targetText,
                                            currentTranslationGloss);
                        continue;
                    }

                    QString safeTitle = title;
                    QString safeTarget = targetText;
                    QString safeGloss = currentTranslationGloss;
                    safeTitle.replace(QLatin1Char('\t'), QLatin1Char(' '));
                    safeTarget.replace(QLatin1Char('\t'), QLatin1Char(' '));
                    safeGloss.replace(QLatin1Char('\t'), QLatin1Char(' '));
                    writer << safeTitle << '\t'
                           << safeTarget << '\t'
                           << safeGloss << Qt::endl;
                    ++acceptedTemplates;
                }
            }

            if ((pages % ProgressInterval) == 0) {
                writer.flush();
                QString progressError;
                if (!writePreprocessProgress(progressFilePath,
                                             QStringLiteral("MediaWiki"),
                                             pages,
                                             acceptedTemplates,
                                             false,
                                             &progressError)) {
                    m_lastError = progressError;
                    return false;
                }
                print(QStringLiteral("[MediaWiki] Paginas=%1 Gravadas=%2 Ignoradas=%3")
                          .arg(terminalCount(pages),
                               terminalCount(acceptedTemplates),
                               terminalCount(ignoredTemplates + skippedAdministrativePages)));
                if ((pages % PreprocessReportInterval) == 0) {
                    writePreprocessReport(QStringLiteral("MediaWiki"),
                                          mediaWikiFilePath,
                                          QString(),
                                          normalizedSourceLang,
                                          normalizedTargetLang,
                                          QFileInfo(outputFilePath).absoluteFilePath(),
                                          startedAt,
                                          true,
                                          mediaWikiDetails(false));
                    writeMediaWikiDetailedReport(false);
                }
            }
        }
    }

    if (xml.hasError()) {
        m_lastError = QStringLiteral("Erro ao ler XML MediaWiki: %1").arg(xml.errorString());
        return false;
    }

    output.flush();
    QString progressError;
    if (!writePreprocessProgress(progressFilePath,
                                 QStringLiteral("MediaWiki"),
                                 pages,
                                 acceptedTemplates,
                                 true,
                                 &progressError)) {
        m_lastError = progressError;
        return false;
    }

    print(QStringLiteral("[MediaWiki] final Paginas=%1 Gravadas=%2 Ignoradas=%3")
              .arg(terminalCount(pages),
                   terminalCount(acceptedTemplates),
                   terminalCount(ignoredTemplates + skippedAdministrativePages)));

    writeMediaWikiDetailedReport(true);
    writePreprocessReport(QStringLiteral("MediaWiki"),
                          mediaWikiFilePath,
                          QString(),
                          normalizedSourceLang,
                          normalizedTargetLang,
                          QFileInfo(outputFilePath).absoluteFilePath(),
                          startedAt,
                          true,
                          mediaWikiDetails(true));
    return true;
}

bool AtlasImporter::preprocessMediaWikiDatasetAllLanguages(const QString &mediaWikiFilePath,
                                                           const QString &sourceLang,
                                                           const QString &outputDirectory,
                                                           QTextStream *progress)
{
    m_lastError.clear();
    QElapsedTimer totalTimer;
    totalTimer.start();
    const QString startedAt = AtlasReport::timestamp();

    QFile input(mediaWikiFilePath);
    if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = QStringLiteral("Nao foi possivel abrir arquivo MediaWiki: %1").arg(mediaWikiFilePath);
        return false;
    }

    if (!QDir().mkpath(outputDirectory)) {
        m_lastError = QStringLiteral("Nao foi possivel criar diretorio de saida pre-processada: %1").arg(outputDirectory);
        return false;
    }

    const QString inferredSourceLang = m_languageNormalizer.normalize(sourceLang);
    const QString sourceBaseName = QFileInfo(mediaWikiFilePath).completeBaseName();
    const QString progressFilePath = hiddenPreprocessProgressPath(mediaWikiFilePath,
                                                                  QStringLiteral("%1_all_supported.progress")
                                                                      .arg(safeMediaWikiFileToken(sourceBaseName)));
    qint64 resumePages = 0;
    qint64 resumeRows = 0;
    bool resumeCompleted = false;
    const bool hasProgress = readPreprocessProgress(progressFilePath, resumePages, resumeRows, resumeCompleted);
    if (resumeCompleted) {
        if (progress != nullptr) {
            *progress << QStringLiteral("[MediaWiki] ja concluido: %1").arg(QFileInfo(mediaWikiFilePath).fileName()) << Qt::endl;
        }
        writePreprocessReport(QStringLiteral("MediaWiki all supported languages"),
                              mediaWikiFilePath,
                              QString(),
                              inferredSourceLang,
                              QString(),
                              QDir(outputDirectory).absolutePath(),
                              startedAt,
                              true,
                              QStringLiteral("Pulado porque o progresso existente esta concluido.\nArquivo de progresso: %1\nPaginas ja processadas: %2\nLinhas ja gravadas: %3")
                                  .arg(QFileInfo(progressFilePath).absoluteFilePath())
                                  .arg(resumePages)
                                  .arg(resumeRows));
        return true;
    }
    const bool resumeMode = hasProgress && resumePages > 0;

    static const QRegularExpression translationTemplateExpression(QStringLiteral(R"(\{\{\s*(t(?:\+check|\+|-check|-simple)?)\s*\|\s*([^|}]+)\s*\|\s*([^|}]+))"),
                                                                  QRegularExpression::CaseInsensitiveOption);

    QElapsedTimer terminalTimer;
    terminalTimer.start();
    auto print = [&](const QString &message) {
        if (progress != nullptr) {
            *progress << message << terminalElapsedSuffix(terminalTimer) << Qt::endl;
        }
    };

    QHash<QString, QFile *> outputFiles;
    QHash<QString, QTextStream *> outputStreams;
    QHash<QString, qint64> savedByPair;
    QHash<QString, QString> outputPathsByPair;

    auto closeOutputs = [&]() {
        const QList<QString> keys = outputStreams.keys();
        for (const QString &key : keys) {
            QTextStream *stream = outputStreams.take(key);
            if (stream != nullptr) {
                stream->flush();
                delete stream;
            }
        }
        const QList<QString> fileKeys = outputFiles.keys();
        for (const QString &key : fileKeys) {
            QFile *file = outputFiles.take(key);
            if (file != nullptr) {
                file->close();
                delete file;
            }
        }
    };

    auto streamForPair = [&](const QString &sourceCode, const QString &targetCode) -> QTextStream * {
        const QString pairKey = QStringLiteral("%1\n%2").arg(sourceCode, targetCode);
        if (outputStreams.contains(pairKey)) {
            return outputStreams.value(pairKey);
        }

        const QString outputFilePath = QDir(outputDirectory).filePath(
            QStringLiteral("%1_%2-%3_preprocessed.tsv")
                .arg(safeMediaWikiFileToken(sourceBaseName),
                     safeMediaWikiFileToken(sourceCode),
                     safeMediaWikiFileToken(targetCode)));

        const bool appendExisting = resumeMode && QFileInfo::exists(outputFilePath);
        QFile *file = new QFile(outputFilePath);
        const QIODevice::OpenMode outputMode = QIODevice::WriteOnly | QIODevice::Text
            | (appendExisting ? QIODevice::Append : QIODevice::Truncate);
        if (!file->open(outputMode)) {
            m_lastError = QStringLiteral("Nao foi possivel criar arquivo pre-processado: %1").arg(outputFilePath);
            delete file;
            return nullptr;
        }

        QTextStream *stream = new QTextStream(file);
        if (appendExisting) {
            configureUtf8Stream(*stream);
        } else {
            configureUtf8OutputStreamWithBom(*stream);
            *stream << "origem_limpa\tdestino_limpo\tgloss" << Qt::endl;
        }

        outputFiles.insert(pairKey, file);
        outputStreams.insert(pairKey, stream);
        savedByPair.insert(pairKey, 0);
        outputPathsByPair.insert(pairKey, outputFilePath);
        return stream;
    };

    print(QStringLiteral("[MediaWiki] lendo: %1").arg(QFileInfo(mediaWikiFilePath).fileName()));
    print(QStringLiteral("[MediaWiki] saida: %1").arg(QDir(outputDirectory).dirName()));
    if (!inferredSourceLang.isEmpty()) {
        print(QStringLiteral("[MediaWiki] idioma inferido: %1").arg(inferredSourceLang));
    }
    if (resumeMode) {
        print(QStringLiteral("[MediaWiki] retomando depois da pagina %1").arg(terminalCount(resumePages)));
    }

    QXmlStreamReader xml(&input);
    QString title;
    QString ns;
    QString text;
    bool inPage = false;
    qint64 pages = 0;
    qint64 skippedAdministrativePages = 0;
    qint64 acceptedMainPages = 0;
    qint64 sourceLanguageSections = 0;
    qint64 ignoredSections = 0;
    qint64 translationBlocks = 0;
    qint64 acceptedTemplates = resumeMode ? resumeRows : 0;
    qint64 ignoredTemplates = 0;
    qint64 unsupportedSourceLanguageSections = 0;
    qint64 unsupportedTargetLanguageTemplates = 0;
    QHash<QString, qint64> mediaWikiSkipCounts;
    QHash<QString, MediaWikiPreprocessRejectExample> mediaWikiSkipExamples;

    auto recordMediaWikiSkip = [&](const QString &reason,
                                   qint64 page,
                                   const QString &pageTitle,
                                   const QString &sourceCode,
                                   const QString &targetCode,
                                   const QString &lineText,
                                   const QString &translation,
                                   const QString &gloss) {
        ++mediaWikiSkipCounts[reason];
        if (!mediaWikiSkipExamples.contains(reason)) {
            mediaWikiSkipExamples.insert(reason,
                                        MediaWikiPreprocessRejectExample{
                                            reason,
                                            page,
                                            pageTitle.left(500),
                                            sourceCode,
                                            targetCode,
                                            lineText.left(500),
                                            translation.left(500),
                                            gloss.left(500)});
        }
    };

    auto mediaWikiDetails = [&](bool finalSnapshot) {
        QString details;
        QTextStream detailsStream(&details);
        detailsStream << "Snapshot: " << (finalSnapshot ? QStringLiteral("final") : QStringLiteral("parcial")) << Qt::endl;
        detailsStream << "Paginas lidas: " << pages << Qt::endl;
        detailsStream << "Paginas administrativas puladas: " << skippedAdministrativePages << Qt::endl;
        detailsStream << "Verbetes aceitos: " << acceptedMainPages << Qt::endl;
        detailsStream << "Secoes de idioma suportado encontradas: " << sourceLanguageSections << Qt::endl;
        detailsStream << "Secoes ignoradas: " << ignoredSections << Qt::endl;
        detailsStream << "Blocos Translations aceitos: " << translationBlocks << Qt::endl;
        detailsStream << "Templates diretos salvos: " << acceptedTemplates << Qt::endl;
        detailsStream << "Pares gerados: " << outputStreams.size() << Qt::endl;
        detailsStream << "Templates/linhas ignorados: " << ignoredTemplates << Qt::endl;
        detailsStream << "Secoes de idioma fonte fora da lista ignoradas: " << unsupportedSourceLanguageSections << Qt::endl;
        detailsStream << "Templates com idioma alvo fora da lista ignorados: " << unsupportedTargetLanguageTemplates << Qt::endl;
        detailsStream << "Arquivo de progresso: " << QFileInfo(progressFilePath).absoluteFilePath() << Qt::endl;
        detailsStream << "Retomado de checkpoint: " << (resumeMode ? QStringLiteral("sim") : QStringLiteral("nao")) << Qt::endl;
        detailsStream << "Tempo acumulado: " << AtlasReport::formatDuration(totalTimer.nsecsElapsed()) << Qt::endl;
        detailsStream << Qt::endl << "Motivos de descarte:" << Qt::endl;
        QStringList reasons = mediaWikiSkipCounts.keys();
        std::sort(reasons.begin(), reasons.end());
        if (reasons.isEmpty()) {
            detailsStream << "* nenhum: 0" << Qt::endl;
        } else {
            for (const QString &reason : reasons) {
                detailsStream << "* " << reason << ": " << mediaWikiSkipCounts.value(reason) << Qt::endl;
            }
        }
        return details;
    };

    auto writeMediaWikiDetailedReport = [&](bool finalSnapshot) {
        QFile detailedReport(QDir(outputDirectory).filePath(QStringLiteral("MediaWiki_Preprocess_Detalhado_Report.txt")));
        if (!detailedReport.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
            return;
        }
        QTextStream detailedStream(&detailedReport);
        configureUtf8Stream(detailedStream);
        detailedStream << "[MEDIAWIKI PRE-PROCESSAMENTO TODOS DETALHADO - "
                       << (finalSnapshot ? QStringLiteral("FINAL") : QStringLiteral("PARCIAL"))
                       << "]" << Qt::endl;
        detailedStream << "Arquivo: " << QFileInfo(mediaWikiFilePath).absoluteFilePath() << Qt::endl;
        detailedStream << "Diretorio de saida: " << QDir(outputDirectory).absolutePath() << Qt::endl;
        detailedStream << "Paginas lidas ate agora: " << pages << Qt::endl;
        detailedStream << "Linhas gravadas ate agora: " << acceptedTemplates << Qt::endl;
        detailedStream << "Pares gerados ate agora: " << outputStreams.size() << Qt::endl;
        detailedStream << Qt::endl << "Exemplo por motivo que desqualificou traducao:" << Qt::endl;
        QStringList reasons = mediaWikiSkipCounts.keys();
        std::sort(reasons.begin(), reasons.end());
        if (reasons.isEmpty()) {
            detailedStream << "* nenhum motivo registrado ate agora" << Qt::endl;
        } else {
            for (const QString &reason : reasons) {
                const MediaWikiPreprocessRejectExample example = mediaWikiSkipExamples.value(reason);
                detailedStream << "------------------------------------------------------------" << Qt::endl;
                detailedStream << "Motivo: " << reason << Qt::endl;
                detailedStream << "Quantidade no arquivo: " << mediaWikiSkipCounts.value(reason) << Qt::endl;
                detailedStream << "Pagina aproximada: " << example.page << Qt::endl;
                detailedStream << "Titulo: " << example.title << Qt::endl;
                detailedStream << "Idioma origem: " << example.sourceLanguage << Qt::endl;
                detailedStream << "Idioma destino: " << example.targetLanguage << Qt::endl;
                detailedStream << "Linha wiki: " << example.line << Qt::endl;
                detailedStream << "Traducao capturada: " << example.translation << Qt::endl;
                detailedStream << "Gloss: " << example.gloss << Qt::endl;
            }
        }
        detailedStream << Qt::endl;
    };

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QStringView name = xml.name();
            if (name == QStringLiteral("page")) {
                inPage = true;
                title.clear();
                ns.clear();
                text.clear();
            } else if (inPage && name == QStringLiteral("title")) {
                title = xml.readElementText(QXmlStreamReader::SkipChildElements);
            } else if (inPage && name == QStringLiteral("ns")) {
                ns = xml.readElementText(QXmlStreamReader::SkipChildElements);
            } else if (inPage && name == QStringLiteral("text")) {
                text = xml.readElementText(QXmlStreamReader::IncludeChildElements);
            }
        } else if (xml.isEndElement() && xml.name() == QStringLiteral("page")) {
            inPage = false;
            ++pages;
            if (pages <= resumePages) {
                continue;
            }

            if (ns != QStringLiteral("0") || isAdministrativeMediaWikiTitle(title) || text.isEmpty()) {
                ++skippedAdministrativePages;
                recordMediaWikiSkip(QStringLiteral("pagina administrativa/sem texto"),
                                    pages,
                                    title,
                                    inferredSourceLang,
                                    QString(),
                                    QString(),
                                    QString(),
                                    QString());
                continue;
            }

            ++acceptedMainPages;
            QString currentSourceLang;
            bool insideSupportedSourceLanguage = false;
            bool inTranslationsSection = false;
            QString currentTranslationGloss;
            const QStringList lines = text.split(QLatin1Char('\n'));
            for (const QString &line : lines) {
                const QString trimmedLine = line.trimmed();
                int headingLevel = 0;
                QString headingText;
                if (parseWikiHeading(trimmedLine, headingLevel, headingText)) {
                    if (headingLevel == 2) {
                        currentSourceLang = mediaWikiHeadingLanguageCode(headingText);
                        insideSupportedSourceLanguage = !currentSourceLang.isEmpty();
                        if (insideSupportedSourceLanguage) {
                            ++sourceLanguageSections;
                        } else {
                            ++unsupportedSourceLanguageSections;
                            recordMediaWikiSkip(QStringLiteral("secao de idioma fora da lista suportada"),
                                                pages,
                                                title,
                                                headingText,
                                                QString(),
                                                trimmedLine,
                                                QString(),
                                                currentTranslationGloss);
                        }
                        inTranslationsSection = false;
                        continue;
                    }
                    if (!insideSupportedSourceLanguage) {
                        continue;
                    }
                    if (isMediaWikiTranslationHeading(headingLevel, headingText)) {
                        inTranslationsSection = true;
                        currentTranslationGloss.clear();
                        ++translationBlocks;
                        continue;
                    }
                    if (isIgnoredMediaWikiSection(headingText)) {
                        ++ignoredSections;
                        recordMediaWikiSkip(QStringLiteral("secao ignorada"),
                                            pages,
                                            title,
                                            currentSourceLang,
                                            QString(),
                                            trimmedLine,
                                            QString(),
                                            currentTranslationGloss);
                    }
                    inTranslationsSection = false;
                    continue;
                }

                if (!insideSupportedSourceLanguage || !inTranslationsSection) {
                    continue;
                }
                QString parsedGloss;
                if (parseMediaWikiTranslationGlossLine(trimmedLine, parsedGloss)) {
                    currentTranslationGloss = cleanText(parsedGloss);
                    continue;
                }
                if (isIgnoredMediaWikiTranslationControlLine(trimmedLine)) {
                    ++ignoredTemplates;
                    recordMediaWikiSkip(QStringLiteral("linha de controle ignorada"),
                                        pages,
                                        title,
                                        currentSourceLang,
                                        QString(),
                                        trimmedLine,
                                        QString(),
                                        currentTranslationGloss);
                    continue;
                }
                if (!trimmedLine.startsWith(QLatin1Char('*'))) {
                    recordMediaWikiSkip(QStringLiteral("linha fora de lista de traducao"),
                                        pages,
                                        title,
                                        currentSourceLang,
                                        QString(),
                                        trimmedLine,
                                        QString(),
                                        currentTranslationGloss);
                    continue;
                }

                QRegularExpressionMatchIterator matches = translationTemplateExpression.globalMatch(trimmedLine);
                while (matches.hasNext()) {
                    const QRegularExpressionMatch match = matches.next();
                    const QString templateName = match.captured(1).trimmed().toLower();
                    const QString targetCode = mediaWikiTemplateLanguageCode(match.captured(2));
                    const QString targetText = cleanText(cleanWikiTranslationText(match.captured(3)));
                    if (targetCode.isEmpty()) {
                        ++unsupportedTargetLanguageTemplates;
                        recordMediaWikiSkip(QStringLiteral("template com idioma alvo fora da lista suportada"),
                                            pages,
                                            title,
                                            currentSourceLang,
                                            match.captured(2).trimmed(),
                                            trimmedLine,
                                            targetText,
                                            currentTranslationGloss);
                    }
                    if ((templateName == QStringLiteral("t-check") || templateName == QStringLiteral("t-simple")) && targetText.isEmpty()) {
                        ++ignoredTemplates;
                        recordMediaWikiSkip(QStringLiteral("template sem traducao"),
                                            pages,
                                            title,
                                            currentSourceLang,
                                            targetCode,
                                            trimmedLine,
                                            targetText,
                                            currentTranslationGloss);
                        continue;
                    }
                    if (targetCode.isEmpty() || targetCode == currentSourceLang || targetText.isEmpty()) {
                        ++ignoredTemplates;
                        recordMediaWikiSkip(QStringLiteral("template fora do par valido"),
                                            pages,
                                            title,
                                            currentSourceLang,
                                            targetCode,
                                            trimmedLine,
                                            targetText,
                                            currentTranslationGloss);
                        continue;
                    }

                    QTextStream *stream = streamForPair(currentSourceLang, targetCode);
                    if (stream == nullptr) {
                        closeOutputs();
                        return false;
                    }

                    QString safeTitle = title;
                    QString safeTarget = targetText;
                    QString safeGloss = currentTranslationGloss;
                    safeTitle.replace(QLatin1Char('\t'), QLatin1Char(' '));
                    safeTarget.replace(QLatin1Char('\t'), QLatin1Char(' '));
                    safeGloss.replace(QLatin1Char('\t'), QLatin1Char(' '));
                    *stream << safeTitle << '\t'
                            << safeTarget << '\t'
                            << safeGloss << Qt::endl;
                    ++acceptedTemplates;
                    ++savedByPair[QStringLiteral("%1\n%2").arg(currentSourceLang, targetCode)];
                }
            }

            if ((pages % ProgressInterval) == 0) {
                for (QTextStream *stream : outputStreams) {
                    if (stream != nullptr) {
                        stream->flush();
                    }
                }
                QString progressError;
                if (!writePreprocessProgress(progressFilePath,
                                             QStringLiteral("MediaWiki all"),
                                             pages,
                                             acceptedTemplates,
                                             false,
                                             &progressError)) {
                    m_lastError = progressError;
                    closeOutputs();
                    return false;
                }
                print(QStringLiteral("[MediaWiki] Paginas=%1 Gravadas=%2 Pares=%3 Ignoradas=%4")
                          .arg(terminalCount(pages),
                               terminalCount(acceptedTemplates),
                               terminalCount(outputStreams.size()),
                               terminalCount(ignoredTemplates + skippedAdministrativePages)));
                if ((pages % PreprocessReportInterval) == 0) {
                    writePreprocessReport(QStringLiteral("MediaWiki todos idiomas suportados"),
                                          mediaWikiFilePath,
                                          QString(),
                                          inferredSourceLang,
                                          QString(),
                                          QDir(outputDirectory).absolutePath(),
                                          startedAt,
                                          true,
                                          mediaWikiDetails(false));
                    writeMediaWikiDetailedReport(false);
                }
            }
        }
    }

    if (xml.hasError()) {
        m_lastError = QStringLiteral("Erro ao ler XML MediaWiki: %1").arg(xml.errorString());
        closeOutputs();
        return false;
    }

    QString progressError;
    if (!writePreprocessProgress(progressFilePath,
                                 QStringLiteral("MediaWiki all"),
                                 pages,
                                 acceptedTemplates,
                                 true,
                                 &progressError)) {
        m_lastError = progressError;
        closeOutputs();
        return false;
    }

    print(QStringLiteral("[MediaWiki] final Paginas=%1 Gravadas=%2 Pares=%3 Ignoradas=%4")
              .arg(terminalCount(pages),
                   terminalCount(acceptedTemplates),
                   terminalCount(outputStreams.size()),
                   terminalCount(ignoredTemplates + skippedAdministrativePages)));
    QStringList generatedPairs = savedByPair.keys();
    std::sort(generatedPairs.begin(), generatedPairs.end());
    for (const QString &pair : generatedPairs) {
        const QStringList parts = pair.split(QLatin1Char('\n'));
        const QString label = parts.size() == 2
                                  ? QStringLiteral("%1 -> %2").arg(parts.at(0), parts.at(1))
                                  : pair;
    }

    closeOutputs();

    QString details = mediaWikiDetails(true);
    QTextStream detailsStream(&details, QIODevice::Append);
    detailsStream << Qt::endl << "Linhas por par gerado:" << Qt::endl;
    if (generatedPairs.isEmpty()) {
        detailsStream << "* nenhum: 0" << Qt::endl;
    } else {
        for (const QString &pair : generatedPairs) {
            const QStringList parts = pair.split(QLatin1Char('\n'));
            const QString label = parts.size() == 2
                                      ? QStringLiteral("%1 -> %2").arg(parts.at(0), parts.at(1))
                                      : pair;
            detailsStream << "* " << label << ": " << savedByPair.value(pair) << Qt::endl;
        }
    }
    writeMediaWikiDetailedReport(true);
    writePreprocessReport(QStringLiteral("MediaWiki todos idiomas suportados"),
                          mediaWikiFilePath,
                          QString(),
                          inferredSourceLang,
                          QString(),
                          QDir(outputDirectory).absolutePath(),
                          startedAt,
                          true,
                          details);
    return true;
}

bool AtlasImporter::importMediaWikiPreprocessedDataset(const QString &preprocessedFilePath,
                                                       const QString &sourceLang,
                                                       const QString &targetLang)
{
    m_stats = ImportStats();
    m_cleanupStats = CleanupStats();
    m_lastError.clear();
    m_currentImportSourceFile = preprocessedFilePath;
    m_rejectedDetailsBuffer.clear();
    m_bufferedPairs = 0;
    m_bufferSourceTexts.clear();
    m_bufferTranslatedTexts.clear();
    m_bufferSourceLangs.clear();
    m_bufferTargetLangs.clear();
    m_bufferSenseGlosses.clear();
    m_rejectedExamples.clear();
    m_lastRejectedSummarySnapshotProcessed = -1;

    QElapsedTimer totalTimer;
    totalTimer.start();
    const QString startedAt = AtlasReport::timestamp();
    const QString normalizedSourceLang = m_languageNormalizer.normalize(sourceLang);
    const QString normalizedTargetLang = m_languageNormalizer.normalize(targetLang);
    if (!m_languageNormalizer.isSupported(normalizedSourceLang)
        || !m_languageNormalizer.isSupported(normalizedTargetLang)) {
        m_lastError = QStringLiteral("Par de idiomas MediaWiki pre-processado fora da lista suportada e ignorado: %1 -> %2")
                          .arg(sourceLang, targetLang);
        return false;
    }

    QFile file(preprocessedFilePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = QStringLiteral("Nao foi possivel abrir arquivo MediaWiki pre-processado: %1").arg(preprocessedFilePath);
        return false;
    }

    QTextStream stream(&file);
    configureUtf8Stream(stream);

    if (!openDatabase() || !ensureSchema() || !ensureImportBufferTable()) {
        return false;
    }

    const QString currentImportKey = importKey(preprocessedFilePath,
                                               QStringLiteral("MediaWiki:preprocessed"),
                                               normalizedSourceLang,
                                               normalizedTargetLang);
    qint64 resumeFromLine = 0;
    bool completed = false;
    if (!loadProgress(currentImportKey, resumeFromLine, completed)) {
        return false;
    }
    if (completed) {
        m_stats.totalTimeNs = totalTimer.nsecsElapsed();
        writeRejectedDetailsSummary(preprocessedFilePath, normalizedSourceLang, normalizedTargetLang, startedAt);
        writeImportReport(QStringLiteral("MediaWiki preprocessed"), preprocessedFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, true);
        return true;
    }
    m_stats.resumedLines = resumeFromLine;
    maybeWriteRejectedDetailsSummary(preprocessedFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, true);

    QSqlQuery insertQuery(m_database);
    QSqlQuery updateQuery(m_database);
    if (!prepareInsertStatement(insertQuery) || !prepareUpdateFrequencyStatement(updateQuery)) {
        return false;
    }

    if (!m_database.transaction()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    bool success = true;
    qint64 pairsSinceCommit = 0;
    qint64 lastProgressPrinted = 0;
    qint64 acceptedRows = 0;

    while (!stream.atEnd()) {
        QElapsedTimer parsingTimer;
        parsingTimer.start();
        QString line = stream.readLine();
        if (m_stats.processedLines == 0 && line.startsWith(QChar(0xFEFF))) {
            line.remove(0, 1);
        }
        m_stats.parsingTimeNs += parsingTimer.nsecsElapsed();
        ++m_stats.processedLines;

        if (m_stats.processedLines <= resumeFromLine) {
            continue;
        }
        if (line.isEmpty()
            || line.startsWith(QLatin1Char('#'))
            || line == QStringLiteral("origem_limpa\tdestino_limpo\tgloss")) {
            continue;
        }

        const QStringList columns = line.split(QLatin1Char('\t'));
        if (columns.size() < 2) {
            recordDiscard(QStringLiteral("broken record"),
                          line,
                          QString(),
                          normalizedSourceLang,
                          normalizedTargetLang);
            continue;
        }

        const QString sourceText = cleanText(columns.at(0));
        const QString targetText = cleanText(columns.at(1));
        const QString senseGloss = columns.size() >= 3 ? cleanText(columns.mid(2).join(QStringLiteral("\t"))) : QString();
        ++acceptedRows;

        QElapsedTimer sqliteTimer;
        sqliteTimer.start();
        if (!insertPreparedPair(insertQuery, updateQuery, sourceText, targetText, normalizedSourceLang, normalizedTargetLang, senseGloss)
            || !insertPreparedPair(insertQuery, updateQuery, targetText, sourceText, normalizedTargetLang, normalizedSourceLang, senseGloss)) {
            m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
            success = false;
            break;
        }
        m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
        pairsSinceCommit += 2;

        if (pairsSinceCommit >= CommitInterval) {
            sqliteTimer.restart();
            if (!mergeBufferedPairs()) {
                m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
                success = false;
                break;
            }
            m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
            if (!saveProgress(currentImportKey,
                              preprocessedFilePath,
                              QStringLiteral("MediaWiki:preprocessed"),
                              normalizedSourceLang,
                              normalizedTargetLang,
                              m_stats.processedLines,
                              false)) {
                success = false;
                break;
            }
            if (!m_database.commit()) {
                m_lastError = m_database.lastError().text();
                ++m_stats.errorCount;
                success = false;
                break;
            }
            if (!m_database.transaction()) {
                m_lastError = m_database.lastError().text();
                ++m_stats.errorCount;
                success = false;
                break;
            }
            pairsSinceCommit = 0;
        }

        if ((m_stats.processedLines % ProgressInterval) == 0
            && m_stats.processedLines != lastProgressPrinted) {
            printProgress();
            maybeWriteRejectedDetailsSummary(preprocessedFilePath, normalizedSourceLang, normalizedTargetLang, startedAt);
            lastProgressPrinted = m_stats.processedLines;
        }
    }

    if (success) {
        QElapsedTimer sqliteTimer;
        sqliteTimer.start();
        success = mergeBufferedPairs();
        m_stats.sqliteTimeNs += sqliteTimer.nsecsElapsed();
    }

    if (success) {
        success = saveProgress(currentImportKey,
                               preprocessedFilePath,
                               QStringLiteral("MediaWiki:preprocessed"),
                               normalizedSourceLang,
                               normalizedTargetLang,
                               m_stats.processedLines,
                               true);
    }

    if (success && !m_database.commit()) {
        m_lastError = m_database.lastError().text();
        ++m_stats.errorCount;
        success = false;
    } else if (!success) {
        m_database.rollback();
    }

    QElapsedTimer indexTimer;
    indexTimer.start();
    if (success && !ensureIndexes()) {
        m_stats.indexTimeNs += indexTimer.nsecsElapsed();
        return false;
    }
    m_stats.indexTimeNs += indexTimer.nsecsElapsed();

    QElapsedTimer cleanupTimer;
    cleanupTimer.start();
    if (success && !optimizeDatabase(false)) {
        m_stats.cleanupTimeNs += cleanupTimer.nsecsElapsed();
        return false;
    }
    m_stats.cleanupTimeNs += cleanupTimer.nsecsElapsed();
    m_cleanupStats.finalTranslationsCount = translationCount();
    m_stats.totalTimeNs = totalTimer.nsecsElapsed();

    QTextStream console(stdout);
    configureUtf8Stream(console);
    console << "[MediaWiki pre-processado] resumo final" << Qt::endl;
    console << "Linhas lidas: " << m_stats.processedLines << Qt::endl;
    console << "Linhas aceitas: " << acceptedRows << Qt::endl;
    flushRejectedDetails();
    maybeWriteRejectedDetailsSummary(preprocessedFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, true);
    writeImportReport(QStringLiteral("MediaWiki preprocessed"), preprocessedFilePath, normalizedSourceLang, normalizedTargetLang, startedAt, success);
    return success;
}

AtlasImporter::ImportStats AtlasImporter::stats() const
{
    return m_stats;
}

AtlasImporter::CleanupStats AtlasImporter::cleanupStats() const
{
    return m_cleanupStats;
}

QString AtlasImporter::lastError() const
{
    return m_lastError;
}

QString AtlasImporter::databasePath() const
{
    return m_databasePath;
}

void AtlasImporter::setDebugRejectedDetailsEnabled(bool enabled)
{
    m_debugRejectedDetails = enabled;
}

bool AtlasImporter::debugRejectedDetailsEnabled() const
{
    return m_debugRejectedDetails;
}

void AtlasImporter::writeImportReport(const QString &sourceType,
                                      const QString &sourceFilePath,
                                      const QString &sourceLang,
                                      const QString &targetLang,
                                      const QString &startedAt,
                                      bool success)
{
    QElapsedTimer reportTimer;
    reportTimer.start();

    QString report;
    QTextStream stream(&report);
    stream << "[IMPORTACAO INICIO]" << Qt::endl;
    stream << "Iniciado em: " << startedAt << Qt::endl;
    stream << "Finalizado em: " << AtlasReport::timestamp() << Qt::endl;
    stream << "Tipo de fonte: " << sourceType << Qt::endl;
    stream << "Arquivo: " << QFileInfo(sourceFilePath).fileName() << Qt::endl;
    stream << "Caminho completo: " << QFileInfo(sourceFilePath).absoluteFilePath() << Qt::endl;
    stream << "Par: " << sourceLang << " <-> " << targetLang << Qt::endl;
    stream << "Banco gerado/usado: " << m_databasePath << Qt::endl;
    stream << "Status: " << (success ? QStringLiteral("sucesso") : QStringLiteral("falha")) << Qt::endl;
    stream << "Detalhes individuais de rejeicao: "
           << (m_debugRejectedDetails ? QStringLiteral("ativados") : QStringLiteral("desativados")) << Qt::endl;
    if (!success && !m_lastError.isEmpty()) {
        stream << "Erro: " << m_lastError << Qt::endl;
    }

    stream << Qt::endl;
    stream << "Registros analisados: " << m_stats.processedLines << Qt::endl;
    stream << "Inseridos: " << m_stats.insertedLines << Qt::endl;
    stream << "Frequencias atualizadas: " << m_stats.updatedFrequencyLines << Qt::endl;
    stream << "Ignorados: " << m_stats.ignoredLines << Qt::endl;
    stream << "Avisos: " << m_stats.qualityWarningsLogged << Qt::endl;
    stream << "Erros: " << m_stats.errorCount << Qt::endl;
    stream << "Duplicados detectados: " << m_stats.duplicateLines << Qt::endl;
    stream << "Linhas descartadas: " << m_stats.discardedLines << Qt::endl;
    stream << "Chamadas de progresso: " << m_stats.progressPrintCount << Qt::endl;
    stream << "Limpezas do banco executadas: " << m_stats.cleanupRunCount << Qt::endl;
    stream << "Frases divididas por pontuacao estrutural: " << m_stats.punctuationSplitPairs << Qt::endl;
    stream << "Segmentos de pontuacao estrutural gerados: " << m_stats.punctuationSplitSegments << Qt::endl;
    stream << "Frases nao divididas por pontuacao incompativel: " << m_stats.punctuationSplitSkipped << Qt::endl;
    stream << "Grupos de alternativas com barra expandidos: " << m_stats.slashAlternativesExpanded << Qt::endl;
    stream << "Candidatos de alternativas com barra gerados: " << m_stats.slashAlternativeCandidates << Qt::endl;

    stream << Qt::endl;
    stream << "Motivos de descarte:" << Qt::endl;
    if (m_stats.discardReasons.isEmpty()) {
        stream << "* nenhum: 0" << Qt::endl;
    } else {
        QStringList reasons = m_stats.discardReasons.keys();
        std::sort(reasons.begin(), reasons.end());
        for (const QString &reason : reasons) {
            stream << "* " << discardReasonLabel(reason) << ": " << m_stats.discardReasons.value(reason) << Qt::endl;
            const QStringList samples = m_stats.discardSamples.value(reason);
            for (const QString &sample : samples) {
                stream << "  Exemplo: " << sample << Qt::endl;
            }
        }
    }

    stream << Qt::endl;
    stream << "Relatorio de divisao por pontuacao estrutural:" << Qt::endl;
    if (m_stats.punctuationSplitPairs == 0 && m_stats.punctuationSplitSkipped == 0) {
        stream << "* nenhum: 0" << Qt::endl;
    } else {
        stream << "* frases divididas: " << m_stats.punctuationSplitPairs << Qt::endl;
        stream << "* segmentos gerados: " << m_stats.punctuationSplitSegments << Qt::endl;
        if (m_stats.punctuationSplitSkipReasons.isEmpty()) {
            stream << "* puladas: 0" << Qt::endl;
        } else {
            QStringList reasons = m_stats.punctuationSplitSkipReasons.keys();
            std::sort(reasons.begin(), reasons.end());
            for (const QString &reason : reasons) {
                stream << "* puladas - " << reason << ": "
                       << m_stats.punctuationSplitSkipReasons.value(reason) << Qt::endl;
                const QStringList samples = m_stats.punctuationSplitSamples.value(reason);
                for (const QString &sample : samples) {
                    stream << "  Exemplo: " << sample << Qt::endl;
                }
            }
        }
    }

    stream << Qt::endl;
    stream << "Relatorio de alternativas com barra:" << Qt::endl;
    if (m_stats.slashAlternativesExpanded == 0) {
        stream << "* nenhum: 0" << Qt::endl;
    } else {
        stream << "* Motivo: alternativas com barra expandidas" << Qt::endl;
        stream << "* Grupos: " << m_stats.slashAlternativesExpanded << Qt::endl;
        stream << "* Candidatos gerados: " << m_stats.slashAlternativeCandidates << Qt::endl;
        for (const QString &sample : m_stats.slashAlternativeSamples) {
            stream << "  " << sample << Qt::endl;
        }
    }

    stream << Qt::endl;
    stream << "Tempos:" << Qt::endl;
    const qint64 reportBuildTimeNs = reportTimer.nsecsElapsed();
    stream << "* leitura/parse: " << AtlasReport::formatDuration(m_stats.parsingTimeNs) << Qt::endl;
    stream << "* insercoes SQLite: " << AtlasReport::formatDuration(m_stats.sqliteTimeNs) << Qt::endl;
    stream << "* indices: " << AtlasReport::formatDuration(m_stats.indexTimeNs) << Qt::endl;
    stream << "* limpeza/manutencao: " << AtlasReport::formatDuration(m_stats.cleanupTimeNs) << Qt::endl;
    stream << "* logs do terminal: " << AtlasReport::formatDuration(m_stats.logTimeNs) << Qt::endl;
    stream << "* escrita de relatorio: " << AtlasReport::formatDuration(m_stats.reportTimeNs + reportBuildTimeNs) << Qt::endl;
    stream << "* total: " << AtlasReport::formatDuration(m_stats.totalTimeNs) << Qt::endl;
    stream << "[IMPORTACAO FIM]" << Qt::endl;

    AtlasReport::append(AtlasReport::File::Import, report);
    m_stats.reportTimeNs += reportTimer.nsecsElapsed();
}

void AtlasImporter::writePreprocessReport(const QString &sourceType,
                                          const QString &sourceFilePath,
                                          const QString &targetFilePath,
                                          const QString &sourceLang,
                                          const QString &targetLang,
                                          const QString &outputPath,
                                          const QString &startedAt,
                                          bool success,
                                          const QString &details)
{
    QElapsedTimer reportTimer;
    reportTimer.start();

    QString report;
    QTextStream stream(&report);
    stream << "[PRE-PROCESSAMENTO INICIO]" << Qt::endl;
    stream << "Iniciado em: " << startedAt << Qt::endl;
    stream << "Finalizado em: " << AtlasReport::timestamp() << Qt::endl;
    stream << "Tipo de fonte: " << sourceType << Qt::endl;
    stream << "Arquivo de origem: " << QFileInfo(sourceFilePath).fileName() << Qt::endl;
    stream << "Caminho completo da origem: " << QFileInfo(sourceFilePath).absoluteFilePath() << Qt::endl;
    if (!targetFilePath.isEmpty()) {
        stream << "Arquivo de destino: " << QFileInfo(targetFilePath).fileName() << Qt::endl;
        stream << "Caminho completo do destino: " << QFileInfo(targetFilePath).absoluteFilePath() << Qt::endl;
    }
    stream << "Par: " << (sourceLang.isEmpty() ? QStringLiteral("automatico/todos suportados") : sourceLang)
           << " -> " << (targetLang.isEmpty() ? QStringLiteral("todos suportados") : targetLang) << Qt::endl;
    stream << "Saida: " << outputPath << Qt::endl;
    stream << "Status: " << (success ? QStringLiteral("sucesso") : QStringLiteral("falha")) << Qt::endl;
    if (!success && !m_lastError.isEmpty()) {
        stream << "Erro: " << m_lastError << Qt::endl;
    }

    if (!details.trimmed().isEmpty()) {
        stream << Qt::endl;
        stream << "Detalhes:" << Qt::endl;
        stream << details.trimmed() << Qt::endl;
    }

    const qint64 reportBuildTimeNs = reportTimer.nsecsElapsed();
    stream << Qt::endl;
    stream << "Tempos:" << Qt::endl;
    stream << "* escrita de relatorio: " << AtlasReport::formatDuration(reportBuildTimeNs) << Qt::endl;
    stream << "[PRE-PROCESSAMENTO FIM]" << Qt::endl;

    AtlasReport::append(AtlasReport::File::Preprocess, report);
}

bool AtlasImporter::openDatabase()
{
    if (m_database.isOpen()) {
        return true;
    }

    if (m_databasePath != QStringLiteral(":memory:")) {
        const QFileInfo databaseInfo(m_databasePath);
        QDir directory = databaseInfo.dir();
        if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
            m_lastError = QStringLiteral("Nao foi possivel criar o diretorio do banco.");
            return false;
        }
    }

    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(m_databasePath);

    if (!m_database.open()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    QSqlQuery pragmas(m_database);
    pragmas.exec(QStringLiteral("PRAGMA busy_timeout = 10000"));
    pragmas.finish();
    pragmas.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    pragmas.finish();
    pragmas.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    pragmas.finish();
    pragmas.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    pragmas.finish();

    return true;
}

bool AtlasImporter::ensureSchema()
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(R"(
        CREATE TABLE IF NOT EXISTS translations (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            source_text TEXT NOT NULL,
            translated_text TEXT NOT NULL,
            source_lang TEXT NOT NULL,
            target_lang TEXT NOT NULL,
            sense_gloss TEXT NOT NULL DEFAULT '',
            frequency INTEGER NOT NULL DEFAULT 1
        )
    )"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    return removeLegacyUniqueConstraint()
        && ensureFrequencySchema()
        && ensureTranslationMetadataSchema()
        && ensureIndexes()
        && ensureProgressTable()
        && ensureQualityLogTable();
}

bool AtlasImporter::ensureIndexes()
{
    const QStringList statements = {
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_translations_source_text ON translations(source_text)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_translations_source_lang ON translations(source_lang)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_translations_target_lang ON translations(target_lang)"),
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_translations_pair_unique ON translations(source_text, translated_text, source_lang, target_lang, sense_gloss)")
    };

    for (const QString &statement : statements) {
        QSqlQuery query(m_database);
        if (!query.exec(statement)) {
            m_lastError = query.lastError().text();
            return false;
        }
        query.finish();
    }

    return ensureLookupIndex();
}

bool AtlasImporter::ensureLookupIndex()
{
    QStringList indexedColumns;
    QSqlQuery indexInfo(m_database);
    if (indexInfo.exec(QStringLiteral("PRAGMA index_info(idx_translations_lookup)"))) {
        while (indexInfo.next()) {
            indexedColumns.append(indexInfo.value(2).toString());
        }
        indexInfo.finish();
    } else {
        m_lastError = indexInfo.lastError().text();
        return false;
    }

    const QStringList expectedColumns = {
        QStringLiteral("source_lang"),
        QStringLiteral("target_lang"),
        QStringLiteral("source_text"),
        QStringLiteral("frequency")
    };

    if (!indexedColumns.isEmpty() && indexedColumns != expectedColumns) {
        QSqlQuery dropQuery(m_database);
        if (!dropQuery.exec(QStringLiteral("DROP INDEX idx_translations_lookup"))) {
            m_lastError = dropQuery.lastError().text();
            return false;
        }
        dropQuery.finish();
        indexedColumns.clear();
    }

    if (indexedColumns.isEmpty()) {
        QSqlQuery createQuery(m_database);
        if (!createQuery.exec(QStringLiteral(
                "CREATE INDEX idx_translations_lookup ON translations(source_lang, target_lang, source_text, frequency DESC)"))) {
            m_lastError = createQuery.lastError().text();
            return false;
        }
        createQuery.finish();
    }

    return true;
}

bool AtlasImporter::ensureFrequencySchema()
{
    bool hasFrequency = false;
    bool hasSenseGloss = false;
    QSqlQuery tableInfo(m_database);
    if (!tableInfo.exec(QStringLiteral("PRAGMA table_info(translations)"))) {
        m_lastError = tableInfo.lastError().text();
        return false;
    }
    while (tableInfo.next()) {
        const QString columnName = tableInfo.value(1).toString();
        if (columnName == QStringLiteral("frequency")) {
            hasFrequency = true;
        } else if (columnName == QStringLiteral("sense_gloss")) {
            hasSenseGloss = true;
        }
    }
    tableInfo.finish();

    bool hasPairUnique = false;
    QSqlQuery indexList(m_database);
    if (!indexList.exec(QStringLiteral("PRAGMA index_list(translations)"))) {
        m_lastError = indexList.lastError().text();
        return false;
    }
    while (indexList.next()) {
        if (indexList.value(1).toString() == QStringLiteral("idx_translations_pair_unique")) {
            hasPairUnique = true;
            break;
        }
    }
    indexList.finish();

    if (hasFrequency && hasPairUnique) {
        return true;
    }

    QSqlQuery duplicateGroupsQuery(m_database);
    qint64 duplicateRows = 0;
    if (duplicateGroupsQuery.exec(QStringLiteral(R"(
        SELECT COALESCE(SUM(group_count - 1), 0)
        FROM (
            SELECT COUNT(*) AS group_count
            FROM translations
            GROUP BY source_text, translated_text, source_lang, target_lang%1
            HAVING COUNT(*) > 1
        )
    )").arg(hasSenseGloss ? QStringLiteral(", sense_gloss") : QString())) && duplicateGroupsQuery.next()) {
        duplicateRows = duplicateGroupsQuery.value(0).toLongLong();
        m_cleanupStats.removedDuplicates += duplicateRows;
    }
    duplicateGroupsQuery.finish();

    if (hasFrequency && duplicateRows == 0) {
        return true;
    }

    if (!m_database.transaction()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    QSqlQuery query(m_database);
    const QString frequencyExpression = hasFrequency ? QStringLiteral("COALESCE(frequency, 1)") : QStringLiteral("1");
    const QString senseGlossColumnDefinition = hasSenseGloss
        ? QStringLiteral("sense_gloss TEXT NOT NULL DEFAULT '',")
        : QString();
    const QString senseGlossInsertColumn = hasSenseGloss ? QStringLiteral(", sense_gloss") : QString();
    const QString senseGlossSelectColumn = hasSenseGloss ? QStringLiteral(", sense_gloss") : QString();
    const QString senseGlossGroupColumn = hasSenseGloss ? QStringLiteral(", sense_gloss") : QString();
    const QStringList migrationStatements = {
        QStringLiteral("DROP TABLE IF EXISTS translations_frequency_migration"),
        QStringLiteral(R"(
            CREATE TABLE translations_frequency_migration (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                source_text TEXT NOT NULL,
                translated_text TEXT NOT NULL,
                source_lang TEXT NOT NULL,
                target_lang TEXT NOT NULL,
                %1
                frequency INTEGER NOT NULL DEFAULT 1
            )
        )").arg(senseGlossColumnDefinition),
        QStringLiteral(R"(
            INSERT INTO translations_frequency_migration (source_text, translated_text, source_lang, target_lang%1, frequency)
            SELECT source_text, translated_text, source_lang, target_lang%2, SUM(%3)
            FROM translations
            GROUP BY source_text, translated_text, source_lang, target_lang%4
        )").arg(senseGlossInsertColumn,
                senseGlossSelectColumn,
                frequencyExpression,
                senseGlossGroupColumn),
        QStringLiteral("DROP TABLE translations"),
        QStringLiteral("ALTER TABLE translations_frequency_migration RENAME TO translations")
    };

    for (const QString &statement : migrationStatements) {
        if (!query.exec(statement)) {
            m_lastError = query.lastError().text();
            m_database.rollback();
            return false;
        }
        query.finish();
    }

    if (!m_database.commit()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    return true;
}

bool AtlasImporter::ensureTranslationMetadataSchema()
{
    bool hasSenseGloss = false;
    QSqlQuery tableInfo(m_database);
    if (!tableInfo.exec(QStringLiteral("PRAGMA table_info(translations)"))) {
        m_lastError = tableInfo.lastError().text();
        return false;
    }
    while (tableInfo.next()) {
        if (tableInfo.value(1).toString() == QStringLiteral("sense_gloss")) {
            hasSenseGloss = true;
            break;
        }
    }
    tableInfo.finish();

    if (!hasSenseGloss) {
        QSqlQuery alterQuery(m_database);
        if (!alterQuery.exec(QStringLiteral("ALTER TABLE translations ADD COLUMN sense_gloss TEXT NOT NULL DEFAULT ''"))) {
            m_lastError = alterQuery.lastError().text();
            return false;
        }
        alterQuery.finish();
    }

    QStringList indexedColumns;
    QSqlQuery indexInfo(m_database);
    if (indexInfo.exec(QStringLiteral("PRAGMA index_info(idx_translations_pair_unique)"))) {
        while (indexInfo.next()) {
            indexedColumns.append(indexInfo.value(2).toString());
        }
        indexInfo.finish();
    } else {
        m_lastError = indexInfo.lastError().text();
        return false;
    }

    const QStringList expectedColumns = {
        QStringLiteral("source_text"),
        QStringLiteral("translated_text"),
        QStringLiteral("source_lang"),
        QStringLiteral("target_lang"),
        QStringLiteral("sense_gloss")
    };

    if (!indexedColumns.isEmpty() && indexedColumns != expectedColumns) {
        QSqlQuery dropQuery(m_database);
        if (!dropQuery.exec(QStringLiteral("DROP INDEX idx_translations_pair_unique"))) {
            m_lastError = dropQuery.lastError().text();
            return false;
        }
        dropQuery.finish();
    }

    return true;
}

bool AtlasImporter::removeLegacyUniqueConstraint()
{
    QSqlQuery schemaQuery(m_database);
    schemaQuery.prepare(QStringLiteral("SELECT sql FROM sqlite_master WHERE type = 'table' AND name = :name"));
    schemaQuery.bindValue(QStringLiteral(":name"), QStringLiteral("translations"));
    if (!schemaQuery.exec() || !schemaQuery.next()) {
        m_lastError = schemaQuery.lastError().text();
        return false;
    }

    QString tableSql = schemaQuery.value(0).toString().toLower();
    tableSql.remove(QChar::Space);
    tableSql.remove(QChar::Tabulation);
    tableSql.remove(QChar::LineFeed);
    tableSql.remove(QChar::CarriageReturn);
    schemaQuery.finish();

    if (!tableSql.contains(QStringLiteral("unique(source_text,source_lang,target_lang)"))) {
        return true;
    }

    if (!m_database.transaction()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    QSqlQuery query(m_database);
    const QStringList migrationStatements = {
        QStringLiteral("DROP TABLE IF EXISTS translations_without_unique"),
        QStringLiteral(R"(
            CREATE TABLE translations_without_unique (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                source_text TEXT NOT NULL,
                translated_text TEXT NOT NULL,
                source_lang TEXT NOT NULL,
                target_lang TEXT NOT NULL
            )
        )"),
        QStringLiteral(R"(
            INSERT INTO translations_without_unique (id, source_text, translated_text, source_lang, target_lang)
            SELECT id, source_text, translated_text, source_lang, target_lang
            FROM translations
        )"),
        QStringLiteral("DROP TABLE translations"),
        QStringLiteral("ALTER TABLE translations_without_unique RENAME TO translations")
    };

    for (const QString &statement : migrationStatements) {
        if (!query.exec(statement)) {
            m_lastError = query.lastError().text();
            m_database.rollback();
            return false;
        }
    }

    if (!m_database.commit()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    return true;
}

bool AtlasImporter::ensureProgressTable()
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(R"(
        CREATE TABLE IF NOT EXISTS import_progress (
            import_key TEXT PRIMARY KEY,
            source_file TEXT NOT NULL,
            target_file TEXT NOT NULL,
            source_lang TEXT NOT NULL,
            target_lang TEXT NOT NULL,
            processed_lines INTEGER NOT NULL DEFAULT 0,
            completed INTEGER NOT NULL DEFAULT 0,
            updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
        )
    )"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    return true;
}

bool AtlasImporter::prepareInsertStatement(QSqlQuery &query)
{
    if (!query.prepare(QStringLiteral(R"(
        INSERT INTO temp.atlas_import_pairs (source_text, translated_text, source_lang, target_lang, sense_gloss)
        VALUES (?, ?, ?, ?, ?)
    )"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    return true;
}

bool AtlasImporter::prepareUpdateFrequencyStatement(QSqlQuery &query)
{
    if (!query.prepare(QStringLiteral(R"(
        UPDATE translations
        SET frequency = frequency + 1
        WHERE source_text = ?
          AND translated_text = ?
          AND source_lang = ?
          AND target_lang = ?
          AND sense_gloss = ?
    )"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    return true;
}

bool AtlasImporter::ensureQualityLogTable()
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(R"(
        CREATE TABLE IF NOT EXISTS import_quality_log (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            source_name TEXT NOT NULL,
            category TEXT NOT NULL,
            detail TEXT NOT NULL,
            position INTEGER NOT NULL DEFAULT 0,
            created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
        )
    )"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    return true;
}

bool AtlasImporter::ensureImportBufferTable()
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(R"(
            CREATE TEMP TABLE IF NOT EXISTS atlas_import_pairs (
                source_text TEXT NOT NULL,
                translated_text TEXT NOT NULL,
                source_lang TEXT NOT NULL,
                target_lang TEXT NOT NULL,
                sense_gloss TEXT NOT NULL DEFAULT ''
            )
    )"))) {
        m_lastError = query.lastError().text();
        return false;
    }
    query.finish();

    if (!query.exec(QStringLiteral("DELETE FROM temp.atlas_import_pairs"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    m_bufferedPairs = 0;
    m_bufferSourceTexts.clear();
    m_bufferTranslatedTexts.clear();
    m_bufferSourceLangs.clear();
    m_bufferTargetLangs.clear();
    m_bufferSenseGlosses.clear();
    return true;
}

bool AtlasImporter::mergeBufferedPairs()
{
    if (m_bufferedPairs <= 0) {
        return true;
    }

    QSqlQuery query(m_database);
    if (!query.prepare(QStringLiteral(R"(
        INSERT INTO temp.atlas_import_pairs (source_text, translated_text, source_lang, target_lang, sense_gloss)
        VALUES (?, ?, ?, ?, ?)
    )"))) {
        m_lastError = query.lastError().text();
        return false;
    }
    query.addBindValue(m_bufferSourceTexts);
    query.addBindValue(m_bufferTranslatedTexts);
    query.addBindValue(m_bufferSourceLangs);
    query.addBindValue(m_bufferTargetLangs);
    query.addBindValue(m_bufferSenseGlosses);
    if (!query.execBatch()) {
        m_lastError = query.lastError().text();
        return false;
    }
    query.finish();

    qint64 uniqueGroups = 0;
    if (!query.exec(QStringLiteral(R"(
        SELECT COUNT(*)
        FROM (
            SELECT source_text, translated_text, source_lang, target_lang, sense_gloss
            FROM temp.atlas_import_pairs
            GROUP BY source_text, translated_text, source_lang, target_lang, sense_gloss
        )
    )")) || !query.next()) {
        m_lastError = query.lastError().text();
        return false;
    }
    uniqueGroups = query.value(0).toLongLong();
    query.finish();

    qint64 insertedGroups = 0;
    if (!query.exec(QStringLiteral(R"(
        SELECT COUNT(*)
        FROM (
            SELECT source_text, translated_text, source_lang, target_lang, sense_gloss
            FROM temp.atlas_import_pairs
            GROUP BY source_text, translated_text, source_lang, target_lang, sense_gloss
        ) AS pairs
        WHERE NOT EXISTS (
            SELECT 1
            FROM translations
            WHERE translations.source_text = pairs.source_text
              AND translations.translated_text = pairs.translated_text
              AND translations.source_lang = pairs.source_lang
              AND translations.target_lang = pairs.target_lang
              AND translations.sense_gloss = pairs.sense_gloss
        )
    )")) || !query.next()) {
        m_lastError = query.lastError().text();
        return false;
    }
    insertedGroups = query.value(0).toLongLong();
    query.finish();

    if (!query.exec(QStringLiteral(R"(
        CREATE INDEX IF NOT EXISTS temp.idx_atlas_import_pairs_merge
        ON atlas_import_pairs(source_text, translated_text, source_lang, target_lang, sense_gloss)
    )"))) {
        m_lastError = query.lastError().text();
        return false;
    }
    query.finish();

    if (!query.exec(QStringLiteral(R"(
        UPDATE translations
        SET frequency = frequency + (
            SELECT COUNT(*)
            FROM temp.atlas_import_pairs
            WHERE atlas_import_pairs.source_text = translations.source_text
              AND atlas_import_pairs.translated_text = translations.translated_text
              AND atlas_import_pairs.source_lang = translations.source_lang
              AND atlas_import_pairs.target_lang = translations.target_lang
              AND atlas_import_pairs.sense_gloss = translations.sense_gloss
        )
        WHERE EXISTS (
            SELECT 1
            FROM temp.atlas_import_pairs
            WHERE atlas_import_pairs.source_text = translations.source_text
              AND atlas_import_pairs.translated_text = translations.translated_text
              AND atlas_import_pairs.source_lang = translations.source_lang
              AND atlas_import_pairs.target_lang = translations.target_lang
              AND atlas_import_pairs.sense_gloss = translations.sense_gloss
        )
    )"))) {
        m_lastError = query.lastError().text();
        return false;
    }
    query.finish();

    if (!query.exec(QStringLiteral(R"(
        INSERT OR IGNORE INTO translations (source_text, translated_text, source_lang, target_lang, sense_gloss, frequency)
        SELECT source_text, translated_text, source_lang, target_lang, sense_gloss, COUNT(*)
        FROM temp.atlas_import_pairs
        GROUP BY source_text, translated_text, source_lang, target_lang, sense_gloss
    )"))) {
        m_lastError = query.lastError().text();
        return false;
    }
    query.finish();

    if (!query.exec(QStringLiteral("DELETE FROM temp.atlas_import_pairs"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    m_stats.insertedLines += insertedGroups;
    m_stats.updatedFrequencyLines += uniqueGroups - insertedGroups;
    m_stats.duplicateLines += m_bufferedPairs - insertedGroups;
    m_bufferedPairs = 0;
    m_bufferSourceTexts.clear();
    m_bufferTranslatedTexts.clear();
    m_bufferSourceLangs.clear();
    m_bufferTargetLangs.clear();
    m_bufferSenseGlosses.clear();
    return true;
}

bool AtlasImporter::insertPreparedPair(QSqlQuery &insertQuery,
                                       QSqlQuery &updateQuery,
                                       const QString &sourceText,
                                       const QString &targetText,
                                       const QString &sourceLang,
                                       const QString &targetLang,
                                       const QString &senseGloss,
                                       bool allowSameText)
{
    Q_UNUSED(updateQuery);

    const QStringList sourceSlashAlternatives = slashTranslationAlternatives(sourceText);
    const QStringList targetSlashAlternatives = slashTranslationAlternatives(targetText);
    if (sourceSlashAlternatives.size() > 1 || targetSlashAlternatives.size() > 1) {
        ++m_stats.slashAlternativesExpanded;
        m_stats.slashAlternativeCandidates += qMax<qsizetype>(sourceSlashAlternatives.size(), 1)
            * qMax<qsizetype>(targetSlashAlternatives.size(), 1);
        if (m_stats.slashAlternativeSamples.size() < 5) {
            if (targetSlashAlternatives.size() > 1) {
                m_stats.slashAlternativeSamples.append(QStringLiteral("Original target: %1 | Generated: %2")
                                                           .arg(targetText.left(220))
                                                           .arg(targetSlashAlternatives.size()));
            } else {
                m_stats.slashAlternativeSamples.append(QStringLiteral("Original source: %1 | Generated: %2")
                                                           .arg(sourceText.left(220))
                                                           .arg(sourceSlashAlternatives.size()));
            }
        }

        const QStringList expandedSources = sourceSlashAlternatives.size() > 1
            ? sourceSlashAlternatives
            : QStringList{sourceText};
        const QStringList expandedTargets = targetSlashAlternatives.size() > 1
            ? targetSlashAlternatives
            : QStringList{targetText};
        for (const QString &sourceAlternative : expandedSources) {
            for (const QString &targetAlternative : expandedTargets) {
                if (!sourceAlternative.trimmed().isEmpty() && !targetAlternative.trimmed().isEmpty()) {
                    insertPreparedPair(insertQuery,
                                       updateQuery,
                                       sourceAlternative.trimmed(),
                                       targetAlternative.trimmed(),
                                       sourceLang,
                                       targetLang,
                                       senseGloss,
                                       allowSameText);
                }
            }
        }
        return true;
    }

    const QString discardReason = discardReasonForPair(sourceText, targetText);
    if (!discardReason.isEmpty()
        && !(allowSameText && discardReason == QStringLiteral("same source target"))) {
        recordDiscard(discardReason, sourceText, targetText, sourceLang, targetLang);
        return true;
    }

    const TranslationOrganizer::PairDecision decision = m_translationOrganizer.organizePair(sourceText,
                                                                                            targetText,
                                                                                            sourceLang,
                                                                                            targetLang);
    if (!decision.accepted) {
        recordDiscard(QStringLiteral("invalid format"), sourceText, targetText, sourceLang, targetLang);
        return true;
    }

    Q_UNUSED(insertQuery);

    auto bufferPair = [&](const QString &source, const QString &target) {
        const QString normalizedSource = normalizedSourceText(source);
        if (normalizedSource.isEmpty() || target.trimmed().isEmpty()) {
            return;
        }
        QString storedSenseGloss = senseGloss.trimmed();
        if (storedSenseGloss.isNull()) {
            storedSenseGloss = QStringLiteral("");
        }
        m_bufferSourceTexts.append(normalizedSource);
        m_bufferTranslatedTexts.append(target);
        m_bufferSourceLangs.append(sourceLang);
        m_bufferTargetLangs.append(targetLang);
        m_bufferSenseGlosses.append(storedSenseGloss);
        ++m_bufferedPairs;
    };

    auto recordPunctuationSample = [&](const QString &reason, const QString &source, const QString &target) {
        ++m_stats.punctuationSplitSkipped;
        ++m_stats.punctuationSplitSkipReasons[reason];
        QStringList &samples = m_stats.punctuationSplitSamples[reason];
        if (samples.size() < 3) {
            samples.append(QStringLiteral("Source=\"%1\" | Target=\"%2\"")
                               .arg(source.left(180), target.left(180)));
        }
    };

    auto importSingleTarget = [&](const QString &candidateTarget) {
        const QString candidateDiscardReason = discardReasonForPair(decision.sourceText, candidateTarget);
        if (!candidateDiscardReason.isEmpty()) {
            recordDiscard(candidateDiscardReason, decision.sourceText, candidateTarget, sourceLang, targetLang);
            return;
        }

        if (isWithinPunctuationSplitWordLimit(decision.sourceText, candidateTarget)) {
            bufferPair(decision.sourceText, candidateTarget);
            return;
        }

        const PunctuationSplitResult splitResult = splitByCompatibleStructuralPunctuation(decision.sourceText,
                                                                                         candidateTarget);
        if (!splitResult.segments.isEmpty()) {
            qsizetype acceptedSegments = 0;
            for (const auto &segment : splitResult.segments) {
                if (!isWithinPunctuationSplitWordLimit(segment.first, segment.second)) {
                    const QString reason = QStringLiteral("segment exceeds 8 words after punctuation split");
                    recordPunctuationSample(reason, segment.first, segment.second);
                    recordDiscard(reason,
                                  segment.first,
                                  segment.second,
                                  sourceLang,
                                  targetLang);
                    continue;
                }
                if (discardReasonForPair(segment.first, segment.second).isEmpty()) {
                    bufferPair(segment.first, segment.second);
                    ++acceptedSegments;
                }
            }
            if (acceptedSegments > 0) {
                ++m_stats.punctuationSplitPairs;
                m_stats.punctuationSplitSegments += acceptedSegments;
                return;
            }
            recordDiscard(QStringLiteral("long phrase not segmentable"),
                          decision.sourceText,
                          candidateTarget,
                          sourceLang,
                          targetLang);
            return;
        }

        const QString reason = splitResult.skipReason.isEmpty()
            ? QStringLiteral("no compatible structural punctuation")
            : splitResult.skipReason;
        recordPunctuationSample(reason, decision.sourceText, candidateTarget);
        recordDiscard(QStringLiteral("long phrase not segmentable"),
                      decision.sourceText,
                      candidateTarget,
                      sourceLang,
                      targetLang);
    };

    importSingleTarget(decision.targetText);
    return true;
}

QString AtlasImporter::discardReasonForPair(const QString &sourceText, const QString &targetText) const
{
    if (sourceText.isEmpty()) {
        return QStringLiteral("empty source text");
    }
    if (targetText.isEmpty()) {
        return QStringLiteral("empty translation");
    }
    if (sourceText.size() > MaximumLineLength || targetText.size() > MaximumLineLength) {
        return QStringLiteral("invalid format");
    }
    if (normalizedSourceText(sourceText) == normalizedSourceText(targetText)) {
        return QStringLiteral("same source target");
    }
    if (looksLikeDesktopMetadata(sourceText) || looksLikeDesktopMetadata(targetText)) {
        return QStringLiteral("metadata");
    }
    auto technicalNoiseReason = [](const QString &text) {
        bool hasTechnicalTrigger = false;
        for (const QChar character : text) {
            if (character == QLatin1Char('<') || character == QLatin1Char('>')
                || character == QLatin1Char('&') || character == QLatin1Char('{')
                || character == QLatin1Char('}') || character == QLatin1Char('[')
                || character == QLatin1Char(']') || character == QLatin1Char(';')
                || character == QLatin1Char(':') || character == QLatin1Char('/')
                || character == QLatin1Char('\\') || character == QLatin1Char('@')
                || character == QLatin1Char('$') || character == QLatin1Char('%')
                || character == QLatin1Char('#') || character == QLatin1Char('=')) {
                hasTechnicalTrigger = true;
                break;
            }
        }

        if (!hasTechnicalTrigger
            && !text.contains(QStringLiteral("http"), Qt::CaseInsensitive)
            && !text.contains(QStringLiteral("www"), Qt::CaseInsensitive)
            && !text.contains(QStringLiteral("tel"), Qt::CaseInsensitive)
            && !text.contains(QStringLiteral("fax"), Qt::CaseInsensitive)
            && !text.contains(QStringLiteral("mail"), Qt::CaseInsensitive)) {
            return QString();
        }

        static const QRegularExpression markupExpression(QStringLiteral(R"(<\/?\w+|&(?:[a-z]+|#\d+);|<!DOCTYPE|<\?xml)"),
                                                         QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression styleExpression(QStringLiteral(R"((?:^|[\s;])(?:margin|padding|font-size|background|color|display|width|height)\s*:|\.[A-Za-z0-9_-]+\s*\{|#(?:[A-Fa-f0-9]{3}){1,2}\b)"),
                                                        QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression urlExpression(QStringLiteral(R"((?:https?|ftp)\s*:\s*/\s*/|www\s*\.|\b\w+\s*:\s*/\s*/|(?:tel|fax)\s*[:.]|\btelephone\b)"),
                                                      QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression codeExpression(QStringLiteral(R"((?:->|=>|::|==|!=|<=|>=|&&|\|\|)|[\{\}\[\];]{2,})"),
                                                       QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression variableExpression(QStringLiteral(R"((?:%\d+|%[A-Za-z_]+|\$\{?\w+\}?|\{\d+\}|@[A-Za-z_]+@|__[A-Za-z0-9_]+__))"));
        static const QRegularExpression pidExpression(QStringLiteral(R"(\bPID\b|\bprocess\s+id\b)"),
                                                      QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression authorizationExpression(QStringLiteral(R"(\bBearer\b|\bOAuth\b|\bAPI[-_ ]?Key\b)"),
                                                               QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression filePathExpression(QStringLiteral(R"((?:[A-Za-z]:\\|/\w+/|\.\.?/|\w+/\w+\.\w{1,6}\b))"));
        static const QRegularExpression emailExpression(QStringLiteral(R"(\b[A-Z0-9._%+\-]+\s*@\s*[A-Z0-9.\-]+\s*\.\s*[A-Z]{2,}\b|\be-?\s*mail\s*:)"),
                                                        QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression catalogMetadataExpression(QStringLiteral(R"(unit\s+synonyms\s+for\s+matching\s+user\s+input|amount\s+in\s+units\s*\((?:real|integer)\))"),
                                                                  QRegularExpression::CaseInsensitiveOption);

        if (emailExpression.match(text).hasMatch()) {
            return QStringLiteral("email/contact");
        }
        if (urlExpression.match(text).hasMatch()) {
            return QStringLiteral("url/contact");
        }
        if (markupExpression.match(text).hasMatch()
            || styleExpression.match(text).hasMatch()
            || codeExpression.match(text).hasMatch()
            || variableExpression.match(text).hasMatch()
            || pidExpression.match(text).hasMatch()
            || authorizationExpression.match(text).hasMatch()
            || filePathExpression.match(text).hasMatch()
            || catalogMetadataExpression.match(text).hasMatch()) {
            return QStringLiteral("metadata");
        }
        return QString();
    };

    auto hasExcessiveSymbols = [](const QString &text) {
        qsizetype lettersOrNumbers = 0;
        qsizetype symbols = 0;
        qsizetype repeatedPunctuationRun = 0;
        qsizetype currentPunctuationRun = 0;

        for (qsizetype index = 0; index < text.size();) {
            qsizetype advance = 1;
            const QChar character = text.at(index);
            if (isLetterNumberOrMarkAt(text, index, &advance)) {
                ++lettersOrNumbers;
                currentPunctuationRun = 0;
            } else if (character.isSpace()
                       || character == QLatin1Char('(')
                       || character == QLatin1Char(')')
                       || character == QLatin1Char('/')
                       || character == QLatin1Char('-')
                       || character == QLatin1Char('\'')
                       || character == QLatin1Char('.')
                       || character == QLatin1Char(',')
                       || character == QLatin1Char(';')
                       || character == QLatin1Char(':')
                       || character == QLatin1Char('!')
                       || character == QLatin1Char('?')) {
                currentPunctuationRun = 0;
            } else {
                ++symbols;
                ++currentPunctuationRun;
                repeatedPunctuationRun = qMax(repeatedPunctuationRun, currentPunctuationRun);
            }
            index += advance;
        }

        if (lettersOrNumbers == 0) {
            return true;
        }

        const double symbolRatio = static_cast<double>(symbols) / static_cast<double>(text.size());
        return symbolRatio > 0.35 || repeatedPunctuationRun >= 4;
    };

    auto looksStrange = [](const QString &text) {
        static const QRegularExpression mostlyNumbersExpression(QStringLiteral(R"(^[\d\s\.,:/_\-+]+$)"));
        static const QRegularExpression repeatedTokenExpression(QStringLiteral(R"(\b(\w+)\b(?:\s+\1\b){3,})"),
                                                               QRegularExpression::CaseInsensitiveOption);

        bool hasLetter = false;
        qsizetype words = 0;
        bool insideWord = false;
        for (qsizetype index = 0; index < text.size();) {
            qsizetype advance = 1;
            if (isLetterNumberOrMarkAt(text, index, &advance)) {
                hasLetter = true;
                if (!insideWord) {
                    ++words;
                    insideWord = true;
                }
            } else {
                insideWord = false;
            }
            index += advance;
        }

        if (!hasLetter && mostlyNumbersExpression.match(text).hasMatch()) {
            return true;
        }
        if (words >= 4 && repeatedTokenExpression.match(text).hasMatch()) {
            return true;
        }
        return false;
    };

    const QString sourceTechnicalReason = technicalNoiseReason(sourceText);
    const QString targetTechnicalReason = technicalNoiseReason(targetText);
    if (!sourceTechnicalReason.isEmpty() || !targetTechnicalReason.isEmpty()) {
        if (sourceTechnicalReason == QStringLiteral("email/contact")
            || targetTechnicalReason == QStringLiteral("email/contact")) {
            return QStringLiteral("email/contact");
        }
        if (sourceTechnicalReason == QStringLiteral("url/contact")
            || targetTechnicalReason == QStringLiteral("url/contact")) {
            return QStringLiteral("url/contact");
        }
        return QStringLiteral("metadata");
    }
    if (hasExcessiveSymbols(sourceText) || hasExcessiveSymbols(targetText) || looksStrange(sourceText) || looksStrange(targetText)) {
        return QStringLiteral("broken record");
    }

    return QString();
}

void AtlasImporter::recordDiscard(const QString &reason,
                                  const QString &sourceText,
                                  const QString &targetText,
                                  const QString &sourceLang,
                                  const QString &targetLang)
{
    ++m_stats.ignoredLines;
    ++m_stats.discardedLines;
    ++m_stats.discardReasons[reason];
    QStringList &samples = m_stats.discardSamples[reason];
    if (samples.size() < 3) {
        samples.append(QStringLiteral("Source=\"%1\" | Target=\"%2\"")
                           .arg(sourceText.left(160), targetText.left(160)));
    }
    if (!m_rejectedExamples.contains(reason)) {
        const QFileInfo sourceInfo(m_currentImportSourceFile);
        m_rejectedExamples.insert(reason,
                                  RejectedExample{
                                      sourceInfo.fileName(),
                                      sourceInfo.absoluteFilePath(),
                                      m_stats.processedLines,
                                      sourceLang,
                                      targetLang,
                                      sourceText.left(500),
                                      targetText.left(500)});
    }
    writeRejectedDetail(reason, sourceText, targetText, sourceLang, targetLang);
}

void AtlasImporter::writeRejectedDetail(const QString &reason,
                                        const QString &sourceText,
                                        const QString &targetText,
                                        const QString &sourceLang,
                                        const QString &targetLang)
{
    if (!m_debugRejectedDetails) {
        return;
    }

    QString entry;
    QTextStream stream(&entry);
    stream << "[REJEITADO]" << Qt::endl;
    stream << "Data/hora: " << AtlasReport::timestamp() << Qt::endl;
    stream << "Arquivo: " << QFileInfo(m_currentImportSourceFile).fileName() << Qt::endl;
    stream << "Caminho completo: " << QFileInfo(m_currentImportSourceFile).absoluteFilePath() << Qt::endl;
    stream << "Linha/posicao aproximada: " << m_stats.processedLines << Qt::endl;
    stream << "Par: " << sourceLang << " -> " << targetLang << Qt::endl;
    stream << "Texto de origem: \"" << sourceText.left(500) << "\"" << Qt::endl;
    stream << "Texto de destino: \"" << targetText.left(500) << "\"" << Qt::endl;
    stream << "Motivo: " << discardReasonLabel(reason) << Qt::endl;

    m_rejectedDetailsBuffer.append(entry);
    m_rejectedDetailsBuffer.append(QStringLiteral("\n"));
    if (m_rejectedDetailsBuffer.size() >= 65536) {
        flushRejectedDetails();
    }
}

void AtlasImporter::flushRejectedDetails()
{
    if (m_rejectedDetailsBuffer.isEmpty()) {
        return;
    }

    AtlasReport::append(AtlasReport::File::ImportRejectedDetails, m_rejectedDetailsBuffer);
    m_rejectedDetailsBuffer.clear();
}

void AtlasImporter::writeRejectedDetailsSummary(const QString &sourceFilePath,
                                                const QString &sourceLang,
                                                const QString &targetLang,
                                                const QString &startedAt)
{
    QElapsedTimer reportTimer;
    reportTimer.start();

    const QFileInfo sourceInfo(sourceFilePath);
    QString report;
    QTextStream stream(&report);
    stream << "[RESUMO DE REJEICOES]" << Qt::endl;
    stream << "Iniciado em: " << startedAt << Qt::endl;
    stream << "Finalizado em: " << AtlasReport::timestamp() << Qt::endl;
    stream << "Arquivo: " << sourceInfo.fileName() << Qt::endl;
    stream << "Caminho completo: " << sourceInfo.absoluteFilePath() << Qt::endl;
    stream << "Par padrao de importacao: " << sourceLang << " -> " << targetLang << Qt::endl;
    stream << "Total rejeitado: " << m_stats.discardedLines << Qt::endl;
    stream << "Modo forense completo: "
           << (m_debugRejectedDetails ? QStringLiteral("ativado") : QStringLiteral("desativado")) << Qt::endl;
    stream << Qt::endl;

    if (m_stats.discardReasons.isEmpty()) {
        stream << "Motivo: nenhum" << Qt::endl;
        stream << "Quantidade: 0" << Qt::endl;
        stream << "Exemplo: nenhum item rejeitado nesta importacao" << Qt::endl;
    } else {
        QStringList reasons = m_stats.discardReasons.keys();
        std::sort(reasons.begin(), reasons.end());
        for (const QString &reason : reasons) {
            const RejectedExample example = m_rejectedExamples.value(reason);
            stream << "Motivo: " << discardReasonLabel(reason) << Qt::endl;
            stream << "Quantidade: " << m_stats.discardReasons.value(reason) << Qt::endl;
            stream << "Exemplo:" << Qt::endl;
            stream << "Arquivo: " << example.fileName << Qt::endl;
            stream << "Linha/posicao: " << example.position << Qt::endl;
            stream << "Par: " << example.sourceLang << " -> " << example.targetLang << Qt::endl;
            stream << "Texto de origem: \"" << example.sourceText << "\"" << Qt::endl;
            stream << "Texto de destino: \"" << example.targetText << "\"" << Qt::endl;
            stream << Qt::endl;
            stream << "------------------------------------------------------------" << Qt::endl;
            stream << Qt::endl;
        }
    }

    AtlasReport::append(AtlasReport::File::ImportRejectedDetails, report);
    m_stats.reportTimeNs += reportTimer.nsecsElapsed();
}

void AtlasImporter::maybeWriteRejectedDetailsSummary(const QString &sourceFilePath,
                                                     const QString &sourceLang,
                                                     const QString &targetLang,
                                                     const QString &startedAt,
                                                     bool force)
{
    if (!force
        && m_lastRejectedSummarySnapshotProcessed >= 0
        && (m_stats.processedLines - m_lastRejectedSummarySnapshotProcessed) < RejectedSummarySnapshotInterval) {
        return;
    }

    writeRejectedDetailsSummary(sourceFilePath, sourceLang, targetLang, startedAt);
    m_lastRejectedSummarySnapshotProcessed = m_stats.processedLines;
}


bool AtlasImporter::isValidTranslationPair(const QString &sourceText, const QString &targetText) const
{
    if (looksLikeDesktopMetadata(sourceText) || looksLikeDesktopMetadata(targetText)) {
        return false;
    }

    auto hasTechnicalNoise = [](const QString &text) {
        static const QRegularExpression markupExpression(QStringLiteral(R"(<\/?\w+|&(?:[a-z]+|#\d+);|<!DOCTYPE|<\?xml)"),
                                                         QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression styleExpression(QStringLiteral(R"((?:^|[\s;])(?:margin|padding|font-size|background|color|display|width|height)\s*:|\.[A-Za-z0-9_-]+\s*\{|#(?:[A-Fa-f0-9]{3}){1,2}\b)"),
                                                        QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression urlExpression(QStringLiteral(R"((?:https?|ftp)://|www\.|\b\w+://)"),
                                                      QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression codeExpression(QStringLiteral(R"((?:->|=>|::|==|!=|<=|>=|&&|\|\|)|[\{\}\[\];]{2,})"),
                                                       QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression variableExpression(QStringLiteral(R"((?:%\d+|%[A-Za-z_]+|\$\{?\w+\}?|\{\d+\}|@[A-Za-z_]+@|__[A-Za-z0-9_]+__))"));
        static const QRegularExpression pidExpression(QStringLiteral(R"(\bPID\b|\bprocess\s+id\b)"),
                                                      QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression authorizationExpression(QStringLiteral(R"(\bBearer\b|\bOAuth\b|\bAPI[-_ ]?Key\b)"),
                                                               QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression filePathExpression(QStringLiteral(R"((?:[A-Za-z]:\\|/\w+/|\.\.?/|\w+/\w+\.\w{1,6}\b))"));
        static const QRegularExpression emailExpression(QStringLiteral(R"(\b[A-Z0-9._%+\-]+@[A-Z0-9.\-]+\.[A-Z]{2,}\b)"),
                                                        QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression catalogMetadataExpression(QStringLiteral(R"(unit\s+synonyms\s+for\s+matching\s+user\s+input|amount\s+in\s+units\s*\((?:real|integer)\))"),
                                                                  QRegularExpression::CaseInsensitiveOption);

        return markupExpression.match(text).hasMatch()
            || styleExpression.match(text).hasMatch()
            || urlExpression.match(text).hasMatch()
            || codeExpression.match(text).hasMatch()
            || variableExpression.match(text).hasMatch()
            || pidExpression.match(text).hasMatch()
            || authorizationExpression.match(text).hasMatch()
            || filePathExpression.match(text).hasMatch()
            || emailExpression.match(text).hasMatch()
            || catalogMetadataExpression.match(text).hasMatch();
    };

    auto hasExcessiveSymbols = [](const QString &text) {
        qsizetype lettersOrNumbers = 0;
        qsizetype symbols = 0;
        qsizetype repeatedPunctuationRun = 0;
        qsizetype currentPunctuationRun = 0;

        for (qsizetype index = 0; index < text.size();) {
            qsizetype advance = 1;
            const QChar character = text.at(index);
            if (isLetterNumberOrMarkAt(text, index, &advance)) {
                ++lettersOrNumbers;
                currentPunctuationRun = 0;
            } else if (character.isSpace()
                       || character == QLatin1Char('(')
                       || character == QLatin1Char(')')
                       || character == QLatin1Char('/')
                       || character == QLatin1Char('-')
                       || character == QLatin1Char('\'')
                       || character == QLatin1Char('.')
                       || character == QLatin1Char(',')
                       || character == QLatin1Char(';')
                       || character == QLatin1Char(':')
                       || character == QLatin1Char('!')
                       || character == QLatin1Char('?')) {
                currentPunctuationRun = 0;
            } else {
                ++symbols;
                ++currentPunctuationRun;
                repeatedPunctuationRun = qMax(repeatedPunctuationRun, currentPunctuationRun);
            }
            index += advance;
        }

        if (lettersOrNumbers == 0) {
            return true;
        }

        const double symbolRatio = static_cast<double>(symbols) / static_cast<double>(text.size());
        return symbolRatio > 0.35 || repeatedPunctuationRun >= 4;
    };

    auto looksStrange = [](const QString &text) {
        static const QRegularExpression mostlyNumbersExpression(QStringLiteral(R"(^[\d\s\.,:/_\-+]+$)"));
        static const QRegularExpression repeatedTokenExpression(QStringLiteral(R"(\b(\w+)\b(?:\s+\1\b){3,})"),
                                                               QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression longTechnicalTokenExpression(QStringLiteral(R"(\b[A-Za-z0-9_./\\-]{35,}\b)"));

        const QStringList words = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        return mostlyNumbersExpression.match(text).hasMatch()
            || repeatedTokenExpression.match(text).hasMatch()
            || longTechnicalTokenExpression.match(text).hasMatch()
            || (words.size() == 1 && text.size() > 40);
    };

    if (sourceText.isEmpty() || targetText.isEmpty()) {
        return false;
    }

    if (sourceText.size() > MaximumLineLength || targetText.size() > MaximumLineLength) {
        return false;
    }

    if (isSameNormalizedText(sourceText, targetText)) {
        return false;
    }

    return !hasTechnicalNoise(sourceText)
        && !hasTechnicalNoise(targetText)
        && !hasExcessiveSymbols(sourceText)
        && !hasExcessiveSymbols(targetText)
        && !looksStrange(sourceText)
        && !looksStrange(targetText);
}


bool AtlasImporter::isInvalidStoredTranslationPair(const QString &sourceText, const QString &targetText) const
{
    auto containsControlNoise = [](const QString &text) {
        qsizetype controlCharacters = 0;
        for (const QChar character : text) {
            if (character.isNull()) {
                return true;
            }
            if (character.category() == QChar::Other_Control && !character.isSpace()) {
                ++controlCharacters;
            }
        }
        return controlCharacters > 0;
    };

    auto hasTechnicalJunk = [](const QString &text) {
        static const QRegularExpression htmlXmlExpression(QStringLiteral(R"(<\/?\w+|<!DOCTYPE|<\?xml|&(?:[a-z]+|#\d+);)"),
                                                          QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression cssExpression(QStringLiteral(R"((?:^|[\s;])(?:margin|padding|font-size|background|display|width|height)\s*:|\.[A-Za-z0-9_-]+\s*\{|#(?:[A-Fa-f0-9]{3}){1,2}\b)"),
                                                      QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression urlExpression(QStringLiteral(R"((?:https?|ftp)://|www\.|\b\w+://)"),
                                                      QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression codeExpression(QStringLiteral(R"((?:\b(?:function|class|return|while|for|var|let|const|include)\b\s*[\(\{;])|(?:->|=>|::|==|!=|<=|>=|&&|\|\|)|[\{\}\[\];]{3,})"),
                                                       QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression authSecretExpression(QStringLiteral(R"((?:Authorization\s*:\s*(?:Bearer|Basic)|\bBearer\s+[A-Za-z0-9._\-]{16,}|\bOAuth\b|\bAPI[-_ ]?Key\s*[:=]|\btoken\s*[:=]\s*[A-Za-z0-9._\-]{12,}))"),
                                                             QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression pidValueExpression(QStringLiteral(R"(\bPID\s*[:=#]?\s*\d+\b|\bprocess\s+id\s*[:=#]?\s*\d+\b)"),
                                                           QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression tooltipMarkupExpression(QStringLiteral(R"(<[^>]*tooltip|tooltip\s*[:=]\s*['\"<{])"),
                                                                QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression emailExpression(QStringLiteral(R"(\b[A-Z0-9._%+\-]+@[A-Z0-9.\-]+\.[A-Z]{2,}\b)"),
                                                        QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression catalogMetadataExpression(QStringLiteral(R"(unit\s+synonyms\s+for\s+matching\s+user\s+input|amount\s+in\s+units\s*\((?:real|integer)\))"),
                                                                  QRegularExpression::CaseInsensitiveOption);

        return htmlXmlExpression.match(text).hasMatch()
            || cssExpression.match(text).hasMatch()
            || urlExpression.match(text).hasMatch()
            || codeExpression.match(text).hasMatch()
            || authSecretExpression.match(text).hasMatch()
            || pidValueExpression.match(text).hasMatch()
            || tooltipMarkupExpression.match(text).hasMatch()
            || emailExpression.match(text).hasMatch()
            || catalogMetadataExpression.match(text).hasMatch();
    };

    auto hasExcessiveSymbolNoise = [](const QString &text) {
        qsizetype lettersOrNumbers = 0;
        qsizetype symbols = 0;
        qsizetype replacementCharacters = 0;
        qsizetype repeatedPunctuationRun = 0;
        qsizetype currentPunctuationRun = 0;

        for (qsizetype index = 0; index < text.size();) {
            qsizetype advance = 1;
            const QChar character = text.at(index);
            if (character == QChar(0xFFFD)) {
                ++replacementCharacters;
            }

            if (isLetterNumberOrMarkAt(text, index, &advance)) {
                ++lettersOrNumbers;
                currentPunctuationRun = 0;
            } else if (character.isSpace()
                       || character == QLatin1Char('(')
                       || character == QLatin1Char(')')
                       || character == QLatin1Char('/')
                       || character == QLatin1Char('-')
                       || character == QLatin1Char('\'')
                       || character == QLatin1Char('.')
                       || character == QLatin1Char(',')
                       || character == QLatin1Char(';')
                       || character == QLatin1Char(':')
                       || character == QLatin1Char('!')
                       || character == QLatin1Char('?')) {
                currentPunctuationRun = 0;
            } else {
                ++symbols;
                ++currentPunctuationRun;
                repeatedPunctuationRun = qMax(repeatedPunctuationRun, currentPunctuationRun);
            }
            index += advance;
        }

        if (lettersOrNumbers == 0) {
            return true;
        }

        const double symbolRatio = static_cast<double>(symbols) / static_cast<double>(qMax<qsizetype>(text.size(), 1));
        const double replacementRatio = static_cast<double>(replacementCharacters) / static_cast<double>(qMax<qsizetype>(text.size(), 1));
        return symbolRatio > 0.45 || repeatedPunctuationRun >= 6 || replacementRatio > 0.05;
    };

    auto looksBroken = [](const QString &text) {
        static const QRegularExpression repeatedTokenExpression(QStringLiteral(R"(\b(\w+)\b(?:\s+\1\b){4,})"),
                                                               QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression mostlyNumbersExpression(QStringLiteral(R"(^[\d\s\.,:/_\-+]+$)"));
        const QStringList words = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        return repeatedTokenExpression.match(text).hasMatch()
            || mostlyNumbersExpression.match(text).hasMatch()
            || (words.size() == 1 && text.size() > 80);
    };

    if (sourceText.trimmed().isEmpty() || targetText.trimmed().isEmpty()) {
        return true;
    }

    if (isSameNormalizedText(sourceText, targetText)) {
        return true;
    }

    constexpr qsizetype CleanupMaximumLength = 800;
    if (sourceText.size() > CleanupMaximumLength || targetText.size() > CleanupMaximumLength) {
        return true;
    }

    return containsControlNoise(sourceText)
        || containsControlNoise(targetText)
        || hasTechnicalJunk(sourceText)
        || hasTechnicalJunk(targetText)
        || hasExcessiveSymbolNoise(sourceText)
        || hasExcessiveSymbolNoise(targetText)
        || looksBroken(sourceText)
        || looksBroken(targetText);
}

bool AtlasImporter::cleanupDatabase()
{
    QElapsedTimer cleanupTimer;
    cleanupTimer.start();
    ++m_stats.cleanupRunCount;

    const qint64 migratedDuplicates = m_cleanupStats.removedDuplicates;
    m_cleanupStats = CleanupStats();
    m_cleanupStats.removedDuplicates = migratedDuplicates;

    if (!m_database.transaction()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    QList<QVariant> invalidIds;
    {
        QSqlQuery selectQuery(m_database);
        if (!selectQuery.exec(QStringLiteral("SELECT id, source_text, translated_text FROM translations"))) {
            m_lastError = selectQuery.lastError().text();
            m_database.rollback();
            return false;
        }

        while (selectQuery.next()) {
            const QString sourceText = selectQuery.value(1).toString();
            const QString targetText = selectQuery.value(2).toString();
            if (isInvalidStoredTranslationPair(sourceText, targetText)) {
                invalidIds.append(selectQuery.value(0));
            }
        }
        selectQuery.finish();
    }

    QSqlQuery deleteQuery(m_database);
    if (!deleteQuery.prepare(QStringLiteral("DELETE FROM translations WHERE id = :id"))) {
        m_lastError = deleteQuery.lastError().text();
        m_database.rollback();
        return false;
    }

    for (const QVariant &invalidId : invalidIds) {
        deleteQuery.bindValue(QStringLiteral(":id"), invalidId);
        if (!deleteQuery.exec()) {
            m_lastError = deleteQuery.lastError().text();
            m_database.rollback();
            return false;
        }
        deleteQuery.finish();
        ++m_cleanupStats.removedInvalidEntries;
    }

    if (!m_database.commit()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    if (!ensureFrequencySchema() || !ensureIndexes() || !optimizeDatabase(m_cleanupStats.removedInvalidEntries > 0
                                                                           || m_cleanupStats.removedDuplicates > 0)) {
        return false;
    }

    m_cleanupStats.finalTranslationsCount = translationCount();
    m_stats.cleanupTimeNs += cleanupTimer.nsecsElapsed();
    return m_cleanupStats.finalTranslationsCount >= 0;
}

bool AtlasImporter::optimizeDatabase(bool compactDatabase)
{
    QStringList maintenanceStatements;
    if (compactDatabase) {
        maintenanceStatements.append(QStringLiteral("VACUUM"));
    }
    maintenanceStatements.append(QStringLiteral("ANALYZE"));
    if (compactDatabase) {
        maintenanceStatements.append(QStringLiteral("REINDEX"));
    }

    for (const QString &statement : maintenanceStatements) {
        QSqlQuery query(m_database);
        if (!query.exec(statement)) {
            m_lastError = query.lastError().text();
            return false;
        }
        query.finish();
    }

    return true;
}

qint64 AtlasImporter::translationCount() const
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM translations")) || !query.next()) {
        return -1;
    }

    const qint64 count = query.value(0).toLongLong();
    query.finish();
    return count;
}


bool AtlasImporter::loadProgress(const QString &importKey, qint64 &processedLines, bool &completed)
{
    processedLines = 0;
    completed = false;

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(R"(
        SELECT processed_lines, completed
        FROM import_progress
        WHERE import_key = :import_key
        LIMIT 1
    )"));
    query.bindValue(QStringLiteral(":import_key"), importKey);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }

    if (query.next()) {
        processedLines = query.value(0).toLongLong();
        completed = query.value(1).toInt() != 0;
    }

    return true;
}

bool AtlasImporter::saveProgress(const QString &importKey,
                                 const QString &sourceFilePath,
                                 const QString &targetFilePath,
                                 const QString &sourceLang,
                                 const QString &targetLang,
                                 qint64 processedLines,
                                 bool completed)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(R"(
        INSERT OR REPLACE INTO import_progress (
            import_key,
            source_file,
            target_file,
            source_lang,
            target_lang,
            processed_lines,
            completed,
            updated_at
        ) VALUES (
            :import_key,
            :source_file,
            :target_file,
            :source_lang,
            :target_lang,
            :processed_lines,
            :completed,
            CURRENT_TIMESTAMP
        )
    )"));
    query.bindValue(QStringLiteral(":import_key"), importKey);
    query.bindValue(QStringLiteral(":source_file"), sourceFilePath);
    query.bindValue(QStringLiteral(":target_file"), targetFilePath);
    query.bindValue(QStringLiteral(":source_lang"), sourceLang);
    query.bindValue(QStringLiteral(":target_lang"), targetLang);
    query.bindValue(QStringLiteral(":processed_lines"), processedLines);
    query.bindValue(QStringLiteral(":completed"), completed ? 1 : 0);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }

    return true;
}

bool AtlasImporter::skipLines(QTextStream &sourceStream, QTextStream &targetStream, qint64 linesToSkip)
{
    for (qint64 line = 0; line < linesToSkip; ++line) {
        if (sourceStream.atEnd() || targetStream.atEnd()) {
        m_lastError = QStringLiteral("O progresso salvo aponta para depois do fim do dataset.");
            return false;
        }

        sourceStream.readLine();
        targetStream.readLine();
    }

    return true;
}

QString AtlasImporter::importKey(const QString &sourceFilePath,
                                 const QString &targetFilePath,
                                 const QString &sourceLang,
                                 const QString &targetLang) const
{
    const QString rawKey = QStringLiteral("%1|%2|%3|%4")
                               .arg(QFileInfo(sourceFilePath).absoluteFilePath(),
                                    QFileInfo(targetFilePath).absoluteFilePath(),
                                    sourceLang,
                                    targetLang);
    return QString::fromLatin1(QCryptographicHash::hash(rawKey.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString AtlasImporter::cleanText(const QString &text) const
{
    static const TextNormalizer normalizer;
    return normalizer.normalizeWhitespace(text);
}

QString AtlasImporter::normalizedSourceText(const QString &text) const
{
    static const TextNormalizer normalizer;
    return normalizer.normalizeForLookup(text);
}

bool AtlasImporter::logImportWarning(const QString &sourceName,
                                     const QString &category,
                                     const QString &detail,
                                     qint64 position)
{
    if (m_stats.qualityWarningsLogged >= MaximumQualityWarningsPerImport) {
        return true;
    }

    QSqlQuery query(m_database);
    if (!query.prepare(QStringLiteral(R"(
        INSERT INTO import_quality_log (source_name, category, detail, position)
        VALUES (:source_name, :category, :detail, :position)
    )"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    QString clippedDetail = detail;
    if (clippedDetail.size() > 800) {
        clippedDetail = clippedDetail.left(800);
    }

    query.bindValue(QStringLiteral(":source_name"), sourceName);
    query.bindValue(QStringLiteral(":category"), category);
    query.bindValue(QStringLiteral(":detail"), clippedDetail);
    query.bindValue(QStringLiteral(":position"), position);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }

    ++m_stats.qualityWarningsLogged;
    return true;
}

void AtlasImporter::printProgress()
{
    QElapsedTimer logTimer;
    logTimer.start();
    QTextStream console(stdout);
    configureUtf8Stream(console);
    console << "Linhas retomadas: " << m_stats.resumedLines << terminalElapsedSuffix(logTimer) << Qt::endl;
    console << "Linhas processadas: " << m_stats.processedLines << terminalElapsedSuffix(logTimer) << Qt::endl;
    console << "Inseridos: " << m_stats.insertedLines << terminalElapsedSuffix(logTimer) << Qt::endl;
    console << "Ignorados: " << m_stats.ignoredLines << terminalElapsedSuffix(logTimer) << Qt::endl;
    console << "Avisos de qualidade registrados: " << m_stats.qualityWarningsLogged << terminalElapsedSuffix(logTimer) << Qt::endl;
    ++m_stats.progressPrintCount;
    m_stats.logTimeNs += logTimer.nsecsElapsed();
}
