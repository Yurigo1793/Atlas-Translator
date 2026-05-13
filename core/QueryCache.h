#ifndef QUERYCACHE_H
#define QUERYCACHE_H

#include <QHash>
#include <QString>
#include <QtGlobal>

class QueryCache
{
public:
    explicit QueryCache(qsizetype maximumEntries = 4096);

    bool get(const QString &key, QString &value) const;
    void put(const QString &key, const QString &value);
    void clear();
    qsizetype size() const;

private:
    qsizetype m_maximumEntries;
    QHash<QString, QString> m_cache;
};

#endif // QUERYCACHE_H
