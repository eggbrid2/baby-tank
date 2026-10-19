package com.babytank.app.wifi

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.BitmapFactory
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiNetworkSpecifier
import android.os.Build
import com.babytank.app.model.VideoPhase
import com.babytank.app.model.VideoState
import java.io.DataInputStream
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.Inet4Address
import java.net.InetAddress
import java.net.URL
import java.security.MessageDigest
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

class BabyTankVideoClient(context: Context) {
    private val appContext = context.applicationContext
    private val connectivity = appContext.getSystemService(ConnectivityManager::class.java)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val mutableState = MutableStateFlow(VideoState())
    val state: StateFlow<VideoState> = mutableState.asStateFlow()
    private var callback: ConnectivityManager.NetworkCallback? = null
    private var streamJob: Job? = null
    @Volatile private var connection: HttpURLConnection? = null

    fun hasWifiPermission(): Boolean = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU ->
            appContext.checkSelfPermission(Manifest.permission.NEARBY_WIFI_DEVICES) ==
                PackageManager.PERMISSION_GRANTED
        else -> appContext.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) ==
            PackageManager.PERMISSION_GRANTED
    }

    @SuppressLint("MissingPermission")
    fun connect(deviceName: String, setupCode: CharArray) {
        if (deviceName.isBlank() || setupCode.size !in 8..64) {
            setupCode.fill('\u0000')
            mutableState.value = VideoState(
                VideoPhase.Failed,
                "需要先选择设备，并输入 8–64 位设备配对码",
            )
            return
        }
        if (!hasWifiPermission()) {
            setupCode.fill('\u0000')
            mutableState.value = VideoState(VideoPhase.Failed, "需要附近 Wi-Fi 权限")
            return
        }
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            setupCode.fill('\u0000')
            mutableState.value = VideoState(
                VideoPhase.NeedsManualWifi,
                "Android 9 及以下请先在系统 Wi-Fi 中连接 $deviceName，再返回测试",
            )
            connectivity.activeNetwork?.let { startStreaming(it, STREAM_URL, null) }
            return
        }

        disconnect(resetState = false)
        val password = deriveSoftApPassword(setupCode)
        setupCode.fill('\u0000')
        val request = try {
            val specifier = WifiNetworkSpecifier.Builder()
                .setSsid(deviceName)
                .setWpa2Passphrase(password)
                .build()
            NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .setNetworkSpecifier(specifier)
                .build()
        } catch (error: RuntimeException) {
            mutableState.value = VideoState(
                VideoPhase.Failed,
                "设备热点参数无效：${error.message ?: "未知错误"}",
            )
            return
        }
        mutableState.value = VideoState(
            VideoPhase.Connecting,
            "请选择并连接 $deviceName；该网络没有互联网是正常现象",
        )
        val networkCallback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) = startStreaming(network, STREAM_URL, null)

            override fun onLost(network: Network) {
                streamJob?.cancel()
                connection?.disconnect()
                mutableState.value = VideoState(VideoPhase.Failed, "摄像头热点已断开")
            }

            override fun onUnavailable() {
                mutableState.value = VideoState(
                    VideoPhase.Failed,
                    "没有连上设备热点，请确认设备在线且配对码正确",
                )
            }
        }
        callback = networkCallback
        try {
            connectivity.requestNetwork(request, networkCallback, 20_000)
        } catch (error: RuntimeException) {
            callback = null
            mutableState.value = VideoState(
                VideoPhase.Failed,
                "无法请求设备热点：${error.message ?: "系统拒绝连接"}",
            )
        }
    }

    fun connectLan(address: String, token: ByteArray) {
        disconnect(resetState = false)
        val network = connectivity.activeNetwork
        val capabilities = network?.let { connectivity.getNetworkCapabilities(it) }
        if (network == null || capabilities?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) != true) {
            token.fill(0)
            fail("手机未连接家庭 Wi-Fi，请与机器人连接同一网络")
            return
        }
        if (!address.matches(Regex("(?:[0-9]{1,3}\\.){3}[0-9]{1,3}")) ||
            InetAddress.getByName(address).isSiteLocalAddress != true) {
            token.fill(0)
            fail("设备返回了无效的局域网地址")
            return
        }
        val key = token.joinToString("") { "%02x".format(it.toInt() and 0xff) }
        token.fill(0)
        mutableState.value = VideoState(VideoPhase.Connecting, "正在通过家庭 Wi-Fi 连接摄像头…")
        startStreaming(network, "http://$address:8081/stream", key)
    }

    fun isOnDeviceAp(): Boolean = findDeviceApNetwork() != null

    fun connectManualAp() {
        val network = findDeviceApNetwork()
        if (network == null) {
            showManualAp()
            return
        }
        disconnect(resetState = false)
        mutableState.value = VideoState(VideoPhase.Connecting,
            "正在读取设备热点摄像头…")
        startStreaming(network, STREAM_URL, null)
    }

    fun showManualAp() {
        mutableState.value = VideoState(VideoPhase.NeedsManualWifi,
            "手机与机器人不在同一网段。请在系统 WLAN 手动连接 BabyTank-694968，允许无互联网，再返回点击连接摄像头。")
    }

    private fun findDeviceApNetwork(): Network? = connectivity.allNetworks.firstOrNull { network ->
        val capabilities = connectivity.getNetworkCapabilities(network)
        capabilities?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true &&
            connectivity.getLinkProperties(network)?.linkAddresses?.any {
                val bytes = (it.address as? Inet4Address)?.address
                bytes != null && bytes.size == 4 &&
                    (bytes[0].toInt() and 0xff) == 192 &&
                    (bytes[1].toInt() and 0xff) == 168 &&
                    (bytes[2].toInt() and 0xff) == 4
            } == true
    }

    fun isLanOnCurrentSubnet(address: String): Boolean {
        val target = runCatching { InetAddress.getByName(address) as Inet4Address }
            .getOrNull()?.address ?: return false
        val network = connectivity.activeNetwork ?: return false
        if (connectivity.getNetworkCapabilities(network)
                ?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) != true) return false
        return connectivity.getLinkProperties(network)?.linkAddresses?.any { local ->
            val bytes = (local.address as? Inet4Address)?.address ?: return@any false
            val prefix = local.prefixLength.coerceIn(0, 32)
            val full = prefix / 8
            val remainder = prefix % 8
            (0 until full).all { bytes[it] == target[it] } &&
                (remainder == 0 ||
                    ((bytes[full].toInt() xor target[full].toInt()) and
                        (0xff shl (8 - remainder))) == 0)
        } == true
    }

    fun fail(detail: String) {
        mutableState.value = VideoState(VideoPhase.Failed, detail)
    }

    private fun deriveSoftApPassword(setupCode: CharArray): String {
        val prefix = "baby-tank-softap-v1:".toByteArray(Charsets.UTF_8)
        val secret = setupCode.concatToString().toByteArray(Charsets.UTF_8)
        val digest = MessageDigest.getInstance("SHA-256").run {
            update(prefix)
            digest(secret)
        }
        secret.fill(0)
        return buildString(16) {
            repeat(8) { append("%02x".format(digest[it].toInt() and 0xff)) }
        }.also { digest.fill(0) }
    }

    private fun startStreaming(network: Network, url: String, token: String?) {
        streamJob?.cancel()
        connection?.disconnect()
        streamJob = scope.launch {
            var attempt = 0
            while (currentCoroutineContext().isActive) {
                try {
                    streamOnce(network, url, token)
                    error("视频流已结束")
                } catch (error: Exception) {
                    if (!currentCoroutineContext().isActive) break
                    attempt += 1
                    mutableState.value = mutableState.value.copy(
                        phase = if (attempt <= 3) VideoPhase.Reconnecting else VideoPhase.Failed,
                        detail = if (attempt <= 3) "视频中断，正在重连…" else
                            "摄像头没有响应：${error.message ?: "未知错误"}",
                        framesPerSecond = 0,
                    )
                    if (attempt > 3) break
                    delay(1_000)
                }
            }
        }
    }

    private suspend fun streamOnce(network: Network, url: String, token: String?) {
        val http = network.openConnection(URL(url)) as HttpURLConnection
        connection = http
        http.connectTimeout = 5_000
        http.readTimeout = 10_000
        http.useCaches = false
        http.instanceFollowRedirects = false
        http.setRequestProperty("Accept", "multipart/x-mixed-replace")
        if (token != null) http.setRequestProperty("X-BabyTank-Camera-Token", token)
        http.connect()
        check(http.responseCode == HttpURLConnection.HTTP_OK) {
            "HTTP ${http.responseCode}"
        }
        var frameCount = 0
        var windowStarted = System.currentTimeMillis()
        http.inputStream.buffered(32 * 1024).use { input ->
            while (currentCoroutineContext().isActive) {
                val jpeg = readJpeg(input) ?: return
                val bitmap = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size) ?: continue
                frameCount += 1
                val now = System.currentTimeMillis()
                val elapsed = now - windowStarted
                val fps = if (elapsed >= 1_000) {
                    val measured = ((frameCount * 1_000L) / elapsed).toInt()
                    frameCount = 0
                    windowStarted = now
                    measured
                } else mutableState.value.framesPerSecond
                mutableState.value = VideoState(
                    VideoPhase.Streaming,
                    "本地视频已连接",
                    bitmap,
                    fps,
                )
            }
        }
    }

    private fun readJpeg(input: InputStream): ByteArray? {
        var length = -1
        var headerBytes = 0
        while (true) {
            val line = readHeaderLine(input) ?: return null
            headerBytes += line.length + 2
            check(headerBytes <= 1024) { "视频帧头过长" }
            if (line.isEmpty()) {
                if (length < 0) continue
                break
            }
            if (line.startsWith("Content-Length:", ignoreCase = true)) {
                length = line.substringAfter(':').trim().toIntOrNull() ?: -1
            }
        }
        check(length in 4..MAX_FRAME_BYTES) { "JPEG 帧大小无效：$length" }
        val jpeg = ByteArray(length)
        DataInputStream(input).readFully(jpeg)
        check(jpeg[0] == 0xff.toByte() && jpeg[1] == 0xd8.toByte() &&
            jpeg[length - 2] == 0xff.toByte() && jpeg[length - 1] == 0xd9.toByte()) {
            "JPEG 帧边界损坏"
        }
        return jpeg
    }

    private fun readHeaderLine(input: InputStream): String? {
        val line = StringBuilder()
        while (line.length < 256) {
            val byte = input.read()
            if (byte < 0) return null
            if (byte == 10) return line.toString().trimEnd('\r')
            line.append(byte.toChar())
        }
        error("视频帧头行过长")
    }

    fun disconnect(resetState: Boolean = true) {
        streamJob?.cancel()
        streamJob = null
        connection?.disconnect()
        connection = null
        callback?.let {
            runCatching { connectivity.unregisterNetworkCallback(it) }
        }
        callback = null
        if (resetState) mutableState.value = VideoState()
    }

    fun close() {
        disconnect()
        scope.cancel()
    }

    private companion object {
        const val STREAM_URL = "http://192.168.4.1/stream"
        const val MAX_FRAME_BYTES = 256 * 1024
    }
}
