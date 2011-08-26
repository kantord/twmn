#include "widget.h"
#include <exception>
#include <iostream>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <QApplication>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <QHBoxLayout>
#include <QTimer>
#include <QLabel>
#include <QDesktopWidget>
#include <QPixmap>
#include <QPainter>
#include <QTextDocument>
#include <QShortcut>
#include <QIcon>
#include <QWheelEvent>
#include "settings.h"
#include "shortcutgrabber.h"

Widget::Widget() : m_shortcutGrabber(this, m_settings)
{
    setWindowFlags(Qt::ToolTip);
    QPropertyAnimation* anim = new QPropertyAnimation(this);
    anim->setTargetObject(this);
    m_animation.addAnimation(anim);
    anim->setEasingCurve(QEasingCurve::OutBounce);
    connect(anim, SIGNAL(finished()), this, SLOT(reverseTrigger()));
    connectForPosition(m_settings.get("gui/position").toString());
    connect(&m_dbus, SIGNAL(messageReceived(Message)), this, SLOT(appendMessageToQueue(Message)));
    connect(&m_visible, SIGNAL(timeout()), this, SLOT(reverseStart()));
    m_visible.setSingleShot(true);
    QAbstractEventDispatcher::instance()->setEventFilter(ShortcutGrabber::eventFilter);
    QHBoxLayout* l = new QHBoxLayout;
    l->setSizeConstraint(QLayout::SetNoConstraint);
    l->setMargin(0);
    l->setContentsMargins(0, 0, 0, 0);
    setLayout(l);
    l->addWidget(m_contentView["icon"] = new QLabel);
    l->addWidget(m_contentView["title"] = new QLabel);
    l->addWidget(m_contentView["text"] = new QLabel);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(onHide()));
    // Let the event loop run
    QTimer::singleShot(30, this, SLOT(init()));
}

Widget::~Widget()
{
}

void Widget::init()
{
    int port = m_settings.get("main/port").toInt();
    if (!m_socket.bind(QHostAddress::Any, port)) {
        qCritical() << "Unable to listen port" << port;
        return;
    }
    connect(&m_socket, SIGNAL(readyRead()), this, SLOT(onDataReceived()));
    m_shortcutGrabber.loadShortcuts();
}

void Widget::onDataReceived()
{
    boost::property_tree::ptree tree;
    Message m;
    try {
        quint64 size = m_socket.pendingDatagramSize();
        QByteArray data(size, '\0');
        m_socket.readDatagram(data.data(), size);
        std::istringstream iss (data.data());
        boost::property_tree::xml_parser::read_xml(iss, tree);
        boost::property_tree::ptree& root = tree.get_child("root");
        boost::property_tree::ptree::iterator it;
        for (it = root.begin(); it != root.end(); ++it) {
            //std::cout << it->first << " - " << it->second.get_value<std::string>() << std::endl;
            m.data[QString::fromStdString(it->first)] = boost::optional<QVariant>(it->second.get_value<std::string>().c_str());
        }
    }
    catch (const std::exception& e) {
        std::cout << "ERROR : " << e.what() << std::endl;
    }
    appendMessageToQueue(m);
}

void Widget::appendMessageToQueue(const Message& msg)
{
    if (msg.data["id"] && !m_messageQueue.isEmpty() && update(msg))
        ;
    else {
        m_messageQueue.push_back(msg);
        QTimer::singleShot(30, this, SLOT(processMessageQueue()));
    }
}

void Widget::processMessageQueue()
{
    if (m_messageQueue.empty())
        return;
    if (m_animation.state() == QAbstractAnimation::Running || (m_animation.totalDuration()-m_animation.currentTime()) < 50)
        return;
    QFont boldFont = font();
    boldFont.setBold(true);
    Message& m = m_messageQueue.front();
    loadDefaults();
    if (m.data["aot"]->toBool())
        raise();
    setupFont();
    setupColors();
    setupIcon();
    setupTitle();
    setupContent();
    connectForPosition(m.data["pos"]->toString());
    m_animation.setDirection(QAnimationGroup::Forward);
    int width = computeWidth();
    qobject_cast<QPropertyAnimation*>(m_animation.animationAt(0))->setEasingCurve(QEasingCurve::OutBounce);
    qobject_cast<QPropertyAnimation*>(m_animation.animationAt(0))->setStartValue(0);
    qobject_cast<QPropertyAnimation*>(m_animation.animationAt(0))->setEndValue(width);
    m_animation.start();
    QString soundCommand = m.data["sc"]->toString();
    if (!soundCommand.isEmpty())
        QProcess::startDetached(soundCommand);
    m_shortcutGrabber.enableShortcuts();
}

void Widget::updateTopLeftAnimation(QVariant value)
{
    const int finalHeight = m_animation.direction() == QAbstractAnimation::Forward ? m_messageQueue.front().data["size"]->toInt() : height();
    QPoint p(0, 0);
    if (m_settings.has("gui/absolute_position") && !m_settings.get("gui/absolute_position").toString().isEmpty()) {
        QPoint tmp = stringToPos(m_settings.get("gui/absolute_position").toString());
        if (!tmp.isNull())
            p = tmp;
    }
    setGeometry(p.y(), p.y(), value.toInt(), finalHeight);
    layout()->setSpacing(0);
    show();
}

void Widget::updateTopRightAnimation(QVariant value)
{
    const int end = QDesktopWidget().availableGeometry(this).width();
    const int val = value.toInt();
    const int finalHeight = m_animation.direction() == QAbstractAnimation::Forward ? m_messageQueue.front().data["size"]->toInt() : height();
    QPoint p(end, 0);
    if (m_settings.has("gui/absolute_position") && !m_settings.get("gui/absolute_position").toString().isEmpty()) {
        QPoint tmp = stringToPos(m_settings.get("gui/absolute_position").toString());
        if (!tmp.isNull())
            p = tmp;
    }
    setGeometry(p.x()-val, p.y(), val, finalHeight);
    layout()->setSpacing(0);
    show();
}

void Widget::updateBottomRightAnimation(QVariant value)
{
    const int wend = QDesktopWidget().availableGeometry(this).width();
    const int hend = QDesktopWidget().availableGeometry(this).height();
    const int finalHeight = m_animation.direction() == QAbstractAnimation::Forward ? m_messageQueue.front().data["size"]->toInt() : height();
    const int val = value.toInt();
    QPoint p(wend, hend);
    if (m_settings.has("gui/absolute_position") && !m_settings.get("gui/absolute_position").toString().isEmpty()) {
        QPoint tmp = stringToPos(m_settings.get("gui/absolute_position").toString());
        if (!tmp.isNull())
            p = tmp;
    }
    setGeometry(p.x()-val, p.y()-height(), val, finalHeight);
    layout()->setSpacing(0);
    show();
}

void Widget::updateBottomLeftAnimation(QVariant value)
{
    const int hend = QDesktopWidget().availableGeometry(this).height();
    const int finalHeight = m_animation.direction() == QAbstractAnimation::Forward ? m_messageQueue.front().data["size"]->toInt() : height();
    const int val = value.toInt();
    QPoint p(0, hend);
    if (m_settings.has("gui/absolute_position") && !m_settings.get("gui/absolute_position").toString().isEmpty()) {
        QPoint tmp = stringToPos(m_settings.get("gui/absolute_position").toString());
        if (!tmp.isNull())
            p = tmp;
    }
    setGeometry(p.x(), p.y()-height(), val, finalHeight);
    layout()->setSpacing(0);
    show();
}

void Widget::updateTopCenterAnimation(QVariant value)
{
    const int finalWidth = qobject_cast<QPropertyAnimation*>(m_animation.animationAt(0))->endValue().toInt();
    const int finalHeight = m_animation.direction() == QAbstractAnimation::Forward ? m_messageQueue.front().data["size"]->toInt() : height();
    const int h = value.toInt() * finalHeight / finalWidth;
    const int wend = QDesktopWidget().availableGeometry(this).width();
    QPoint p(wend, 0);
    if (m_settings.has("gui/absolute_position") && !m_settings.get("gui/absolute_position").toString().isEmpty()) {
        QPoint tmp = stringToPos(m_settings.get("gui/absolute_position").toString());
        if (!tmp.isNull())
            p = tmp;
    }
    setGeometry(p.x()/2 - finalWidth/2, p.y(), finalWidth, h);
    layout()->setSpacing(0);
    show();
}

void Widget::updateBottomCenterAnimation(QVariant value)
{
    const int finalWidth = qobject_cast<QPropertyAnimation*>(m_animation.animationAt(0))->endValue().toInt();
    const int finalHeight = m_animation.direction() == QAbstractAnimation::Forward ? m_messageQueue.front().data["size"]->toInt() : height();
    const int h = value.toInt() * finalHeight / finalWidth;
    const int wend = QDesktopWidget().availableGeometry(this).width();
    const int hend = QDesktopWidget().availableGeometry(this).height();
    QPoint p(wend, hend);
    if (m_settings.has("gui/absolute_position") && !m_settings.get("gui/absolute_position").toString().isEmpty()) {
        QPoint tmp = stringToPos(m_settings.get("gui/absolute_position").toString());
        if (!tmp.isNull())
            p = tmp;
    }
    setGeometry(p.x()/2 - finalWidth/2, p.y()-h, finalWidth, h);
    layout()->setSpacing(0);
    show();
}

void Widget::updateCenterAnimation(QVariant value)
{
    const int finalWidth = qobject_cast<QPropertyAnimation*>(m_animation.animationAt(0))->endValue().toInt();
    const int finalHeight = m_animation.direction() == QAbstractAnimation::Forward ? m_messageQueue.front().data["size"]->toInt() : height();
    const int h = value.toInt() * finalHeight / finalWidth;
    const int wend = QDesktopWidget().availableGeometry(this).width();
    const int hend = QDesktopWidget().availableGeometry(this).height();
    QPoint p(wend, hend);
    if (m_settings.has("gui/absolute_position") && !m_settings.get("gui/absolute_position").toString().isEmpty()) {
        QPoint tmp = stringToPos(m_settings.get("gui/absolute_position").toString());
        if (!tmp.isNull())
            p = tmp;
    }
    setGeometry(p.x()/2 - value.toInt()/2, p.y()/2 - h/2, value.toInt(), h);
    layout()->setSpacing(0);
    show();
}

void Widget::reverseTrigger()
{
    if (m_animation.direction() == QAnimationGroup::Backward) {
        QTimer::singleShot(30, this, SLOT(processMessageQueue()));
        return;
    }
    m_visible.setInterval(m_messageQueue.front().data["duration"]->toInt());
    m_visible.start();
}

void Widget::reverseStart()
{
    if (m_messageQueue.size() <= 1) {
        if (!m_messageQueue.isEmpty())
            m_messageQueue.pop_front();
        m_animation.setDirection(QAnimationGroup::Backward);
        qobject_cast<QPropertyAnimation*>(m_animation.animationAt(0))->setEasingCurve(QEasingCurve::InCubic);
        m_animation.start();
        m_shortcutGrabber.disableShortcuts();
    }
    else
        autoNext();
}

int Widget::computeWidth()
{
    Message& m = m_messageQueue.front();
    QFont boldFont = font();
    boldFont.setBold(true);
    int width = 0;
    QString text = m_contentView["text"]->text();
    width += QFontMetrics(boldFont).width(m_contentView["title"]->text());
    if (Qt::mightBeRichText(text)) {
        QTextDocument doc;
        doc.setUseDesignMetrics(true);
        doc.setHtml(text);
        doc.setDefaultFont(font());
        width += doc.idealWidth();
    }
    else
        width += QFontMetrics(font()).width(text);
    if (m.data["icon"])
        width += m_contentView["icon"]->pixmap()->width();
    return width;
}

void Widget::setupFont()
{
    Message& m = m_messageQueue.front();
    QFont font("Sans");
    QString name = m.data["fn"]->toString();
    // Trick to detect a font in XFD format.
    if (name.count('-') >= 4)
        font.setRawName(name);
    else {
        font.setPixelSize(m.data["fs"]->toInt());
        font.setFamily(name);
    }
    QApplication::setFont(font);
}

void Widget::setupColors()
{
    Message& m = m_messageQueue.front();
    QString bg = m.data["bg"]->toString();
    QString fg = m.data["fg"]->toString();
    QString sheet;
    if (!bg.isEmpty())
        sheet += QString("background-color: %1;").arg(bg);
    if (!fg.isEmpty())
        sheet += QString("color: %1;").arg(fg);
    setStyleSheet(sheet);
}

void Widget::connectForPosition(QString position)
{
    QPropertyAnimation* anim = qobject_cast<QPropertyAnimation*>(m_animation.animationAt(0));
    if (!anim)
        return;
    disconnect(anim, SIGNAL(valueChanged(QVariant)), this, SLOT(updateTopLeftAnimation(QVariant)));
    disconnect(anim, SIGNAL(valueChanged(QVariant)), this, SLOT(updateTopRightAnimation(QVariant)));
    disconnect(anim, SIGNAL(valueChanged(QVariant)), this, SLOT(updateBottomRightAnimation(QVariant)));
    disconnect(anim, SIGNAL(valueChanged(QVariant)), this, SLOT(updateBottomLeftAnimation(QVariant)));
    disconnect(anim, SIGNAL(valueChanged(QVariant)), this, SLOT(updateTopCenterAnimation(QVariant)));
    disconnect(anim, SIGNAL(valueChanged(QVariant)), this, SLOT(updateBottomCenterAnimation(QVariant)));
    disconnect(anim, SIGNAL(valueChanged(QVariant)), this, SLOT(updateCenterAnimation(QVariant)));
    anim->setDuration(1000);
    if (position == "top_left" || position == "tl") {
        connect(anim, SIGNAL(valueChanged(QVariant)), this, SLOT(updateTopLeftAnimation(QVariant)));
    }
    else if (position == "top_right" || position == "tr") {
        connect(anim, SIGNAL(valueChanged(QVariant)), this, SLOT(updateTopRightAnimation(QVariant)));
    }
    else if (position == "bottom_right" || position == "br") {
        connect(anim, SIGNAL(valueChanged(QVariant)), this, SLOT(updateBottomRightAnimation(QVariant)));
    }
    else if (position == "bottom_left" || position == "bl") {
        connect(anim, SIGNAL(valueChanged(QVariant)), this, SLOT(updateBottomLeftAnimation(QVariant)));
    }
    else if (position == "top_center" || position == "tc") {
        connect(anim, SIGNAL(valueChanged(QVariant)), this, SLOT(updateTopCenterAnimation(QVariant)));
    }
    else if (position == "bottom_center" || position == "bc") {
        connect(anim, SIGNAL(valueChanged(QVariant)), this, SLOT(updateBottomCenterAnimation(QVariant)));
    }
    else if (position == "center" || position == "c") {
        connect(anim, SIGNAL(valueChanged(QVariant)), this, SLOT(updateCenterAnimation(QVariant)));
    }
    else {
        // top_right seems to be the classic case so fallback to it.
        connect(anim, SIGNAL(valueChanged(QVariant)), this, SLOT(updateTopRightAnimation(QVariant)));
        qDebug() << "default position";
    }
}

void Widget::setupIcon()
{
    Message& m = m_messageQueue.front();
    bool done = true;
    if (m.data["icon"]) {
        QPixmap pix = qvariant_cast<QPixmap>(*m.data["icon"]);
        if (pix.isNull())
            pix = loadPixmap(m.data["icon"]->toString());
        if (!pix.isNull())
            m.data["icon"].reset(pix);
        else if (pix.isNull())
            done = false;
        if (pix.height() > m.data["size"]->toInt())
            pix = pix.scaled(m.data["size"]->toInt(), m.data["size"]->toInt());
        m.data["icon"].reset(pix);
        m_contentView["icon"]->setPixmap(pix);
        m_contentView["icon"]->setMaximumWidth(9999);
    }
    if (!done) {
        m_contentView["icon"]->setPixmap(QPixmap());
        m_contentView["icon"]->setFixedWidth(2);
    }
}

void Widget::setupTitle()
{
    QFont boldFont = font();
    boldFont.setBold(true);
    Message& m = m_messageQueue.front();
    if (m.data["title"]) {              // avoid ugly space if no icon is set
        QString text = (m.data["icon"] ? " " : "") + m.data["title"]->toString() + " ";
        foreach (QString i, QStringList() << "\n" << "\r" << "<br/>" << "<br />")
            text.replace(i, " ");
        m_contentView["title"]->setText(text);
        m_contentView["title"]->setFont(boldFont);
        m_contentView["title"]->setMaximumWidth(9999);
    }
    else {
        m_contentView["title"]->setText("");
        m_contentView["title"]->setFixedWidth(0);
    }
}

void Widget::setupContent()
{
    Message& m = m_messageQueue.front();
    if (m.data["content"]) {
        QString text = (m.data["icon"] && !m.data["title"] ? " " : "") + m.data["content"]->toString() + " ";
        foreach (QString i, QStringList() << "\n" << "\r" << "<br/>" << "<br />")
            text.replace(i, " ");
        m_contentView["text"]->setText(text);
        m_contentView["text"]->setMaximumWidth(9999);
    }
    else {
        m_contentView["text"]->setText("");
        m_contentView["text"]->setFixedWidth(0);
    }
}

void Widget::loadDefaults()
{
    // "content" << "icon" << "title" << "layout" << "size" << "pos" << "fn" << "fs" << "duration" < "sc" << "bg" << "fg";
    Message& m = m_messageQueue.front();
    Settings* s = &m_settings;
    if (m.data["layout"]) {
        QString name = m.data["layout"]->toString();
        name.remove(".conf");
        s = new Settings(name);
        s->fillWith(m_settings);
        qDebug() << "Layout loaded : " << name;
        qDebug() << s->get("gui/foreground_color");
    }
    if (!m.data["bg"])
        m.data["bg"] = boost::optional<QVariant>(s->get("gui/background_color"));
    if (!m.data["fg"])
        m.data["fg"] = boost::optional<QVariant>(s->get("gui/foreground_color"));
    if (!m.data["sc"])
        m.data["sc"] = boost::optional<QVariant>(s->get("main/sound_command"));
    if (!m.data["duration"])
        m.data["duration"] = boost::optional<QVariant>(s->get("main/duration"));
    if (!m.data["fs"])
        m.data["fs"] = boost::optional<QVariant>(s->get("gui/font_size"));
    if (!m.data["fn"])
        m.data["fn"] = boost::optional<QVariant>(s->get("gui/font"));
    if (!m.data["pos"])
        m.data["pos"] = boost::optional<QVariant>(s->get("gui/position"));
    if (!m.data["size"])
        m.data["size"] = boost::optional<QVariant>(s->get("gui/height"));
    if (!m.data["icon"])
        m.data["icon"] = loadPixmap(s->has("gui/icon") ? s->get("gui/icon").toString() : "");
    if (!m.data["aot"])
        m.data["aot"] = boost::optional<QVariant>(s->get("gui/always_on_top"));
    if (!m.data["ac"])
        m.data["ac"] = boost::optional<QVariant>(s->get("main/activate_command"));
    if (s != &m_settings)
        delete s;
}

QPixmap Widget::loadPixmap(QString pattern)
{
    QPixmap icon(pattern);
    if (icon.isNull()) {
        if (m_settings.has("icons/" + pattern))
            icon = QPixmap(m_settings.get("icons/" + pattern).toString());
        else {
            ///TODO: Load standard icons. Surprisingly this doesn't work
            //icon = QIcon::fromTheme(pattern).pixmap(999, 999);
            //if (icon.isNull()) {
                QImage img(1, 1, QImage::Format_ARGB32);
                QPainter p;
                p.begin(&img);
                p.fillRect(0, 0, 1, 1, QBrush(QColor::fromRgb(255, 255, 255, 0)));
                p.end();
                icon = QPixmap::fromImage(img);
            //}
        }
    }
    return icon;
}

bool Widget::update(const Message &m)
{
    bool found = false;
    for (QQueue<Message>::iterator it = m_messageQueue.begin(); it != m_messageQueue.end(); ++it) {
        if (it->data["id"] && it->data["id"]->toInt() == m.data["id"]->toInt()) {
            it->data = m.data;
            found = true;
        }
    }
    if (found && !m_messageQueue.isEmpty() && m_messageQueue.front().data["id"]->toInt() == m.data["id"]->toInt()) {
        loadDefaults();
        setupFont();
        setupColors();
        setupIcon();
        setupTitle();
        setupContent();
        updateFinalWidth();
        connectForPosition(m_messageQueue.front().data["pos"]->toString());
        m_visible.start();
    }
    return found;
}

QPoint Widget::stringToPos(QString string)
{
    string.replace("X", "x");
    string.replace("*", "x");
    const QStringList splitted = string.split("x");
    if (string.isEmpty() || !string.contains('x') || splitted.size() < 2)
        return QPoint();
    QPoint ret;
    ret.setX(QString(splitted[0]).toInt());
    ret.setY(QString(splitted[1]).toInt());
    return ret;
}

void Widget::updateFinalWidth()
{
    if (m_messageQueue.empty())
        return;
    QString position = m_messageQueue.front().data["pos"]->toString();
    int width = computeWidth();
    qobject_cast<QPropertyAnimation*>(m_animation.animationAt(0))->setEndValue(width);
    if (position == "top_left" || position == "tl")
        updateTopLeftAnimation(width);
    else if (position == "top_right" || position == "tr")
        updateTopRightAnimation(width);
    else if (position == "bottom_right" || position == "br")
        updateBottomRightAnimation(width);
    else if (position == "bottom_left" || position == "bl")
        updateBottomLeftAnimation(width);
    else if (position == "top_center" || position == "tc")
        updateTopCenterAnimation(width);
    else if (position == "bottom_center" || position == "bc")
        updateBottomCenterAnimation(width);
    else if (position == "center" || position == "c")
        updateCenterAnimation(width);
}

void Widget::onPrevious()
{
    m_visible.start();
    if (m_previousStack.size() < 1)
        return;
    Message m = m_previousStack.pop();
    m_messageQueue.push_front(m);
    loadDefaults();
    setupFont();
    setupColors();
    setupIcon();
    setupTitle();
    setupContent();
    connectForPosition(m_messageQueue.front().data["pos"]->toString());
    updateFinalWidth();
}

void Widget::onNext()
{
    m_visible.start();
    if (m_messageQueue.size() < 2)
        return;
    Message m = m_messageQueue.front();
    m.data["manually_shown"] = boost::optional<QVariant>(true);
    m_previousStack.push(m);
    m_messageQueue.pop_front();
    loadDefaults();
    setupFont();
    setupColors();
    setupIcon();
    setupTitle();
    setupContent();
    connectForPosition(m_messageQueue.front().data["pos"]->toString());
    updateFinalWidth();
}

void Widget::onActivate()
{
    Q_ASSERT(!m_messageQueue.isEmpty());
    QProcess::startDetached(m_messageQueue.front().data["ac"]->toString());
    m_visible.start();
}

void Widget::onHide()
{
    m_messageQueue.clear();
    m_visible.setInterval(2);
    m_visible.start();
}

void Widget::autoNext()
{
    Message&m = m_messageQueue.front();
    // The user already saw it manually.
    if (m.data["manually_shown"]) {
        m_messageQueue.pop_front();
        reverseStart();
    }
    else {
        QString soundCommand = m.data["sc"]->toString();
        if (!soundCommand.isEmpty())
            QProcess::startDetached(soundCommand);
    }
    onNext();
}

void Widget::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
        onActivate();
    QWidget::mousePressEvent(e);
}

void Widget::wheelEvent(QWheelEvent *e)
{
    if (e->delta() > 0)
        onPrevious();
    else if (e->delta() < 0)
        onNext();
    QWidget::wheelEvent(e);
}
