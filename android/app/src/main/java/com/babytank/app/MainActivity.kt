package com.babytank.app

import android.app.Application
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.babytank.app.ble.BabyTankProvisioner
import com.babytank.app.model.NearbyDevice
import com.babytank.app.ui.BabyTankApp
import com.babytank.app.ui.BabyTankTheme
import com.babytank.app.wifi.BabyTankVideoClient
import com.babytank.app.wifi.BabyTankPortalClient
import com.babytank.app.wifi.WifiNetworkDiscovery
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch
import kotlin.math.abs
import kotlin.math.roundToInt

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            BabyTankTheme {
                BabyTankApp()
            }
        }
    }
}

class BabyTankViewModel(application: Application) : AndroidViewModel(application) {
    private val provisioner = BabyTankProvisioner(application)
    private val videoClient = BabyTankVideoClient(application)
    private val portalClient = BabyTankPortalClient(application)
    private val wifiDiscovery = WifiNetworkDiscovery(application)
    val scanState = provisioner.state
    val controlState = provisioner.controlState
    val personalityStatus = provisioner.personalityStatus
    val diagnosticState = provisioner.diagnosticState
    val videoState = videoClient.state
    val portalState = portalClient.state
    val wifiState = wifiDiscovery.state

    private val rememberedDevice = provisioner.rememberedDevice()
    private val mutableSelectedDevice = MutableStateFlow(rememberedDevice)
    val selectedDevice: StateFlow<NearbyDevice?> = mutableSelectedDevice.asStateFlow()
    private var autoReconnectAddress: String? = rememberedDevice?.address

    init {
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

    fun hasSetupPermission(): Boolean =
        provisioner.hasRuntimePermission() && wifiDiscovery.hasPermission()

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

    fun lookAt(horizontal: Int, vertical: Int) = provisioner.lookAt(horizontal, vertical)

    fun setLight(mode: Int) = provisioner.setLight(mode)

    fun testSpeaker(variant: Int = 0) = provisioner.testSpeaker(variant)

    fun setVolume(volume: Int) = provisioner.setVolume(volume)

    fun announceGameResult(result: Int) = provisioner.announceGameResult(result)

    fun startListening() = provisioner.startListening()

    fun setRemoteMode(enabled: Boolean) = provisioner.setRemoteMode(enabled)

    fun fireCannon() = provisioner.fireCannon()

    fun syncPersonality(curiosity: Int, warmth: Int, talkativeness: Int) =
        provisioner.syncPersonality(curiosity, warmth, talkativeness)

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
        if (mutableSelectedDevice.value == null) return
        provisioner.requestCameraLanInfo { result ->
            result.fold(
                onSuccess = { lan ->
                    if (lan != null && videoClient.isLanOnCurrentSubnet(lan.address)) {
                        videoClient.connectLan(lan.address, lan.token)
                    } else {
                        lan?.token?.fill(0)
                        videoClient.fail("手机和宝宝坦克需要连接同一个局域网")
                    }
                },
                onFailure = { videoClient.fail("无法读取摄像头地址：${it.message ?: "蓝牙未连接"}") },
            )
        }
    }

    fun disconnectCamera() = videoClient.disconnect()

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
