/****************************************************************************
**
** Copyright (C) 2021 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtNfc module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:BSD$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** BSD License Usage
** Alternatively, you may use this file under the terms of the BSD license
** as follows:
**
** "Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in
**     the documentation and/or other materials provided with the
**     distribution.
**   * Neither the name of The Qt Company Ltd nor the names of its
**     contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
** LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
** OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "mimeimagerecordeditor.h"
#include "ui_mimeimagerecordeditor.h"

#include <QtGui/QImageReader>
#include <QtWidgets/QFileDialog>
#include <QtCore/QBuffer>
#include <QScreen>
#include <QLayout>

#if (defined(Q_OS_ANDROID) && !defined(Q_OS_ANDROID_EMBEDDED)) || defined(Q_OS_IOS)
#    define MOBILE_PLATFORM
#endif

static QString imageFormatToMimeType(const QByteArray &format)
{
    if (format == "bmp")
        return QStringLiteral("image/bmp");
    else if (format == "gif")
        return QStringLiteral("image/gif");
    else if (format == "jpg" || format == "jpeg")
        return QStringLiteral("image/jpeg");
    else if (format == "mng")
        return QStringLiteral("video/x-mng");
    else if (format == "png")
        return QStringLiteral("image/png");
    else if (format == "pbm")
        return QStringLiteral("image/x-portable-bitmap");
    else if (format == "pgm")
        return QStringLiteral("image/x-portable-graymap");
    else if (format == "ppm")
        return QStringLiteral("image/x-portable-pixmap");
    else if (format == "tiff")
        return QStringLiteral("image/tiff");
    else if (format == "xbm")
        return QStringLiteral("image/x-xbitmap");
    else if (format == "xpm")
        return QStringLiteral("image/x-xpixmap");
    else if (format == "svg")
        return QStringLiteral("image/svg+xml");
    else
        return QString();
}

MimeImageRecordEditor::MimeImageRecordEditor(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::MimeImageRecordEditor)
{
    ui->setupUi(this);
#ifdef MOBILE_PLATFORM
    connect(screen(), &QScreen::orientationChanged, this,
            &MimeImageRecordEditor::handleScreenOrientationChange);
#endif
}

MimeImageRecordEditor::~MimeImageRecordEditor()
{
    delete ui;
}

void MimeImageRecordEditor::setRecord(const QNdefRecord &record)
{
    m_record = record;

    QByteArray data = record.payload();
    QBuffer buffer(&data);
    buffer.open(QIODevice::ReadOnly);

    QImageReader reader(&buffer);

    ui->mimeImageType->setText(imageFormatToMimeType(reader.format()));
    ui->mimeImageFile->clear();

    m_pixmap = QPixmap::fromImage(reader.read());
    updatePixmap();
}

QNdefRecord MimeImageRecordEditor::record() const
{
    return m_record;
}

void MimeImageRecordEditor::handleScreenOrientationChange(Qt::ScreenOrientation orientation)
{
    Q_UNUSED(orientation);
#ifdef MOBILE_PLATFORM
    if (m_imageSelected) {
        ui->mimeImageImage->clear();
        adjustSize();
        m_screenRotated = true;
    }
#endif
}

void MimeImageRecordEditor::resizeEvent(QResizeEvent *)
{
    if (m_imageSelected) {
#ifdef MOBILE_PLATFORM
        if (m_screenRotated) {
            updatePixmap();
            m_screenRotated = false;
        }
#else
        updatePixmap();
#endif
    }
}

void MimeImageRecordEditor::updatePixmap()
{
    // Calculate the desired width of the image. It's calculated based on the
    // screen size minus the content margins.
    const auto parentContentMargins = parentWidget()->layout()->contentsMargins();
    const auto thisContentMargins = layout()->contentsMargins();
#ifdef MOBILE_PLATFORM
    // Because of QTBUG-94459 the screen size might be incorrect, so we check
    // the orientation to find the actual width
    const auto w = screen()->availableSize().width();
    const auto h = screen()->availableSize().height();
    const auto screenWidth =
            (screen()->orientation() == Qt::PortraitOrientation) ? qMin(w, h) : qMax(w, h);
#else
    const auto screenWidth = width();
#endif
    const auto imageWidth = screenWidth - parentContentMargins.right() - parentContentMargins.left()
            - thisContentMargins.right() - thisContentMargins.left();

    if (!m_pixmap.isNull()) {
        if (m_pixmap.width() > imageWidth)
            ui->mimeImageImage->setPixmap(m_pixmap.scaledToWidth(imageWidth));
        else
            ui->mimeImageImage->setPixmap(m_pixmap);
    } else {
        ui->mimeImageImage->setText("Can't show the image");
    }
}

void MimeImageRecordEditor::on_mimeImageOpen_clicked()
{
    QString mimeDataFile = QFileDialog::getOpenFileName(this, tr("Select Image File"));
    if (mimeDataFile.isEmpty())
        return;

    QFile imageFile(mimeDataFile);
    if (!imageFile.open(QIODevice::ReadOnly)) {
        ui->mimeImageFile->clear();
        ui->mimeImageImage->clear();
    }

    QByteArray imageData = imageFile.readAll();

    QBuffer buffer(&imageData);
    buffer.open(QIODevice::ReadOnly);

    QImageReader reader(&buffer);
    QString mimeType = imageFormatToMimeType(reader.format());
    ui->mimeImageType->setText(mimeType);

    ui->mimeImageFile->setText(mimeDataFile);
    const QImage image = reader.read();
    m_pixmap = QPixmap::fromImage(image);
    m_imageSelected = true;
    updatePixmap();

    m_record.setTypeNameFormat(QNdefRecord::Mime);
    m_record.setType(mimeType.toLatin1());
    m_record.setPayload(imageData);
}
