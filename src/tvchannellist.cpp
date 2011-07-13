/*
 * Copyright (C) 2011  Southern Storm Software, Pty Ltd.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tvchannellist.h"
#include "tvprogramme.h"
#include <QtCore/qset.h>
#include <QtCore/qdir.h>
#include <QtCore/qdebug.h>
#include <QtNetwork/qnetworkdiskcache.h>

//#define DEBUG_NETWORK 1

TvChannelList::TvChannelList(QObject *parent)
    : QObject(parent)
    , m_startUrlRefresh(24)
    , m_hasDataFor(false)
    , m_throttled(false)
    , m_busy(false)
    , m_largeIcons(false)
    , m_haveChannelNumbers(false)
    , m_progress(1.0f)
    , m_requestsToDo(0)
    , m_requestsDone(0)
    , m_reply(0)
{
    QString cacheDir = QDir::homePath() +
                       QLatin1String("/.qtvguide/cache");
    QNetworkDiskCache *cache = new QNetworkDiskCache(this);
    cache->setCacheDirectory(cacheDir);
    m_nam.setCache(cache);

    connect(&m_nam, SIGNAL(authenticationRequired(QNetworkReply*,QAuthenticator*)),
            this, SLOT(authenticationRequired(QNetworkReply*,QAuthenticator*)));
#ifndef QT_NO_OPENSSL
    connect(&m_nam, SIGNAL(sslErrors(QNetworkReply*,QList<QSslError>)),
            this, SLOT(sslErrors(QNetworkReply*,QList<QSslError>)));
#endif

    m_throttleTimer = new QTimer(this);
    m_throttleTimer->setSingleShot(true);
    connect(m_throttleTimer, SIGNAL(timeout()),
            this, SLOT(throttleTimeout()));

    reloadService();
}

TvChannelList::~TvChannelList()
{
    qDeleteAll(m_channels);
    qDeleteAll(m_bookmarks);
}

TvChannel *TvChannelList::channel(const QString &id) const
{
    return m_channels.value(id, 0);
}

static bool sortActiveChannels(TvChannel *c1, TvChannel *c2)
{
    return c1->compare(c2) < 0;
}

void TvChannelList::load(QXmlStreamReader *reader, const QUrl &url)
{
    QString channelId;
    TvChannel *channel;
    TvProgramme *programme;
    bool newChannels = false;
    QSet<TvChannel *> newProgrammes;

    // Will leave the XML stream positioned on </tv>.
    Q_ASSERT(reader->isStartElement());
    Q_ASSERT(reader->name() == QLatin1String("tv"));
    while (!reader->hasError()) {
        QXmlStreamReader::TokenType token = reader->readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (reader->name() == QLatin1String("channel")) {
                channelId = reader->attributes().value
                        (QLatin1String("id")).toString();
                channel = m_channels.value(channelId, 0);
                if (channel) {
                    if (channel->load(reader)) {
                        newChannels = true;
                        if (channel->trimProgrammes())
                            newProgrammes.insert(channel);
                    }
                } else {
                    channel = new TvChannel(this);
                    channel->load(reader);
                    m_channels.insert(channelId, channel);
                    newChannels = true;
                    if (m_hiddenChannelIds.contains(channelId))
                        channel->setHidden(true);
                }
                if (channel->hasDataFor())
                    m_hasDataFor = true;
            } else if (reader->name() == QLatin1String("programme")) {
                channelId = reader->attributes().value
                        (QLatin1String("channel")).toString();
                channel = m_channels.value(channelId, 0);
                if (!channel) {
                    channel = new TvChannel(this);
                    channel->setId(channelId);
                    channel->setName(channelId);
                    m_channels.insert(channelId, channel);
                    newChannels = true;
                    if (m_hiddenChannelIds.contains(channelId))
                        channel->setHidden(true);
                }
                programme = new TvProgramme(channel);
                programme->load(reader);
                channel->addProgramme(programme);
                newProgrammes.insert(channel);
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (reader->name() == QLatin1String("tv"))
                break;
        }
    }

    // Emit pending signals.
    if (newChannels) {
        // Construct the sorted "active channels" list.  If we have
        // "datafor" declarations in the channel list, then omit
        // any channels that have no information recorded.
        if (m_hasDataFor) {
            QMap<QString, TvChannel *>::ConstIterator it;
            m_activeChannels.clear();
            for (it = m_channels.constBegin();
                    it != m_channels.constEnd(); ++it) {
                TvChannel *channel = it.value();
                if (channel->hasDataFor())
                    m_activeChannels.append(channel);
            }
        } else {
            m_activeChannels = m_channels.values();
        }
        if (m_startUrl.host().endsWith(QLatin1String(".oztivo.net")))
            loadOzTivoChannelData();
        qSort(m_activeChannels.begin(),
              m_activeChannels.end(), sortActiveChannels);
        emit channelsChanged();
    }
    if (!newProgrammes.isEmpty()) {
        QSet<TvChannel *>::ConstIterator it;
        for (it = newProgrammes.constBegin();
                it != newProgrammes.constEnd(); ++it) {
            emit programmesChanged(*it);
        }
    }
    if (url == m_startUrl)
        emit channelIndexLoaded();
}

void TvChannelList::loadOzTivoChannelData()
{
    QFile file(QLatin1String(":/data/channels_oztivo.xml"));
    if (!file.open(QIODevice::ReadOnly))
        return;
    QXmlStreamReader reader(&file);
    while (!reader.hasError()) {
        QXmlStreamReader::TokenType tokenType = reader.readNext();
        if (tokenType == QXmlStreamReader::StartElement) {
            if (reader.name() == QLatin1String("channel")) {
                QString channelId = reader.attributes().value
                        (QLatin1String("id")).toString();
                TvChannel *channel = this->channel(channelId);
                if (channel)
                    loadOzTivoChannelData(&reader, channel);
            }
        }
    }
}

void TvChannelList::loadOzTivoChannelData
    (QXmlStreamReader *reader, TvChannel *channel)
{
    // Will leave the XML stream positioned on </channel>.
    Q_ASSERT(reader->isStartElement());
    Q_ASSERT(reader->name() == QLatin1String("channel"));
    while (!reader->hasError()) {
        QXmlStreamReader::TokenType token = reader->readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (reader->name() == QLatin1String("number")) {
                if (reader->attributes().value
                        (QLatin1String("system")) ==
                                QLatin1String("foxtel")) {
                    // Ignore foxtel channel numbers on channels
                    // that already have a free-to-air digital number.
                    if (!channel->channelNumbers().isEmpty())
                        continue;
                }
                QString num = reader->readElementText
                    (QXmlStreamReader::SkipChildElements);
                channel->addChannelNumber(num);
                m_haveChannelNumbers = true;
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (reader->name() == QLatin1String("channel"))
                break;
        }
    }
}

void TvChannelList::refreshChannels(bool forceReload)
{
    // Add the start URL to the front of the queue to fetch
    // it as soon as the current request completes.
    if (m_startUrl.isValid()) {
        Request req;
        req.urls += m_startUrl;
        req.priority = 0;
        req.channel = 0;
        req.date = QDate();
        requestData(req, QDateTime(),
                    forceReload ? -1 : m_startUrlRefresh);
    }
}

// Request a particular day's data based on user selections.
void TvChannelList::requestChannelDay(TvChannel *channel, const QDate &date, int days, bool trimPrevious)
{
    Q_ASSERT(channel);

    // No point performing a network request if no data for the day.
    if (!channel->hasDataFor(date))
        return;

    // Trim requests for priority 1 and 2, which are the requests
    // for the current day and the next day.  Since we are about
    // to request a different day for the UI, there's no point
    // retrieving the previous day's data any more.
    if (trimPrevious)
        trimRequests(1, 2);

    // Fetch the day URL and start a request for it.
    QList<QUrl> urls = channel->dayUrls(date);
    if (urls.isEmpty())
        return;
    Request req;
    req.urls = urls;
    req.priority = 1;
    req.channel = channel;
    req.date = date;
    requestData(req, channel->dayLastModified(date));

    // Add extra days if we want a 7-day outlook.  And add one more
    // day after that to populate "Late Night" timeslots, which are
    // actually "Early Morning" the next day.
    int extraDay = 1;
    while (extraDay <= days) {
        QDate nextDay = date.addDays(extraDay);
        if (channel->hasDataFor(nextDay)) {
            urls = channel->dayUrls(nextDay);
            if (!urls.isEmpty()) {
                req.urls = urls;
                req.priority = 2;
                req.channel = channel;
                req.date = nextDay;
                requestData(req, channel->dayLastModified(nextDay));
            }
        }
        ++extraDay;
    }
}

void TvChannelList::abort()
{
    m_currentRequest = QUrl();
    m_requests.clear();
    m_contents.clear();
    m_busy = false;
    m_progress = 1.0f;
    m_requestsToDo = 0;
    m_requestsDone = 0;
    if (m_reply) {
        disconnect(m_reply, SIGNAL(readyRead()), this, SLOT(requestReadyRead()));
        disconnect(m_reply, SIGNAL(finished()), this, SLOT(requestFinished()));
        disconnect(m_reply, SIGNAL(error(QNetworkReply::NetworkError)),
                   this, SLOT(requestError(QNetworkReply::NetworkError)));
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = 0;
    }
    emit busyChanged(m_busy);
    emit progressChanged(m_progress);
}

void TvChannelList::reload()
{
    // Clear the "last-fetched" times and force a request to
    // the server to get the channel list.  We'll still use
    // If-Modified-Since to reuse the local disk copy if possible,
    // but we want to know if the cache is up to date on reload.
    m_lastFetch.clear();
    refreshChannels(true);
}

void TvChannelList::reloadService()
{
    abort();

    qDeleteAll(m_channels);
    qDeleteAll(m_bookmarks);
    m_channels.clear();
    m_activeChannels.clear();
    m_hiddenChannelIds.clear();
    m_iconFiles.clear();
    m_hasDataFor = false;
    m_largeIcons = false;
    m_haveChannelNumbers = false;
    m_bookmarks.clear();
    m_indexedBookmarks.clear();
    m_serviceId = QString();
    m_serviceName = QString();
    m_startUrl = QUrl();

    emit channelsChanged();
    emit bookmarksChanged();

    QSettings settings(QLatin1String("Southern Storm"),
                       QLatin1String("qtvguide"));
    settings.beginGroup(QLatin1String("Service"));
    m_serviceId = settings.value(QLatin1String("id")).toString();
    m_serviceName = settings.value(QLatin1String("name")).toString();
    QString url = settings.value(QLatin1String("url")).toString();
    if (!url.isEmpty())
        m_startUrl = QUrl(url);
    else
        m_startUrl = QUrl();
    m_startUrlRefresh = settings.value(QLatin1String("refresh"), 24).toInt();
    if (m_startUrlRefresh < 1)
        m_startUrlRefresh = 1;
    settings.endGroup();
    loadServiceSettings(&settings);

    QTimer::singleShot(0, this, SLOT(refreshChannels()));
}

void TvChannelList::updateChannels(bool largeIcons)
{
    QSet<QString> hidden;
    QMap<QString, QString> iconFiles;
    for (int index = 0; index < m_activeChannels.size(); ++index) {
        TvChannel *channel = m_activeChannels.at(index);
        if (channel->isHidden())
            hidden.insert(channel->id());
        QString file = channel->iconFile();
        if (!file.isEmpty())
            iconFiles.insert(channel->id(), file);
    }
    if (m_hiddenChannelIds != hidden ||
            m_iconFiles != iconFiles ||
            m_largeIcons != largeIcons) {
        m_hiddenChannelIds = hidden;
        m_iconFiles = iconFiles;
        m_largeIcons = largeIcons;
        saveChannelSettings();
        emit hiddenChannelsChanged();
    }
}

void TvChannelList::authenticationRequired(QNetworkReply *reply, QAuthenticator *authenticator)
{
    // TODO
    Q_UNUSED(reply);
    Q_UNUSED(authenticator);
}

#ifndef QT_NO_OPENSSL

void TvChannelList::sslErrors(QNetworkReply *reply, const QList<QSslError> &errors)
{
    // TODO
    Q_UNUSED(reply);
    Q_UNUSED(errors);
}

#endif

void TvChannelList::throttleTimeout()
{
    m_throttled = false;
    nextPending();
}

void TvChannelList::requestReadyRead()
{
    char buffer[1024];
    qint64 len;
    while ((len = m_reply->read(buffer, sizeof(buffer))) > 0)
        m_contents.append(buffer, (int)len);
}

void TvChannelList::requestFinished()
{
    if (!m_reply)
        return;
    m_reply->deleteLater();
    m_reply = 0;
    if (!m_contents.isEmpty()) {
#ifdef DEBUG_NETWORK
        qWarning() << "fetch succeeded:" << m_currentRequest << "size:" << m_contents.size();
#endif
        m_lastFetch.insert(m_currentRequest, QDateTime::currentDateTime());
        QXmlStreamReader reader(m_contents);
        while (!reader.hasError()) {
            QXmlStreamReader::TokenType tokenType = reader.readNext();
            if (tokenType == QXmlStreamReader::StartElement) {
                if (reader.name() == QLatin1String("tv"))
                    load(&reader, m_currentRequest);
            } else if (tokenType == QXmlStreamReader::EndDocument) {
                break;
            }
        }
        m_contents = QByteArray();
    } else {
#ifdef DEBUG_NETWORK
        qWarning() << "fetch failed:" << m_currentRequest;
#endif
    }
    int index = 0;
    while (index < m_requests.size()) {
        // Remove repeated entries for the same URL at other priorities.
        if (m_requests.at(index).urls.contains(m_currentRequest)) {
            m_requests.removeAt(index);
            --m_requestsToDo;
        } else {
            ++index;
        }
    }
    m_currentRequest = QUrl();
    ++m_requestsDone;
    nextPending();
    if (!m_currentRequest.isValid() && m_busy && m_requests.isEmpty()) {
        // Turn off the busy flag.
        m_busy = false;
        m_progress = 1.0f;
        m_requestsToDo = 0;
        m_requestsDone = 0;
        emit busyChanged(m_busy);
        emit progressChanged(m_progress);
    }
}

void TvChannelList::requestError(QNetworkReply::NetworkError error)
{
    qWarning() << "TvChannelList: request for url"
               << m_currentRequest << "failed, error =" << int(error);
}

void TvChannelList::requestData
    (const Request &req, const QDateTime &lastmod, int refreshAge)
{
    // Bail out if the url is currently being requested.
    if (m_currentRequest.isValid() && req.urls.contains(m_currentRequest))
        return;

    // Look in the cache to see if the data is fresh enough.
    // Check all URL's in the list in case the same data is
    // fresh under a different URL.  We assume that the data is
    // fresh if the Last-Modified time matches what we expect
    // or if the last time we fetched it within this process
    // was less than an hour ago.
    QUrl url;
    QIODevice *device = 0;
    QDateTime age = QDateTime::currentDateTime().addSecs(-(60 * 60));
    QDateTime refAge = QDateTime::currentDateTime().addSecs(-(60 * 60 * refreshAge));
    QDateTime lastFetch;
    for (int index = 0; index < req.urls.size(); ++index) {
        url = req.urls.at(index);
        if (lastmod.isValid()) {
            QNetworkCacheMetaData meta = m_nam.cache()->metaData(url);
            if (meta.isValid() && meta.lastModified() == lastmod) {
                device = m_nam.cache()->data(url);
                if (device) {
#ifdef DEBUG_NETWORK
                    qWarning() << "using cache for:" << url
                               << "last modified:" << lastmod.toLocalTime();
#endif
                    break;
                }
            }
        } else if (refreshAge != -1) {
            QNetworkCacheMetaData meta = m_nam.cache()->metaData(url);
            if (meta.isValid() && meta.lastModified() >= refAge) {
                device = m_nam.cache()->data(url);
                if (device) {
#ifdef DEBUG_NETWORK
                    qWarning() << "using cache for:" << url
                               << "last modified:" << meta.lastModified().toLocalTime()
                               << "refresh: every" << refreshAge << "hours";
#endif
                    break;
                }
            }
        }
        lastFetch = m_lastFetch.value(url, QDateTime());
        if (lastFetch.isValid() && lastFetch >= age) {
            device = m_nam.cache()->data(url);
            if (device) {
#ifdef DEBUG_NETWORK
                qWarning() << "using cache for:" << url
                           << "last fetched:" << lastFetch;
#endif
                break;
            }
        }
    }
    if (device) {
        QXmlStreamReader *reader = new QXmlStreamReader(device);
        while (!reader->hasError()) {
            QXmlStreamReader::TokenType tokenType = reader->readNext();
            if (tokenType == QXmlStreamReader::StartElement) {
                if (reader->name() == QLatin1String("tv"))
                    load(reader, url);
            } else if (tokenType == QXmlStreamReader::EndDocument) {
                break;
            }
        }
        delete reader;
        delete device;
        return;
    }

    // Add the request to the queue, in priority order.
    int index = 0;
    url = req.urls.at(0);
    while (index < m_requests.size()) {
        const Request &r = m_requests.at(index);
        if (r.priority == req.priority && r.urls.contains(url))
            return;     // We have already queued this request.
        if (r.priority > req.priority)
            break;
        ++index;
    }
    m_requests.insert(index, req);
    ++m_requestsToDo;

    // Start the first request if nothing else is active.
    nextPending();
}

void TvChannelList::trimRequests(int first, int last)
{
    int index = 0;
    bool removed = false;
    while (index < m_requests.size()) {
        const Request &r = m_requests.at(index);
        if (r.priority >= first && r.priority <= last) {
            m_requests.removeAt(index);
            --m_requestsToDo;
            removed = true;
        } else {
            ++index;
        }
    }
    if (removed) {
        if (m_requests.isEmpty() && !m_currentRequest.isValid()) {
            m_busy = false;
            m_progress = 1.0f;
            m_requestsToDo = 0;
            m_requestsDone = 0;
            emit busyChanged(m_busy);
            emit progressChanged(m_progress);
        } else {
            forceProgressUpdate();
        }
    }
}

void TvChannelList::nextPending()
{
    // Bail out if already processing a request, there are no
    // pending requests, or we are currently throttled.
    if (m_currentRequest.isValid() || m_requests.isEmpty() || m_throttled) {
        forceProgressUpdate();
        return;
    }

    // Initiate a GET request for the next pending URL.
    Request req = m_requests.takeFirst();
    m_currentRequest = req.urls.at(0);
    QNetworkRequest request;
    request.setUrl(m_currentRequest);
    request.setRawHeader("User-Agent", "qtvguide/" TVGUIDE_VERSION);
    m_contents = QByteArray();
    m_reply = m_nam.get(request);
    connect(m_reply, SIGNAL(readyRead()), this, SLOT(requestReadyRead()));
    connect(m_reply, SIGNAL(finished()), this, SLOT(requestFinished()));
    connect(m_reply, SIGNAL(error(QNetworkReply::NetworkError)),
            this, SLOT(requestError(QNetworkReply::NetworkError)));
    m_lastFetch.remove(m_currentRequest);
#ifdef DEBUG_NETWORK
    qWarning() << "fetching from network:" << m_currentRequest;
#endif

    // Start the throttle timer.  According to the OzTivo guidelines,
    // there must be at least 1 second between requests.  Requests
    // must also be performed in serial; never in parallel.
    // http://www.oztivo.net/twiki/bin/view/TVGuide/StaticXMLGuideAPI
    //
    // If a request takes 3 seconds to complete then the next request
    // will start immediately.  But if the request takes 0.5 seconds
    // to complete then there will be a 0.5 second delay before the
    // next GET is sent.  This should give slightly better performance
    // for interactive use and when fetching the data for multiple
    // days or channels, while still technically sending no more than
    // one request per second.
    m_throttleTimer->start(1000);
    m_throttled = true;

    // Tell the UI that a network request has been initiated.
    emit networkRequest(req.channel, req.date);

    // Turn on the busy flag and report the progress.
    if (!m_busy) {
        m_busy = true;
        emit busyChanged(true);
    }
    forceProgressUpdate();
}

void TvChannelList::forceProgressUpdate()
{
    if (m_requestsDone < m_requestsToDo)
        m_progress = qreal(m_requestsDone) / qreal(m_requestsToDo);
    else
        m_progress = 1.0f;
    emit progressChanged(m_progress);
}

void TvChannelList::loadServiceSettings(QSettings *settings)
{
    if (m_serviceId.isEmpty())
        return;

    settings->beginGroup(m_serviceId);
    m_largeIcons = settings->value(QLatin1String("largeIcons"), false).toBool();
    m_hiddenChannelIds.clear();
    m_iconFiles.clear();
    int size = settings->beginReadArray(QLatin1String("channels"));
    for (int index = 0; index < size; ++index) {
        settings->setArrayIndex(index);
        QString id = settings->value(QLatin1String("id")).toString();
        if (id.isEmpty())
            continue;
        bool hidden = settings->value(QLatin1String("hidden"), false).toBool();
        if (hidden)
            m_hiddenChannelIds.insert(id);
        QString file = settings->value(QLatin1String("icon")).toString();
        if (!file.isEmpty())
            m_iconFiles.insert(id, file);
    }
    settings->endArray();

    qDeleteAll(m_bookmarks);
    m_bookmarks.clear();
    m_indexedBookmarks.clear();
    size = settings->beginReadArray(QLatin1String("bookmarks"));
    for (int index = 0; index < size; ++index) {
        settings->setArrayIndex(index);
        TvBookmark *bookmark = new TvBookmark();
        bookmark->load(settings);
        m_bookmarks.append(bookmark);
        m_indexedBookmarks.insert(bookmark->title().toLower(), bookmark);
    }
    settings->endArray();
    settings->endGroup();
}

void TvChannelList::saveChannelSettings()
{
    if (m_serviceId.isEmpty())
        return;
    QSettings settings(QLatin1String("Southern Storm"),
                       QLatin1String("qtvguide"));
    settings.beginGroup(m_serviceId);
    settings.setValue(QLatin1String("largeIcons"), m_largeIcons);
    settings.beginWriteArray(QLatin1String("channels"));
    int aindex = 0;
    for (int index = 0; index < m_activeChannels.size(); ++index) {
        TvChannel *channel = m_activeChannels.at(index);
        if (!channel->isHidden() && channel->iconFile().isEmpty())
            continue;
        settings.setArrayIndex(aindex++);
        settings.setValue(QLatin1String("id"), channel->id());
        settings.setValue(QLatin1String("hidden"), channel->isHidden());
        QString file = channel->iconFile();
        if (file.isEmpty())
            settings.remove(QLatin1String("icon"));
        else
            settings.setValue(QLatin1String("icon"), file);
    }
    settings.endArray();
    settings.endGroup();
    settings.sync();
}

void TvChannelList::saveBookmarks()
{
    if (m_serviceId.isEmpty())
        return;
    QSettings settings(QLatin1String("Southern Storm"),
                       QLatin1String("qtvguide"));
    settings.beginGroup(m_serviceId);
    settings.beginWriteArray(QLatin1String("bookmarks"));
    for (int index = 0; index < m_bookmarks.size(); ++index) {
        TvBookmark *bookmark = m_bookmarks.at(index);
        settings.setArrayIndex(index);
        bookmark->save(&settings);
    }
    settings.endArray();
    settings.endGroup();
    settings.sync();
}

void TvChannelList::addBookmark(TvBookmark *bookmark)
{
    Q_ASSERT(bookmark);
    m_bookmarks.append(bookmark);
    m_indexedBookmarks.insert(bookmark->title().toLower(), bookmark);
    emit bookmarksChanged();
    saveBookmarks();
}

void TvChannelList::removeBookmark(TvBookmark *bookmark, bool notify)
{
    Q_ASSERT(bookmark);
    m_bookmarks.removeAll(bookmark);
    m_indexedBookmarks.remove(bookmark->title().toLower(), bookmark);
    if (notify) {
        emit bookmarksChanged();
        saveBookmarks();
    }
}

TvBookmark::Match TvChannelList::matchBookmarks
    (const TvProgramme *programme, TvBookmark **bookmark,
     TvBookmark::MatchOptions options) const
{
    QMultiMap<QString, TvBookmark *>::ConstIterator it;
    it = m_indexedBookmarks.constFind(programme->title().toLower());
    TvBookmark::Match result = TvBookmark::NoMatch;
    while (it != m_indexedBookmarks.constEnd()) {
        TvBookmark::Match match = it.value()->match(programme, options);
        if (match != TvBookmark::NoMatch) {
            if (match == TvBookmark::ShouldMatch) {
                if (result != TvBookmark::TitleMatch) {
                    *bookmark = it.value();
                    result = TvBookmark::ShouldMatch;
                }
            } else if (match != TvBookmark::TitleMatch) {
                *bookmark = it.value();
                return match;
            } else {
                *bookmark = it.value();
                result = TvBookmark::TitleMatch;
            }
        }
        ++it;
    }
    return result;
}

void TvChannelList::replaceBookmarks(const QList<TvBookmark *> &bookmarks)
{
    qDeleteAll(m_bookmarks);
    m_bookmarks = bookmarks;
    m_indexedBookmarks.clear();
    for (int index = 0; index < bookmarks.size(); ++index) {
        TvBookmark *bookmark = bookmarks.at(index);
        m_indexedBookmarks.insert(bookmark->title().toLower(), bookmark);
    }
    emit bookmarksChanged();
    saveBookmarks();
}
