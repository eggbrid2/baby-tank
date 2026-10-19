package com.babytank.app.wifi

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.wifi.WifiManager
import java.net.Inet4Address
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

data class PortalState(val connected: Boolean = false, val detail: String = "")

/** Uses an already-connected robot Wi-Fi. This class never requests a network. */
class BabyTankPortalClient(context: Context) {
    private val connectivity = context.applicationContext
        .getSystemService(ConnectivityManager::class.java)
    private val mutableState = MutableStateFlow(PortalState())
    private val wifi = context.applicationContext.getSystemService(WifiManager::class.java)
    val state: StateFlow<PortalState> = mutableState.asStateFlow()
    private var robotNetwork: Network? = null

    @Suppress("DEPRECATION")
    fun open(deviceName: String?) {
        val currentSsid = wifi.connectionInfo.ssid.trim('"')
        if (!currentSsid.startsWith("BabyTank-") ||
            (deviceName != null && currentSsid != deviceName)) {
            mutableState.value = PortalState(
                detail = "请先在手机系统 Wi-Fi 中连接机器人 BabyTank- 热点，再返回打开配网页",
            )
            return
        }
        val network = connectivity.allNetworks.firstOrNull { candidate ->
            val capabilities = connectivity.getNetworkCapabilities(candidate)
            val links = connectivity.getLinkProperties(candidate)
            capabilities?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true &&
                links?.linkAddresses?.any { address ->
                    val ip = address.address
                    ip is Inet4Address && (ip.address[0].toInt() and 0xff) == 192 &&
                        (ip.address[1].toInt() and 0xff) == 168 &&
                        (ip.address[2].toInt() and 0xff) == 4
                } == true
        }
        if (network == null) {
            mutableState.value = PortalState(detail = "已找到设备 Wi-Fi，但尚未取得 192.168.4.x 地址")
            return
        }
        if (!connectivity.bindProcessToNetwork(network)) {
            mutableState.value = PortalState(detail = "无法将页面路由到设备热点")
            return
        }
        robotNetwork = network
        mutableState.value = PortalState(true, "已连接设备热点")
    }

    fun close() {
        if (robotNetwork != null) connectivity.bindProcessToNetwork(null)
        robotNetwork = null
        mutableState.value = PortalState()
    }
}
