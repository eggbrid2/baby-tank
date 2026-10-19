package com.babytank.app.wifi

import android.Manifest
import android.annotation.SuppressLint
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.wifi.WifiInfo
import android.net.wifi.WifiManager
import android.os.Build
import com.babytank.app.model.WifiDiscoveryState
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

class WifiNetworkDiscovery(context: Context) {
    private val appContext = context.applicationContext
    private val wifi = appContext.getSystemService(WifiManager::class.java)
    private val connectivity = appContext.getSystemService(ConnectivityManager::class.java)
    private val mutableState = MutableStateFlow(WifiDiscoveryState())
    val state: StateFlow<WifiDiscoveryState> = mutableState.asStateFlow()

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            publishResults()
        }
    }

    init {
        val filter = IntentFilter(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            appContext.registerReceiver(
                receiver,
                filter,
                Context.RECEIVER_NOT_EXPORTED
            )
        } else {
            @Suppress("DEPRECATION")
            appContext.registerReceiver(receiver, filter)
        }
        refreshCurrent()
    }

    fun hasPermission(): Boolean {
        val location = appContext.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) ==
            PackageManager.PERMISSION_GRANTED
        val nearby = Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU ||
            appContext.checkSelfPermission(Manifest.permission.NEARBY_WIFI_DEVICES) ==
            PackageManager.PERMISSION_GRANTED
        return location && nearby
    }

    @SuppressLint("MissingPermission", "Deprecated")
    fun refreshCurrent() {
        if (!hasPermission()) return
        val info = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val network = connectivity.activeNetwork
            connectivity.getNetworkCapabilities(network)?.transportInfo as? WifiInfo
        } else {
            wifi.connectionInfo
        }
        mutableState.value = mutableState.value.copy(currentSsid = cleanSsid(info?.ssid))
    }

    @SuppressLint("MissingPermission")
    fun scan() {
        if (!hasPermission()) {
            mutableState.value = mutableState.value.copy(error = "缺少 Wi-Fi 权限")
            return
        }
        refreshCurrent()
        mutableState.value = mutableState.value.copy(scanning = true, error = null)
        if (!wifi.startScan()) publishResults()
    }

    @SuppressLint("MissingPermission")
    private fun publishResults() {
        val names = wifi.scanResults
            .asSequence()
            .sortedByDescending { it.level }
            .mapNotNull { cleanSsid(it.SSID) }
            .distinct()
            .take(12)
            .toList()
        mutableState.value = mutableState.value.copy(networks = names, scanning = false)
        refreshCurrent()
    }

    private fun cleanSsid(value: String?): String? = value
        ?.removeSurrounding("\"")
        ?.takeUnless { it.isBlank() || it == "<unknown ssid>" }

    fun close() {
        runCatching { appContext.unregisterReceiver(receiver) }
    }
}
