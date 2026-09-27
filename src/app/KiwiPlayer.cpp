// SPDX-License-Identifier: GPL-3.0-or-later
#include "KiwiPlayer.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QComboBox>
#include <QCompleter>
#include <QHBoxLayout>
#include <QIODevice>
#include <QLabel>
#include <QLineEdit>
#include <QMediaDevices>
#include <QPainter>
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
                           "The list comes from the public KiwiSDR directory; you can also paste an "
                           "address of your own, e.g. http://example.ddns.net:8073"));
    m_receiver = new QComboBox;
    m_receiver->setEditable(true);
    m_receiver->setInsertPolicy(QComboBox::NoInsert);
    m_receiver->setMinimumWidth(260);
    m_receiver->lineEdit()->setPlaceholderText(tr("type a place or name, or paste http://receiver:8073"));
    m_receiver->setToolTip(caption->toolTip());
    // typing filters the list: "finland", "loop", a call sign ...
    auto* completer = new QCompleter(m_receiver->model(), m_receiver);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setMaxVisibleItems(15);
    m_receiver->setCompleter(completer);
    connect(m_receiver, &QComboBox::activated, this, [this](int index) {
        const QString url = m_receiver->itemData(index).toString();
        if (url.isEmpty())
            return;
        m_current = url;
        // picking a receiver means: listen to it
        if (isPlaying())
            stop();
        togglePlay();
    });
    connect(m_receiver->lineEdit(), &QLineEdit::returnPressed, this, [this]() {
        if (!isPlaying())
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
    m_status = new QLabel;
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);

    layout->addWidget(caption);
    layout->addWidget(m_receiver, 1);
    layout->addWidget(m_play);
    layout->addWidget(m_volume);
    layout->addWidget(m_meter);
    m_status->setIndent(4);
    rows->addWidget(m_status);

    connect(&m_client, &KiwiClient::stateChanged, this, &KiwiPlayer::setStatus);
    connect(&m_client, &KiwiClient::audio, this, &KiwiPlayer::onAudio);
    connect(&m_client, &KiwiClient::closed, this, &KiwiPlayer::onClosed);
    connect(&m_client, &KiwiClient::sMeter, this, [this](double dBm) { m_meter->setDbm(dBm); });
}

KiwiPlayer::~KiwiPlayer()
{
    m_client.close();
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

void KiwiPlayer::setReceivers(const QStringList& custom, const QString& current)
{
    m_custom = custom;
    m_current = current;
    rebuildList();
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
    for (const QString& url : m_custom)
        m_receiver->addItem(url, url);
    if (!m_custom.isEmpty() && !m_directory.isEmpty())
        m_receiver->insertSeparator(m_receiver->count());
    for (const KiwiDirectory::Receiver& r : m_directory)
    {
        if (r.offline)
            continue;
        m_receiver->addItem(labelFor(r), r.url);
    }
    // show the current choice by its label when it is in the list
    int idx = -1;
    for (int i = 0; i < m_receiver->count() && idx < 0; ++i)
        if (m_receiver->itemData(i).toString() == m_current)
            idx = i;
    if (idx >= 0)
        m_receiver->setCurrentIndex(idx);
    else
        m_receiver->setCurrentText(m_current);
}

QString KiwiPlayer::currentReceiver() const
{
    // a picked entry carries its address; typed text is used as is when
    // it looks like one
    const int idx = m_receiver->currentIndex();
    const QString typed = m_receiver->currentText().trimmed();
    if (idx >= 0 && m_receiver->itemText(idx) == typed)
        return m_receiver->itemData(idx).toString();
    if (typed.contains(QLatin1Char('.')) && !typed.contains(QLatin1Char(' ')))
        return typed;
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
    m_current = cur;
    // an address that is not in the public directory is kept as the user's own
    bool known = false;
    for (const KiwiDirectory::Receiver& r : m_directory)
        if (r.url == cur)
            known = true;
    if (!known && !m_custom.contains(cur))
    {
        m_custom.prepend(cur);
        rebuildList();
    }
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
        setStatus(tr("Paste a receiver address first (see kiwisdr.com/public)"));
        return;
    }
    rememberCurrent();
    m_play->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    m_client.tune(m_kHz, m_mode);
    m_client.open(QUrl(cur));
}

void KiwiPlayer::stop()
{
    m_client.close();
    if (m_sink)
        m_sink->stop();
    delete m_sink;
    m_sink = nullptr;
    delete m_queue;
    m_queue = nullptr;
    m_sinkRate = 0;
    m_play->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_meter->clear();
    setStatus(QString());
}

void KiwiPlayer::tune(double kHz, const QString& mode)
{
    m_kHz = kHz;
    m_mode = mode;
    m_client.tune(kHz, mode);
}

void KiwiPlayer::onAudio(const QByteArray& pcm)
{
    const int rate = m_client.sampleRate();
    if (!m_sink || m_sinkRate != rate)
    {
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
            setStatus(tr("No sound output device"));
            m_client.close();
            return;
        }
        if (!dev.isFormatSupported(fmt))
        {
            setStatus(tr("Sound device %1 does not take %2 Hz mono").arg(dev.description()).arg(rate));
            m_client.close();
            return;
        }
        m_queue = new PcmQueue(rate * 2, this);
        m_sink = new QAudioSink(dev, fmt, this);
        m_sink->setBufferSize(rate * 2 / 2);   // half a second in the device
        m_sink->setVolume(m_volume->value() / 100.0);
        connect(m_sink, &QAudioSink::stateChanged, this, [this](QAudio::State st) {
            const QString t = sinkStateText(st, m_sink->error());
            if (!t.isEmpty())
                setStatus(tr("%1: %2").arg(m_client.receiver().host(), t));
        });
        m_sink->start(m_queue);   // pull mode: the sink asks, the queue answers
        m_sinkRate = rate;
    }
    m_queue->push(pcm);
}

void KiwiPlayer::onClosed(const QString& reason)
{
    if (m_sink)
        m_sink->stop();
    delete m_sink;
    m_sink = nullptr;
    delete m_queue;
    m_queue = nullptr;
    m_sinkRate = 0;
    m_play->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_meter->clear();
    setStatus(reason.isEmpty() ? QString() : tr("Stopped: %1").arg(reason));
}

void KiwiPlayer::setStatus(const QString& text)
{
    m_status->setText(text);
}
