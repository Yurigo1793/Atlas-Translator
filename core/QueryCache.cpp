#include "QueryCache.h"

QueryCache::QueryCache(qsizetype maximumEntries)
    : m_maximumEntries(maximumEntries)
{
}

bool QueryCache::get(const QString &key, QString &value) const
{
    const auto iterator = m_cache.constFind(key);
    if (iterator == m_cache.constEnd()) {
        return false;
    }

    value = iterator.value();
    return true;
}

void QueryCache::put(const QString &key, const QString &value)
{
    if (m_cache.size() >= m_maximumEntries) {
        m_cache.clear();
    }

    m_cache.insert(key, value);
}

void QueryCache::clear()
{
    m_cache.clear();
}

qsizetype QueryCache::size() const
{
    return m_cache.size();
}
