/****************************************************************************
**
** Copyright (C) 2018 The Qt Company Ltd.
** Copyright (C) 2014 Denis Shienkov <denis.shienkov@gmail.com>
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtBluetooth module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qlowenergycontroller_win_p.h"
#include "qbluetoothdevicediscoveryagent_p.h"

#include <QtCore/QLoggingCategory>
#include <QtCore/QIODevice> // for open modes
#include <QtCore/QEvent>
#include <QtCore/QMutex>

#include <algorithm> // for std::max

#include <windows/qwinlowenergybluetooth_p.h>

#include <setupapi.h>

QT_BEGIN_NAMESPACE

Q_DECLARE_LOGGING_CATEGORY(QT_BT_WINDOWS)

Q_GLOBAL_STATIC(QLibrary, bluetoothapis)

Q_GLOBAL_STATIC(QVector<QLowEnergyControllerPrivateWin32 *>, qControllers)
static QMutex controllersGuard(QMutex::NonRecursive);

const QEvent::Type CharactericticValueEventType = static_cast<QEvent::Type>(QEvent::User + 1);

class CharactericticValueEvent : public QEvent
{
public:
    explicit CharactericticValueEvent(const PBLUETOOTH_GATT_VALUE_CHANGED_EVENT gattValueChangedEvent)
        : QEvent(CharactericticValueEventType)
        , m_handle(0)
    {
        if (!gattValueChangedEvent || gattValueChangedEvent->CharacteristicValueDataSize == 0)
            return;

        m_handle = gattValueChangedEvent->ChangedAttributeHandle;

        const PBTH_LE_GATT_CHARACTERISTIC_VALUE gattValue = gattValueChangedEvent->CharacteristicValue;
        if (!gattValue)
            return;

        m_value = QByteArray(reinterpret_cast<const char *>(&gattValue->Data[0]),
                gattValue->DataSize);
    }

    QByteArray m_value;
    QLowEnergyHandle m_handle;
};

// Bit masks of ClientCharacteristicConfiguration value, see btle spec.
namespace ClientCharacteristicConfigurationValue {
enum { UseNotifications = 0x1, UseIndications = 0x2 };
}

static bool gattFunctionsResolved = false;

static QBluetoothAddress getDeviceAddress(const QString &servicePath)
{
    const int firstbound = servicePath.indexOf(QStringLiteral("_"));
    const int lastbound = servicePath.indexOf(QLatin1Char('#'), firstbound);
    const QString hex = servicePath.mid(firstbound + 1, lastbound - firstbound - 1);
    bool ok = false;
    return QBluetoothAddress(hex.toULongLong(&ok, 16));
}

static QString getServiceSystemPath(const QBluetoothAddress &deviceAddress,
                                    const QBluetoothUuid &serviceUuid, int *systemErrorCode)
{
    const HDEVINFO deviceInfoSet = ::SetupDiGetClassDevs(
                reinterpret_cast<const GUID *>(&serviceUuid),
                NULL,
                0,
                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        *systemErrorCode = ::GetLastError();
        return QString();
    }

    QString foundSystemPath;
    DWORD index = 0;

    for (;;) {
        SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
        ::ZeroMemory(&deviceInterfaceData, sizeof(deviceInterfaceData));
        deviceInterfaceData.cbSize = sizeof(deviceInterfaceData);

        if (!::SetupDiEnumDeviceInterfaces(
                    deviceInfoSet,
                    NULL,
                    reinterpret_cast<const GUID *>(&serviceUuid),
                    index++,
                    &deviceInterfaceData)) {
            *systemErrorCode = ::GetLastError();
            break;
        }

        DWORD deviceInterfaceDetailDataSize = 0;
        if (!::SetupDiGetDeviceInterfaceDetail(
                    deviceInfoSet,
                    &deviceInterfaceData,
                    NULL,
                    deviceInterfaceDetailDataSize,
                    &deviceInterfaceDetailDataSize,
                    NULL)) {
            if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
                *systemErrorCode = ::GetLastError();
                break;
            }
        }

        SP_DEVINFO_DATA deviceInfoData;
        ::ZeroMemory(&deviceInfoData, sizeof(deviceInfoData));
        deviceInfoData.cbSize = sizeof(deviceInfoData);

        QByteArray deviceInterfaceDetailDataBuffer(
                    deviceInterfaceDetailDataSize, 0);

        PSP_INTERFACE_DEVICE_DETAIL_DATA deviceInterfaceDetailData =
                reinterpret_cast<PSP_INTERFACE_DEVICE_DETAIL_DATA>
                (deviceInterfaceDetailDataBuffer.data());

        deviceInterfaceDetailData->cbSize =
                sizeof(SP_INTERFACE_DEVICE_DETAIL_DATA);

        if (!::SetupDiGetDeviceInterfaceDetail(
                    deviceInfoSet,
                    &deviceInterfaceData,
                    deviceInterfaceDetailData,
                    deviceInterfaceDetailDataBuffer.size(),
                    &deviceInterfaceDetailDataSize,
                    &deviceInfoData)) {
            *systemErrorCode = ::GetLastError();
            break;
        }

        // We need to check on required device address which contains in a
        // system path. As it is not enough to use only service UUID for this.
        const auto candidateSystemPath = QString::fromWCharArray(deviceInterfaceDetailData->DevicePath);
        const auto candidateDeviceAddress = getDeviceAddress(candidateSystemPath);
        if (candidateDeviceAddress == deviceAddress) {
            foundSystemPath = candidateSystemPath;
            *systemErrorCode = NO_ERROR;
            break;
        }
    }

    ::SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return foundSystemPath;
}

static HANDLE openSystemDevice(
        const QString &systemPath, QIODevice::OpenMode openMode, int *systemErrorCode)
{
    DWORD desiredAccess = 0;
    DWORD shareMode = 0;

    if (openMode & QIODevice::ReadOnly) {
        desiredAccess |= GENERIC_READ;
        shareMode |= FILE_SHARE_READ;
    }

    if (openMode & QIODevice::WriteOnly) {
        desiredAccess |= GENERIC_WRITE;
        shareMode |= FILE_SHARE_WRITE;
    }

    const HANDLE hDevice = ::CreateFile(
                reinterpret_cast<const wchar_t *>(systemPath.utf16()),
                desiredAccess,
                shareMode,
                NULL,
                OPEN_EXISTING,
                0,
                NULL);

    *systemErrorCode = (INVALID_HANDLE_VALUE == hDevice)
            ? ::GetLastError() : NO_ERROR;
    return hDevice;
}

static HANDLE openSystemService(const QBluetoothAddress &deviceAddress,
        const QBluetoothUuid &service, QIODevice::OpenMode openMode, int *systemErrorCode)
{
    const QString serviceSystemPath = getServiceSystemPath(
                deviceAddress, service, systemErrorCode);

    if (*systemErrorCode != NO_ERROR)
        return INVALID_HANDLE_VALUE;

    const HANDLE hService = openSystemDevice(
                serviceSystemPath, openMode, systemErrorCode);

    if (*systemErrorCode != NO_ERROR)
        return INVALID_HANDLE_VALUE;

    return hService;
}

static void closeSystemDevice(HANDLE hDevice)
{
    if (hDevice && hDevice != INVALID_HANDLE_VALUE)
        ::CloseHandle(hDevice);
}

static QVector<BTH_LE_GATT_SERVICE> enumeratePrimaryGattServices(
        HANDLE hDevice, int *systemErrorCode)
{
    if (!gattFunctionsResolved) {
        *systemErrorCode = ERROR_NOT_SUPPORTED;
        return QVector<BTH_LE_GATT_SERVICE>();
    }

    QVector<BTH_LE_GATT_SERVICE> foundServices;
    USHORT servicesCount = 0;
    for (;;) {
        const HRESULT hr = ::BluetoothGATTGetServices(
                    hDevice,
                    servicesCount,
                    foundServices.isEmpty() ? NULL : &foundServices[0],
                    &servicesCount,
                    BLUETOOTH_GATT_FLAG_NONE);

        if (SUCCEEDED(hr)) {
            *systemErrorCode = NO_ERROR;
            return foundServices;
        } else {
            const DWORD error = WIN32_FROM_HRESULT(hr);
            if (error == ERROR_MORE_DATA) {
                foundServices.resize(servicesCount);
            } else {
                *systemErrorCode = error;
                return QVector<BTH_LE_GATT_SERVICE>();
            }
        }
    }
}

static QVector<BTH_LE_GATT_CHARACTERISTIC> enumerateGattCharacteristics(
        HANDLE hService, PBTH_LE_GATT_SERVICE gattService, int *systemErrorCode)
{
    if (!gattFunctionsResolved) {
        *systemErrorCode = ERROR_NOT_SUPPORTED;
        return QVector<BTH_LE_GATT_CHARACTERISTIC>();
    }

    QVector<BTH_LE_GATT_CHARACTERISTIC> foundCharacteristics;
    USHORT characteristicsCount = 0;
    for (;;) {
        const HRESULT hr = ::BluetoothGATTGetCharacteristics(
                    hService,
                    gattService,
                    characteristicsCount,
                    foundCharacteristics.isEmpty() ? NULL : &foundCharacteristics[0],
                    &characteristicsCount,
                    BLUETOOTH_GATT_FLAG_NONE);

        if (SUCCEEDED(hr)) {
            *systemErrorCode = NO_ERROR;
            return foundCharacteristics;
        } else {
            const DWORD error = WIN32_FROM_HRESULT(hr);
            if (error == ERROR_MORE_DATA) {
                foundCharacteristics.resize(characteristicsCount);
            } else {
                *systemErrorCode = error;
                return QVector<BTH_LE_GATT_CHARACTERISTIC>();
            }
        }
    }
}

static QByteArray getGattCharacteristicValue(
        HANDLE hService, PBTH_LE_GATT_CHARACTERISTIC gattCharacteristic, int *systemErrorCode)
{
    if (!gattFunctionsResolved) {
        *systemErrorCode = ERROR_NOT_SUPPORTED;
        return QByteArray();
    }

    QByteArray valueBuffer;
    USHORT valueBufferSize = 0;
    for (;;) {
        const auto valuePtr = valueBuffer.isEmpty()
                ? NULL
                : reinterpret_cast<PBTH_LE_GATT_CHARACTERISTIC_VALUE>(valueBuffer.data());

        const HRESULT hr = ::BluetoothGATTGetCharacteristicValue(
                    hService,
                    gattCharacteristic,
                    valueBufferSize,
                    valuePtr,
                    &valueBufferSize,
                    BLUETOOTH_GATT_FLAG_NONE);

        if (SUCCEEDED(hr)) {
            *systemErrorCode = NO_ERROR;
            return QByteArray(reinterpret_cast<const char *>(&valuePtr->Data[0]),
                    valuePtr->DataSize);
        } else {
            const DWORD error = WIN32_FROM_HRESULT(hr);
            if (error == ERROR_MORE_DATA) {
                valueBuffer.resize(valueBufferSize);
            } else {
                *systemErrorCode = error;
                return QByteArray();
            }
        }
    }
}

static void setGattCharacteristicValue(
        HANDLE hService, PBTH_LE_GATT_CHARACTERISTIC gattCharacteristic,
        const QByteArray &value, DWORD flags, int *systemErrorCode)
{
    if (!gattFunctionsResolved) {
        *systemErrorCode = ERROR_NOT_SUPPORTED;
        return;
    }

    QByteArray valueBuffer;
    QDataStream out(&valueBuffer, QIODevice::WriteOnly);
    ULONG dataSize = value.size();
    out.writeRawData(reinterpret_cast<const char *>(&dataSize), sizeof(dataSize));
    out.writeRawData(value.constData(), value.size());

    BTH_LE_GATT_RELIABLE_WRITE_CONTEXT reliableWriteContext = 0;

    const HRESULT hr = ::BluetoothGATTSetCharacteristicValue(
                hService,
                gattCharacteristic,
                reinterpret_cast<PBTH_LE_GATT_CHARACTERISTIC_VALUE>(valueBuffer.data()),
                reliableWriteContext,
                flags);

    if (SUCCEEDED(hr))
        *systemErrorCode = NO_ERROR;
    else
        *systemErrorCode = WIN32_FROM_HRESULT(hr);
}

static QVector<BTH_LE_GATT_DESCRIPTOR> enumerateGattDescriptors(
        HANDLE hService, PBTH_LE_GATT_CHARACTERISTIC gattCharacteristic, int *systemErrorCode)
{
    if (!gattFunctionsResolved) {
        *systemErrorCode = ERROR_NOT_SUPPORTED;
        return QVector<BTH_LE_GATT_DESCRIPTOR>();
    }

    QVector<BTH_LE_GATT_DESCRIPTOR> foundDescriptors;
    USHORT descriptorsCount = 0;
    for (;;) {
        const HRESULT hr = ::BluetoothGATTGetDescriptors(
                    hService,
                    gattCharacteristic,
                    descriptorsCount,
                    foundDescriptors.isEmpty() ? NULL : &foundDescriptors[0],
                    &descriptorsCount,
                    BLUETOOTH_GATT_FLAG_NONE);

        if (SUCCEEDED(hr)) {
            *systemErrorCode = NO_ERROR;
            return foundDescriptors;
        } else {
            const DWORD error = WIN32_FROM_HRESULT(hr);
            if (error == ERROR_MORE_DATA) {
                foundDescriptors.resize(descriptorsCount);
            } else {
                *systemErrorCode = error;
                return QVector<BTH_LE_GATT_DESCRIPTOR>();
            }
        }
    }
}

static QByteArray getGattDescriptorValue(
        HANDLE hService, PBTH_LE_GATT_DESCRIPTOR gattDescriptor, int *systemErrorCode)
{
    if (!gattFunctionsResolved) {
        *systemErrorCode = ERROR_NOT_SUPPORTED;
        return QByteArray();
    }

    QByteArray valueBuffer;
    USHORT valueBufferSize = 0;
    for (;;) {
        const auto valuePtr = valueBuffer.isEmpty()
                ? NULL
                : reinterpret_cast<PBTH_LE_GATT_DESCRIPTOR_VALUE>(valueBuffer.data());

        const HRESULT hr = ::BluetoothGATTGetDescriptorValue(
                    hService,
                    gattDescriptor,
                    valueBufferSize,
                    valuePtr,
                    &valueBufferSize,
                    BLUETOOTH_GATT_FLAG_NONE);

        if (SUCCEEDED(hr)) {
            *systemErrorCode = NO_ERROR;
            return QByteArray(reinterpret_cast<const char *>(&valuePtr->Data[0]),
                    valuePtr->DataSize);
        } else {
            const DWORD error = WIN32_FROM_HRESULT(hr);
            if (error == ERROR_MORE_DATA) {
                valueBuffer.resize(valueBufferSize);
            } else {
                *systemErrorCode = error;
                return QByteArray();
            }
        }
    }
}

static void setGattDescriptorValue(
        HANDLE hService, PBTH_LE_GATT_DESCRIPTOR gattDescriptor,
        QByteArray value, int *systemErrorCode)
{
    if (!gattFunctionsResolved) {
        *systemErrorCode = ERROR_NOT_SUPPORTED;
        return;
    }

    const int requiredValueBufferSize = sizeof(BTH_LE_GATT_DESCRIPTOR_VALUE)
                + value.size();

    QByteArray valueBuffer(requiredValueBufferSize, 0);

    PBTH_LE_GATT_DESCRIPTOR_VALUE gattValue = reinterpret_cast<
                PBTH_LE_GATT_DESCRIPTOR_VALUE>(valueBuffer.data());

    gattValue->DescriptorType = gattDescriptor->DescriptorType;

    if (gattValue->DescriptorType == ClientCharacteristicConfiguration) {
        QDataStream in(value);
        quint8 u;
        in >> u;

        // We need to setup appropriate fields that allow to subscribe for events.
        gattValue->ClientCharacteristicConfiguration.IsSubscribeToNotification =
                bool(u & ClientCharacteristicConfigurationValue::UseNotifications);
        gattValue->ClientCharacteristicConfiguration.IsSubscribeToIndication =
                bool(u & ClientCharacteristicConfigurationValue::UseIndications);
    }

    gattValue->DataSize = ULONG(value.size());
    ::memcpy(gattValue->Data, value.constData(), value.size());

    const HRESULT hr = ::BluetoothGATTSetDescriptorValue(
                hService,
                gattDescriptor,
                gattValue,
                BLUETOOTH_GATT_FLAG_NONE);

    if (SUCCEEDED(hr))
        *systemErrorCode = NO_ERROR;
    else
        *systemErrorCode = WIN32_FROM_HRESULT(hr);
}

static void WINAPI eventChangedCallbackEntry(
        BTH_LE_GATT_EVENT_TYPE eventType, PVOID eventOutParameter, PVOID context)
{
    if ((eventType != CharacteristicValueChangedEvent) || !eventOutParameter || !context)
        return;

    QMutexLocker locker(&controllersGuard);
    const auto target = static_cast<QLowEnergyControllerPrivateWin32 *>(context);
    if (!qControllers->contains(target))
        return;

    CharactericticValueEvent *e = new CharactericticValueEvent(
                reinterpret_cast<const PBLUETOOTH_GATT_VALUE_CHANGED_EVENT>(eventOutParameter));

    QCoreApplication::postEvent(target, e);
}

static HANDLE registerEvent(
        HANDLE hService, BTH_LE_GATT_CHARACTERISTIC gattCharacteristic,
        PVOID context, int *systemErrorCode)
{
    if (!gattFunctionsResolved) {
        *systemErrorCode = ERROR_NOT_SUPPORTED;
        return INVALID_HANDLE_VALUE;
    }

    HANDLE hEvent = INVALID_HANDLE_VALUE;

    BLUETOOTH_GATT_VALUE_CHANGED_EVENT_REGISTRATION registration;
    ::ZeroMemory(&registration, sizeof(registration));
    registration.NumCharacteristics = 1;
    registration.Characteristics[0] = gattCharacteristic;

    const HRESULT hr = ::BluetoothGATTRegisterEvent(
                hService,
                CharacteristicValueChangedEvent,
                &registration,
                eventChangedCallbackEntry,
                context,
                &hEvent,
                BLUETOOTH_GATT_FLAG_NONE);

    if (SUCCEEDED(hr))
        *systemErrorCode = NO_ERROR;
    else
        *systemErrorCode = WIN32_FROM_HRESULT(hr);

    return hEvent;
}

static void unregisterEvent(HANDLE hEvent, int *systemErrorCode)
{
    if (!gattFunctionsResolved) {
        *systemErrorCode = ERROR_NOT_SUPPORTED;
        return;
    }

    const HRESULT hr = ::BluetoothGATTUnregisterEvent(
                hEvent,
                BLUETOOTH_GATT_FLAG_NONE);

    if (SUCCEEDED(hr))
        *systemErrorCode = NO_ERROR;
    else
        *systemErrorCode = WIN32_FROM_HRESULT(hr);
}

static QBluetoothUuid qtBluetoothUuidFromNativeLeUuid(const BTH_LE_UUID &uuid)
{
    return uuid.IsShortUuid ? QBluetoothUuid(uuid.Value.ShortUuid)
                            : QBluetoothUuid(uuid.Value.LongUuid);
}

static BTH_LE_UUID nativeLeUuidFromQtBluetoothUuid(const QBluetoothUuid &uuid)
{
    BTH_LE_UUID gattUuid;
    ::ZeroMemory(&gattUuid, sizeof(gattUuid));
    if (uuid.minimumSize() == 2) {
        gattUuid.IsShortUuid = TRUE;
        gattUuid.Value.ShortUuid = uuid.data1; // other fields should be empty!
    } else {
        gattUuid.Value.LongUuid = uuid;
    }
    return gattUuid;
}

static BTH_LE_GATT_CHARACTERISTIC recoverNativeLeGattCharacteristic(
        QLowEnergyHandle serviceHandle, QLowEnergyHandle characteristicHandle,
        const QLowEnergyServicePrivate::CharData &characteristicData)
{
    BTH_LE_GATT_CHARACTERISTIC gattCharacteristic;

    gattCharacteristic.ServiceHandle = serviceHandle;
    gattCharacteristic.AttributeHandle = characteristicHandle;
    gattCharacteristic.CharacteristicValueHandle = characteristicData.valueHandle;

    gattCharacteristic.CharacteristicUuid = nativeLeUuidFromQtBluetoothUuid(
                characteristicData.uuid);

    gattCharacteristic.HasExtendedProperties = bool(characteristicData.properties
            & QLowEnergyCharacteristic::ExtendedProperty);
    gattCharacteristic.IsBroadcastable = bool(characteristicData.properties
            & QLowEnergyCharacteristic::Broadcasting);
    gattCharacteristic.IsIndicatable = bool(characteristicData.properties
            & QLowEnergyCharacteristic::Indicate);
    gattCharacteristic.IsNotifiable = bool(characteristicData.properties
            & QLowEnergyCharacteristic::Notify);
    gattCharacteristic.IsReadable = bool(characteristicData.properties
            & QLowEnergyCharacteristic::Read);
    gattCharacteristic.IsSignedWritable = bool(characteristicData.properties
            & QLowEnergyCharacteristic::WriteSigned);
    gattCharacteristic.IsWritable = bool(characteristicData.properties
            & QLowEnergyCharacteristic::Write);
    gattCharacteristic.IsWritableWithoutResponse = bool(characteristicData.properties
            & QLowEnergyCharacteristic::WriteNoResponse);

    return gattCharacteristic;
}

static BTH_LE_GATT_DESCRIPTOR_TYPE nativeLeGattDescriptorTypeFromUuid(
        const QBluetoothUuid &uuid)
{
    switch (uuid.toUInt16()) {
    case QBluetoothUuid::CharacteristicExtendedProperties:
        return CharacteristicExtendedProperties;
    case QBluetoothUuid::CharacteristicUserDescription:
        return CharacteristicUserDescription;
    case QBluetoothUuid::ClientCharacteristicConfiguration:
        return ClientCharacteristicConfiguration;
    case QBluetoothUuid::ServerCharacteristicConfiguration:
        return ServerCharacteristicConfiguration;
    case QBluetoothUuid::CharacteristicPresentationFormat:
        return CharacteristicFormat;
    case QBluetoothUuid::CharacteristicAggregateFormat:
        return CharacteristicAggregateFormat;
    default:
        return CustomDescriptor;
    }
}

static BTH_LE_GATT_DESCRIPTOR recoverNativeLeGattDescriptor(
        QLowEnergyHandle serviceHandle, QLowEnergyHandle characteristicHandle,
        QLowEnergyHandle descriptorHandle,
         const QLowEnergyServicePrivate::DescData &descriptorData)
{
    BTH_LE_GATT_DESCRIPTOR gattDescriptor;

    gattDescriptor.ServiceHandle = serviceHandle;
    gattDescriptor.CharacteristicHandle = characteristicHandle;
    gattDescriptor.AttributeHandle = descriptorHandle;

    gattDescriptor.DescriptorUuid = nativeLeUuidFromQtBluetoothUuid(
                descriptorData.uuid);

    gattDescriptor.DescriptorType = nativeLeGattDescriptorTypeFromUuid
            (descriptorData.uuid);

    return gattDescriptor;
}

void QLowEnergyControllerPrivateWin32::customEvent(QEvent *e)
{
    if (e->type() != CharactericticValueEventType)
        return;

    const CharactericticValueEvent *characteristicEvent
            = static_cast<CharactericticValueEvent *>(e);

    updateValueOfCharacteristic(characteristicEvent->m_handle,
                                characteristicEvent->m_value, false);

    const QSharedPointer<QLowEnergyServicePrivate> service = serviceForHandle(
                characteristicEvent->m_handle);
    if (service.isNull())
        return;

    const QLowEnergyCharacteristic ch(service, characteristicEvent->m_handle);
    emit service->characteristicChanged(ch, characteristicEvent->m_value);
}

QLowEnergyControllerPrivateWin32::QLowEnergyControllerPrivateWin32()
    : QLowEnergyControllerPrivate()
{
    QMutexLocker locker(&controllersGuard);
    qControllers()->append(this);

    gattFunctionsResolved = resolveFunctions(bluetoothapis());
    if (!gattFunctionsResolved) {
        qCWarning(QT_BT_WINDOWS) << "LE is not supported on this OS";
        return;
    }
}

QLowEnergyControllerPrivateWin32::~QLowEnergyControllerPrivateWin32()
{
    QMutexLocker locker(&controllersGuard);
    qControllers()->removeAll(this);
}

void QLowEnergyControllerPrivateWin32::init()
{
    Q_UNIMPLEMENTED();
}

void QLowEnergyControllerPrivateWin32::connectToDevice()
{
    // required to pass unit test on default backend
    if (remoteDevice.isNull()) {
        qWarning() << "Invalid/null remote device address";
        setError(QLowEnergyController::UnknownRemoteDeviceError);
        return;
    }

    if (!deviceSystemPath.isEmpty()) {
        qCDebug(QT_BT_WINDOWS) << "Already is connected";
        return;
    }

    setState(QLowEnergyController::ConnectingState);

    deviceSystemPath =
            QBluetoothDeviceDiscoveryAgentPrivate::discoveredLeDeviceSystemPath(
                remoteDevice);

    if (deviceSystemPath.isEmpty()) {
        qCWarning(QT_BT_WINDOWS) << qt_error_string(ERROR_PATH_NOT_FOUND);
        setError(QLowEnergyController::UnknownRemoteDeviceError);
        setState(QLowEnergyController::UnconnectedState);
        return;
    }

    setState(QLowEnergyController::ConnectedState);

    Q_Q(QLowEnergyController);
    emit q->connected();
}

void QLowEnergyControllerPrivateWin32::disconnectFromDevice()
{
    if (deviceSystemPath.isEmpty()) {
        qCDebug(QT_BT_WINDOWS) << "Already is disconnected";
        return;
    }

    setState(QLowEnergyController::ClosingState);
    deviceSystemPath.clear();
    setState(QLowEnergyController::UnconnectedState);

    Q_Q(QLowEnergyController);
    emit q->disconnected();
}

void QLowEnergyControllerPrivateWin32::discoverServices()
{
    int systemErrorCode = NO_ERROR;

    const HANDLE hDevice = openSystemDevice(
                deviceSystemPath, QIODevice::ReadOnly, &systemErrorCode);

    if (systemErrorCode != NO_ERROR) {
        qCWarning(QT_BT_WINDOWS) << qt_error_string(systemErrorCode);
        setError(QLowEnergyController::NetworkError);
        setState(QLowEnergyController::ConnectedState);
        return;
    }

    const QVector<BTH_LE_GATT_SERVICE> foundServices =
            enumeratePrimaryGattServices(hDevice, &systemErrorCode);

    closeSystemDevice(hDevice);

    if (systemErrorCode != NO_ERROR) {
        qCWarning(QT_BT_WINDOWS) << qt_error_string(systemErrorCode);
        setError(QLowEnergyController::NetworkError);
        setState(QLowEnergyController::ConnectedState);
        return;
    }

    setState(QLowEnergyController::DiscoveringState);

    Q_Q(QLowEnergyController);

    for (const BTH_LE_GATT_SERVICE &service : foundServices) {
        const QBluetoothUuid uuid = qtBluetoothUuidFromNativeLeUuid(
                    service.ServiceUuid);
        qCDebug(QT_BT_WINDOWS) << "Found uuid:" << uuid;

        QLowEnergyServicePrivate *priv = new QLowEnergyServicePrivate();
        priv->uuid = uuid;
        priv->type = QLowEnergyService::PrimaryService;
        priv->startHandle = service.AttributeHandle;
        priv->setController(this);

        QSharedPointer<QLowEnergyServicePrivate> pointer(priv);
        serviceList.insert(uuid, pointer);

        emit q->serviceDiscovered(uuid);
    }

    setState(QLowEnergyController::DiscoveredState);
    emit q->discoveryFinished();
}

void QLowEnergyControllerPrivateWin32::discoverServiceDetails(
        const QBluetoothUuid &service)
{
    if (!serviceList.contains(service)) {
        qCWarning(QT_BT_WINDOWS) << "Discovery of unknown service" << service.toString()
                                 << "not possible";
        return;
    }

    const QSharedPointer<QLowEnergyServicePrivate> servicePrivate =
                serviceList.value(service);

    int systemErrorCode = NO_ERROR;

    const HANDLE hService = openSystemService(
                remoteDevice, service, QIODevice::ReadOnly, &systemErrorCode);

    if (systemErrorCode != NO_ERROR) {
        qCWarning(QT_BT_WINDOWS) << "Unable to open service" << service.toString()
                                 << ":" << qt_error_string(systemErrorCode);
        servicePrivate->setError(QLowEnergyService::UnknownError);
        servicePrivate->setState(QLowEnergyService::DiscoveryRequired);
        return;
    }

    // We assume that the service does not have any characteristics with descriptors.
    servicePrivate->endHandle = servicePrivate->startHandle;

    const QVector<BTH_LE_GATT_CHARACTERISTIC> foundCharacteristics =
            enumerateGattCharacteristics(hService, NULL, &systemErrorCode);

    if (systemErrorCode != NO_ERROR) {
        closeSystemDevice(hService);
        qCWarning(QT_BT_WINDOWS) << "Unable to get characteristics for service" << service.toString()
                                 << ":" << qt_error_string(systemErrorCode);
        servicePrivate->setError(QLowEnergyService::CharacteristicReadError);
        servicePrivate->setState(QLowEnergyService::DiscoveryRequired);
        return;
    }

    for (const BTH_LE_GATT_CHARACTERISTIC &gattCharacteristic : foundCharacteristics) {
        const QLowEnergyHandle characteristicHandle = gattCharacteristic.AttributeHandle;

        QLowEnergyServicePrivate::CharData detailsData;

        detailsData.hValueChangeEvent = NULL;

        detailsData.uuid = qtBluetoothUuidFromNativeLeUuid(
                    gattCharacteristic.CharacteristicUuid);
        detailsData.valueHandle = gattCharacteristic.CharacteristicValueHandle;

        QLowEnergyCharacteristic::PropertyTypes properties = QLowEnergyCharacteristic::Unknown;
        if (gattCharacteristic.HasExtendedProperties)
            properties |= QLowEnergyCharacteristic::ExtendedProperty;
        if (gattCharacteristic.IsBroadcastable)
            properties |= QLowEnergyCharacteristic::Broadcasting;
        if (gattCharacteristic.IsIndicatable)
            properties |= QLowEnergyCharacteristic::Indicate;
        if (gattCharacteristic.IsNotifiable)
            properties |= QLowEnergyCharacteristic::Notify;
        if (gattCharacteristic.IsReadable)
            properties |= QLowEnergyCharacteristic::Read;
        if (gattCharacteristic.IsSignedWritable)
            properties |= QLowEnergyCharacteristic::WriteSigned;
        if (gattCharacteristic.IsWritable)
            properties |= QLowEnergyCharacteristic::Write;
        if (gattCharacteristic.IsWritableWithoutResponse)
            properties |= QLowEnergyCharacteristic::WriteNoResponse;

        detailsData.properties = properties;
        detailsData.value = getGattCharacteristicValue(
                    hService, const_cast<PBTH_LE_GATT_CHARACTERISTIC>(
                        &gattCharacteristic), &systemErrorCode);

        if (systemErrorCode != NO_ERROR) {
            // We do not interrupt enumerating of characteristics
            // if value can not be read
            qCWarning(QT_BT_WINDOWS) << "Unable to get value for characteristic"
                                     << detailsData.uuid.toString()
                                     << "of the service" << service.toString()
                                     << ":" << qt_error_string(systemErrorCode);
        }

        // We assume that the characteristic has no any descriptors. So, the
        // biggest characteristic + 1 will indicate an end handle of service.
        servicePrivate->endHandle = std::max(
                    servicePrivate->endHandle,
                    QLowEnergyHandle(gattCharacteristic.AttributeHandle + 1));

        const QVector<BTH_LE_GATT_DESCRIPTOR> foundDescriptors = enumerateGattDescriptors(
                    hService, const_cast<PBTH_LE_GATT_CHARACTERISTIC>(
                        &gattCharacteristic), &systemErrorCode);

        if (systemErrorCode != NO_ERROR) {
            if (systemErrorCode != ERROR_NOT_FOUND) {
                closeSystemDevice(hService);
                qCWarning(QT_BT_WINDOWS) << "Unable to get descriptor for characteristic"
                                         << detailsData.uuid.toString()
                                         << "of the service" << service.toString()
                                         << ":" << qt_error_string(systemErrorCode);
                servicePrivate->setError(QLowEnergyService::DescriptorReadError);
                servicePrivate->setState(QLowEnergyService::DiscoveryRequired);
                return;
            }
        }

        for (const BTH_LE_GATT_DESCRIPTOR &gattDescriptor : foundDescriptors) {
            const QLowEnergyHandle descriptorHandle = gattDescriptor.AttributeHandle;

            QLowEnergyServicePrivate::DescData data;
            data.uuid = qtBluetoothUuidFromNativeLeUuid(
                        gattDescriptor.DescriptorUuid);

            data.value = getGattDescriptorValue(hService, const_cast<PBTH_LE_GATT_DESCRIPTOR>(
                                                    &gattDescriptor), &systemErrorCode);

            if (systemErrorCode != NO_ERROR) {
                closeSystemDevice(hService);
                qCWarning(QT_BT_WINDOWS) << "Unable to get value for descriptor"
                                         << data.uuid.toString()
                                         << "for characteristic"
                                         << detailsData.uuid.toString()
                                         << "of the service" << service.toString()
                                         << ":" << qt_error_string(systemErrorCode);
                servicePrivate->setError(QLowEnergyService::DescriptorReadError);
                servicePrivate->setState(QLowEnergyService::DiscoveryRequired);
                return;
            }

            // Biggest descriptor will contain an end handle of service.
            servicePrivate->endHandle = std::max(
                        servicePrivate->endHandle,
                        QLowEnergyHandle(gattDescriptor.AttributeHandle));

            detailsData.descriptorList.insert(descriptorHandle, data);
        }

        servicePrivate->characteristicList.insert(characteristicHandle, detailsData);
    }

    closeSystemDevice(hService);

    servicePrivate->setState(QLowEnergyService::ServiceDiscovered);
}

void QLowEnergyControllerPrivateWin32::startAdvertising(const QLowEnergyAdvertisingParameters &, const QLowEnergyAdvertisingData &, const QLowEnergyAdvertisingData &)
{
    Q_UNIMPLEMENTED();
}

void QLowEnergyControllerPrivateWin32::stopAdvertising()
{
    Q_UNIMPLEMENTED();
}

void QLowEnergyControllerPrivateWin32::requestConnectionUpdate(const QLowEnergyConnectionParameters &)
{
    Q_UNIMPLEMENTED();
}

void QLowEnergyControllerPrivateWin32::readCharacteristic(
        const QSharedPointer<QLowEnergyServicePrivate> service,
        const QLowEnergyHandle charHandle)
{
    Q_ASSERT(!service.isNull());
    if (!service->characteristicList.contains(charHandle))
        return;

    const QLowEnergyServicePrivate::CharData &charDetails
            = service->characteristicList[charHandle];
    if (!(charDetails.properties & QLowEnergyCharacteristic::Read)) {
        // if this succeeds the device has a bug, char is advertised as
        // non-readable. We try to be permissive and let the remote
        // device answer to the read attempt
        qCWarning(QT_BT_WINDOWS) << "Reading non-readable char" << charHandle;
    }

    int systemErrorCode = NO_ERROR;

    const HANDLE hService = openSystemService(
                remoteDevice, service->uuid, QIODevice::ReadOnly, &systemErrorCode);

    if (systemErrorCode != NO_ERROR) {
        qCWarning(QT_BT_WINDOWS) << "Unable to open service" << service->uuid.toString()
                                 << ":" << qt_error_string(systemErrorCode);
        service->setError(QLowEnergyService::CharacteristicReadError);
        return;
    }

    BTH_LE_GATT_CHARACTERISTIC gattCharacteristic = recoverNativeLeGattCharacteristic(
                service->startHandle, charHandle, charDetails);

    const QByteArray characteristicValue = getGattCharacteristicValue(
            hService, &gattCharacteristic, &systemErrorCode);
    closeSystemDevice(hService);

    if (systemErrorCode != NO_ERROR) {
        qCWarning(QT_BT_WINDOWS) << "Unable to get value for characteristic"
                                 << charDetails.uuid.toString()
                                 << "of the service" << service->uuid.toString()
                                 << ":" << qt_error_string(systemErrorCode);
        service->setError(QLowEnergyService::CharacteristicReadError);
        return;
    }

    updateValueOfCharacteristic(charHandle, characteristicValue, false);

    const QLowEnergyCharacteristic ch(service, charHandle);
    emit service->characteristicRead(ch, characteristicValue);
}

void QLowEnergyControllerPrivateWin32::writeCharacteristic(
        const QSharedPointer<QLowEnergyServicePrivate> service,
        const QLowEnergyHandle charHandle,
        const QByteArray &newValue,
        QLowEnergyService::WriteMode mode)
{
    Q_ASSERT(!service.isNull());
    if (!service->characteristicList.contains(charHandle))
        return;

    int systemErrorCode = NO_ERROR;

    const HANDLE hService = openSystemService(
                remoteDevice, service->uuid, QIODevice::ReadWrite, &systemErrorCode);

    if (systemErrorCode != NO_ERROR) {
        qCWarning(QT_BT_WINDOWS) << "Unable to open service" << service->uuid.toString()
                                 << ":" << qt_error_string(systemErrorCode);
        service->setError(QLowEnergyService::CharacteristicWriteError);
        return;
    }

    const QLowEnergyServicePrivate::CharData &charDetails
            = service->characteristicList[charHandle];

    BTH_LE_GATT_CHARACTERISTIC gattCharacteristic = recoverNativeLeGattCharacteristic(
                service->startHandle, charHandle, charDetails);

    const DWORD flags = (mode == QLowEnergyService::WriteWithResponse)
            ? BLUETOOTH_GATT_FLAG_NONE
            : BLUETOOTH_GATT_FLAG_WRITE_WITHOUT_RESPONSE;

    // TODO: If a device is not connected, this function will block
    // for some time. So, need to re-implement of writeCharacteristic()
    // with use QFutureWatcher.
    setGattCharacteristicValue(hService, &gattCharacteristic,
                               newValue, flags, &systemErrorCode);
    closeSystemDevice(hService);

    if (systemErrorCode != NO_ERROR) {
        qCWarning(QT_BT_WINDOWS) << "Unable to set value for characteristic"
                                 << charDetails.uuid.toString()
                                 << "of the service" << service->uuid.toString()
                                 << ":" << qt_error_string(systemErrorCode);
        service->setError(QLowEnergyService::CharacteristicWriteError);
        return;
    }

    updateValueOfCharacteristic(charHandle, newValue, false);

    if (mode == QLowEnergyService::WriteWithResponse) {
        const QLowEnergyCharacteristic ch(service, charHandle);
        emit service->characteristicWritten(ch, newValue);
    }
}

void QLowEnergyControllerPrivateWin32::readDescriptor(
        const QSharedPointer<QLowEnergyServicePrivate> service,
        const QLowEnergyHandle charHandle,
        const QLowEnergyHandle descriptorHandle)
{
    Q_ASSERT(!service.isNull());
    if (!service->characteristicList.contains(charHandle))
        return;

    const QLowEnergyServicePrivate::CharData &charDetails
            = service->characteristicList[charHandle];
    if (!charDetails.descriptorList.contains(descriptorHandle))
        return;

    int systemErrorCode = NO_ERROR;

    const HANDLE hService = openSystemService(
                remoteDevice, service->uuid, QIODevice::ReadOnly, &systemErrorCode);

    if (systemErrorCode != NO_ERROR) {
        qCWarning(QT_BT_WINDOWS) << "Unable to open service" << service->uuid.toString()
                                 << ":" << qt_error_string(systemErrorCode);
        service->setError(QLowEnergyService::DescriptorReadError);
        return;
    }

    const QLowEnergyServicePrivate::DescData &dscrDetails
            = charDetails.descriptorList[descriptorHandle];

    BTH_LE_GATT_DESCRIPTOR gattDescriptor = recoverNativeLeGattDescriptor(
                service->startHandle, charHandle, descriptorHandle, dscrDetails);

    const QByteArray value = getGattDescriptorValue(
                hService, const_cast<PBTH_LE_GATT_DESCRIPTOR>(
                    &gattDescriptor), &systemErrorCode);
    closeSystemDevice(hService);

    if (systemErrorCode != NO_ERROR) {
        qCWarning(QT_BT_WINDOWS) << "Unable to get value for descriptor"
                                 << dscrDetails.uuid.toString()
                                 << "for characteristic"
                                 << charDetails.uuid.toString()
                                 << "of the service" << service->uuid.toString()
                                 << ":" << qt_error_string(systemErrorCode);
        service->setError(QLowEnergyService::DescriptorReadError);
        return;
    }

    updateValueOfDescriptor(charHandle, descriptorHandle, value, false);

    QLowEnergyDescriptor dscr(service, charHandle, descriptorHandle);
    emit service->descriptorRead(dscr, value);
}

void QLowEnergyControllerPrivateWin32::writeDescriptor(
        const QSharedPointer<QLowEnergyServicePrivate> service,
        const QLowEnergyHandle charHandle,
        const QLowEnergyHandle descriptorHandle,
        const QByteArray &newValue)
{
    Q_ASSERT(!service.isNull());
    if (!service->characteristicList.contains(charHandle))
        return;

    QLowEnergyServicePrivate::CharData &charDetails
            = service->characteristicList[charHandle];
    if (!charDetails.descriptorList.contains(descriptorHandle))
        return;

    int systemErrorCode = NO_ERROR;

    const HANDLE hService = openSystemService(
                remoteDevice, service->uuid, QIODevice::ReadWrite, &systemErrorCode);

    if (systemErrorCode != NO_ERROR) {
        qCWarning(QT_BT_WINDOWS) << "Unable to open service" << service->uuid.toString()
                                 << ":" << qt_error_string(systemErrorCode);
        service->setError(QLowEnergyService::DescriptorWriteError);
        return;
    }

    const QLowEnergyServicePrivate::DescData &dscrDetails
            = charDetails.descriptorList[descriptorHandle];

    BTH_LE_GATT_DESCRIPTOR gattDescriptor = recoverNativeLeGattDescriptor(
                service->startHandle, charHandle, descriptorHandle, dscrDetails);

    setGattDescriptorValue(hService, &gattDescriptor, newValue, &systemErrorCode);

    if (systemErrorCode != NO_ERROR) {
        closeSystemDevice(hService);
        qCWarning(QT_BT_WINDOWS) << "Unable to set value for descriptor"
                                 << dscrDetails.uuid.toString()
                                 << "for characteristic"
                                 << charDetails.uuid.toString()
                                 << "of the service" << service->uuid.toString()
                                 << ":" << qt_error_string(systemErrorCode);
        service->setError(QLowEnergyService::DescriptorWriteError);
        return;
    }

    if (gattDescriptor.DescriptorType == ClientCharacteristicConfiguration) {

        QDataStream in(newValue);
        quint8 u;
        in >> u;

        if (u & ClientCharacteristicConfigurationValue::UseNotifications
                || u & ClientCharacteristicConfigurationValue::UseIndications) {
            if (!charDetails.hValueChangeEvent) {
                BTH_LE_GATT_CHARACTERISTIC gattCharacteristic = recoverNativeLeGattCharacteristic(
                            service->startHandle, charHandle, charDetails);

                charDetails.hValueChangeEvent = registerEvent(
                            hService, gattCharacteristic, this, &systemErrorCode);
            }
        } else {
            if (charDetails.hValueChangeEvent) {
                unregisterEvent(charDetails.hValueChangeEvent, &systemErrorCode);
                charDetails.hValueChangeEvent = NULL;
            }
        }

        closeSystemDevice(hService);

        if (systemErrorCode != NO_ERROR) {
            qCWarning(QT_BT_WINDOWS) << "Unable to subscribe events for descriptor"
                                     << dscrDetails.uuid.toString()
                                     << "for characteristic"
                                     << charDetails.uuid.toString()
                                     << "of the service" << service->uuid.toString()
                                     << ":" << qt_error_string(systemErrorCode);
            service->setError(QLowEnergyService::DescriptorWriteError);
            return;
        }
    } else {
        closeSystemDevice(hService);
    }

    updateValueOfDescriptor(charHandle, descriptorHandle, newValue, false);

    const QLowEnergyDescriptor dscr(service, charHandle, descriptorHandle);
    emit service->descriptorWritten(dscr, newValue);
}

void QLowEnergyControllerPrivateWin32::addToGenericAttributeList(const QLowEnergyServiceData &, QLowEnergyHandle)
{
    Q_UNIMPLEMENTED();
}

QT_END_NAMESPACE
