package com.babytank.app.ble

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanResult
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import com.babytank.app.model.BabyTankBleContract
import com.babytank.app.model.BleScanState
import com.babytank.app.model.ControlLinkPhase
import com.babytank.app.model.ControlLinkState
import com.babytank.app.model.DiagnosticPhase
import com.babytank.app.model.DiagnosticSnapshot
import com.babytank.app.model.DiagnosticState
import com.babytank.app.model.NearbyDevice
import com.babytank.app.model.ScanPhase
import com.espressif.provisioning.DeviceConnectionEvent
import com.espressif.provisioning.ESPConstants
import com.espressif.provisioning.ESPDevice
import com.espressif.provisioning.ESPProvisionManager
import com.espressif.provisioning.listeners.BleScanListener
import com.espressif.provisioning.listeners.ProvisionListener
import com.espressif.provisioning.listeners.ResponseListener
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.greenrobot.eventbus.EventBus
import org.greenrobot.eventbus.Subscribe
import org.greenrobot.eventbus.ThreadMode

/**
 * Owns Espressif's encrypted provisioning session. No Wi-Fi password or
 * proof-of-possession value is retained after it is handed to the library.
 */
class BabyTankProvisioner(context: Context) {
    data class CameraLanInfo(val address: String, val token: ByteArray)
    private data class QueuedBleOperation(
        val generation: Int,
        val run: ((() -> Unit) -> Unit),
    )
    private val appContext = context.applicationContext
    private val preferences = appContext.getSharedPreferences("baby_tank_device", Context.MODE_PRIVATE)
    private val bluetoothManager = appContext.getSystemService(BluetoothManager::class.java)
    private val manager = ESPProvisionManager.getInstance(appContext)
    private val peripherals = linkedMapOf<String, BluetoothDevice>()
    private val serviceUuids = linkedMapOf<String, String>()
    private val found = linkedMapOf<String, NearbyDevice>()
    private val mutableState = MutableStateFlow(BleScanState())
    val state: StateFlow<BleScanState> = mutableState.asStateFlow()
    private val mutableControlState = MutableStateFlow(ControlLinkState())
    val controlState: StateFlow<ControlLinkState> = mutableControlState.asStateFlow()
    private val mutablePersonalityStatus = MutableStateFlow("尚未同步到设备")
    val personalityStatus: StateFlow<String> = mutablePersonalityStatus.asStateFlow()
    private val mutableDiagnosticState = MutableStateFlow(DiagnosticState())
    val diagnosticState: StateFlow<DiagnosticState> = mutableDiagnosticState.asStateFlow()
    private val mutableCloudDeviceId = MutableStateFlow<String?>(null)
    val cloudDeviceId: StateFlow<String?> = mutableCloudDeviceId.asStateFlow()
    private var espDevice: ESPDevice? = null
    private var connectingDevice: NearbyDevice? = null
    private val motionLock = Any()
    private var motionPending = false
    private var motionQueued = false
    private var motionLeft = 0
    private var motionRight = 0
    private var motionSequence = 1
    private var motionGeneration = 0
    private var motionRevision = 0
    private val bleOperationLock = Any()
    private val bleOperations = ArrayDeque<QueuedBleOperation>()
    private var bleOperationPending = false
    private var bleOperationGeneration = 0

    init {
        EventBus.getDefault().register(this)
    }

    private fun enqueueBleOperation(run: ((() -> Unit) -> Unit)) {
        var start: QueuedBleOperation? = null
        synchronized(bleOperationLock) {
            val operation = QueuedBleOperation(bleOperationGeneration, run)
            if (bleOperationPending) {
                bleOperations.addLast(operation)
            } else {
                bleOperationPending = true
                start = operation
            }
        }
        start?.let(::startBleOperation)
    }

    private fun startBleOperation(operation: QueuedBleOperation) {
        try {
            operation.run { finishBleOperation(operation.generation) }
        } catch (error: RuntimeException) {
            finishBleOperation(operation.generation)
            throw error
        }
    }

    private fun finishBleOperation(generation: Int) {
        var next: QueuedBleOperation? = null
        synchronized(bleOperationLock) {
            if (generation != bleOperationGeneration) return
            next = if (bleOperations.isEmpty()) null else bleOperations.removeFirst()
            bleOperationPending = next != null
        }
        next?.let(::startBleOperation)
    }

    private fun resetBleOperations() {
        synchronized(bleOperationLock) {
            ++bleOperationGeneration
            bleOperations.clear()
            bleOperationPending = false
        }
    }

    fun rememberedDevice(): NearbyDevice? {
        val address = preferences.getString("address", null) ?: return null
        val name = preferences.getString("name", null) ?: return null
        return NearbyDevice(address, name, -127)
    }

    private val scanListener = object : BleScanListener {
        override fun scanStartFailed() {
            mutableState.value = BleScanState(
                phase = ScanPhase.BluetoothOff,
                detail = "请先打开蓝牙",
            )
        }

        @SuppressLint("MissingPermission")
        override fun onPeripheralFound(device: BluetoothDevice, scanResult: ScanResult) {
            val address = device.address
            val name = scanResult.scanRecord?.deviceName ?: device.name ?: "Baby Tank"
            val serviceUuid = scanResult.scanRecord?.serviceUuids
                ?.firstOrNull()
                ?.uuid
                ?.toString()
                ?: BabyTankBleContract.provisioningService.toString()
            peripherals[address] = device
            serviceUuids[address] = serviceUuid
            found[address] = NearbyDevice(address, name, scanResult.rssi)
            mutableState.value = mutableState.value.copy(
                devices = found.values.sortedByDescending(NearbyDevice::rssi),
            )
        }

        override fun scanCompleted() {
            if (mutableState.value.phase == ScanPhase.Scanning) {
                mutableState.value = mutableState.value.copy(
                    phase = ScanPhase.Idle,
                detail = if (found.isEmpty()) "没有发现 Baby Tank，请确认头部已通电" else null,
                )
            }
        }

        override fun onFailure(error: Exception) {
            mutableState.value = mutableState.value.copy(
                phase = ScanPhase.Failed,
                detail = error.message ?: "BLE 扫描失败",
            )
        }
    }

    fun hasRuntimePermission(): Boolean = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        appContext.checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED &&
            appContext.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
    } else {
        appContext.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
    }

    fun startScan() {
        if (!appContext.packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE)) {
            mutableState.value = BleScanState(ScanPhase.Unsupported, detail = "手机不支持 BLE")
            return
        }
        if (!hasRuntimePermission()) {
            mutableState.value = BleScanState(ScanPhase.MissingPermission, detail = "需要附近设备权限")
            return
        }
        if (bluetoothManager?.adapter?.isEnabled != true) {
            mutableState.value = BleScanState(ScanPhase.BluetoothOff, detail = "请先打开蓝牙")
            return
        }
        stopScan()
        peripherals.clear()
        serviceUuids.clear()
        found.clear()
        mutableState.value = BleScanState(ScanPhase.Scanning)
        manager.searchBleEspDevices(BabyTankBleContract.deviceNamePrefix, scanListener)
    }

    fun stopScan() {
        if (hasRuntimePermission()) manager.stopBleScan()
        if (mutableState.value.phase == ScanPhase.Scanning) {
            mutableState.value = mutableState.value.copy(phase = ScanPhase.Idle)
        }
    }

    @SuppressLint("MissingPermission")
    fun connect(device: NearbyDevice, proofOfPossession: CharArray) {
        if (proofOfPossession.size !in 8..64) {
            proofOfPossession.fill('\u0000')
            mutableState.value = mutableState.value.copy(
                phase = ScanPhase.Failed,
                detail = "设备配对码应为 8–64 个字符",
            )
            return
        }
        val peripheral = peripherals[device.address]
        if (peripheral == null) {
            proofOfPossession.fill('\u0000')
            mutableState.value = mutableState.value.copy(
                phase = ScanPhase.Failed,
                detail = "设备扫描记录已失效，请重新扫描",
            )
            return
        }
        stopScan()
        connectingDevice = device
        mutableCloudDeviceId.value = null
        resetBleOperations()
        espDevice?.disconnectDevice()
        espDevice = manager.createESPDevice(
            ESPConstants.TransportType.TRANSPORT_BLE,
            ESPConstants.SecurityType.SECURITY_1,
        ).also {
            it.setProofOfPossession(proofOfPossession.concatToString())
            proofOfPossession.fill('\u0000')
            mutableState.value = mutableState.value.copy(
                phase = ScanPhase.Connecting,
                detail = "正在建立加密会话…",
            )
            it.connectBLEDevice(
                peripheral,
                serviceUuids[device.address]
                    ?: BabyTankBleContract.provisioningService.toString(),
            )
        }
    }

    fun provision(ssid: String, password: CharArray) {
        if (ssid.startsWith(BabyTankBleContract.deviceNamePrefix, ignoreCase = true)) {
            password.fill('\u0000')
            mutableState.value = mutableState.value.copy(
                phase = ScanPhase.Failed,
                detail = "不能把机器人自己的热点设为目标 Wi-Fi",
            )
            return
        }
        if (mutableState.value.phase != ScanPhase.Connected || ssid.isBlank()) {
            password.fill('\u0000')
            mutableState.value = mutableState.value.copy(
                phase = ScanPhase.Failed,
                detail = "请先建立安全连接并填写 Wi-Fi 名称",
            )
            return
        }
        val secret = password.concatToString()
        password.fill('\u0000')
        mutableState.value = mutableState.value.copy(
            phase = ScanPhase.Provisioning,
            detail = "正在准备设备网络…",
        )
        val device = espDevice ?: run {
            fail("设备连接已失效", IllegalStateException("missing ESP device"))
            return
        }
        device.sendDataToCustomEndPoint("baby-wifi-prepare", byteArrayOf(1), object : ResponseListener {
            override fun onSuccess(returnData: ByteArray) {
                if (returnData.size != 1 || returnData[0].toInt() != 0) {
                    fail("设备无法准备网络", IllegalStateException("配网状态重置失败"))
                    return
                }
                update("正在验证家庭网络…")
                device.provision(ssid, secret, object : ProvisionListener {
            override fun createSessionFailed(error: Exception) = fail("安全会话失败", error)
            override fun wifiConfigSent() = update("网络信息已安全发送")
            override fun wifiConfigFailed(error: Exception) = fail("网络信息发送失败", error)
            override fun wifiConfigApplied() = update("设备正在连接家庭网络…")
            override fun wifiConfigApplyFailed(error: Exception) = fail("设备无法应用网络信息", error)

            override fun provisioningFailedFromDevice(reason: ESPConstants.ProvisionFailureReason) {
                val message = when (reason) {
                    ESPConstants.ProvisionFailureReason.AUTH_FAILED -> "Wi-Fi 密码不正确"
                    ESPConstants.ProvisionFailureReason.NETWORK_NOT_FOUND -> "设备找不到这个 Wi-Fi"
                    ESPConstants.ProvisionFailureReason.DEVICE_DISCONNECTED -> "配网时蓝牙连接中断"
                    ESPConstants.ProvisionFailureReason.UNKNOWN -> "设备未能连接 Wi-Fi"
                }
                mutableState.value = mutableState.value.copy(phase = ScanPhase.Failed, detail = message)
            }

            override fun deviceProvisioningSuccess() {
                mutableState.value = mutableState.value.copy(
                    phase = ScanPhase.Provisioned,
                    detail = "配网成功，Baby Tank 已加入家庭网络",
                )
            }

            override fun onProvisioningFailed(error: Exception) = fail("配网失败", error)
                })
            }

            override fun onFailure(error: Exception) = fail("设备无法准备网络", error)
        })
    }

    private fun update(message: String) {
        mutableState.value = mutableState.value.copy(detail = message)
    }

    private fun fail(prefix: String, error: Exception) {
        mutableState.value = mutableState.value.copy(
            phase = ScanPhase.Failed,
            detail = "$prefix：${error.message ?: "未知错误"}",
        )
    }

    fun testControlChannel() {
        val device = espDevice
        if (device == null || mutableState.value.phase !in
            setOf(ScanPhase.Connected, ScanPhase.Provisioned)
        ) {
            mutableControlState.value = ControlLinkState(
                ControlLinkPhase.Failed,
                "请先在连接页建立安全蓝牙会话",
            )
            return
        }
        // STOP is the safety handshake: the head only reports success after
        // an exact UART echo from the chassis. Ping alone proves too little.
        val request = byteArrayOf(1, 0, 1, 0, 0, 0, 0, 0)
        mutableControlState.value = ControlLinkState(
            ControlLinkPhase.Testing,
            "正在验证加密控制端点…",
        )
        enqueueBleOperation { done ->
            device.sendDataToCustomEndPoint("baby-control", request, object : ResponseListener {
                override fun onSuccess(returnData: ByteArray) {
                    val valid = returnData.size == 8 &&
                        returnData[0].toInt() == 1 &&
                        (returnData[1].toInt() and 0xff) == 0x80 &&
                        returnData[2].toInt() == 1 &&
                        returnData[3].toInt() == 0
                    mutableControlState.value = if (valid) {
                        ControlLinkState(ControlLinkPhase.Available, "底盘 STOP 回显已验证，可遥控")
                    } else {
                        ControlLinkState(ControlLinkPhase.Failed, "设备返回了不兼容的控制协议")
                    }
                    done()
                }

                override fun onFailure(error: Exception) {
                    mutableControlState.value = ControlLinkState(
                        ControlLinkPhase.Failed,
                        error.message ?: "BLE 控制端点无响应",
                    )
                    done()
                }
            })
        }
    }

    fun refreshCloudIdentity() {
        val device = espDevice ?: return
        enqueueBleOperation { done ->
            device.sendDataToCustomEndPoint("baby-identity", byteArrayOf(1),
                object : ResponseListener {
                    override fun onSuccess(returnData: ByteArray) {
                        val length = returnData.getOrNull(1)?.toInt()?.and(0xff) ?: 0
                        val id = if (returnData.size == length + 2 && length in 1..64 &&
                            returnData[0].toInt() == 1) {
                            String(returnData, 2, length, Charsets.UTF_8)
                                .takeIf { it.matches(Regex("[A-Za-z0-9._-]{1,64}")) }
                        } else null
                        mutableCloudDeviceId.value = id
                        done()
                    }

                    override fun onFailure(error: Exception) {
                        mutableCloudDeviceId.value = null
                        done()
                    }
                })
        }
    }

    fun refreshDiagnostics() = sendDiagnostic(0, label = "设备状态")

    fun showExpression(index: Int) {
        showExpressionConfirmed(index, null)
    }

    fun showExpressionConfirmed(index: Int, onAccepted: ((Boolean) -> Unit)?) {
        val duration = if (index == 24) 1700 else 6000
        sendDiagnostic(
            opcode = 1,
            argument0 = index,
            argument1 = if (index == 24) 100 else 82,
            argument2 = duration and 0xff,
            argument3 = duration shr 8,
            label = "表情测试",
            onAccepted = onAccepted,
        )
    }

    fun lookAt(horizontal: Int, vertical: Int) = sendDiagnostic(
        opcode = 2,
        argument0 = horizontal.coerceIn(-100, 100) and 0xff,
        argument1 = vertical.coerceIn(-100, 100) and 0xff,
        argument2 = 1800 and 0xff,
        argument3 = 1800 shr 8,
        label = "视线测试",
    )

    fun setLight(mode: Int) = sendDiagnostic(
        opcode = 3,
        argument0 = mode,
        label = "灯光模式",
    )

    fun testSpeaker(variant: Int = 0) = sendDiagnostic(
        opcode = 4,
        argument0 = variant,
        label = "扬声器测试",
    )

    fun setVolume(volume: Int) = sendDiagnostic(
        opcode = 8,
        argument0 = volume.coerceIn(0, 100),
        label = "音量",
    )

    /** 0=robot win, 1=robot lose, 2=draw; spoken by embedded PCM. */
    fun announceGameResult(result: Int) = sendDiagnostic(
        opcode = 9,
        argument0 = result.coerceIn(0, 2),
        label = "游戏结果",
    )

    fun announceGameResultConfirmed(result: Int, onAccepted: (Boolean) -> Unit) =
        sendDiagnostic(opcode = 9, argument0 = result.coerceIn(0, 2),
            label = "游戏结果", onAccepted = onAccepted)

    /** Completes the on-head eating animation and returns the session reaction:
     * 0 normal, 1 favorite, 2 less preferred, 3 repeated, 4 after-meal drink. */
    fun finishFeeding(item: Int, onResult: (Boolean, Int) -> Unit) =
        sendDiagnostic(
            opcode = 10,
            argument0 = item,
            label = "喂食反馈",
            onDetailed = onResult,
        )

    /** Uses the same offline voice/face pool as a physical side touch. */
    fun pokeConfirmed(onAccepted: (Boolean) -> Unit) =
        sendDiagnostic(opcode = 11, label = "戳一戳", onAccepted = onAccepted)

    /** Enters ordinary wakeable sleep; deep-night policy is a separate gate. */
    fun sleepConfirmed(onAccepted: (Boolean) -> Unit) =
        sendDiagnostic(opcode = 12, label = "睡觉", onAccepted = onAccepted)

    fun startListening() = sendDiagnostic(opcode = 5, label = "语音对话")

    fun stopListening() = sendDiagnostic(opcode = 13, label = "结束对话")

    fun restartDevice(onAccepted: (Boolean) -> Unit) = sendDiagnostic(
        opcode = 14,
        label = "重新启动设备",
        onAccepted = onAccepted,
    )

    fun setSetupHotspot(enabled: Boolean, onAccepted: (Boolean) -> Unit) =
        sendDiagnostic(
            opcode = 15,
            argument0 = if (enabled) 1 else 0,
            label = if (enabled) "开启配网热点" else "关闭配网热点",
            onAccepted = onAccepted,
        )

    fun setRemoteMode(enabled: Boolean) = sendDiagnostic(
        opcode = 6,
        argument0 = if (enabled) 1 else 0,
        label = if (enabled) "遥控模式" else "AI 唤醒",
    )

    fun fireCannon() = sendDiagnostic(
        opcode = 7,
        label = "开炮",
    )

    private fun sendDiagnostic(
        opcode: Int,
        argument0: Int = 0,
        argument1: Int = 0,
        argument2: Int = 0,
        argument3: Int = 0,
        argument4: Int = 0,
        argument5: Int = 0,
        label: String,
        onAccepted: ((Boolean) -> Unit)? = null,
        onDetailed: ((Boolean, Int) -> Unit)? = null,
    ) {
        val device = espDevice
        if (device == null || mutableState.value.phase !in
            setOf(ScanPhase.Connected, ScanPhase.Provisioned)
        ) {
            mutableDiagnosticState.value = DiagnosticState(
                DiagnosticPhase.Failed, "请先连接 Baby Tank", label,
            )
            onAccepted?.invoke(false)
            onDetailed?.invoke(false, 0)
            return
        }
        val request = byteArrayOf(
            1, opcode.toByte(), argument0.toByte(), argument1.toByte(),
            argument2.toByte(), argument3.toByte(), argument4.toByte(),
            argument5.toByte(),
        )
        mutableDiagnosticState.value = mutableDiagnosticState.value.copy(
            phase = DiagnosticPhase.Running,
            detail = "$label…",
            lastCommand = label,
        )
        enqueueBleOperation { done ->
            device.sendDataToCustomEndPoint("baby-diagnostics", request,
                object : ResponseListener {
                override fun onSuccess(returnData: ByteArray) {
                    if (returnData.size != 40 || returnData[0].toInt() != 1 ||
                        (returnData[1].toInt() and 0x7f) != opcode) {
                        mutableDiagnosticState.value = DiagnosticState(
                            DiagnosticPhase.Failed, "固件诊断协议不兼容", label,
                        )
                        done()
                        onAccepted?.invoke(false)
                        onDetailed?.invoke(false, 0)
                        return
                    }
                    val status = returnData[2].toInt() and 0xff
                    val snapshot = parseDiagnosticSnapshot(returnData)
                    mutableDiagnosticState.value = DiagnosticState(
                        phase = if (status == 0) DiagnosticPhase.Ready else DiagnosticPhase.Failed,
                        detail = when (status) {
                            0 -> "$label 完成"
                            1 -> "$label 参数无效"
                            2 -> "$label 当前不可用"
                            3 -> "$label 正忙"
                            else -> "$label 返回未知状态"
                        },
                        lastCommand = label,
                        snapshot = snapshot,
                    )
                    done()
                    onAccepted?.invoke(status == 0)
                    onDetailed?.invoke(status == 0, returnData[39].toInt() and 0xff)
                }

                override fun onFailure(error: Exception) {
                    mutableDiagnosticState.value = mutableDiagnosticState.value.copy(
                        phase = DiagnosticPhase.Failed,
                        detail = "$label 失败：${error.message ?: "蓝牙无响应"}",
                        lastCommand = label,
                    )
                    done()
                    onAccepted?.invoke(false)
                    onDetailed?.invoke(false, 0)
                }
                },
            )
        }
    }

    private fun parseDiagnosticSnapshot(data: ByteArray): DiagnosticSnapshot {
        fun u8(index: Int) = data[index].toInt() and 0xff
        fun u16(index: Int) = u8(index) or (u8(index + 1) shl 8)
        fun u32(index: Int) = u8(index).toLong() or
            (u8(index + 1).toLong() shl 8) or
            (u8(index + 2).toLong() shl 16) or
            (u8(index + 3).toLong() shl 24)
        val flags = u8(3)
        return DiagnosticSnapshot(
            setupHotspotEnabled = u8(11) and (1 shl 1) != 0,
            wifiConnected = flags and (1 shl 0) != 0,
            cloudReady = flags and (1 shl 1) != 0,
            softApReady = flags and (1 shl 2) != 0,
            cameraReady = flags and (1 shl 3) != 0,
            chassisReady = flags and (1 shl 4) != 0,
            chassisVerified = flags and (1 shl 5) != 0,
            audioPlaying = flags and (1 shl 6) != 0,
            touchReady = flags and (1 shl 7) != 0,
            expression = u8(4),
            intensity = u8(5),
            speechLevel = u8(6),
            speakerVolume = u8(38).coerceIn(0, 100),
            conversationPhase = u8(7),
            remoteControlMode = u8(11) and 1 != 0,
            touchActiveMask = u8(8),
            internetPhase = u8(9),
            uptimeSeconds = u32(12),
            freeInternalHeap = u32(16),
            touchCounts = List(3) { u16(20 + it * 2) },
            touchDeltas = List(3) { u16(26 + it * 2) },
            touchThresholds = List(3) { u16(32 + it * 2) },
        )
    }

    fun requestCameraLanInfo(onResult: (Result<CameraLanInfo?>) -> Unit) {
        val device = espDevice
        if (device == null || mutableState.value.phase !in
            setOf(ScanPhase.Connected, ScanPhase.Provisioned)
        ) {
            onResult(Result.failure(IllegalStateException("请先连接设备蓝牙")))
            return
        }
        enqueueBleOperation { done ->
            device.sendDataToCustomEndPoint("baby-camera-info", byteArrayOf(1),
                object : ResponseListener {
                override fun onSuccess(returnData: ByteArray) {
                    if (returnData.size != 22 || returnData[0].toInt() != 1) {
                        onResult(Result.failure(IllegalStateException("设备摄像头协议不兼容")))
                        done()
                        return
                    }
                    if (returnData[1].toInt() == 2) {
                        onResult(Result.failure(IllegalStateException(
                            "设备处于深夜休眠或时间待校准，请先让头部联网同步时间",
                        )))
                        done()
                        return
                    }
                    if (returnData[1].toInt() != 0) {
                        onResult(Result.success(null))
                        done()
                        return
                    }
                    val address = (2..5).joinToString(".") {
                        (returnData[it].toInt() and 0xff).toString()
                    }
                    onResult(Result.success(CameraLanInfo(address,
                        returnData.copyOfRange(6, 22))))
                    done()
                }

                override fun onFailure(error: Exception) {
                    onResult(Result.failure(error))
                    done()
                }
                })
        }
    }

    fun enableCameraHotspot(onResult: (Result<Unit>) -> Unit) {
        val device = espDevice
        if (device == null || mutableState.value.phase !in
            setOf(ScanPhase.Connected, ScanPhase.Provisioned)
        ) {
            onResult(Result.failure(IllegalStateException("请先连接设备蓝牙")))
            return
        }
        enqueueBleOperation { done ->
            device.sendDataToCustomEndPoint("baby-camera-info", byteArrayOf(2),
                object : ResponseListener {
                override fun onSuccess(returnData: ByteArray) {
                    if (returnData.size == 22 && returnData[0].toInt() == 1 &&
                        returnData[1].toInt() != 2) {
                        onResult(Result.success(Unit))
                    } else {
                        onResult(Result.failure(IllegalStateException(
                            if (returnData.size == 22 && returnData[1].toInt() == 2)
                                "设备处于深夜休眠或时间待校准，请先让头部联网同步时间"
                            else "设备热点未能启动",
                        )))
                    }
                    done()
                }

                override fun onFailure(error: Exception) {
                    onResult(Result.failure(error))
                    done()
                }
                })
        }
    }

    fun syncPersonality(curiosity: Int, warmth: Int, talkativeness: Int) {
        val device = espDevice
        if (device == null || mutableState.value.phase !in
            setOf(ScanPhase.Connected, ScanPhase.Provisioned)
        ) {
            mutablePersonalityStatus.value = "请先连接设备蓝牙"
            return
        }
        val values = intArrayOf(curiosity, warmth, talkativeness)
        if (values.any { it !in 0..100 }) {
            mutablePersonalityStatus.value = "人格参数必须在 0–100 之间"
            return
        }
        mutablePersonalityStatus.value = "正在通过加密蓝牙同步…"
        device.sendDataToCustomEndPoint(
            "baby-personality",
            byteArrayOf(1, values[0].toByte(), values[1].toByte(), values[2].toByte()),
            object : ResponseListener {
                override fun onSuccess(returnData: ByteArray) {
                    val accepted = returnData.size == 5 && returnData[0].toInt() == 1 &&
                        returnData[1].toInt() == 0 &&
                        (returnData[2].toInt() and 0xff) == values[0] &&
                        (returnData[3].toInt() and 0xff) == values[1] &&
                        (returnData[4].toInt() and 0xff) == values[2]
                    mutablePersonalityStatus.value = if (accepted) "已同步并保存到设备" else
                        "设备未确认人格配置"
                }

                override fun onFailure(error: Exception) {
                    mutablePersonalityStatus.value =
                        "同步失败：${error.message ?: "蓝牙无响应"}"
                }
            },
        )
    }

    fun drive(left: Int, right: Int) = queueMotion(left, right)

    fun stopMotion() = queueMotion(0, 0)

    private fun queueMotion(left: Int, right: Int) {
        synchronized(motionLock) {
            motionLeft = left.coerceIn(-1000, 1000)
            motionRight = right.coerceIn(-1000, 1000)
            ++motionRevision
            motionQueued = true
            if (!motionPending) dispatchMotionLocked()
        }
    }

    private fun dispatchMotionLocked() {
        val device = espDevice
        if (device == null || mutableState.value.phase !in
            setOf(ScanPhase.Connected, ScanPhase.Provisioned)
        ) {
            motionQueued = false
            mutableControlState.value = ControlLinkState(
                ControlLinkPhase.Failed, "控制会话已断开；底盘将自动停车",
            )
            return
        }
        val left = motionLeft
        val right = motionRight
        val stopped = left == 0 && right == 0
        val sequence = motionSequence++ and 0xff
        val generation = motionGeneration
        val revision = motionRevision
        val request = byteArrayOf(
            1, if (stopped) 0 else 1, sequence.toByte(), 0,
            left.toByte(), (left shr 8).toByte(),
            right.toByte(), (right shr 8).toByte(),
        )
        motionQueued = false
        motionPending = true
        enqueueBleOperation { done ->
            val stale = synchronized(motionLock) { revision != motionRevision }
            if (stale) {
                finishMotion(generation)
                done()
                return@enqueueBleOperation
            }
            try {
                device.sendDataToCustomEndPoint("baby-control", request, object : ResponseListener {
                override fun onSuccess(returnData: ByteArray) {
                    if (generation != motionGeneration) return
                    val accepted = returnData.size == 8 &&
                        returnData[0].toInt() == 1 &&
                        (returnData[1].toInt() and 0xff) == (if (stopped) 0x80 else 0x81) &&
                        (returnData[2].toInt() and 0xff) == sequence &&
                        returnData[3].toInt() == 0
                    mutableControlState.value = if (accepted) {
                        ControlLinkState(ControlLinkPhase.Available,
                            "底盘已回显命令；松手自动停车")
                    } else {
                        ControlLinkState(ControlLinkPhase.Failed,
                            "头部拒绝控制命令，请停止操作")
                    }
                    finishMotion(generation)
                    done()
                }

                override fun onFailure(error: Exception) {
                    if (generation != motionGeneration) return
                    mutableControlState.value = ControlLinkState(
                        ControlLinkPhase.Failed,
                        "控制链路中断：${error.message ?: "未知错误"}",
                    )
                    finishMotion(generation)
                    done()
                }
                })
            } catch (error: RuntimeException) {
                motionPending = false
                mutableControlState.value = ControlLinkState(
                    ControlLinkPhase.Failed,
                    "无法发送控制命令：${error.message ?: "未知错误"}",
                )
                done()
            }
        }
    }

    private fun finishMotion(generation: Int) {
        synchronized(motionLock) {
            if (generation != motionGeneration) return
            motionPending = false
            if (motionQueued) dispatchMotionLocked()
        }
    }

    @Subscribe(threadMode = ThreadMode.MAIN)
    fun onDeviceConnection(event: DeviceConnectionEvent) {
        when (event.eventType) {
            ESPConstants.EVENT_DEVICE_CONNECTED -> {
                connectingDevice?.let { device ->
                    preferences.edit()
                        .putString("address", device.address)
                        .putString("name", device.name)
                        .apply()
                }
                connectingDevice = null
                mutableState.value = mutableState.value.copy(
                    phase = ScanPhase.Connected,
                    detail = "安全通道已连接；仅首次或换网时配置 Wi-Fi",
                )
                // Establish Security1 through exactly one custom endpoint first.
                // Starting diagnostics and camera-info in parallel here caused
                // duplicate handshakes and left the remote disabled.
                testControlChannel()
                refreshCloudIdentity()
            }
            ESPConstants.EVENT_DEVICE_DISCONNECTED -> {
                mutableCloudDeviceId.value = null
                resetBleOperations()
                synchronized(motionLock) {
                    ++motionGeneration
                    ++motionRevision
                    motionPending = false
                    motionQueued = false
                    motionLeft = 0
                    motionRight = 0
                }
                mutableControlState.value = ControlLinkState(
                    ControlLinkPhase.Failed, "蓝牙断开；等待头部及底盘超时停车",
                )
                mutableState.value = mutableState.value.copy(
                    phase = ScanPhase.Idle,
                    detail = "设备连接已断开",
                )
            }
            ESPConstants.EVENT_DEVICE_CONNECTION_FAILED -> {
                resetBleOperations()
                connectingDevice = null
                mutableState.value = mutableState.value.copy(
                    phase = ScanPhase.Failed,
                    detail = "无法连接设备，请核对配对码后重试",
                )
            }
        }
    }

    fun close() {
        stopScan()
        espDevice?.disconnectDevice()
        if (EventBus.getDefault().isRegistered(this)) EventBus.getDefault().unregister(this)
    }
}
