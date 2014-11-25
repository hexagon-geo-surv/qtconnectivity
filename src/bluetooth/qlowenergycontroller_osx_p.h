/****************************************************************************
**
** Copyright (C) 2014 Digia Plc and/or its subsidiary(-ies).
** Copyright (C) 2013 Javier S. Pedro <maemo@javispedro.com>
** Contact: http://www.qt-project.org/legal
**
** This file is part of the QtBluetooth module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL21$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia. For licensing terms and
** conditions see http://qt.digia.com/licensing. For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file. Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights. These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** $QT_END_LICENSE$
**
****************************************************************************/
#ifndef QLOWENERGYCONTROLLER_OSX_P_H
#define QLOWENERGYCONTROLLER_OSX_P_H

#include "qlowenergyserviceprivate_p.h"
#include "osx/osxbtcentralmanager_p.h"
#include "qlowenergycontroller_p.h"
#include "qlowenergycontroller.h"
#include "osx/osxbtutility_p.h"
#include "qbluetoothaddress.h"
#include "qbluetoothuuid.h"

#include <QtCore/qsharedpointer.h>
#include <QtCore/qglobal.h>
#include <QtCore/qstring.h>
#include <QtCore/qmap.h>

QT_BEGIN_NAMESPACE

class QByteArray;

// The suffix OSX is not the very right, it's also iOS.
class QLowEnergyControllerPrivateOSX : public QLowEnergyControllerPrivate,
                                       public OSXBluetooth::CentralManagerDelegate
{
    friend class QLowEnergyController;
    friend class QLowEnergyService;
public:
    QLowEnergyControllerPrivateOSX(QLowEnergyController *q);
    QLowEnergyControllerPrivateOSX(QLowEnergyController *q,
                                   const QBluetoothDeviceInfo &uuid);
    ~QLowEnergyControllerPrivateOSX();

    bool isValid() const;

private:
    // CentralManagerDelegate:
    void LEnotSupported() Q_DECL_OVERRIDE;
    void connectSuccess() Q_DECL_OVERRIDE;

    void serviceDiscoveryFinished(LEServices services) Q_DECL_OVERRIDE;
    void serviceDetailsDiscoveryFinished(LEService service) Q_DECL_OVERRIDE;
    void characteristicWriteNotification(LECharacteristic ch) Q_DECL_OVERRIDE;
    void descriptorWriteNotification(QLowEnergyHandle descHandle,
                                     const QByteArray &newValue) Q_DECL_OVERRIDE;
    void disconnected() Q_DECL_OVERRIDE;
    void error(QLowEnergyController::Error errorCode) Q_DECL_OVERRIDE;
    void error(const QBluetoothUuid &serviceUuid,
               QLowEnergyController::Error errorCode) Q_DECL_OVERRIDE;
    void error(const QBluetoothUuid &serviceUuid,
               QLowEnergyHandle charHandle,
               QLowEnergyService::ServiceError error) Q_DECL_OVERRIDE;

    void connectToDevice();
    void discoverServices();
    void discoverServiceDetails(const QBluetoothUuid &serviceUuid);

    void setNotifyValue(QSharedPointer<QLowEnergyServicePrivate> service,
                        QLowEnergyHandle charHandle, const QByteArray &newValue);

    void writeCharacteristic(QSharedPointer<QLowEnergyServicePrivate> service,
                             QLowEnergyHandle charHandle, const QByteArray &newValue,
                             bool writeWithResponse);

    quint16 updateValueOfCharacteristic(QLowEnergyHandle charHandle,
                                        const QByteArray &value,
                                        bool appendValue);

    void writeDescriptor(QSharedPointer<QLowEnergyServicePrivate> service,
                         QLowEnergyHandle descriptorHandle,
                         const QByteArray &newValue);


    quint16 updateValueOfDescriptor(QLowEnergyHandle charHandle,
                                    QLowEnergyHandle descHandle,
                                    const QByteArray &value,
                                    bool appendValue);

    // 'Lookup' functions:
    QSharedPointer<QLowEnergyServicePrivate> serviceForHandle(QLowEnergyHandle serviceHandle);
    QLowEnergyCharacteristic characteristicForHandle(QLowEnergyHandle charHandle);
    QLowEnergyDescriptor descriptorForHandle(QLowEnergyHandle descriptorHandle);

    void setErrorDescription(QLowEnergyController::Error errorCode);
    void invalidateServices();

    QLowEnergyController *q_ptr;
    QBluetoothUuid deviceUuid;
    // To be sure we set controller's state correctly
    // (Connecting or Connected) we have to know if we're
    // still inside connectToDevice - this is important,
    // if a peripheral is _already_ connected from Core Bluetooth's
    // point of view.
    bool isConnecting;

    QString errorString;
    QLowEnergyController::Error lastError;

    QBluetoothAddress localAddress;
    QBluetoothAddress remoteAddress;

    QLowEnergyController::ControllerState controllerState;
    QLowEnergyController::RemoteAddressType addressType;

    typedef OSXBluetooth::ObjCScopedPointer<ObjCCentralManager> CentralManager;
    CentralManager centralManager;

    typedef QMap<QBluetoothUuid, QSharedPointer<QLowEnergyServicePrivate> > ServiceMap;
    typedef ServiceMap::const_iterator ConstServiceIterator;
    typedef ServiceMap::iterator ServiceIterator;
    ServiceMap discoveredServices;

    // While Core Bluetooth has _startHandle/_endHandle for
    // CBServices, this information is not a part of a public
    // API and can not be used. Instead we have to 'emulate'
    // these handles using something that looks like/works like
    // handles:
    QLowEnergyHandle lastValidHandle;
};

QT_END_NAMESPACE

#endif
