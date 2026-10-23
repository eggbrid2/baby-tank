package com.babytank.app

import android.app.Application
import android.content.ComponentCallbacks2
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.os.Bundle
import android.net.Uri
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.viewModels
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.babytank.app.ble.BabyTankProvisioner
import com.babytank.app.cloud.ParentMemoryClient
import com.babytank.app.cloud.SketchGuessTurn
import com.babytank.app.cloud.TextTurnStatus
import com.babytank.app.model.NearbyDevice
import com.babytank.app.model.ScanPhase
import com.babytank.app.ui.BabyTankApp
import com.babytank.app.ui.BabyTankTheme
import com.babytank.app.wifi.BabyTankVideoClient
import com.babytank.app.wifi.BabyTankPortalClient
import com.babytank.app.wifi.WifiNetworkDiscovery
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.delay
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import kotlin.math.abs
import kotlin.math.roundToInt

data class ChatMessage(val id: String, val fromUser: Boolean, val text: String)
data class TextChatState(
    val messages: List<ChatMessage> = emptyList(),
    val sending: Boolean = false,
    val status: String = "",
    val error: String? = null,
)
data class SketchPoint(val x: Float, val y: Float)
data class SketchStroke(val points: List<SketchPoint>)
data class DrawingGuessState(
    val round: Int = 1,
    val sending: Boolean = false,
    val guess: String? = null,
    val status: String = "画好后，让宝宝坦克猜一猜",
    val error: String? = null,
    val completed: Boolean = false,
)

class MainActivity : ComponentActivity() {
    private val babyTankViewModel: BabyTankViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            BabyTankTheme {
                BabyTankApp(babyTankViewModel)
            }
        }
    }

    override fun onStop() {
        babyTankViewModel.releaseTransientResources()
        super.onStop()
    }

    override fun onTrimMemory(level: Int) {
        super.onTrimMemory(level)
        if (level >= ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW) {
            babyTankViewModel.releaseTransientResources()
        }
    }
}

class BabyTankViewModel(application: Application) : AndroidViewModel(application) {
    private val provisioner = BabyTankProvisioner(application)
    private val videoClient = BabyTankVideoClient(application)
    private val portalClient = BabyTankPortalClient(application)
    private val wifiDiscovery = WifiNetworkDiscovery(application)
    private val parentMemoryClient = ParentMemoryClient(application)
    val scanState = provisioner.state
    val controlState = provisioner.controlState
    val personalityStatus = provisioner.personalityStatus
    val diagnosticState = provisioner.diagnosticState
    val videoState = videoClient.state
    val videoFrame = videoClient.frame
    val portalState = portalClient.state
    val wifiState = wifiDiscovery.state
    val parentMemoryState = parentMemoryClient.state
    private val mutableTextChat = MutableStateFlow(TextChatState())
    val textChat: StateFlow<TextChatState> = mutableTextChat.asStateFlow()
    private val mutableDrawingGuess = MutableStateFlow(DrawingGuessState())
    val drawingGuess: StateFlow<DrawingGuessState> = mutableDrawingGuess.asStateFlow()
    private var drawingGuessJob: Job? = null
    val cloudDeviceId = provisioner.cloudDeviceId
    private val mutableInteractionSync = MutableStateFlow("成长数值尚未同步")
    val interactionSync: StateFlow<String> = mutableInteractionSync.asStateFlow()

    private val rememberedDevice = provisioner.rememberedDevice()
    private val mutableSelectedDevice = MutableStateFlow(rememberedDevice)
    val selectedDevice: StateFlow<NearbyDevice?> = mutableSelectedDevice.asStateFlow()
    private var autoReconnectAddress: String? = rememberedDevice?.address

    init {
        viewModelScope.launch {
            while (isActive) {
                delay(15_000)
                if (scanState.value.phase in setOf(ScanPhase.Connected, ScanPhase.Provisioned) &&
                    cloudDeviceId.value == null) {
                    provisioner.refreshCloudIdentity()
                }
            }
        }
        if (parentMemoryClient.hasAccess) viewModelScope.launch {
            parentMemoryClient.refresh()
        }
        if (autoReconnectAddress != null && provisioner.hasRuntimePermission()) {
            viewModelScope.launch {
                scanState.collect { scan ->
                    val address = autoReconnectAddress ?: return@collect
                    val device = scan.devices.firstOrNull { it.address == address }
                    if (device != null && scan.phase == com.babytank.app.model.ScanPhase.Scanning) {
                        autoReconnectAddress = null
                        mutableSelectedDevice.value = device
                        provisioner.connect(device, BuildConfig.BABY_TANK_SETUP_POP.toCharArray())
                    }
                }
            }
            provisioner.startScan()
        }
    }

    fun hasBlePermission(): Boolean = provisioner.hasRuntimePermission()

    fun hasWifiSetupPermission(): Boolean = wifiDiscovery.hasPermission()

    fun startScan() = provisioner.startScan()

    fun stopScan() = provisioner.stopScan()

    fun selectDevice(device: NearbyDevice) {
        viewModelScope.launch {
            autoReconnectAddress = null
            provisioner.stopScan()
            mutableSelectedDevice.value = device
        }
    }

    fun connectDevice() {
        val device = mutableSelectedDevice.value
        if (device == null) return
        provisioner.connect(device, BuildConfig.BABY_TANK_SETUP_POP.toCharArray())
    }

    fun refreshWifi() = wifiDiscovery.refreshCurrent()

    fun scanWifi() = wifiDiscovery.scan()

    fun provisionWifi(ssid: String, password: CharArray) =
        provisioner.provision(ssid, password)

    fun testControlChannel() = provisioner.testControlChannel()

    fun refreshDiagnostics() = provisioner.refreshDiagnostics()

    fun showExpression(index: Int) = provisioner.showExpression(index)

    fun showExpressionConfirmed(index: Int, onAccepted: (Boolean) -> Unit) =
        provisioner.showExpressionConfirmed(index, onAccepted)

    fun lookAt(horizontal: Int, vertical: Int) = provisioner.lookAt(horizontal, vertical)

    fun setLight(mode: Int) = provisioner.setLight(mode)

    fun testSpeaker(variant: Int = 0) = provisioner.testSpeaker(variant)

    fun setVolume(volume: Int) = provisioner.setVolume(volume)

    fun announceGameResult(result: Int) = provisioner.announceGameResult(result)

    fun announceGameResultConfirmed(result: Int, onAccepted: (Boolean) -> Unit) =
        provisioner.announceGameResultConfirmed(result, onAccepted)

    fun finishFeeding(item: Int, onResult: (Boolean, Int) -> Unit) =
        provisioner.finishFeeding(item, onResult)

    fun pokeConfirmed(onAccepted: (Boolean) -> Unit) =
        provisioner.pokeConfirmed(onAccepted)

    fun putToSleep(onAccepted: (Boolean) -> Unit) {
        videoClient.disconnect()
        provisioner.stopMotion()
        provisioner.setRemoteMode(false)
        provisioner.sleepConfirmed { accepted ->
            viewModelScope.launch {
                if (accepted) {
                    val headId = cloudDeviceId.value
                    val pairedId = parentMemoryState.value.pairedDeviceId
                    if (parentMemoryClient.hasAccess && headId != null && headId == pairedId) {
                        try {
                            parentMemoryClient.recordInteraction("rested")
                            mutableInteractionSync.value = "休息已同步到成长状态"
                        } catch (error: Exception) {
                            mutableInteractionSync.value =
                                "已经睡着；成长状态稍后同步"
                        }
                    }
                    delay(350)
                    provisioner.refreshDiagnostics()
                }
                onAccepted(accepted)
            }
        }
    }

    fun startListening() = provisioner.startListening()

    fun stopListening() = provisioner.stopListening()

    fun restartDevice(onAccepted: (Boolean) -> Unit) {
        videoClient.disconnect()
        provisioner.stopMotion()
        provisioner.restartDevice(onAccepted)
    }

    fun setSetupHotspot(enabled: Boolean, onAccepted: (Boolean) -> Unit) =
        provisioner.setSetupHotspot(enabled, onAccepted)

    fun setRemoteMode(enabled: Boolean) = provisioner.setRemoteMode(enabled)

    fun fireCannon() = provisioner.fireCannon()

    fun syncPersonality(curiosity: Int, warmth: Int, talkativeness: Int) =
        viewModelScope.launch {
            parentMemoryClient.setPersonality(curiosity, warmth, talkativeness)
        }

    fun refreshParentMemory() = viewModelScope.launch { parentMemoryClient.refresh() }

    fun refreshHomeState() = viewModelScope.launch { parentMemoryClient.refreshOverview() }

    fun pairParent(code: String) = viewModelScope.launch { parentMemoryClient.pair(code) }

    fun sendTextMessage(raw: String) {
        val message = raw.trim()
        if (message.isEmpty() || message.length > 280 || mutableTextChat.value.sending) return
        val messageId = java.util.UUID.randomUUID().toString()
        mutableTextChat.value = mutableTextChat.value.copy(
            messages = mutableTextChat.value.messages + ChatMessage(messageId, true, message),
            sending = true, status = "正在交给机器人", error = null,
        )
        viewModelScope.launch {
            try {
                val paired = parentMemoryState.value.pairedDeviceId
                val connectedHead = cloudDeviceId.value
                if (paired == null) throw IllegalStateException("请先在成长记忆中配对机器人")
                if (connectedHead != null && connectedHead != paired) {
                    throw IllegalStateException("当前连接的机器人与云端配对设备不一致")
                }
                // A text turn needs the voice runtime, not the camera-only
                // remote runtime. Release video and request AI mode before
                // submitting; the cloud keeps the turn queued briefly while
                // the head completes that transition.
                videoClient.disconnect()
                if (scanState.value.phase in setOf(ScanPhase.Connected, ScanPhase.Provisioned)) {
                    provisioner.setRemoteMode(false)
                }
                var turn: TextTurnStatus = parentMemoryClient.submitTextTurn(message, messageId)
                var shownReply: String? = null
                repeat(240) {
                    if (turn.reply != null && turn.reply != shownReply) {
                        mutableTextChat.value = mutableTextChat.value.copy(
                            messages = mutableTextChat.value.messages +
                                ChatMessage("$messageId-reply", false, turn.reply!!),
                            status = "机器人正在播报",
                        )
                        shownReply = turn.reply
                    }
                    if (turn.status == "completed") {
                        mutableTextChat.value = mutableTextChat.value.copy(
                            sending = false, status = "已送达机器人")
                        return@launch
                    }
                    if (turn.status == "failed") {
                        throw IllegalStateException(when (turn.error) {
                            "device_offline" -> "机器人未在10秒内接收消息，请退出遥控/摄像头并检查网络"
                            "device_policy_blocked" -> "机器人当前不可用，消息未执行"
                            "device_playback_failed" -> "机器人未能完整播报或执行本次回复"
                            else -> "这次对话失败，请稍后重试"
                        })
                    }
                    mutableTextChat.value = mutableTextChat.value.copy(status = when (turn.status) {
                        "waiting_vision" -> "机器人正在看眼前"
                        "reply_ready" -> "机器人正在播报"
                        "processing" -> "机器人正在思考"
                        else -> "等待机器人接收"
                    })
                    delay(750)
                    turn = parentMemoryClient.readTextTurn(turn.id)
                }
                throw IllegalStateException("等待机器人回复超时，请检查网络")
            } catch (error: Exception) {
                mutableTextChat.value = mutableTextChat.value.copy(
                    sending = false, status = "", error = error.message ?: "发送失败")
            }
        }
    }

    fun submitDrawingGuess(strokes: List<SketchStroke>) {
        val state = mutableDrawingGuess.value
        if (state.sending || state.completed || strokes.isEmpty()) return
        mutableDrawingGuess.value = state.copy(
            sending = true, guess = null, status = "正在看你的画…", error = null)
        drawingGuessJob?.cancel()
        drawingGuessJob = viewModelScope.launch {
            try {
                val paired = parentMemoryState.value.pairedDeviceId
                    ?: throw IllegalStateException("请先在成长记忆中配对机器人")
                val connectedHead = cloudDeviceId.value
                if (connectedHead != null && connectedHead != paired) {
                    throw IllegalStateException("当前连接的机器人与云端配对设备不一致")
                }
                videoClient.disconnect()
                if (scanState.value.phase in setOf(ScanPhase.Connected, ScanPhase.Provisioned)) {
                    provisioner.setRemoteMode(false)
                }
                val jpeg = withContext(Dispatchers.Default) { renderSketch(strokes) }
                val requestId = java.util.UUID.randomUUID().toString()
                val queued: SketchGuessTurn = parentMemoryClient.submitSketchGuess(
                    jpeg, state.round, requestId)
                mutableDrawingGuess.value = mutableDrawingGuess.value.copy(
                    guess = queued.guess, status = "我猜是：${queued.guess}，正在告诉机器人")
                var turn = parentMemoryClient.readTextTurn(queued.id)
                repeat(80) {
                    if (turn.status == "completed") {
                        mutableDrawingGuess.value = mutableDrawingGuess.value.copy(
                            sending = false, status = "机器人已经说出答案")
                        return@launch
                    }
                    if (turn.status == "failed") {
                        throw IllegalStateException(when (turn.error) {
                            "device_offline" -> "机器人暂时不在线，先检查联网状态"
                            "device_policy_blocked" -> "机器人当前在休息，不能播报"
                            "device_playback_failed" -> "机器人没能完整说出答案"
                            else -> "这轮猜画没有完成，请重试"
                        })
                    }
                    delay(500)
                    turn = parentMemoryClient.readTextTurn(queued.id)
                }
                throw IllegalStateException("机器人播报超时，请检查网络")
            } catch (error: Exception) {
                mutableDrawingGuess.value = mutableDrawingGuess.value.copy(
                    sending = false, status = "", error = error.message ?: "猜画失败")
            }
        }
    }

    fun wrongDrawingGuess() {
        val state = mutableDrawingGuess.value
        if (state.sending || state.guess == null || state.completed) return
        mutableDrawingGuess.value = state.copy(
            sending = true, status = "宝宝坦克有点失落…", error = null)
        provisioner.announceGameResultConfirmed(1) { accepted ->
            mutableDrawingGuess.value = if (accepted) {
                DrawingGuessState(
                    round = (state.round + 1).coerceAtMost(3),
                    status = "再添几笔，我再猜一次",
                )
            } else {
                state.copy(
                    sending = false,
                    status = "",
                    error = "机器人没有收到反馈，请先连接设备",
                )
            }
        }
    }

    fun completeDrawingGuess() {
        mutableDrawingGuess.value = mutableDrawingGuess.value.copy(
            sending = false, completed = true, status = "猜对啦！")
    }

    fun resetDrawingGuess() {
        drawingGuessJob?.cancel()
        drawingGuessJob = null
        mutableDrawingGuess.value = DrawingGuessState()
    }

    private fun renderSketch(strokes: List<SketchStroke>): ByteArray {
        val size = 384
        val bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.RGB_565)
        val canvas = Canvas(bitmap)
        canvas.drawColor(Color.rgb(255, 253, 247))
        val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.rgb(23, 36, 45)
            style = Paint.Style.STROKE
            strokeWidth = 14f
            strokeCap = Paint.Cap.ROUND
            strokeJoin = Paint.Join.ROUND
        }
        var pointCount = 0
        strokes.forEach { stroke ->
            val points = stroke.points
            pointCount += points.size
            if (points.size == 1) {
                canvas.drawCircle(points[0].x * size, points[0].y * size, 7f,
                    paint.apply { style = Paint.Style.FILL })
                paint.style = Paint.Style.STROKE
            } else if (points.size > 1) {
                val path = Path().apply {
                    moveTo(points[0].x * size, points[0].y * size)
                    points.drop(1).forEach { lineTo(it.x * size, it.y * size) }
                }
                canvas.drawPath(path, paint)
            }
        }
        if (pointCount < 4) {
            bitmap.recycle()
            throw IllegalStateException("再多画几笔，我才能猜出来")
        }
        return ByteArrayOutputStream().use { output ->
            bitmap.compress(Bitmap.CompressFormat.JPEG, 86, output)
            bitmap.recycle()
            output.toByteArray().also {
                if (it.size > 96 * 1024) throw IllegalStateException("画面太复杂，请简化几笔")
            }
        }
    }

    fun addParentMemory(kind: String, subject: String, value: String) =
        viewModelScope.launch { parentMemoryClient.addMemory(kind, subject, value) }

    fun deleteParentMemory(id: String) = viewModelScope.launch {
        parentMemoryClient.deleteMemory(id)
    }

    fun updateParentMemory(id: String, revision: Int, value: String) =
        viewModelScope.launch { parentMemoryClient.updateMemory(id, revision, value) }

    fun clearParentMemories() = viewModelScope.launch {
        parentMemoryClient.clearMemories()
    }

    fun setParentPolicy(timeZone: String, deepNightLock: Boolean) =
        viewModelScope.launch { parentMemoryClient.setPolicy(timeZone, deepNightLock) }

    fun setQuestionHistory(enabled: Boolean) = viewModelScope.launch {
        parentMemoryClient.setQuestionHistory(enabled)
    }

    fun setQuestionRetention(days: Int) = viewModelScope.launch {
        parentMemoryClient.setQuestionHistory(parentMemoryState.value.historyEnabled, days)
    }

    fun exportParentData(uri: Uri) = viewModelScope.launch {
        parentMemoryClient.exportTo(uri)
    }

    fun deleteQuestionHistory(id: Int? = null) = viewModelScope.launch {
        parentMemoryClient.deleteQuestion(id)
    }

    fun loadMoreQuestions() = viewModelScope.launch { parentMemoryClient.loadMoreQuestions() }

    fun logoutParent() = viewModelScope.launch { parentMemoryClient.logout() }

    fun recordInteraction(kind: String) {
        if (scanState.value.phase !in setOf(ScanPhase.Connected, ScanPhase.Provisioned)) {
            mutableInteractionSync.value = "蓝牙未连接，成长数值未改变"
            return
        }
        val headId = cloudDeviceId.value
        val pairedId = parentMemoryState.value.pairedDeviceId
        if (headId == null) {
            mutableInteractionSync.value = "头部身份尚未确认，成长数值未改变"
            return
        }
        if (!parentMemoryClient.hasAccess || pairedId == null) {
            mutableInteractionSync.value = "请先在成长记忆中完成家长配对，才能同步数值"
            return
        }
        if (headId != pairedId) {
            mutableInteractionSync.value = "当前坦克与家长配对设备不一致，成长数值未改变"
            return
        }
        mutableInteractionSync.value = "正在同步成长数值…"
        viewModelScope.launch {
            try {
                parentMemoryClient.recordInteraction(kind)
                mutableInteractionSync.value = "成长数值已同步到云端"
            } catch (error: Exception) {
                mutableInteractionSync.value =
                    "成长数值未同步：${error.message ?: "网络不可用"}"
            }
        }
    }

    fun interactionNotAccepted() {
        mutableInteractionSync.value = "头部未接受动作，成长数值未改变"
    }

    fun drive(x: Float, y: Float) {
        // Keep the verified four cardinal mappings, while letting the remote
        // screen choose a bounded 40-100% track speed.
        val speed = (maxOf(abs(x), abs(y)) * 1000f).roundToInt()
            .coerceIn(0, 1000)
        val (left, right) = when {
            maxOf(abs(x), abs(y)) < .25f -> 0 to 0
            abs(y) >= abs(x) && y > 0 -> speed to speed
            abs(y) >= abs(x) -> -speed to -speed
            x > 0 -> speed to -speed
            else -> -speed to speed
        }
        provisioner.drive(left, right)
    }

    fun stopMotion() = provisioner.stopMotion()

    fun hasWifiPermission(): Boolean = videoClient.hasWifiPermission()

    fun connectCamera() {
        val selected = mutableSelectedDevice.value ?: return
        provisioner.requestCameraLanInfo { result ->
            result.fold(
                onSuccess = { lan ->
                    if (lan != null && videoClient.isLanOnCurrentSubnet(lan.address)) {
                        videoClient.connectLan(lan.address, lan.token)
                    } else {
                        lan?.token?.fill(0)
                        /* The phone may already be on the robot AP, or the
                         * household Wi-Fi may be down. Keep Wi-Fi selection
                         * manual; only bring up the robot's fallback AP. */
                        provisioner.enableCameraHotspot { hotspot ->
                            hotspot.fold(
                                onSuccess = {
                                    if (videoClient.isOnDeviceAp()) {
                                        videoClient.connectManualAp()
                                    } else {
                                        videoClient.showManualAp(selected.name)
                                    }
                                },
                                onFailure = {
                                    videoClient.fail(it.message ?: "设备摄像头热点未能启动")
                                },
                            )
                        }
                    }
                },
                onFailure = { videoClient.fail("无法读取摄像头地址：${it.message ?: "蓝牙未连接"}") },
            )
        }
    }

    fun disconnectCamera() = videoClient.disconnect()

    fun releaseTransientResources() {
        videoClient.disconnect()
        provisioner.stopMotion()
        provisioner.setRemoteMode(false)
    }

    fun openSetupPortal() {
        videoClient.disconnect()
        portalClient.open(mutableSelectedDevice.value?.name)
    }

    fun closeSetupPortal() = portalClient.close()

    override fun onCleared() {
        provisioner.close()
        videoClient.close()
        portalClient.close()
        wifiDiscovery.close()
        super.onCleared()
    }
}
