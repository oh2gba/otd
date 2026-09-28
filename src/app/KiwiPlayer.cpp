// SPDX-License-Identifier: GPL-3.0-or-later
#include "KiwiPlayer.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QComboBox>
#include <QInputDialog>
#include <QHBoxLayout>
#include <QIODevice>
#include <QLabel>
#include <QLineEdit>
#include <QMediaDevices>
#include <QPainter>
#include <cmath>
#include <QIcon>
#include <QPixmap>
#include <QPolygonF>
#include <QSlider>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <cstring>

// A plain S-meter: a bar from S0 to S9+60 with the reading written on it.
// Green up to S9, red beyond, as on a radio. clear() blanks it.
class SMeter : public QWidget
{
public:
    explicit SMeter(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(150, 18);
        setMaximumHeight(20);
    }
    void setDbm(double dBm)
    {
        m_dBm = dBm;
        m_valid = true;
        setToolTip(QStringLiteral("%1 dBm").arg(dBm, 0, 'f', 0));
        update();
    }
    void clear()
    {
        m_valid = false;
        setToolTip(QString());
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = rect().adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(palette().color(QPalette::Mid));
        p.setBrush(palette().base());
        p.drawRoundedRect(r, 3, 3);
        if (!m_valid)
            return;
        // S0 = -127 dBm ... S9 = -73 dBm takes 60 % of the bar, +60 dB the rest
        const double s9 = 0.6;
        double frac;
        if (m_dBm <= -73.0)
            frac = s9 * qBound(0.0, (m_dBm + 127.0) / 54.0, 1.0);
        else
            frac = s9 + (1.0 - s9) * qBound(0.0, (m_dBm + 73.0) / 60.0, 1.0);
        const QRectF inner = r.adjusted(1, 1, -1, -1);
        QRectF fill = inner;
        fill.setWidth(inner.width() * frac);
        p.setPen(Qt::NoPen);
        const double s9x = inner.left() + inner.width() * s9;
        p.setBrush(QColor(0x3f, 0xb9, 0x50));
        p.drawRect(QRectF(fill.left(), fill.top(), qMin(fill.right(), s9x) - fill.left(), fill.height()));
        if (fill.right() > s9x)
        {
            p.setBrush(QColor(0xf8, 0x51, 0x49));
            p.drawRect(QRectF(s9x, fill.top(), fill.right() - s9x, fill.height()));
        }
        // ticks at every S unit and every 20 dB above S9
        p.setPen(QPen(palette().color(QPalette::Mid), 1));
        for (int s = 1; s <= 9; ++s)
        {
            const double x = inner.left() + inner.width() * s9 * s / 9.0;
            p.drawLine(QPointF(x, inner.bottom() - 3), QPointF(x, inner.bottom()));
        }
        for (int db = 20; db <= 60; db += 20)
        {
            const double x = s9x + inner.width() * (1.0 - s9) * db / 60.0;
            p.drawLine(QPointF(x, inner.bottom() - 3), QPointF(x, inner.bottom()));
        }
        // the reading
        QString text;
        if (m_dBm >= -73.0)
            text = QStringLiteral("S9+%1").arg(qRound((m_dBm + 73.0) / 10.0) * 10);
        else
            text = QStringLiteral("S%1").arg(qBound(0, qRound((m_dBm + 127.0) / 6.0), 9));
        QFont f = font();
        f.setBold(true);
        f.setPointSizeF(f.pointSizeF() * 0.85);
        p.setFont(f);
        p.setPen(palette().color(QPalette::Text));
        p.drawText(inner.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignRight, text);
    }

private:
    double m_dBm = -127.0;
    bool m_valid = false;
};

// Audio arrives from the network in bursts; the sound card wants a steady
// trickle. This queue sits between them: the sink pulls from it, gets
// silence while the first bit is collected or when the network stalls,
// and the queue is trimmed when the network runs far ahead.
class PcmQueue : public QIODevice
{
public:
    explicit PcmQueue(int bytesPerSecond, QObject* parent = nullptr)
        : QIODevice(parent)
        , m_preroll(bytesPerSecond * 3 / 10)   // 0.3 s before the first sound
        , m_max(bytesPerSecond * 3)            // never more than 3 s behind
    {
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    void push(const QByteArray& pcm)
    {
        m_buf.append(pcm);
        if (m_buf.size() > m_max)
            m_buf.remove(0, m_buf.size() - m_max);
        if (m_buf.size() >= m_preroll)
            m_primed = true;
    }
    int buffered() const { return m_buf.size(); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return m_buf.size() + QIODevice::bytesAvailable(); }

protected:
    qint64 readData(char* data, qint64 maxlen) override
    {
        if (maxlen <= 0)
            return 0;
        qint64 n = 0;
        if (m_primed)
        {
            n = qMin<qint64>(maxlen, m_buf.size());
            std::memcpy(data, m_buf.constData(), size_t(n));
            m_buf.remove(0, int(n));
            if (m_buf.isEmpty())
                m_primed = false;   // ran dry: collect a little again
        }
        if (n < maxlen)
            std::memset(data + n, 0, size_t(maxlen - n));
        return maxlen;
    }
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    QByteArray m_buf;
    const int m_preroll;
    const int m_max;
    bool m_primed = false;
};

KiwiPlayer::KiwiPlayer(QWidget* parent)
    : QWidget(parent)
{
    // two rows: the controls, and under them the status text, so that a
    // changing status never moves the button
    auto* rows = new QVBoxLayout(this);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(2);
    auto* layout = new QHBoxLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    rows->addLayout(layout);

    auto* caption = new QLabel(tr("Listen on KiwiSDR:"));
    caption->setToolTip(tr("Hear the tuned frequency through a public KiwiSDR receiver on the internet.\n"
                           "The list comes from the public KiwiSDR directory, sorted by place; "
                           "type the first letters of a place to jump there."));
    // a plain list: nothing to type into, picking an entry starts it
    m_receiver = new QComboBox;
    m_receiver->setEditable(false);
    m_receiver->setMinimumWidth(260);
    m_receiver->setMaxVisibleItems(25);
    m_receiver->setToolTip(caption->toolTip());
    connect(m_receiver, &QComboBox::activated, this, [this](int index) {
        const QString url = m_receiver->itemData(index).toString();
        if (url.isEmpty())
            return;
        m_current = url;
        updateStar();
        // picking a receiver means: listen to it
        if (isPlaying())
            stop();
        togglePlay();
    });

    // a search box narrows the list to the entries containing the text:
    // "netherlands", "loop", a call sign; the list itself stays a list
    m_search = new QLineEdit;
    m_search->setPlaceholderText(tr("search receivers"));
    m_search->setClearButtonEnabled(true);
    m_search->setMaximumWidth(180);
    m_search->setToolTip(tr("Show only the receivers whose place or name contains this text"));
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString&) { rebuildList(); });

    // the demodulation mode: the rig's while a rig is followed (greyed),
    // otherwise the listener's own choice
    m_modeBox = new QComboBox;
    m_modeBox->addItems({QStringLiteral("AM"), QStringLiteral("USB"), QStringLiteral("LSB"),
                         QStringLiteral("CW"), QStringLiteral("NFM")});
    m_modeBox->setToolTip(tr("Mode the receiver demodulates; follows the rig when one is connected"));
    connect(m_modeBox, &QComboBox::activated, this, [this](int) {
        if (m_rigDriven)
            return;
        m_manualMode = m_modeBox->currentText();
        m_mode = m_manualMode;
        m_client.tune(m_kHz, m_mode);
        emit manualModeChanged(m_manualMode);
    });

    // the star marks a favourite: starred receivers lead the list
    m_star = new QToolButton;   // a plain button like + and play, quiet grey star
    connect(m_star, &QToolButton::clicked, this, [this]() {
        const QString cur = currentReceiver();
        if (cur.isEmpty())
            return;
        if (m_favourites.contains(cur))
            m_favourites.removeAll(cur);
        else
            m_favourites.prepend(cur);
        m_current = cur;
        rebuildList();
        emit receiversChanged();
    });

    // own addresses go in through a small dialog, not the list itself
    m_add = new QToolButton;
    m_add->setText(QStringLiteral("+"));
    m_add->setToolTip(tr("Add a receiver of your own by its address, e.g. http://example.ddns.net:8073"));
    connect(m_add, &QToolButton::clicked, this, [this]() {
        bool ok = false;
        const QString text = QInputDialog::getText(this, tr("Add a KiwiSDR receiver"),
                                                   tr("Address of the receiver (http://host:port):"),
                                                   QLineEdit::Normal, QStringLiteral("http://"), &ok).trimmed();
        // anything the client can connect to: host:port, IP:port, with or
        // without http://; the untouched "http://" has no host
        if (!ok || KiwiClient::receiverUrl(text).host().isEmpty())
            return;
        m_current = text;
        if (!m_custom.contains(text))
            m_custom.prepend(text);
        rebuildList();
        emit receiversChanged();
        if (isPlaying())
            stop();
        togglePlay();
    });

    m_play = new QToolButton;
    m_play->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_play->setToolTip(tr("Start or stop listening"));
    connect(m_play, &QToolButton::clicked, this, &KiwiPlayer::togglePlay);

    m_volume = new QSlider(Qt::Horizontal);
    m_volume->setRange(0, 100);
    m_volume->setValue(70);
    m_volume->setMaximumWidth(120);
    m_volume->setToolTip(tr("Volume"));
    connect(m_volume, &QSlider::valueChanged, this, [this](int v) {
        if (m_sink)
            m_sink->setVolume(v / 100.0);
    });

    m_meter = new SMeter;

    layout->addWidget(caption);
    layout->addWidget(m_search);
    layout->addWidget(m_receiver, 1);
    layout->addWidget(m_star);
    layout->addWidget(m_add);
    layout->addWidget(m_play);
    layout->addWidget(m_modeBox);
    layout->addWidget(m_volume);
    layout->addWidget(m_meter);

    connect(&m_client, &KiwiClient::stateChanged, this, &KiwiPlayer::setStatus);
    connect(&m_client, &KiwiClient::audio, this, &KiwiPlayer::onAudio);
    connect(&m_client, &KiwiClient::closed, this, &KiwiPlayer::onClosed);
    connect(&m_client, &KiwiClient::sMeter, this, [this](double dBm) { m_meter->setDbm(dBm); });
}

KiwiPlayer::~KiwiPlayer()
{
    m_client.close();
    if (m_sink)
        m_sink->disconnect(this);
    delete m_sink;
}

namespace
{
QString sinkStateText(QAudio::State st, QAudio::Error err)
{
    if (err == QAudio::OpenError)
        return KiwiPlayer::tr("sound device could not be opened");
    if (err == QAudio::IOError)
        return KiwiPlayer::tr("sound device error");
    if (err == QAudio::FatalError)
        return KiwiPlayer::tr("sound device failed");
    switch (st)
    {
    case QAudio::ActiveState:    return KiwiPlayer::tr("playing");
    case QAudio::SuspendedState: return KiwiPlayer::tr("sound suspended");
    case QAudio::StoppedState:   return KiwiPlayer::tr("sound stopped");
    case QAudio::IdleState:      return KiwiPlayer::tr("waiting for audio");
    }
    return QString();
}
}

QStringList KiwiPlayer::receivers() const
{
    return m_custom;
}

void KiwiPlayer::setReceivers(const QStringList& custom, const QStringList& favourites, const QString& current)
{
    m_custom = custom;
    m_favourites = favourites;
    m_current = current;
    rebuildList();
}

// A five-pointed star: filled amber when marked, a grey outline otherwise.
QIcon KiwiPlayer::starIcon(bool on)
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPolygonF star;
    for (int i = 0; i < 10; ++i)
    {
        const double a = -M_PI / 2 + i * M_PI / 5;
        const double r = (i % 2 == 0) ? 7.0 : 3.0;
        star << QPointF(8 + r * std::cos(a), 8.5 + r * std::sin(a));
    }
    // grey either way: filled when starred, an outline when not
    const QColor grey(0x8a, 0x94, 0x9d);
    p.setPen(QPen(grey, 1.2));
    p.setBrush(on ? QBrush(grey) : Qt::NoBrush);
    p.drawPolygon(star);
    return QIcon(pm);
}

void KiwiPlayer::updateStar()
{
    const QString cur = currentReceiver();
    const bool on = !cur.isEmpty() && m_favourites.contains(cur);
    m_star->setIcon(starIcon(on));
    m_star->setEnabled(!cur.isEmpty());
    m_star->setToolTip(on ? tr("Remove the star from this receiver") : tr("Star this receiver: it stays at the top of the list"));
}

void KiwiPlayer::setDirectory(const QList<KiwiDirectory::Receiver>& list)
{
    m_directory = list;
    rebuildList();
}

QString KiwiPlayer::labelFor(const KiwiDirectory::Receiver& r)
{
    QString label = r.location.isEmpty() ? r.name : r.location + QStringLiteral(" \u2014 ") + r.name;
    if (r.usersMax > 0)
        label += QStringLiteral("  (%1/%2)").arg(r.users).arg(r.usersMax);
    return label;
}

void KiwiPlayer::rebuildList()
{
    const QSignalBlocker block(m_receiver);
    m_receiver->clear();
    const QString needle = m_search ? m_search->text().trimmed() : QString();
    auto matches = [&](const QString& a, const QString& b, const QString& c) {
        return needle.isEmpty() || a.contains(needle, Qt::CaseInsensitive)
               || b.contains(needle, Qt::CaseInsensitive) || c.contains(needle, Qt::CaseInsensitive);
    };
    auto labelOf = [&](const QString& url) {
        for (const KiwiDirectory::Receiver& r : m_directory)
            if (r.url == url)
                return labelFor(r);
        return url;
    };
    // starred receivers first
    int shown = 0;
    for (const QString& url : m_favourites)
    {
        const QString label = labelOf(url);
        if (!matches(label, url, QString()))
            continue;
        m_receiver->addItem(starIcon(true), label, url);
        ++shown;
    }
    if (shown > 0)
        m_receiver->insertSeparator(m_receiver->count());
    // then the user's own addresses
    int own = 0;
    for (const QString& url : m_custom)
    {
        const QString label = labelOf(url);
        if (m_favourites.contains(url) || !matches(label, url, QString()))
            continue;
        m_receiver->addItem(starIcon(false), label, url);
        ++own;
        ++shown;
    }
    if (own > 0 && !m_directory.isEmpty())
        m_receiver->insertSeparator(m_receiver->count());
    // then the public directory
    for (const KiwiDirectory::Receiver& r : m_directory)
    {
        if (r.offline || m_favourites.contains(r.url) || m_custom.contains(r.url))
            continue;
        if (!matches(r.location, r.name, r.url))
            continue;
        m_receiver->addItem(starIcon(false), labelFor(r), r.url);
        ++shown;
    }
    if (!needle.isEmpty() && shown == 0)
        m_receiver->addItem(tr("(no receiver matches \"%1\")").arg(needle), QString());
    // The current receiver is always there and chosen, even when the search
    // hides it or an update calls it offline or leaves it out: the list names
    // the receiver being heard, and the star, + and the saved choice act on
    // it, not on whatever happens to be first.
    if (!m_current.isEmpty() && m_receiver->findData(m_current) < 0)
    {
        if (m_receiver->count() > 0)
            m_receiver->insertSeparator(0);
        m_receiver->insertItem(0, starIcon(m_favourites.contains(m_current)), labelOf(m_current), m_current);
    }
    const int idx = m_current.isEmpty() ? -1 : m_receiver->findData(m_current);
    if (idx >= 0)
        m_receiver->setCurrentIndex(idx);
    else if (m_receiver->count() > 0)
        m_receiver->setCurrentIndex(0);
    updateStar();
}

QString KiwiPlayer::currentReceiver() const
{
    const int idx = m_receiver->currentIndex();
    if (idx >= 0 && !m_receiver->itemData(idx).toString().isEmpty())
        return m_receiver->itemData(idx).toString();
    return m_current;
}

int KiwiPlayer::volume() const
{
    return m_volume->value();
}

void KiwiPlayer::setVolume(int percent)
{
    m_volume->setValue(qBound(0, percent, 100));
}

void KiwiPlayer::rememberCurrent()
{
    const QString cur = currentReceiver();
    if (cur.isEmpty())
        return;
    // Only + makes an address the user's own. A directory receiver that is
    // off the list for a while stays the current one (rebuildList keeps it)
    // without being copied into the own addresses for good.
    m_current = cur;
    emit receiversChanged();
}

void KiwiPlayer::togglePlay()
{
    if (isPlaying())
    {
        stop();
        return;
    }
    const QString cur = currentReceiver();
    if (cur.isEmpty())
    {
        setStatus(tr("Pick a receiver from the list, or add one with +"));
        return;
    }
    rememberCurrent();
    emit playRequested();
    m_play->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    m_client.tune(m_kHz, m_mode);
    m_client.open(cur);
}

void KiwiPlayer::stop()
{
    stopWith(QString());
}

// The one way playing ends: the session, the sound and the controls go
// back to rest, and the status says why (empty: stopped on request).
void KiwiPlayer::stopWith(const QString& status)
{
    m_client.close();   // emits nothing, so the status below stays
    ++m_sessionsEnded;
    if (m_sink)
    {
        m_sink->disconnect(this);   // no state texts from a sink on its way out
        m_sink->stop();
    }
    delete m_sink;
    m_sink = nullptr;
    delete m_queue;
    m_queue = nullptr;
    m_sinkRate = 0;
    m_play->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_meter->clear();
    setStatus(status);
}

void KiwiPlayer::tune(double kHz, const QString& rigMode)
{
    m_kHz = kHz;
    m_rigDriven = !rigMode.isEmpty();
    m_modeBox->setEnabled(!m_rigDriven);
    if (m_rigDriven)
    {
        // show the rig's mode in the box too, mapped onto what the receiver has
        const QString up = rigMode.toUpper();
        // (Hamlib's data and ECSS modes end in the sideband: PKTUSB, ECSSLSB)
        const QString shown = up.endsWith(QLatin1String("USB"))   ? QStringLiteral("USB")
                            : up.endsWith(QLatin1String("LSB"))   ? QStringLiteral("LSB")
                            : up.startsWith(QLatin1String("CW"))  ? QStringLiteral("CW")
                            : up.contains(QLatin1String("FM"))    ? QStringLiteral("NFM")
                                                                   : QStringLiteral("AM");
        const QSignalBlocker b(m_modeBox);
        m_modeBox->setCurrentText(shown);
        m_mode = shown;
    }
    else
    {
        // back in the listener's hands: their own mode, not the rig's last one
        const QSignalBlocker b(m_modeBox);
        m_modeBox->setCurrentText(m_manualMode);
        m_mode = m_manualMode;
    }
    m_client.tune(kHz, m_mode);
}

void KiwiPlayer::setManualMode(const QString& mode)
{
    if (m_modeBox->findText(mode) < 0)
        return;
    m_manualMode = mode;
    if (m_rigDriven)
        return;   // the rig leads for now; this applies when it stops
    const QSignalBlocker b(m_modeBox);
    m_modeBox->setCurrentText(mode);
    m_mode = mode;
    m_client.tune(m_kHz, m_mode);
}

void KiwiPlayer::onAudio(const QByteArray& pcm)
{
    const int rate = m_client.sampleRate();
    if (!m_sink || m_sinkRate != rate)
    {
        if (m_sink)
            m_sink->disconnect(this);
        delete m_sink;
        m_sink = nullptr;
        delete m_queue;
        m_queue = nullptr;
        QAudioFormat fmt;
        fmt.setSampleRate(rate);
        fmt.setChannelCount(1);
        fmt.setSampleFormat(QAudioFormat::Int16);
        const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
        if (dev.isNull())
        {
            stopWith(tr("No sound output device"));
            return;
        }
        if (!dev.isFormatSupported(fmt))
        {
            stopWith(tr("Sound device %1 does not take %2 Hz mono").arg(dev.description()).arg(rate));
            return;
        }
        m_queue = new PcmQueue(rate * 2, this);
        m_sink = new QAudioSink(dev, fmt, this);
        m_sink->setBufferSize(rate * 2 / 2);   // half a second in the device
        m_sink->setVolume(m_volume->value() / 100.0);
        connect(m_sink, &QAudioSink::stateChanged, this, [this](QAudio::State st) {
            onSinkState(st, m_sink->error());
        });
        m_sink->start(m_queue);   // pull mode: the sink asks, the queue answers
        m_sinkRate = rate;
    }
    m_queue->push(pcm);
}

void KiwiPlayer::onSinkState(QAudio::State state, QAudio::Error error)
{
    const QString t = sinkStateText(state, error);
    const bool failed = state == QAudio::StoppedState
                        && (error == QAudio::OpenError || error == QAudio::IOError || error == QAudio::FatalError);
    if (!failed)
    {
        if (!t.isEmpty())
            setStatus(tr("%1: %2").arg(m_client.receiver().host(), t));
        return;
    }
    // The sound is gone, so the session ends too. Not from here: this runs
    // inside the sink's own signal and stopWith deletes the sink. If the
    // session has ended some other way meanwhile, there is nothing to do.
    const int session = m_sessionsEnded;
    QMetaObject::invokeMethod(this, [this, session, t]() {
        if (session == m_sessionsEnded)
            stopWith(tr("Stopped: %1").arg(t));
    }, Qt::QueuedConnection);
}

void KiwiPlayer::onClosed(const QString& reason)
{
    stopWith(tr("Stopped: %1").arg(reason));
}

void KiwiPlayer::setStatus(const QString& text)
{
    // shown by the main window in its status bar
    m_statusText = text;
    emit statusChanged(text);
}
