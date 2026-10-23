package com.babytank.app.model

import android.graphics.Bitmap
import java.util.UUID

object BabyTankBleContract {
    const val deviceNamePrefix = "BabyTank-"
    val provisioningService: UUID = UUID.fromString("0000ffff-0000-1000-8000-00805f9b34fb")
}

data class NearbyDevice(
    val address: String,
    val name: String,
    val rssi: Int,
)

enum class ScanPhase {
    Idle,
    Scanning,
    Connecting,
    Connected,
    Provisioning,
    Provisioned,
    BluetoothOff,
    MissingPermission,
    Unsupported,
    Failed,
}

data class BleScanState(
    val phase: ScanPhase = ScanPhase.Idle,
    val devices: List<NearbyDevice> = emptyList(),
    val detail: String? = null,
)

enum class ControlLinkPhase { Idle, Testing, Available, Unavailable, Failed }

data class ControlLinkState(
    val phase: ControlLinkPhase = ControlLinkPhase.Idle,
    val detail: String = "尚未测试 BLE 控制通道",
)

enum class DiagnosticPhase { Idle, Running, Ready, Failed }

data class DiagnosticSnapshot(
    val setupHotspotEnabled: Boolean = false,
    val wifiConnected: Boolean = false,
    val cloudReady: Boolean = false,
    val softApReady: Boolean = false,
    val cameraReady: Boolean = false,
    val chassisReady: Boolean = false,
    val chassisVerified: Boolean = false,
    val audioPlaying: Boolean = false,
    val touchReady: Boolean = false,
    val expression: Int = 0,
    val intensity: Int = 0,
    val speechLevel: Int = 0,
    val speakerVolume: Int = 75,
    val conversationPhase: Int = 0,
    val remoteControlMode: Boolean = false,
    val touchActiveMask: Int = 0,
    val internetPhase: Int = 0,
    val uptimeSeconds: Long = 0,
    val freeInternalHeap: Long = 0,
    val touchCounts: List<Int> = listOf(0, 0, 0),
    val touchDeltas: List<Int> = listOf(0, 0, 0),
    val touchThresholds: List<Int> = listOf(0, 0, 0),
)

data class DiagnosticState(
    val phase: DiagnosticPhase = DiagnosticPhase.Idle,
    val detail: String = "尚未读取设备状态",
    val lastCommand: String? = null,
    val snapshot: DiagnosticSnapshot? = null,
)

data class BabyExpression(val index: Int, val title: String)

val BabyExpressions = listOf(
    "平静", "开心", "大笑", "兴奋", "喜欢", "好奇", "惊讶", "害羞",
    "得意", "无聊", "不耐烦", "抱怨", "生气", "暴怒", "难过", "哭泣",
    "担心", "困惑", "晕乎", "不舒服", "思考", "困倦", "睡觉", "吐泡泡",
    "开炮爆炸", "倾听", "思考中", "吃东西", "喝东西", "石头", "布",
    "剪刀", "打架", "骰子1", "骰子2", "骰子3", "骰子4", "骰子5", "骰子6",
    "草莓", "苹果", "香蕉", "蛋糕", "饼干", "面包", "饭团", "冰淇淋",
    "牛奶", "果汁", "水", "酸奶", "豆浆", "热可可",
).mapIndexed(::BabyExpression)

enum class VideoPhase { Idle, Connecting, Streaming, Reconnecting, NeedsManualWifi, Failed }

data class VideoState(
    val phase: VideoPhase = VideoPhase.Idle,
    val detail: String = "尚未连接摄像头",
    val frame: Bitmap? = null,
    val framesPerSecond: Int = 0,
)

data class WifiDiscoveryState(
    val currentSsid: String? = null,
    val networks: List<String> = emptyList(),
    val scanning: Boolean = false,
    val error: String? = null,
)

enum class HardwareGroup(val title: String) {
    Interaction("交互"),
    Audio("语音与音频"),
    Network("网络与 AI"),
    Vision("视觉"),
    Motion("底盘与传感器"),
    System("系统"),
}

enum class CapabilityReadiness {
    AppReady,
    NeedsDevice,
    NeedsDiagnostics,
    NeedsFirmwareProtocol,
    NeedsChassis,
}

data class HardwareCapability(
    val id: String,
    val title: String,
    val description: String,
    val group: HardwareGroup,
    val readiness: CapabilityReadiness,
    val destructive: Boolean = false,
)

object HardwareCatalog {
    val all = listOf(
        HardwareCapability("ble", "蓝牙发现", "扫描 Baby Tank 并读取信号强度", HardwareGroup.Network, CapabilityReadiness.AppReady),
        HardwareCapability("ble_control", "蓝牙控制通道", "加密 Ping、序号回执与能力握手", HardwareGroup.Network, CapabilityReadiness.AppReady),
        HardwareCapability("wifi", "Wi-Fi 配网", "安全写入家庭网络并验证联网", HardwareGroup.Network, CapabilityReadiness.AppReady),
        HardwareCapability("display", "屏幕与表情", "逐项播放表情、动画和亮度测试", HardwareGroup.Interaction, CapabilityReadiness.NeedsDiagnostics),
        HardwareCapability("touch", "顶部触摸", "查看原始值、阈值和触发次数", HardwareGroup.Interaction, CapabilityReadiness.NeedsDiagnostics),
        HardwareCapability("microphone", "麦克风", "实时电平、噪声底和短录音回放", HardwareGroup.Audio, CapabilityReadiness.NeedsDiagnostics),
        HardwareCapability("speaker", "扬声器", "音量阶梯、左右声道和 PCM 测试", HardwareGroup.Audio, CapabilityReadiness.NeedsDiagnostics),
        HardwareCapability("wakenet", "本地唤醒", "统计“喵喵同学”检测与置信结果", HardwareGroup.Audio, CapabilityReadiness.NeedsDiagnostics),
        HardwareCapability("dialogue", "AI 对话闭环", "ASR、模型、TTS 与首字延迟", HardwareGroup.Network, CapabilityReadiness.NeedsDiagnostics),
        HardwareCapability("camera", "摄像头", "连接设备热点并预览本地 MJPEG", HardwareGroup.Vision, CapabilityReadiness.AppReady),
        HardwareCapability("motor", "履带运动", "前后左右、速度限制和失联停车", HardwareGroup.Motion, CapabilityReadiness.NeedsChassis, destructive = true),
        HardwareCapability("emergency_stop", "急停", "验证 App、头部和底盘三层停车", HardwareGroup.Motion, CapabilityReadiness.NeedsChassis, destructive = true),
        HardwareCapability("lights", "底盘灯光", "前后灯与状态反馈", HardwareGroup.Motion, CapabilityReadiness.NeedsChassis),
        HardwareCapability("imu", "IMU", "姿态、静止噪声和方向校验", HardwareGroup.Motion, CapabilityReadiness.NeedsFirmwareProtocol),
        HardwareCapability("battery", "电池与供电", "电压、电量、充电和欠压复位", HardwareGroup.System, CapabilityReadiness.NeedsFirmwareProtocol),
        HardwareCapability("ota", "OTA", "版本检查、A/B 切换和失败回滚", HardwareGroup.System, CapabilityReadiness.NeedsFirmwareProtocol, destructive = true),
    )
}
