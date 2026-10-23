package com.babytank.app.ui

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.ActivityInfo
import android.graphics.Bitmap
import android.media.AudioManager
import android.media.ToneGenerator
import android.os.Build
import android.provider.Settings
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.babytank.app.BabyTankViewModel
import com.babytank.app.cloud.ParentMemoryState
import com.babytank.app.R
import com.babytank.app.model.CapabilityReadiness
import com.babytank.app.model.BabyExpressions
import com.babytank.app.model.ControlLinkPhase
import com.babytank.app.model.ControlLinkState
import com.babytank.app.model.DiagnosticPhase
import com.babytank.app.model.DiagnosticSnapshot
import com.babytank.app.model.DiagnosticState
import com.babytank.app.model.HardwareCapability
import com.babytank.app.model.HardwareCatalog
import com.babytank.app.model.HardwareGroup
import com.babytank.app.model.NearbyDevice
import com.babytank.app.model.ScanPhase
import com.babytank.app.model.VideoPhase
import com.babytank.app.model.VideoState
import com.babytank.app.wifi.PortalState
import kotlin.math.sqrt
import kotlin.math.roundToInt
import java.security.SecureRandom
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.flow.StateFlow

@Composable
fun BabyTankApp(viewModel: BabyTankViewModel = viewModel()) {
    var screen by rememberSaveable { mutableStateOf(AppScreen.Home) }
    val exportLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/json")) { uri ->
        if (uri != null) viewModel.exportParentData(uri)
    }
    val openParentMemory = {
        screen = AppScreen.Memory
    }
    val scanState by viewModel.scanState.collectAsStateWithLifecycle()
    val selectedDevice by viewModel.selectedDevice.collectAsStateWithLifecycle()
    val controlState by viewModel.controlState.collectAsStateWithLifecycle()
    val diagnosticState by viewModel.diagnosticState.collectAsStateWithLifecycle()
    val videoState by viewModel.videoState.collectAsStateWithLifecycle()
    val videoFrame = viewModel.videoFrame
    val portalState by viewModel.portalState.collectAsStateWithLifecycle()
    val parentMemoryState by viewModel.parentMemoryState.collectAsStateWithLifecycle()
    val cloudDeviceId by viewModel.cloudDeviceId.collectAsStateWithLifecycle()
    val interactionSync by viewModel.interactionSync.collectAsStateWithLifecycle()
    val textChat by viewModel.textChat.collectAsStateWithLifecycle()
    val drawingGuess by viewModel.drawingGuess.collectAsStateWithLifecycle()
    BackHandler(enabled = screen != AppScreen.Home) {
        if (screen == AppScreen.Control) {
            viewModel.stopMotion()
            viewModel.disconnectCamera()
            viewModel.setRemoteMode(false)
        }
        screen = parentScreen(screen)
    }
    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_STOP) {
                viewModel.stopMotion()
                viewModel.disconnectCamera()
                viewModel.setRemoteMode(false)
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }
    LaunchedEffect(screen, scanState.phase) {
        if (screen != AppScreen.Control && scanState.phase in
            setOf(ScanPhase.Connected, ScanPhase.Provisioned)
        ) {
            viewModel.disconnectCamera()
            viewModel.setRemoteMode(false)
        }
    }
    LaunchedEffect(screen, scanState.phase) {
        if (screen == AppScreen.Home && scanState.phase in
            setOf(ScanPhase.Connected, ScanPhase.Provisioned)
        ) {
            while (isActive) {
                viewModel.refreshDiagnostics()
                delay(10_000)
            }
        }
    }
    LaunchedEffect(screen, scanState.phase) {
        if (screen == AppScreen.Settings && scanState.phase in
            setOf(ScanPhase.Connected, ScanPhase.Provisioned)
        ) {
            viewModel.refreshDiagnostics()
        }
    }
    LaunchedEffect(screen) {
        if (screen == AppScreen.Home) {
            while (isActive) {
                viewModel.refreshHomeState()
                delay(60_000)
            }
        }
    }

    if (screen == AppScreen.Control) {
        ControlScreen(
            selectedDevice,
            scanState.phase,
            controlState,
            videoState,
            videoFrame,
            { viewModel.startScan(); screen = AppScreen.Pair },
            viewModel::hasWifiPermission,
            viewModel::connectCamera,
            viewModel::disconnectCamera,
            viewModel::drive,
            viewModel::stopMotion,
            viewModel::testControlChannel,
            viewModel::setLight,
            viewModel::testSpeaker,
            viewModel::fireCannon,
            viewModel::setRemoteMode,
            onClose = {
                viewModel.stopMotion()
                viewModel.setRemoteMode(false)
                screen = AppScreen.Home
            },
        )
        return
    }

    Scaffold(
        containerColor = CanvasWarm,
    ) { padding ->
        AnimatedContent(
            targetState = screen,
            label = "screen",
            modifier = Modifier.padding(padding),
        ) { target ->
            when (target) {
                AppScreen.Home -> HomeScreen(
                    selectedDevice,
                    scanState.phase,
                    diagnosticState.snapshot,
                    parentMemoryState,
                    cloudDeviceId,
                    onDrive = { screen = AppScreen.Control },
                    onTalk = viewModel::startListening,
                    onStopTalk = viewModel::stopListening,
                    onTextChat = { screen = AppScreen.Chat },
                    onPlay = { screen = AppScreen.Interaction },
                    onConnect = { screen = AppScreen.Pair },
                    onMemory = openParentMemory,
                    onSleep = viewModel::putToSleep,
                    onSettings = { screen = AppScreen.Settings },
                )
                AppScreen.Settings -> SettingsScreen(
                    device = selectedDevice,
                    phase = scanState.phase,
                    diagnostics = diagnosticState,
                    onPair = { screen = AppScreen.Pair },
                    onNetwork = { screen = AppScreen.Network },
                    onCharacter = { screen = AppScreen.Character },
                    onMemory = openParentMemory,
                    onLab = { screen = AppScreen.Lab },
                    onVolume = viewModel::setVolume,
                    onSetupHotspot = viewModel::setSetupHotspot,
                    onRestart = viewModel::restartDevice,
                    onBack = { screen = AppScreen.Home },
                )
                AppScreen.Interaction -> InteractionScreen(
                    connected = selectedDevice != null && scanState.phase in
                        setOf(ScanPhase.Connected, ScanPhase.Provisioned),
                    onExpression = viewModel::showExpression,
                    onExpressionConfirmed = viewModel::showExpressionConfirmed,
                    onGameResultConfirmed = viewModel::announceGameResultConfirmed,
                    onFeedFinished = viewModel::finishFeeding,
                    onPokeConfirmed = viewModel::pokeConfirmed,
                    onNeedEvent = viewModel::recordInteraction,
                    onNotAccepted = viewModel::interactionNotAccepted,
                    syncStatus = interactionSync,
                    onGuessDrawing = {
                        viewModel.resetDrawingGuess()
                        screen = AppScreen.GuessDrawing
                    },
                    onBack = { screen = AppScreen.Home },
                )
                AppScreen.Pair -> PairScreen(
                    phase = scanState.phase,
                    devices = scanState.devices,
                    detail = scanState.detail,
                    selectedDevice = selectedDevice,
                    hasPermission = viewModel::hasBlePermission,
                    onScan = viewModel::startScan,
                    onDevice = viewModel::selectDevice,
                    onConnect = viewModel::connectDevice,
                    onBack = { screen = AppScreen.Settings },
                )
                AppScreen.Network -> NetworkSetupScreen(
                    portal = portalState,
                    hasPermission = viewModel::hasWifiSetupPermission,
                    onOpenPortal = viewModel::openSetupPortal,
                    onClosePortal = viewModel::closeSetupPortal,
                    onBack = { screen = AppScreen.Settings },
                )
                AppScreen.Control -> ControlScreen(
                    selectedDevice,
                    scanState.phase,
                    controlState,
                    videoState,
                    videoFrame,
                    { viewModel.startScan(); screen = AppScreen.Pair },
                    viewModel::hasWifiPermission,
                    viewModel::connectCamera,
                    viewModel::disconnectCamera,
                    viewModel::drive,
                    viewModel::stopMotion,
                    viewModel::testControlChannel,
                    viewModel::setLight,
                    viewModel::testSpeaker,
                    viewModel::fireCannon,
                    viewModel::setRemoteMode,
                    onClose = {
                        viewModel.stopMotion()
                        viewModel.setRemoteMode(false)
                        screen = AppScreen.Home
                    },
                )
                AppScreen.Lab -> HardwareLabScreen(
                    selectedDevice,
                    controlState.detail,
                    diagnosticState,
                    onBleTest = { screen = AppScreen.Pair },
                    onControlTest = viewModel::testControlChannel,
                    onCameraTest = { screen = AppScreen.Control },
                    onRefresh = viewModel::refreshDiagnostics,
                    onExpression = viewModel::showExpression,
                    onLook = viewModel::lookAt,
                    onLight = viewModel::setLight,
                    onSpeaker = viewModel::testSpeaker,
                    onListen = viewModel::startListening,
                    onStopListen = viewModel::stopListening,
                    onBack = { screen = AppScreen.Settings },
                )
                AppScreen.Character -> CharacterScreen(
                    selectedDevice,
                    parentMemoryState,
                    viewModel::syncPersonality,
                    onBack = { screen = AppScreen.Settings },
                )
                AppScreen.Memory -> ParentMemoryScreen(
                    state = parentMemoryState,
                    onPair = viewModel::pairParent,
                    onRefresh = viewModel::refreshParentMemory,
                    onAddMemory = viewModel::addParentMemory,
                    onDeleteMemory = viewModel::deleteParentMemory,
                    onUpdateMemory = viewModel::updateParentMemory,
                    onClearMemories = viewModel::clearParentMemories,
                    onHistory = viewModel::setQuestionHistory,
                    onRetention = viewModel::setQuestionRetention,
                    onPolicy = viewModel::setParentPolicy,
                    onExport = { exportLauncher.launch("baby-tank-memory.json") },
                    onDeleteQuestion = viewModel::deleteQuestionHistory,
                    onLoadMoreQuestions = viewModel::loadMoreQuestions,
                    onCharacter = { screen = AppScreen.Character },
                    onLogout = viewModel::logoutParent,
                    onBack = { screen = AppScreen.Settings },
                )
                AppScreen.Chat -> TextChatScreen(
                    state = textChat,
                    paired = parentMemoryState.pairedDeviceId != null,
                    onSend = viewModel::sendTextMessage,
                    onPair = { screen = AppScreen.Memory },
                    onBack = { screen = AppScreen.Home },
                )
                AppScreen.GuessDrawing -> GuessDrawingScreen(
                    state = drawingGuess,
                    paired = parentMemoryState.pairedDeviceId != null,
                    onGuess = viewModel::submitDrawingGuess,
                    onCorrect = viewModel::completeDrawingGuess,
                    onWrong = viewModel::wrongDrawingGuess,
                    onReset = viewModel::resetDrawingGuess,
                    onPair = { screen = AppScreen.Memory },
                    onBack = {
                        viewModel.resetDrawingGuess()
                        screen = AppScreen.Interaction
                    },
                )
            }
        }
    }
}

@Composable
private fun Page(content: LazyListScope.() -> Unit) {
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(horizontal = 20.dp, vertical = 22.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
        content = content,
    )
}

@Composable
private fun DetailPage(
    title: String,
    onBack: () -> Unit,
    trailing: (@Composable () -> Unit)? = null,
    content: LazyListScope.() -> Unit,
) {
    Column(Modifier.fillMaxSize().background(CanvasWarm)) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Card(
                modifier = Modifier.size(44.dp).clickable(onClick = onBack),
                shape = CircleShape,
                colors = CardDefaults.cardColors(containerColor = Paper),
                border = androidx.compose.foundation.BorderStroke(1.dp, Line),
            ) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    MiniGlyph(GlyphKind.Left, true, Modifier.size(20.dp))
                }
            }
            Spacer(Modifier.width(13.dp))
            Text(
                title,
                modifier = Modifier.weight(1f),
                fontSize = 24.sp,
                fontWeight = FontWeight.Black,
                color = Ink,
            )
            trailing?.invoke()
        }
        LazyColumn(
            modifier = Modifier.fillMaxWidth().weight(1f),
            contentPadding = PaddingValues(start = 20.dp, end = 20.dp, top = 8.dp, bottom = 32.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
            content = content,
        )
    }
}

@Composable
private fun HomeScreen(
    device: NearbyDevice?,
    phase: ScanPhase,
    diagnostics: DiagnosticSnapshot?,
    character: ParentMemoryState,
    cloudDeviceId: String?,
    onDrive: () -> Unit,
    onTalk: () -> Unit,
    onStopTalk: () -> Unit,
    onTextChat: () -> Unit,
    onPlay: () -> Unit,
    onConnect: () -> Unit,
    onMemory: () -> Unit,
    onSleep: ((Boolean) -> Unit) -> Unit,
    onSettings: () -> Unit,
) {
    var sleepBusy by remember { mutableStateOf(false) }
    var sleepAccepted by remember { mutableStateOf(false) }
    LaunchedEffect(sleepAccepted) {
        if (sleepAccepted) {
            delay(5_000)
            sleepAccepted = false
        }
    }
    Page {
    val connected = device != null && phase in setOf(ScanPhase.Connected, ScanPhase.Provisioned)
    val conversationActive = (diagnostics?.conversationPhase ?: 0) in 1..5
    val aiStatus = when {
        !connected -> "设备未连接"
        diagnostics == null -> "正在读取状态"
        diagnostics.internetPhase == 3 -> "网络需要登录"
        diagnostics.internetPhase == 4 || !diagnostics.wifiConnected -> "已断网"
        diagnostics.conversationPhase in 1..2 -> "正在聆听"
        diagnostics.conversationPhase == 3 -> "识别中"
        diagnostics.conversationPhase == 4 -> "正在思考"
        diagnostics.conversationPhase == 5 || diagnostics.audioPlaying -> "正在回答"
        diagnostics.conversationPhase == 7 -> "对话出错"
        diagnostics.cloudReady -> "在线待机"
        else -> "AI 连接中"
    }
    val characterMatches = connected && cloudDeviceId != null &&
        cloudDeviceId == character.pairedDeviceId
    val stateFresh = characterMatches && character.stateSyncedAtMillis > 0L &&
        System.currentTimeMillis() - character.stateSyncedAtMillis < 120_000L &&
        character.error == null
    val mood = if (stateFresh) character.mood else null
    val hunger = character.needs["hunger"] ?: 0
    val thirst = character.needs["thirst"] ?: 0
    val energy = character.needs["energy"] ?: 100
    val stamina = character.needs["stamina"] ?: 100
    val sleepy = stateFresh && (mood == "sleepy" || energy <= 35 || stamina <= 35)
    val deviceSleeping = diagnostics?.expression == 22 &&
        diagnostics.conversationPhase !in 1..5
    item {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(
                    "BABY TANK",
                    fontSize = 10.sp,
                    letterSpacing = 2.4.sp,
                    color = CyanDeep,
                    fontWeight = FontWeight.Bold,
                )
                Spacer(Modifier.height(3.dp))
                Text("今天也一起玩吧", fontSize = 26.sp, fontWeight = FontWeight.Bold, color = Ink)
            }
            Card(
                modifier = Modifier.size(42.dp).clickable(onClick = onSettings),
                shape = CircleShape,
                colors = CardDefaults.cardColors(containerColor = Paper),
                border = androidx.compose.foundation.BorderStroke(1.dp, Line),
            ) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    MiniGlyph(GlyphKind.Settings, true, Modifier.size(24.dp),
                        tint = CyanDeep)
                }
            }
        }
    }
    item {
        Card(
            shape = RoundedCornerShape(32.dp),
            colors = CardDefaults.cardColors(containerColor = Ink),
            modifier = Modifier.fillMaxWidth().height(228.dp).clickable(
                onClick = when {
                    !connected -> onConnect
                    conversationActive -> onStopTalk
                    else -> onTalk
                },
            ),
        ) {
            Column(
                Modifier.fillMaxSize().padding(start = 20.dp, top = 18.dp, end = 20.dp, bottom = 16.dp),
            ) {
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Box(
                        Modifier.size(7.dp).background(
                            if (connected) Color(0xFF70DCA0) else Color.White.copy(alpha = .35f),
                            CircleShape,
                        ),
                    )
                    Spacer(Modifier.width(8.dp))
                    if (!connected || aiStatus !in setOf("在线待机", "正在读取状态")) {
                        Text(
                            aiStatus,
                            color = Color.White.copy(alpha = .72f),
                            fontSize = 12.sp,
                            fontWeight = FontWeight.Medium,
                        )
                    }
                    Spacer(Modifier.weight(1f))
                    Box(
                        Modifier.size(34.dp).background(Color.White.copy(alpha = .08f), CircleShape),
                        contentAlignment = Alignment.Center,
                    ) {
                        MiniGlyph(GlyphKind.Dialogue, true, Modifier.size(18.dp), tint = Cyan)
                    }
                }
                BabyFace(
                    Modifier.fillMaxWidth().weight(1f).padding(horizontal = 32.dp, vertical = 3.dp),
                    connected = connected,
                    mood = mood,
                )
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(
                            when {
                                !connected -> "连接宝宝坦克"
                                conversationActive -> "点一下，结束对话"
                                else -> "点一下，说句话"
                            },
                            color = Color.White,
                            fontSize = 18.sp,
                            fontWeight = FontWeight.SemiBold,
                        )
                    }
                    Text("→", color = Cyan, fontSize = 22.sp, fontWeight = FontWeight.Light)
                }
            }
        }
    }
    item {
        PetNeedsCard(
            hasState = stateFresh,
            hunger = hunger,
            thirst = thirst,
            energy = ((energy + stamina) / 2).coerceIn(0, 100),
            sleepy = sleepy,
            sleeping = deviceSleeping || sleepAccepted,
            sleepBusy = sleepBusy,
            onNeeds = onPlay,
            onSleep = {
                if (!sleepBusy && connected) {
                    sleepBusy = true
                    onSleep { accepted ->
                        sleepBusy = false
                        sleepAccepted = accepted
                    }
                }
            },
        )
    }
    item {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            HomeActionCard(
                title = "打字聊天", color = Paper, art = HomeArt.Chat,
                modifier = Modifier.weight(1f), onClick = onTextChat,
            )
            HomeActionCard(
                title = "成长记忆", color = Color(0xFFF1EFE8), art = HomeArt.Memory,
                modifier = Modifier.weight(1f), onClick = onMemory,
            )
        }
    }
    item {
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            HomeActionCard(
                title = "遥控",
                color = Color(0xFFF1EFE8),
                art = HomeArt.Drive,
                modifier = Modifier.weight(1f),
                onClick = if (connected) onDrive else onConnect,
            )
            HomeActionCard(
                title = "互动",
                color = Paper,
                art = HomeArt.Play,
                modifier = Modifier.weight(1f),
                onClick = if (connected) onPlay else onConnect,
            )
        }
    }
}
}

private enum class InteractionSection { Hub, Feed, Games, Poke, Rps, Fight, Dice }

private enum class InteractionArt {
    Games, Poke, Feed, Guess, Rock, Paper, Scissors, Fight, Dice,
    Strawberry, Apple, Banana, Cake, Cookie, Bread, RiceBall, IceCream,
    Milk, Juice, Water, Yogurt, SoyMilk, Cocoa,
}

private suspend fun playGameTone(tone: ToneGenerator, outcome: Int) {
    val notes = when {
        outcome > 0 -> intArrayOf(
            ToneGenerator.TONE_DTMF_4,
            ToneGenerator.TONE_DTMF_6,
            ToneGenerator.TONE_DTMF_8,
        )
        outcome < 0 -> intArrayOf(
            ToneGenerator.TONE_DTMF_8,
            ToneGenerator.TONE_DTMF_6,
            ToneGenerator.TONE_DTMF_4,
        )
        else -> intArrayOf(ToneGenerator.TONE_PROP_BEEP, ToneGenerator.TONE_PROP_BEEP)
    }
    notes.forEachIndexed { index, note ->
        tone.startTone(note, if (outcome == 0) 110 else 90)
        if (index < notes.lastIndex) delay(105)
    }
}

private data class ArtChoice(
    val title: String,
    val expression: Int,
    val art: InteractionArt,
)

@Composable
private fun InteractionScreen(
    connected: Boolean,
    onExpression: (Int) -> Unit,
    onExpressionConfirmed: (Int, (Boolean) -> Unit) -> Unit,
    onGameResultConfirmed: (Int, (Boolean) -> Unit) -> Unit,
    onFeedFinished: (Int, (Boolean, Int) -> Unit) -> Unit,
    onPokeConfirmed: ((Boolean) -> Unit) -> Unit,
    onNeedEvent: (String) -> Unit,
    onNotAccepted: () -> Unit,
    syncStatus: String,
    onGuessDrawing: () -> Unit,
    onBack: () -> Unit,
) {
    var section by rememberSaveable { mutableStateOf(InteractionSection.Hub) }
    val random = remember { SecureRandom() }
    val scope = rememberCoroutineScope()
    val tone = remember { ToneGenerator(AudioManager.STREAM_MUSIC, 62) }
    DisposableEffect(Unit) { onDispose { tone.release() } }
    var result by rememberSaveable { mutableStateOf("") }
    var outcome by rememberSaveable { mutableStateOf(0) }
    var robotArt by rememberSaveable { mutableStateOf(-1) }
    var rpsBusy by rememberSaveable { mutableStateOf(false) }
    var userScore by rememberSaveable { mutableStateOf(0) }
    var robotScore by rememberSaveable { mutableStateOf(0) }
    var robotTarget by rememberSaveable { mutableStateOf(0) }
    var fightSeconds by rememberSaveable { mutableStateOf(5) }
    var fightRunning by rememberSaveable { mutableStateOf(false) }
    var fightResultSettled by rememberSaveable { mutableStateOf(false) }
    var diceRolling by rememberSaveable { mutableStateOf(false) }
    var userDie by rememberSaveable { mutableStateOf(1) }
    var robotDie by rememberSaveable { mutableStateOf(1) }
    var feedingChoice by remember { mutableStateOf<ArtChoice?>(null) }
    var feedingRun by remember { mutableStateOf(0) }

    fun resetRound() {
        result = ""
        outcome = 0
        robotArt = -1
        rpsBusy = false
        fightResultSettled = false
        diceRolling = false
        feedingChoice = null
    }

    fun announceResult(userOutcome: Int, onSettled: (() -> Unit)? = null) {
        onGameResultConfirmed(if (userOutcome > 0) 1 else if (userOutcome < 0) 0 else 2) {
            if (it) onNeedEvent("played") else onNotAccepted()
            onSettled?.invoke()
        }
        scope.launch { playGameTone(tone, userOutcome) }
    }

    val pageTitle = when (section) {
        InteractionSection.Hub -> "一起玩"
        InteractionSection.Feed -> "喂东西"
        InteractionSection.Games -> "玩游戏"
        InteractionSection.Poke -> "戳一戳"
        InteractionSection.Rps -> "石头剪刀布"
        InteractionSection.Fight -> "打架"
        InteractionSection.Dice -> "摇骰子"
    }
    val navigateBack = {
        if (section == InteractionSection.Hub) {
            onBack()
        } else {
            resetRound()
            fightRunning = false
            section = InteractionSection.Hub
        }
    }
    BackHandler(onBack = navigateBack)

    LaunchedEffect(fightRunning, robotTarget) {
        if (!fightRunning) return@LaunchedEffect
        repeat(50) { step ->
            delay(100)
            robotScore = ((step + 1) * robotTarget / 50).coerceAtMost(robotTarget)
            fightSeconds = (5 - (step + 1) / 10).coerceAtLeast(0)
        }
        outcome = userScore.compareTo(robotTarget)
        result = "$userScore : $robotTarget"
        onExpression(
            when {
                outcome > 0 -> 14 // Robot lost: sad.
                outcome < 0 -> 8  // Robot won: proud.
                else -> 5
            },
        )
        announceResult(outcome) { fightResultSettled = true }
        fightRunning = false
    }

    LaunchedEffect(feedingRun) {
        val choice = feedingChoice ?: return@LaunchedEffect
        result = ""
        delay(3400)
        onFeedFinished(choice.expression) { accepted, reaction ->
            if (accepted) {
                result = when (reaction) {
                    1 -> "${choice.title}正合它的口味！"
                    2 -> "吃完啦，下次想换个口味"
                    3 -> "怎么又是${choice.title}呀"
                    4 -> "吃饱喝足，嗝～"
                    else -> if (choice.expression >= 47) {
                        "${choice.title}喝光啦"
                    } else {
                        "${choice.title}吃光啦"
                    }
                }
                onNeedEvent(if (choice.expression >= 47) "drank" else "fed")
            } else {
                result = "头部没有完成这次互动"
                onNotAccepted()
            }
            feedingChoice = null
        }
    }

    feedingChoice?.let { choice ->
        Dialog(
            onDismissRequest = {},
            properties = DialogProperties(
                dismissOnBackPress = false,
                dismissOnClickOutside = false,
                usePlatformDefaultWidth = false,
            ),
        ) {
            Box(
                modifier = Modifier.fillMaxSize()
                    .background(Color.Black.copy(alpha = .64f))
                    .padding(horizontal = 18.dp),
                contentAlignment = Alignment.Center,
            ) {
                Card(
                    modifier = Modifier.fillMaxWidth(.72f).height(132.dp),
                    shape = RoundedCornerShape(30.dp),
                    colors = CardDefaults.cardColors(containerColor = Paper),
                ) {
                    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                        Text(
                            if (choice.expression >= 47) "正在喝…" else "正在吃…",
                            color = Ink,
                            fontSize = 28.sp,
                            fontWeight = FontWeight.Black,
                        )
                    }
                }
            }
        }
    }

    DetailPage(
        title = pageTitle,
        onBack = navigateBack,
        trailing = {
            if (!connected) StatusPill("未连接", Coral)
        },
    ) {
        if (syncStatus != "成长数值尚未同步" &&
            syncStatus != "成长数值已同步到云端" &&
            syncStatus != "正在同步成长数值…"
        ) {
            item { InteractionResult(syncStatus) }
        }
        when (section) {
            InteractionSection.Hub -> {
                item {
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        InteractionMenuTile("玩游戏", InteractionArt.Games, Paper, Modifier.weight(1f)) {
                            section = InteractionSection.Games
                        }
                        InteractionMenuTile("戳一戳", InteractionArt.Poke, Color(0xFFF1EFE8), Modifier.weight(1f)) {
                            section = InteractionSection.Poke
                        }
                    }
                }
                item {
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        InteractionMenuTile("喂东西", InteractionArt.Feed, Color(0xFFF1EFE8), Modifier.weight(1f)) {
                            section = InteractionSection.Feed
                        }
                        InteractionMenuTile(
                            "猜猜我画的", InteractionArt.Guess, Paper,
                            Modifier.weight(1f), onGuessDrawing)
                    }
                }
            }
            InteractionSection.Feed -> {
                item { Text("食物", color = Ink, fontSize = 18.sp, fontWeight = FontWeight.Bold) }
                val foods = listOf(
                    ArtChoice("草莓", 39, InteractionArt.Strawberry),
                    ArtChoice("苹果", 40, InteractionArt.Apple),
                    ArtChoice("香蕉", 41, InteractionArt.Banana),
                    ArtChoice("蛋糕", 42, InteractionArt.Cake),
                    ArtChoice("饼干", 43, InteractionArt.Cookie),
                    ArtChoice("面包", 44, InteractionArt.Bread),
                    ArtChoice("饭团", 45, InteractionArt.RiceBall),
                    ArtChoice("冰淇淋", 46, InteractionArt.IceCream),
                )
                items(foods.chunked(2)) { row ->
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        row.forEach { choice ->
                            ArtChoiceButton(choice, connected && feedingChoice == null, Modifier.weight(1f)) {
                                tone.startTone(ToneGenerator.TONE_PROP_BEEP, 70)
                                feedingChoice = choice
                                onExpressionConfirmed(choice.expression) { accepted ->
                                    if (accepted) feedingRun += 1 else {
                                        feedingChoice = null
                                        result = "头部未接受动作，成长数值未改变"
                                        onNotAccepted()
                                    }
                                }
                            }
                        }
                        if (row.size == 1) Spacer(Modifier.weight(1f))
                    }
                }
                item { Text("饮品", color = Ink, fontSize = 18.sp, fontWeight = FontWeight.Bold) }
                val drinks = listOf(
                    ArtChoice("牛奶", 47, InteractionArt.Milk),
                    ArtChoice("果汁", 48, InteractionArt.Juice),
                    ArtChoice("水", 49, InteractionArt.Water),
                    ArtChoice("酸奶", 50, InteractionArt.Yogurt),
                    ArtChoice("豆浆", 51, InteractionArt.SoyMilk),
                    ArtChoice("热可可", 52, InteractionArt.Cocoa),
                )
                items(drinks.chunked(2)) { row ->
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        row.forEach { choice ->
                            ArtChoiceButton(choice, connected && feedingChoice == null, Modifier.weight(1f)) {
                                tone.startTone(ToneGenerator.TONE_PROP_BEEP, 70)
                                feedingChoice = choice
                                onExpressionConfirmed(choice.expression) { accepted ->
                                    if (accepted) feedingRun += 1 else {
                                        feedingChoice = null
                                        result = "头部未接受动作，成长数值未改变"
                                        onNotAccepted()
                                    }
                                }
                            }
                        }
                    }
                }
                if (result.isNotBlank()) item { InteractionResult(result) }
            }
            InteractionSection.Games -> {
                item {
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        InteractionMenuTile("石头剪刀布", InteractionArt.Scissors, Lavender.copy(.55f), Modifier.weight(1f)) {
                            resetRound(); section = InteractionSection.Rps
                        }
                        InteractionMenuTile("打架", InteractionArt.Fight, Coral.copy(.22f), Modifier.weight(1f)) {
                            resetRound(); section = InteractionSection.Fight
                        }
                        InteractionMenuTile("摇骰子", InteractionArt.Dice, MintWash, Modifier.weight(1f)) {
                            resetRound(); section = InteractionSection.Dice
                        }
                    }
                }
            }
            InteractionSection.Poke -> {
                item {
                    Button(
                        onClick = {
                            onPokeConfirmed {
                                if (it) {
                                    result = "戳到啦"
                                    onNeedEvent("poked")
                                } else {
                                    result = "它现在正忙，等一下再戳"
                                    onNotAccepted()
                                }
                            }
                        },
                        enabled = connected,
                        modifier = Modifier.fillMaxWidth().height(230.dp),
                        colors = ButtonDefaults.buttonColors(containerColor = Cyan),
                        shape = RoundedCornerShape(34.dp),
                    ) {
                        InteractionArtIcon(InteractionArt.Poke, Ink, Modifier.size(150.dp))
                    }
                }
                if (result.isNotBlank()) item { InteractionResult(result) }
            }
            InteractionSection.Rps -> {
                item {
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(9.dp)) {
                        listOf(
                            ArtChoice("石头", 29, InteractionArt.Rock),
                            ArtChoice("布", 30, InteractionArt.Paper),
                            ArtChoice("剪刀", 31, InteractionArt.Scissors),
                        ).forEachIndexed { user, choice ->
                            ArtChoiceButton(choice, connected && !rpsBusy, Modifier.weight(1f)) {
                                rpsBusy = true
                                result = ""
                                tone.startTone(ToneGenerator.TONE_PROP_BEEP, 70)
                                val robot = random.nextInt(3)
                                val robotChoice = listOf("石头", "布", "剪刀")[robot]
                                robotArt = robot
                                onExpression(29 + robot)
                                val round = (user - robot + 3) % 3
                                val finalOutcome = if (round == 1) 1 else if (round == 2) -1 else 0
                                scope.launch {
                                    // 先让手机和设备完整展示手势，再揭晓输赢，避免结果“抢跑”。
                                    delay(1100)
                                    outcome = finalOutcome
                                    result = "宝宝出了$robotChoice"
                                    onExpression(if (finalOutcome > 0) 14 else if (finalOutcome < 0) 8 else 5)
                                    announceResult(finalOutcome)
                                    delay(350)
                                    rpsBusy = false
                                }
                            }
                        }
                    }
                }
                if (robotArt >= 0) item {
                    Card(
                        modifier = Modifier.fillMaxWidth(),
                        shape = RoundedCornerShape(30.dp),
                        colors = CardDefaults.cardColors(containerColor = Ink),
                    ) {
                        Box(
                            Modifier.fillMaxWidth().height(210.dp),
                            contentAlignment = Alignment.Center,
                        ) {
                            AnimatedContent(targetState = robotArt, label = "robotRpsReveal") { artIndex ->
                                InteractionArtIcon(
                                    listOf(InteractionArt.Rock, InteractionArt.Paper,
                                        InteractionArt.Scissors)[artIndex.coerceIn(0, 2)],
                                    Color.White,
                                    Modifier.size(170.dp),
                                )
                            }
                        }
                    }
                }
                if (result.isNotBlank()) item {
                    GameResultCard(result, outcome,
                        listOf(InteractionArt.Rock, InteractionArt.Paper,
                            InteractionArt.Scissors)[robotArt.coerceIn(0, 2)])
                }
            }
            InteractionSection.Fight -> {
                item {
                    Card(shape = RoundedCornerShape(28.dp),
                        colors = CardDefaults.cardColors(containerColor = Ink)) {
                        Column(
                            Modifier.fillMaxWidth().padding(24.dp),
                            horizontalAlignment = Alignment.CenterHorizontally,
                            verticalArrangement = Arrangement.spacedBy(12.dp),
                        ) {
                            Text(if (fightRunning) "$fightSeconds" else "5 秒连点",
                                color = Color.White, fontSize = 34.sp, fontWeight = FontWeight.Black)
                            Row(horizontalArrangement = Arrangement.spacedBy(28.dp)) {
                                ScoreBlock("你", userScore, Cyan)
                                ScoreBlock("宝宝", robotScore, Cream)
                            }
                            Button(
                                onClick = {
                                    if (!fightRunning) {
                                        userScore = 0
                                        robotScore = 0
                                        robotTarget = 16 + random.nextInt(20)
                                        fightSeconds = 5
                                        result = ""
                                        fightRunning = true
                                        onExpression(32)
                                    } else {
                                        userScore += 1
                                        tone.startTone(ToneGenerator.TONE_PROP_BEEP2, 35)
                                    }
                                },
                                enabled = connected && (fightRunning || result.isBlank()),
                                modifier = Modifier.fillMaxWidth().height(76.dp),
                                colors = ButtonDefaults.buttonColors(containerColor = Coral),
                                shape = RoundedCornerShape(25.dp),
                            ) { InteractionArtIcon(InteractionArt.Fight, Ink, Modifier.size(64.dp)) }
                        }
                    }
                }
                if (result.isNotBlank()) {
                    item { GameResultCard(result, outcome, InteractionArt.Fight) }
                    item {
                        Button(
                            onClick = {
                                result = ""
                                userScore = 0
                                robotScore = 0
                                fightSeconds = 5
                                fightResultSettled = false
                            },
                            enabled = connected && fightResultSettled,
                            modifier = Modifier.fillMaxWidth().height(52.dp),
                            shape = RoundedCornerShape(18.dp),
                        ) { Text(if (fightResultSettled) "再来一局" else "正在确认本局结果") }
                    }
                }
            }
            InteractionSection.Dice -> {
                item {
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                        DicePanel("你", userDie, diceRolling, Cyan, Modifier.weight(1f))
                        DicePanel("宝宝", robotDie, diceRolling, Cream, Modifier.weight(1f))
                    }
                }
                item {
                    Button(
                        onClick = {
                            if (diceRolling) return@Button
                            diceRolling = true
                            outcome = 0
                            result = ""
                            val finalUser = 1 + random.nextInt(6)
                            val finalRobot = 1 + random.nextInt(6)
                            onExpression(32 + finalRobot)
                            scope.launch {
                                repeat(12) { step ->
                                    userDie = 1 + random.nextInt(6)
                                    robotDie = 1 + random.nextInt(6)
                                    tone.startTone(ToneGenerator.TONE_PROP_BEEP2, 28)
                                    delay(55L + step * 7L)
                                }
                                userDie = finalUser
                                robotDie = finalRobot
                                outcome = finalUser.compareTo(finalRobot)
                                result = "$finalUser : $finalRobot"
                                delay(320)
                                onExpression(if (outcome > 0) 14 else if (outcome < 0) 8 else 5)
                                announceResult(outcome)
                                diceRolling = false
                            }
                        },
                        enabled = connected && !diceRolling,
                        modifier = Modifier.fillMaxWidth().height(100.dp),
                        colors = ButtonDefaults.buttonColors(containerColor = Cyan),
                        shape = RoundedCornerShape(28.dp),
                    ) { InteractionArtIcon(InteractionArt.Dice, Ink, Modifier.size(76.dp)) }
                }
                if (result.isNotBlank()) item { GameResultCard(result, outcome, InteractionArt.Dice) }
            }
        }
    }
}

@Composable
private fun InteractionMenuTile(
    title: String,
    art: InteractionArt,
    color: Color,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    var pressed by remember { mutableStateOf(false) }
    val scale by animateFloatAsState(if (pressed) .965f else 1f, label = "menuPress")
    Card(
        modifier = modifier.height(154.dp)
            .graphicsLayer(scaleX = scale, scaleY = scale)
            .pointerInput(Unit) {
                detectTapGestures(onPress = {
                    pressed = true
                    val released = tryAwaitRelease()
                    pressed = false
                    if (released) onClick()
                })
            },
        shape = RoundedCornerShape(22.dp),
        colors = CardDefaults.cardColors(containerColor = color),
        border = androidx.compose.foundation.BorderStroke(1.dp, Line.copy(alpha = .72f)),
    ) {
        Box(Modifier.fillMaxSize().padding(15.dp)) {
            Text(
                title,
                modifier = Modifier.align(Alignment.TopStart),
                fontSize = 16.sp,
                fontWeight = FontWeight.Bold,
                color = Ink,
                maxLines = 1,
            )
            InteractionArtIcon(
                art,
                Ink,
                Modifier.align(Alignment.BottomEnd).size(94.dp),
            )
        }
    }
}

@Composable
private fun ArtChoiceButton(
    choice: ArtChoice,
    enabled: Boolean,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    var pressed by remember { mutableStateOf(false) }
    val scale by animateFloatAsState(if (pressed) .91f else 1f, label = "choicePress")
    Card(
        modifier = modifier.height(142.dp)
            .graphicsLayer(scaleX = scale, scaleY = scale)
            .pointerInput(enabled) {
                detectTapGestures(onPress = {
                    if (!enabled) return@detectTapGestures
                    pressed = true
                    val released = tryAwaitRelease()
                    pressed = false
                    if (released) onClick()
                })
            },
        shape = RoundedCornerShape(24.dp),
        colors = CardDefaults.cardColors(
            containerColor = if (enabled) Paper else Paper.copy(alpha = .42f),
        ),
    ) {
        Column(
            Modifier.fillMaxSize().padding(10.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.SpaceBetween,
        ) {
            InteractionArtIcon(
                choice.art,
                if (enabled) Ink else SoftInk.copy(alpha = .45f),
                Modifier.fillMaxWidth().weight(1f),
            )
            Text(choice.title, fontSize = 12.sp, fontWeight = FontWeight.Bold, color = SoftInk)
        }
    }
}

@Composable
private fun InteractionArtIcon(
    art: InteractionArt,
    ink: Color,
    modifier: Modifier = Modifier,
) {
    val drawable = when (art) {
        InteractionArt.Games -> R.drawable.flat_interaction_games
        InteractionArt.Poke -> R.drawable.flat_interaction_poke
        InteractionArt.Feed -> R.drawable.flat_interaction_feed
        InteractionArt.Guess -> R.drawable.game_action_guess
        InteractionArt.Rock -> R.drawable.flat_game_rock
        InteractionArt.Paper -> R.drawable.flat_game_paper
        InteractionArt.Scissors -> R.drawable.flat_game_scissors
        InteractionArt.Fight -> R.drawable.flat_game_fight
        InteractionArt.Dice -> R.drawable.flat_interaction_dice
        InteractionArt.Strawberry -> R.drawable.flat_food_strawberry
        InteractionArt.Apple -> R.drawable.flat_food_apple
        InteractionArt.Banana -> R.drawable.flat_food_banana
        InteractionArt.Cake -> R.drawable.flat_food_cake
        InteractionArt.Cookie -> R.drawable.flat_food_cookie
        InteractionArt.Bread -> R.drawable.flat_food_bread
        InteractionArt.RiceBall -> R.drawable.flat_food_rice_ball
        InteractionArt.IceCream -> R.drawable.flat_food_ice_cream
        InteractionArt.Milk -> R.drawable.flat_drink_milk
        InteractionArt.Juice -> R.drawable.flat_drink_juice
        InteractionArt.Water -> R.drawable.flat_drink_water
        InteractionArt.Yogurt -> R.drawable.flat_drink_yogurt
        InteractionArt.SoyMilk -> R.drawable.flat_drink_soy_milk
        InteractionArt.Cocoa -> R.drawable.flat_drink_cocoa
    }
    Image(
        painter = painterResource(drawable),
        contentDescription = null,
        modifier = modifier,
        contentScale = ContentScale.Fit,
    )
}

@Suppress("unused")
@Composable
private fun LegacyInteractionArtIcon(
    art: InteractionArt,
    ink: Color,
    modifier: Modifier = Modifier,
) = Canvas(modifier) {
    val u = size.minDimension / 100f
    fun rr(x: Float, y: Float, w: Float, h: Float, radius: Float, color: Color) {
        drawRoundRect(
            color = color,
            topLeft = Offset(x * u, y * u),
            size = Size(w * u, h * u),
            cornerRadius = CornerRadius(radius * u, radius * u),
        )
    }
    fun circle(x: Float, y: Float, radius: Float, color: Color) {
        drawCircle(color, radius * u, Offset(x * u, y * u))
    }
    fun line(x0: Float, y0: Float, x1: Float, y1: Float, width: Float, color: Color) {
        drawLine(color, Offset(x0 * u, y0 * u), Offset(x1 * u, y1 * u),
            width * u, cap = StrokeCap.Round)
    }
    fun dice(value: Int, color: Color) {
        rr(18f, 18f, 64f, 64f, 15f, color)
        val dots = when (value) {
            1 -> listOf(50f to 50f)
            2 -> listOf(34f to 34f, 66f to 66f)
            3 -> listOf(32f to 32f, 50f to 50f, 68f to 68f)
            4 -> listOf(33f to 33f, 67f to 33f, 33f to 67f, 67f to 67f)
            5 -> listOf(32f to 32f, 68f to 32f, 50f to 50f, 32f to 68f, 68f to 68f)
            else -> listOf(32f to 30f, 68f to 30f, 32f to 50f,
                68f to 50f, 32f to 70f, 68f to 70f)
        }
        dots.forEach { (x, y) -> circle(x, y, 5f, if (color == Ink) Cream else Ink) }
    }

    when (art) {
        InteractionArt.Guess -> {
            rr(13f, 19f, 61f, 65f, 11f, Cream)
            line(25f, 66f, 39f, 48f, 6f, Coral)
            line(39f, 48f, 52f, 61f, 6f, CyanDeep)
            line(52f, 61f, 66f, 36f, 6f, Ink)
            circle(81f, 29f, 14f, Cyan)
            line(79f, 24f, 84f, 28f, 4f, Ink)
            circle(81f, 37f, 2.5f, Ink)
        }
        InteractionArt.Games -> {
            dice(5, ink)
            circle(79f, 24f, 11f, Coral)
            line(74f, 24f, 84f, 24f, 3f, Color.White)
            line(79f, 19f, 79f, 29f, 3f, Color.White)
        }
        InteractionArt.Poke -> {
            circle(50f, 27f, 16f, Coral.copy(alpha = .9f))
            circle(50f, 27f, 7f, if (ink == Ink) Color.White else Ink)
            line(50f, 88f, 50f, 39f, 17f, ink)
            line(50f, 88f, 68f, 63f, 14f, ink)
        }
        InteractionArt.Feed -> {
            circle(38f, 55f, 22f, Coral)
            circle(57f, 55f, 22f, Coral)
            line(49f, 36f, 54f, 20f, 5f, ink)
            rr(68f, 38f, 23f, 44f, 7f, Cyan)
            line(78f, 38f, 88f, 18f, 4f, Cream)
        }
        InteractionArt.Rock -> {
            rr(17f, 42f, 66f, 42f, 17f, ink)
            repeat(4) { finger -> rr(20f + finger * 16f, 22f, 17f, 34f, 8f, Cream) }
            rr(72f, 47f, 16f, 32f, 7f, Cream)
        }
        InteractionArt.Paper -> {
            rr(24f, 46f, 54f, 40f, 15f, ink)
            repeat(4) { finger -> rr(25f + finger * 15f, 12f + finger * 2f,
                13f, 50f - finger * 3f, 6f, Cream) }
            line(72f, 56f, 90f, 38f, 11f, Cream)
        }
        InteractionArt.Scissors -> {
            line(45f, 72f, 29f, 17f, 13f, Cream)
            line(56f, 72f, 76f, 18f, 13f, Cream)
            rr(24f, 61f, 59f, 27f, 12f, ink)
            circle(27f, 16f, 8f, Coral)
            circle(78f, 17f, 8f, Cyan)
        }
        InteractionArt.Fight -> {
            rr(15f, 42f, 70f, 44f, 17f, ink)
            repeat(4) { finger -> rr(17f + finger * 18f, 20f, 19f, 39f, 8f, Cream) }
            line(8f, 29f, 18f, 38f, 4f, Coral)
            line(84f, 20f, 93f, 10f, 4f, Cyan)
        }
        InteractionArt.Dice -> dice(5, ink)
        InteractionArt.Strawberry -> {
            circle(43f, 55f, 24f, Coral); circle(58f, 55f, 24f, Coral)
            line(35f, 32f, 50f, 19f, 7f, CyanDeep); line(66f, 32f, 50f, 19f, 7f, CyanDeep)
            listOf(37f to 51f, 57f to 45f, 49f to 66f).forEach { circle(it.first, it.second, 3f, Cream) }
        }
        InteractionArt.Apple -> {
            circle(40f, 57f, 27f, Coral); circle(61f, 57f, 27f, Coral)
            line(51f, 34f, 56f, 17f, 5f, ink); rr(55f, 18f, 24f, 10f, 6f, CyanDeep)
        }
        InteractionArt.Banana -> {
            drawArc(Cream, 28f, 155f, false, Offset(18f * u, 10f * u),
                Size(65f * u, 77f * u), style = Stroke(15f * u, cap = StrokeCap.Round))
            drawArc(ink, 31f, 144f, false, Offset(27f * u, 16f * u),
                Size(48f * u, 61f * u), style = Stroke(5f * u, cap = StrokeCap.Round))
        }
        InteractionArt.Cake -> {
            rr(15f, 43f, 70f, 42f, 9f, Cream); rr(15f, 41f, 70f, 13f, 6f, Coral)
            line(50f, 42f, 50f, 17f, 5f, Cyan); circle(50f, 13f, 6f, Coral)
        }
        InteractionArt.Cookie -> {
            circle(50f, 52f, 36f, Cream)
            listOf(35f to 37f, 60f to 32f, 67f to 58f, 43f to 67f, 29f to 57f)
                .forEach { circle(it.first, it.second, 5f, Ink) }
        }
        InteractionArt.Bread -> {
            rr(13f, 25f, 74f, 62f, 22f, Cream); rr(19f, 33f, 62f, 48f, 17f, Coral)
            rr(25f, 38f, 50f, 38f, 14f, ink)
        }
        InteractionArt.RiceBall -> {
            val path = Path().apply { moveTo(50f * u, 12f * u); lineTo(88f * u, 83f * u)
                lineTo(12f * u, 83f * u); close() }
            drawPath(path, Cream); rr(38f, 55f, 24f, 30f, 2f, ink)
        }
        InteractionArt.IceCream -> {
            val cone = Path().apply { moveTo(30f * u, 48f * u); lineTo(70f * u, 48f * u)
                lineTo(50f * u, 91f * u); close() }
            drawPath(cone, Cream); circle(39f, 39f, 19f, Coral); circle(60f, 39f, 19f, Cyan)
        }
        InteractionArt.Milk, InteractionArt.Juice, InteractionArt.Water,
        InteractionArt.Yogurt, InteractionArt.SoyMilk -> {
            val body = when (art) {
                InteractionArt.Juice -> Coral
                InteractionArt.Water -> Cyan
                else -> Cream
            }
            rr(24f, 20f, 52f, 68f, 10f, body); rr(32f, 29f, 36f, 50f, 7f, ink)
            circle(50f, 54f, 10f, if (art == InteractionArt.Water) Cream else Cyan)
            if (art == InteractionArt.Yogurt) rr(19f, 17f, 62f, 10f, 5f, Coral)
        }
        InteractionArt.Cocoa -> {
            rr(18f, 37f, 59f, 46f, 12f, Coral)
            drawCircle(Cream, 14f * u, Offset(78f * u, 58f * u), style = Stroke(7f * u))
            line(35f, 34f, 30f, 16f, 4f, Cream); line(52f, 34f, 58f, 14f, 4f, Cream)
        }
    }
}

@Composable
private fun GameResultCard(text: String, outcome: Int, art: InteractionArt) {
    val background = when {
        outcome > 0 -> Cyan.copy(alpha = .25f)
        outcome < 0 -> Coral.copy(alpha = .25f)
        else -> Cream.copy(alpha = .62f)
    }
    val title = when {
        outcome > 0 -> "你赢了"
        outcome < 0 -> "宝宝赢了"
        else -> "平局"
    }
    Card(shape = RoundedCornerShape(28.dp),
        colors = CardDefaults.cardColors(containerColor = background)) {
        AnimatedContent(targetState = Triple(title, text, outcome), label = "gameResult") { state ->
            Row(
                Modifier.fillMaxWidth().padding(18.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(14.dp),
            ) {
                ResultMark(state.third, Modifier.size(70.dp))
                Column(Modifier.weight(1f)) {
                    Text(state.first, fontSize = 28.sp, fontWeight = FontWeight.Black, color = Ink)
                    Text(state.second, fontSize = 14.sp, fontWeight = FontWeight.Bold, color = SoftInk)
                }
                InteractionArtIcon(art, Ink, Modifier.size(66.dp))
            }
        }
    }
}

@Composable
private fun ResultMark(outcome: Int, modifier: Modifier = Modifier) {
    Image(
        painter = painterResource(
            when {
                outcome > 0 -> R.drawable.flat_game_win
                outcome < 0 -> R.drawable.flat_game_lose
                else -> R.drawable.flat_interaction_dice
            },
        ),
        contentDescription = null,
        modifier = modifier,
        contentScale = ContentScale.Fit,
    )
}

@Suppress("unused")
@Composable
private fun LegacyResultMark(outcome: Int, modifier: Modifier = Modifier) = Canvas(modifier) {
    val color = when { outcome > 0 -> CyanDeep; outcome < 0 -> Coral; else -> Cream }
    drawCircle(color)
    val stroke = size.minDimension * .11f
    if (outcome > 0) {
        drawLine(Color.White, Offset(size.width * .25f, size.height * .52f),
            Offset(size.width * .44f, size.height * .70f), stroke, StrokeCap.Round)
        drawLine(Color.White, Offset(size.width * .44f, size.height * .70f),
            Offset(size.width * .77f, size.height * .31f), stroke, StrokeCap.Round)
    } else if (outcome < 0) {
        drawLine(Color.White, Offset(size.width * .30f, size.height * .30f),
            Offset(size.width * .70f, size.height * .70f), stroke, StrokeCap.Round)
        drawLine(Color.White, Offset(size.width * .70f, size.height * .30f),
            Offset(size.width * .30f, size.height * .70f), stroke, StrokeCap.Round)
    } else {
        repeat(2) { row ->
            val y = size.height * (.42f + row * .18f)
            drawLine(Ink, Offset(size.width * .29f, y), Offset(size.width * .71f, y),
                stroke * .72f, StrokeCap.Round)
        }
    }
}

@Composable
private fun DicePanel(
    label: String,
    value: Int,
    rolling: Boolean,
    color: Color,
    modifier: Modifier = Modifier,
) {
    Card(modifier, shape = RoundedCornerShape(26.dp),
        colors = CardDefaults.cardColors(containerColor = Ink)) {
        Column(
            Modifier.fillMaxWidth().padding(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(label, color = Color.White.copy(.72f), fontWeight = FontWeight.Bold)
            AnimatedContent(targetState = value.coerceIn(1, 6), label = "diceRoll") { shown ->
                DiceFace(shown, rolling, color, Modifier.size(118.dp))
            }
        }
    }
}

@Composable
private fun DiceFace(value: Int, rolling: Boolean, color: Color, modifier: Modifier = Modifier) =
    Canvas(modifier) {
        val inset = if (rolling) 10f else 8f
        drawRoundRect(color, Offset(inset, inset),
            Size(size.width - inset * 2, size.height - inset * 2),
            CornerRadius(24f, 24f))
        val points = when (value.coerceIn(1, 6)) {
            1 -> listOf(.5f to .5f)
            2 -> listOf(.32f to .32f, .68f to .68f)
            3 -> listOf(.3f to .3f, .5f to .5f, .7f to .7f)
            4 -> listOf(.31f to .31f, .69f to .31f, .31f to .69f, .69f to .69f)
            5 -> listOf(.3f to .3f, .7f to .3f, .5f to .5f, .3f to .7f, .7f to .7f)
            else -> listOf(.3f to .27f, .7f to .27f, .3f to .5f,
                .7f to .5f, .3f to .73f, .7f to .73f)
        }
        points.forEach { (x, y) ->
            drawCircle(Ink, size.minDimension * .055f, Offset(size.width * x, size.height * y))
        }
    }

@Composable
private fun InteractionEntry(
    title: String,
    subtitle: String,
    color: Color,
    glyph: GlyphKind,
    onClick: () -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth().clickable(onClick = onClick),
        shape = RoundedCornerShape(25.dp),
        colors = CardDefaults.cardColors(containerColor = Paper),
    ) {
        Row(Modifier.fillMaxWidth().padding(18.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(52.dp).background(color, RoundedCornerShape(17.dp)),
                contentAlignment = Alignment.Center) {
                MiniGlyph(glyph, true, Modifier.size(27.dp))
            }
            Spacer(Modifier.width(14.dp))
            Column(Modifier.weight(1f)) {
                Text(title, fontSize = 18.sp, fontWeight = FontWeight.Black)
                Text(subtitle, color = SoftInk, fontSize = 12.sp)
            }
            MiniGlyph(GlyphKind.Right, true, Modifier.size(20.dp))
        }
    }
}

@Composable
private fun InteractionButton(
    text: String,
    enabled: Boolean,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    Button(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier.height(54.dp),
        shape = RoundedCornerShape(18.dp),
        colors = ButtonDefaults.buttonColors(containerColor = Ink),
    ) { Text(text, fontWeight = FontWeight.Black) }
}

@Composable
private fun InteractionHint(title: String, subtitle: String) {
    Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
        Text(title, fontSize = 18.sp, fontWeight = FontWeight.Black)
        Text(subtitle, color = SoftInk, fontSize = 12.sp)
    }
}

@Composable
private fun InteractionResult(text: String) {
    Card(shape = RoundedCornerShape(22.dp),
        colors = CardDefaults.cardColors(containerColor = MintWash)) {
        Text(text, Modifier.fillMaxWidth().padding(18.dp),
            fontSize = 17.sp, fontWeight = FontWeight.Black, color = Ink)
    }
}

@Composable
private fun ScoreBlock(label: String, score: Int, color: Color) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Text(label, color = Color.White.copy(.65f), fontSize = 12.sp)
        Text(score.toString(), color = color, fontSize = 34.sp, fontWeight = FontWeight.Black)
    }
}

@Composable
private fun SettingsScreen(
    device: NearbyDevice?,
    phase: ScanPhase,
    diagnostics: DiagnosticState,
    onPair: () -> Unit,
    onNetwork: () -> Unit,
    onCharacter: () -> Unit,
    onMemory: () -> Unit,
    onLab: () -> Unit,
    onVolume: (Int) -> Unit,
    onSetupHotspot: (Boolean, (Boolean) -> Unit) -> Unit,
    onRestart: ((Boolean) -> Unit) -> Unit,
    onBack: () -> Unit,
) {
    val connected = device != null && phase in setOf(ScanPhase.Connected, ScanPhase.Provisioned)
    var confirmRestart by rememberSaveable { mutableStateOf(false) }
    var restartAccepted by rememberSaveable { mutableStateOf(false) }
    if (confirmRestart) {
        AlertDialog(
            onDismissRequest = { confirmRestart = false },
            title = { Text("重新启动设备？") },
            text = { Text("设备会先停车，然后重新启动头部软件。蓝牙会暂时断开，通常需要稍后重新连接。") },
            confirmButton = {
                Button(
                    onClick = {
                        confirmRestart = false
                        onRestart { accepted -> restartAccepted = accepted }
                    },
                    colors = ButtonDefaults.buttonColors(containerColor = Coral),
                ) { Text("确认重新启动") }
            },
            dismissButton = {
                OutlinedButton(onClick = { confirmRestart = false }) {
                    Text("取消")
                }
            },
        )
    }
    DetailPage("设置", onBack) {
        item {
            SettingsRow("连接设备", if (connected) "已连接" else "未连接", GlyphKind.Pair, MintWash, onPair)
        }
        item {
            val network = when (diagnostics.snapshot?.internetPhase) {
                2 -> "已联网"
                3 -> "需要登录"
                4 -> "已断网"
                else -> ""
            }
            SettingsRow("网络设置", network, GlyphKind.Signal, SkyWash, onNetwork)
        }
        item {
            SetupHotspotSetting(
                snapshot = diagnostics.snapshot,
                enabled = connected && diagnostics.snapshot != null &&
                    diagnostics.phase != DiagnosticPhase.Running,
                onSetupHotspot = onSetupHotspot,
            )
        }
        item {
            VolumeSetting(
                volume = diagnostics.snapshot?.speakerVolume ?: 75,
                enabled = connected,
                onVolume = onVolume,
            )
        }
        item {
            SettingsRow("宝宝性格", "", GlyphKind.Character,
                Lavender.copy(.48f), onCharacter)
        }
        item {
            SettingsRow("成长记忆", "", GlyphKind.Character,
                MintWash, onMemory)
        }
        item {
            SettingsRow("功能检测", "", GlyphKind.Lab,
                Cream.copy(.55f), onLab)
        }
        item {
            Button(
                onClick = { confirmRestart = true },
                enabled = connected && diagnostics.phase != DiagnosticPhase.Running,
                modifier = Modifier.fillMaxWidth().height(54.dp),
                shape = RoundedCornerShape(18.dp),
                colors = ButtonDefaults.buttonColors(containerColor = Coral),
            ) { Text(if (restartAccepted) "重新启动命令已发送" else "重新启动设备") }
        }
    }
}

@Composable
private fun SetupHotspotSetting(
    snapshot: DiagnosticSnapshot?,
    enabled: Boolean,
    onSetupHotspot: (Boolean, (Boolean) -> Unit) -> Unit,
) {
    var rejected by rememberSaveable { mutableStateOf(false) }
    val checked = snapshot?.setupHotspotEnabled == true
    val subtitle = when {
        snapshot == null -> "连接设备并读取状态后可用"
        rejected -> "操作未完成，请确认头部固件已更新"
        !checked -> "已关闭 · 家庭 Wi-Fi 和云连接不受影响"
        else -> "已开启 · 可连接 BabyTank 配网"
    }
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(23.dp),
        colors = CardDefaults.cardColors(containerColor = Paper),
    ) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 17.dp, vertical = 15.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                Modifier.size(46.dp).background(SkyWash, RoundedCornerShape(15.dp)),
                contentAlignment = Alignment.Center,
            ) {
                MiniGlyph(GlyphKind.Signal, checked, Modifier.size(23.dp))
            }
            Spacer(Modifier.width(13.dp))
            Column(Modifier.weight(1f)) {
                Text("配网热点", fontWeight = FontWeight.Black, fontSize = 16.sp)
                Text(subtitle, color = SoftInk, fontSize = 12.sp)
            }
            Switch(
                checked = checked,
                enabled = enabled,
                onCheckedChange = { target ->
                    rejected = false
                    onSetupHotspot(target) { accepted -> rejected = !accepted }
                },
                colors = SwitchDefaults.colors(
                    checkedThumbColor = Paper,
                    checkedTrackColor = Ink,
                    uncheckedThumbColor = SoftInk,
                    uncheckedTrackColor = Paper,
                    uncheckedBorderColor = Ink.copy(alpha = .18f),
                ),
            )
        }
    }
}

@Composable
private fun VolumeSetting(
    volume: Int,
    enabled: Boolean,
    onVolume: (Int) -> Unit,
) {
    var draft by rememberSaveable { mutableFloatStateOf(volume.toFloat()) }
    LaunchedEffect(volume) { draft = volume.toFloat() }
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(23.dp),
        colors = CardDefaults.cardColors(containerColor = Paper),
    ) {
        Column(
            Modifier.fillMaxWidth().padding(horizontal = 17.dp, vertical = 15.dp),
            verticalArrangement = Arrangement.spacedBy(7.dp),
        ) {
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("音量", fontWeight = FontWeight.Black, fontSize = 16.sp)
                Text("${draft.roundToInt()}%", color = SoftInk, fontWeight = FontWeight.Bold)
            }
            Slider(
                value = draft,
                onValueChange = { draft = (it / 5f).roundToInt() * 5f },
                onValueChangeFinished = { onVolume(draft.roundToInt()) },
                enabled = enabled,
                valueRange = 0f..100f,
                steps = 19,
                colors = SliderDefaults.colors(
                    thumbColor = CyanDeep,
                    activeTrackColor = CyanDeep,
                    inactiveTrackColor = Ink.copy(alpha = .10f),
                ),
            )
        }
    }
}

@Composable
private fun SettingsRow(
    title: String,
    subtitle: String,
    glyph: GlyphKind,
    color: Color,
    onClick: () -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth().clickable(onClick = onClick),
        shape = RoundedCornerShape(23.dp),
        colors = CardDefaults.cardColors(containerColor = Paper),
    ) {
        Row(Modifier.fillMaxWidth().padding(17.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(46.dp).background(color, RoundedCornerShape(15.dp)),
                contentAlignment = Alignment.Center) {
                MiniGlyph(glyph, true, Modifier.size(23.dp))
            }
            Spacer(Modifier.width(13.dp))
            Column(Modifier.weight(1f)) {
                Text(title, fontWeight = FontWeight.Black, fontSize = 16.sp)
                if (subtitle.isNotBlank()) Text(subtitle, color = SoftInk, fontSize = 12.sp)
            }
            MiniGlyph(GlyphKind.Right, true, Modifier.size(18.dp))
        }
    }
}

private enum class PetNeedKind(val label: String, val glyph: GlyphKind) {
    Food("饭饭", GlyphKind.Food),
    Water("水水", GlyphKind.Water),
    Energy("精神", GlyphKind.Sleep),
}

@Composable
private fun PetNeedsCard(
    hasState: Boolean,
    hunger: Int,
    thirst: Int,
    energy: Int,
    sleepy: Boolean,
    sleeping: Boolean,
    sleepBusy: Boolean,
    onNeeds: () -> Unit,
    onSleep: () -> Unit,
) {
    Card(
        shape = RoundedCornerShape(24.dp),
        colors = CardDefaults.cardColors(containerColor = Paper),
        border = androidx.compose.foundation.BorderStroke(1.dp, Line.copy(alpha = .62f)),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 14.dp)) {
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                PetNeedCell(
                    kind = PetNeedKind.Food,
                    wellbeing = 100 - hunger,
                    hasState = hasState,
                    urgent = hasState && hunger >= 50,
                    modifier = Modifier.weight(1f),
                    onClick = onNeeds,
                )
                PetNeedCell(
                    kind = PetNeedKind.Water,
                    wellbeing = 100 - thirst,
                    hasState = hasState,
                    urgent = hasState && thirst >= 50,
                    modifier = Modifier.weight(1f),
                    onClick = onNeeds,
                )
                PetNeedCell(
                    kind = PetNeedKind.Energy,
                    wellbeing = energy,
                    hasState = hasState,
                    urgent = sleepy,
                    modifier = Modifier.weight(1f),
                    onClick = onNeeds,
                )
            }
            if (sleeping || sleepy) {
                Spacer(Modifier.height(12.dp))
                Row(
                    Modifier.fillMaxWidth()
                        .background(if (sleeping) MintWash else ButterWash,
                            RoundedCornerShape(18.dp))
                        .padding(start = 13.dp, end = 7.dp, top = 7.dp, bottom = 7.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    MiniGlyph(GlyphKind.Sleep, true, Modifier.size(21.dp), tint = Ink)
                    Spacer(Modifier.width(9.dp))
                    Text(
                        if (sleeping) "睡得香香的" else "有点困了",
                        modifier = Modifier.weight(1f),
                        color = Ink,
                        fontWeight = FontWeight.SemiBold,
                        fontSize = 14.sp,
                    )
                    if (!sleeping) {
                        Button(
                            onClick = onSleep,
                            enabled = !sleepBusy,
                            shape = RoundedCornerShape(14.dp),
                            colors = ButtonDefaults.buttonColors(
                                containerColor = Ink,
                                contentColor = Color.White,
                                disabledContainerColor = SoftInk,
                            ),
                            contentPadding = PaddingValues(horizontal = 16.dp, vertical = 9.dp),
                        ) {
                            Text(if (sleepBusy) "哄睡中" else "去睡觉",
                                fontWeight = FontWeight.Bold, fontSize = 13.sp)
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun PetNeedCell(
    kind: PetNeedKind,
    wellbeing: Int,
    hasState: Boolean,
    urgent: Boolean,
    modifier: Modifier,
    onClick: () -> Unit,
) {
    val active = if (hasState) ((wellbeing.coerceIn(0, 100) + 19) / 20) else 0
    val accent = if (urgent) Coral else CyanDeep
    Column(
        modifier = modifier.clickable(enabled = hasState, onClick = onClick)
            .padding(vertical = 2.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Box(
            Modifier.size(34.dp).background(
                if (urgent) PeachWash else MintWash,
                CircleShape,
            ),
            contentAlignment = Alignment.Center,
        ) {
            MiniGlyph(kind.glyph, true, Modifier.size(19.dp), tint = accent)
        }
        Spacer(Modifier.height(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(3.dp)) {
            repeat(5) { index ->
                Box(
                    Modifier.size(width = 10.dp, height = 5.dp).background(
                        if (index < active) accent else Line,
                        RoundedCornerShape(50),
                    ),
                )
            }
        }
        Spacer(Modifier.height(5.dp))
        Text(
            if (!hasState) "—" else when {
                urgent && kind == PetNeedKind.Food -> "饿了"
                urgent && kind == PetNeedKind.Water -> "渴了"
                urgent && kind == PetNeedKind.Energy -> "困了"
                else -> kind.label
            },
            color = if (urgent) Coral else SoftInk,
            fontSize = 11.sp,
            fontWeight = if (urgent) FontWeight.Bold else FontWeight.Medium,
        )
    }
}

private enum class HomeArt { Chat, Memory, Drive, Play }

@Composable
private fun HomeActionCard(
    title: String,
    color: Color,
    art: HomeArt,
    modifier: Modifier,
    onClick: () -> Unit,
) {
    Card(
        modifier = modifier.height(142.dp).clickable(onClick = onClick),
        shape = RoundedCornerShape(22.dp),
        colors = CardDefaults.cardColors(containerColor = color),
        border = androidx.compose.foundation.BorderStroke(1.dp, Line.copy(alpha = .55f)),
    ) {
        Box(Modifier.fillMaxSize()) {
            Text(
                title,
                Modifier.align(Alignment.TopStart).padding(15.dp),
                fontWeight = FontWeight.Bold,
                fontSize = 16.sp,
                color = Ink,
            )
            when (art) {
                HomeArt.Drive, HomeArt.Play -> Image(
                    painter = painterResource(
                        if (art == HomeArt.Drive) R.drawable.icon_home_drive_flat_generated
                        else R.drawable.icon_home_play_flat_generated
                    ),
                    contentDescription = null,
                    modifier = Modifier.align(Alignment.BottomEnd).padding(8.dp).size(92.dp),
                    contentScale = ContentScale.Fit,
                )
                HomeArt.Chat, HomeArt.Memory -> Box(
                    Modifier.align(Alignment.BottomEnd).padding(15.dp).size(58.dp)
                        .background(Ink, RoundedCornerShape(18.dp)),
                    contentAlignment = Alignment.Center,
                ) {
                    MiniGlyph(
                        if (art == HomeArt.Chat) GlyphKind.Dialogue else GlyphKind.Character,
                        true,
                        Modifier.size(29.dp),
                        tint = Color.White,
                    )
                }
            }
        }
    }
}

@Composable
private fun PairScreen(
    phase: ScanPhase,
    devices: List<NearbyDevice>,
    detail: String?,
    selectedDevice: NearbyDevice?,
    hasPermission: () -> Boolean,
    onScan: () -> Unit,
    onDevice: (NearbyDevice) -> Unit,
    onConnect: () -> Unit,
    onBack: () -> Unit,
) {
    val permissions = remember {
        buildList {
            add(Manifest.permission.ACCESS_COARSE_LOCATION)
            add(Manifest.permission.ACCESS_FINE_LOCATION)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                add(Manifest.permission.BLUETOOTH_SCAN)
                add(Manifest.permission.BLUETOOTH_CONNECT)
            }
        }.toTypedArray()
    }
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) { grants ->
        if (grants.values.all { it }) onScan()
    }

    DetailPage("连接设备", onBack) {
        item {
            Button(
                onClick = {
                    if (hasPermission()) {
                        onScan()
                    } else {
                        permissionLauncher.launch(permissions)
                    }
                },
                modifier = Modifier.fillMaxWidth().height(54.dp),
                shape = RoundedCornerShape(18.dp),
                colors = ButtonDefaults.buttonColors(containerColor = Ink),
            ) {
                Text(if (phase == ScanPhase.Scanning) "扫描中" else "扫描设备", fontWeight = FontWeight.Bold)
            }
            if (phase in setOf(ScanPhase.Failed, ScanPhase.BluetoothOff,
                    ScanPhase.MissingPermission, ScanPhase.Unsupported) && detail != null) {
                Text(detail, color = Coral, fontSize = 12.sp, modifier = Modifier.padding(top = 8.dp))
            }
        }
        if (devices.isEmpty()) {
            item {
                EmptyDeviceCard(scanning = phase == ScanPhase.Scanning)
            }
        } else {
            items(devices, key = NearbyDevice::address) { device ->
                DeviceCard(device, selectedDevice?.address == device.address,
                    devices.count { it.name == device.name } > 1) { onDevice(device) }
            }
        }
        item {
            Button(
                onClick = onConnect,
                enabled = selectedDevice != null && phase !in setOf(ScanPhase.Connecting, ScanPhase.Connected, ScanPhase.Provisioning, ScanPhase.Provisioned),
                modifier = Modifier.fillMaxWidth().height(52.dp),
                shape = RoundedCornerShape(18.dp),
                colors = ButtonDefaults.buttonColors(containerColor = Ink),
            ) {
                Text(when (phase) {
                    ScanPhase.Connecting -> "连接中"
                    ScanPhase.Connected, ScanPhase.Provisioned -> "设备已连接"
                    else -> "连接设备"
                })
            }
        }
    }
}

@Composable
private fun NetworkSetupScreen(
    portal: PortalState,
    hasPermission: () -> Boolean,
    onOpenPortal: () -> Unit,
    onClosePortal: () -> Unit,
    onBack: () -> Unit,
) {
    val context = LocalContext.current
    val permissions = remember {
        buildList {
            add(Manifest.permission.ACCESS_FINE_LOCATION)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                add(Manifest.permission.NEARBY_WIFI_DEVICES)
            }
        }.toTypedArray()
    }
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) { granted -> if (granted.values.all { it }) onOpenPortal() }
    val openPortal = {
        if (hasPermission()) onOpenPortal() else permissionLauncher.launch(permissions)
    }
    DisposableEffect(Unit) { onDispose { onClosePortal() } }

    if (portal.connected) {
        BackHandler { onClosePortal() }
        Column(Modifier.fillMaxSize().background(CanvasWarm)) {
            Row(Modifier.fillMaxWidth().padding(20.dp),
                verticalAlignment = Alignment.CenterVertically) {
                OutlinedButton(onClick = onClosePortal) { Text("返回") }
                Spacer(Modifier.width(12.dp))
                Text("设备配网页", fontSize = 22.sp, fontWeight = FontWeight.Bold, color = Ink)
            }
            val webView = remember {
                WebView(context).apply {
                    settings.javaScriptEnabled = true
                    settings.domStorageEnabled = false
                    settings.allowFileAccess = false
                    settings.allowContentAccess = false
                    webViewClient = WebViewClient()
                    loadUrl("http://192.168.4.1/")
                }
            }
            DisposableEffect(webView) { onDispose { webView.destroy() } }
            AndroidView(factory = { webView }, modifier = Modifier.fillMaxWidth().weight(1f))
        }
        return
    }

    DetailPage("网络设置", onBack) {
        item {
            Button(
                onClick = { context.startActivity(Intent(Settings.ACTION_WIFI_SETTINGS)) },
                modifier = Modifier.fillMaxWidth().height(54.dp),
                colors = ButtonDefaults.buttonColors(containerColor = Paper, contentColor = Ink),
                shape = RoundedCornerShape(18.dp),
            ) { Text("打开手机 Wi-Fi") }
        }
        item {
            Text("先打开手机 Wi-Fi 并连接 BabyTank 热点，再回这里打开配网页。",
                color = SoftInk, fontSize = 14.sp)
        }
        item {
            Button(
                onClick = openPortal,
                modifier = Modifier.fillMaxWidth().height(54.dp),
                colors = ButtonDefaults.buttonColors(containerColor = Ink),
                shape = RoundedCornerShape(18.dp),
            ) { Text("打开设备配网页") }
            if (portal.detail.isNotBlank()) {
                Text(portal.detail, color = Coral, fontSize = 13.sp,
                    modifier = Modifier.padding(top = 10.dp))
            }
        }
    }
}

@Composable
private fun EmptyDeviceCard(scanning: Boolean) {
    Card(shape = RoundedCornerShape(24.dp), colors = CardDefaults.cardColors(containerColor = Paper)) {
        Column(Modifier.fillMaxWidth().padding(26.dp), horizontalAlignment = Alignment.CenterHorizontally) {
            MiniGlyph(GlyphKind.Signal, scanning, Modifier.size(34.dp))
            Spacer(Modifier.height(12.dp))
            Text(if (scanning) "扫描中" else "无设备", fontWeight = FontWeight.Bold)
        }
    }
}

@Composable
private fun DeviceCard(device: NearbyDevice, selected: Boolean,
    showIdentifier: Boolean, onClick: () -> Unit) {
    Card(
        modifier = Modifier.fillMaxWidth().clickable(onClick = onClick),
        shape = RoundedCornerShape(22.dp),
        colors = CardDefaults.cardColors(containerColor = if (selected) MintWash else Paper),
        border = if (selected) androidx.compose.foundation.BorderStroke(1.5.dp, CyanDeep) else null,
    ) {
        Row(Modifier.padding(17.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(44.dp).background(Ink, RoundedCornerShape(14.dp)), contentAlignment = Alignment.Center) {
                MiniGlyph(GlyphKind.Pair, true, tint = Color.White)
            }
            Spacer(Modifier.width(13.dp))
            Column(Modifier.weight(1f)) {
                Text(device.name, fontWeight = FontWeight.Black)
                if (showIdentifier) Text(device.address.takeLast(5),
                    fontSize = 11.sp, color = SoftInk)
            }
            if (selected) StatusPill("已选择", CyanDeep)
        }
    }
}

@Composable
private fun ControlScreen(
    device: NearbyDevice?,
    connectionPhase: ScanPhase,
    controlState: ControlLinkState,
    video: VideoState,
    videoFrame: StateFlow<Bitmap?>,
    onOpenConnection: () -> Unit,
    hasWifiPermission: () -> Boolean,
    onConnectCamera: () -> Unit,
    onDisconnectCamera: () -> Unit,
    onDrive: (Float, Float) -> Unit,
    onStop: () -> Unit,
    onVerifyControl: () -> Unit,
    onSetLight: (Int) -> Unit,
    onPlaySound: (Int) -> Unit,
    onFireCannon: () -> Unit,
    onRemoteMode: (Boolean) -> Unit,
    onClose: () -> Unit,
) {
    val context = LocalContext.current
    val activity = context as? Activity
    DisposableEffect(activity) {
        val previous = activity?.requestedOrientation
        activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        onDispose {
            activity?.requestedOrientation = previous ?: ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        }
    }
    val connected = device != null && connectionPhase in
        setOf(ScanPhase.Connected, ScanPhase.Provisioned)
    val controlReady = connected && controlState.phase == ControlLinkPhase.Available
    var speedPercent by remember { mutableFloatStateOf(100f) }
    val driveAtSelectedSpeed: (Float, Float) -> Unit = { x, y ->
        val scale = speedPercent / 100f
        onDrive(x * scale, y * scale)
    }
    val cameraPermissions = remember {
        when {
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU ->
                arrayOf(Manifest.permission.NEARBY_WIFI_DEVICES)
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ->
                arrayOf(Manifest.permission.ACCESS_COARSE_LOCATION, Manifest.permission.ACCESS_FINE_LOCATION)
            else -> arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }
    }
    val cameraPermissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) { grants -> if (grants.values.all { it }) onConnectCamera() }
    LaunchedEffect(connected) {
        if (connected) {
            while (isActive) {
                onRemoteMode(true)
                delay(2000)
            }
        }
    }
    DisposableEffect(Unit) {
        onDispose { onRemoteMode(false) }
    }
    LaunchedEffect(connected, video.phase) {
        if (connected && video.phase == VideoPhase.Idle && hasWifiPermission()) {
            delay(900)
            onConnectCamera()
        }
    }
    LaunchedEffect(connected) {
        /* Verify once when the remote opens. Retrying every time a failed
         * response updated controlState created a permanent BLE loop while
         * the chassis was powered off. Besides wasting radio time, those
         * Testing/Failed transitions recomposed the video screen repeatedly.
         * STOP remains available as the explicit, safe retry action. */
        if (connected) onVerifyControl()
    }
    Box(
        Modifier.fillMaxSize().background(
            Brush.verticalGradient(listOf(Ink, Color(0xFF11151A), Ink)),
        ),
    ) {
        Row(
            Modifier.fillMaxWidth().align(Alignment.TopCenter)
                .statusBarsPadding()
                .padding(horizontal = 18.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Card(
                modifier = Modifier.size(48.dp).clickable(onClick = onClose),
                shape = CircleShape,
                colors = CardDefaults.cardColors(containerColor = Paper.copy(.92f)),
            ) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    MiniGlyph(GlyphKind.Left, true, Modifier.size(21.dp))
                }
            }
            Column(Modifier.weight(1f)) {
                Text("宝宝视角", color = Color.White, fontSize = 18.sp, fontWeight = FontWeight.Black)
                Text(
                    when {
                        video.phase == VideoPhase.Streaming && controlReady -> "可以出发"
                        video.phase == VideoPhase.Streaming ->
                            "画面已连接 · 底盘未连接，点停止重试"
                        controlState.phase == ControlLinkPhase.Testing -> "正在检查底盘"
                        controlState.phase == ControlLinkPhase.Failed -> controlState.detail
                        connected -> "正在准备画面"
                        else -> "等待连接"
                    },
                    color = Color.White.copy(.68f), fontSize = 11.sp,
                )
            }
            StatusPill(
                when {
                    controlReady -> "遥控中"
                    controlState.phase == ControlLinkPhase.Testing -> "检测中"
                    controlState.phase == ControlLinkPhase.Failed -> "底盘离线"
                    else -> "准备中"
                },
                if (controlReady) Cyan else Coral,
            )
        }

        Row(
            Modifier.fillMaxSize()
                .padding(top = 76.dp, bottom = 12.dp, start = 18.dp, end = 18.dp)
                .navigationBarsPadding()
                .align(Alignment.BottomCenter),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text("方向", color = Color.White.copy(.72f), fontSize = 12.sp,
                    fontWeight = FontWeight.Bold)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Spacer(Modifier.size(68.dp))
                    DriveDirectionButton("前进", GlyphKind.Up, 0f, 1f, controlReady, driveAtSelectedSpeed, onStop)
                    Spacer(Modifier.size(68.dp))
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    DriveDirectionButton("左转", GlyphKind.Left, -1f, 0f, controlReady, driveAtSelectedSpeed, onStop)
                    Card(
                        modifier = Modifier.size(68.dp).clickable(enabled = connected) { onStop() },
                        shape = CircleShape,
                        colors = CardDefaults.cardColors(containerColor = Coral),
                    ) {
                        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                                MiniGlyph(GlyphKind.Stop, true, Modifier.size(22.dp), tint = Ink)
                                Text("停止", color = Ink, fontSize = 11.sp, fontWeight = FontWeight.Black)
                            }
                        }
                    }
                    DriveDirectionButton("右转", GlyphKind.Right, 1f, 0f, controlReady, driveAtSelectedSpeed, onStop)
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Spacer(Modifier.size(68.dp))
                    DriveDirectionButton("后退", GlyphKind.Down, 0f, -1f, controlReady, driveAtSelectedSpeed, onStop)
                    Spacer(Modifier.size(68.dp))
                }
            }

            Column(
                modifier = Modifier.weight(1f).fillMaxHeight(),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Card(
                    modifier = Modifier.weight(1f).fillMaxWidth(),
                    shape = RoundedCornerShape(28.dp),
                    colors = CardDefaults.cardColors(containerColor = Color.Black),
                    border = androidx.compose.foundation.BorderStroke(2.dp, Color.White.copy(.12f)),
                ) {
                    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                        CameraFrame(videoFrame, connected)
                        if (video.phase != VideoPhase.Streaming) {
                            Button(
                                onClick = {
                                    when {
                                        !connected -> onOpenConnection()
                                        hasWifiPermission() -> onConnectCamera()
                                        else -> cameraPermissionLauncher.launch(cameraPermissions)
                                    }
                                },
                                enabled = !connected || video.phase !in
                                    setOf(VideoPhase.Connecting, VideoPhase.Reconnecting),
                                shape = RoundedCornerShape(18.dp),
                                colors = ButtonDefaults.buttonColors(containerColor = CyanDeep),
                            ) {
                                Text(
                                    when {
                                        !connected -> "连接宝宝坦克"
                                        else -> "打开摄像头"
                                    },
                                    fontWeight = FontWeight.Black,
                                )
                            }
                        }
                    }
                }
                RemoteSpeedControl(
                    speedPercent = speedPercent,
                    enabled = controlReady,
                    onSpeedChange = { speedPercent = it },
                    onSpeedChangeFinished = onStop,
                    modifier = Modifier.fillMaxWidth(),
                )
            }

            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text("功能", color = Color.White.copy(.72f), fontSize = 12.sp,
                    fontWeight = FontWeight.Bold)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    RemoteActionButton(
                        if (video.phase == VideoPhase.Streaming) "关画面" else "摄像头",
                        GlyphKind.Camera, Cyan, connected || video.phase == VideoPhase.Streaming,
                    ) {
                        if (video.phase == VideoPhase.Streaming) onDisconnectCamera()
                        else if (hasWifiPermission()) onConnectCamera()
                        else cameraPermissionLauncher.launch(cameraPermissions)
                    }
                    RemoteActionButton("白灯", GlyphKind.Light, Cream, connected) { onSetLight(2) }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    RemoteActionButton("彩灯", GlyphKind.RainbowLight, Lavender, connected) { onSetLight(6) }
                    RemoteActionButton("夜灯", GlyphKind.NightLight, MintWash, connected) { onSetLight(8) }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    RemoteActionButton("汽车喇叭", GlyphKind.Horn, Cyan, connected) { onPlaySound(0) }
                    RemoteActionButton("开炮", GlyphKind.Cannon, Coral, connected) {
                        if (controlReady) onStop()
                        onFireCannon()
                    }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    RemoteActionButton("请让路", GlyphKind.Dialogue, Cream, connected) { onPlaySound(5) }
                    RemoteActionButton("应答音", GlyphKind.Character, Lavender, connected) {
                        onPlaySound(listOf(3, 4).random())
                    }
                }
            }
        }
    }
}

@Composable
private fun CameraFrame(frameFlow: StateFlow<Bitmap?>, connected: Boolean) {
    /* Keep high-frequency bitmap invalidation in this small recomposition
     * scope. Direction controls, sliders and action buttons remain untouched
     * when a new MJPEG frame arrives. */
    val frame by frameFlow.collectAsStateWithLifecycle()
    if (frame != null) {
        Image(
            bitmap = frame!!.asImageBitmap(),
            contentDescription = "宝宝视角实时画面",
            contentScale = ContentScale.Crop,
            modifier = Modifier.fillMaxSize(),
        )
    } else {
        BabyFace(Modifier.fillMaxSize(), connected = connected)
    }
}

@Composable
private fun RemoteSpeedControl(
    speedPercent: Float,
    enabled: Boolean,
    onSpeedChange: (Float) -> Unit,
    onSpeedChangeFinished: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Card(
        modifier = modifier.height(50.dp),
        shape = RoundedCornerShape(18.dp),
        colors = CardDefaults.cardColors(containerColor = Ink.copy(.82f)),
        border = androidx.compose.foundation.BorderStroke(1.dp, Color.White.copy(.2f)),
    ) {
        Row(
            modifier = Modifier.fillMaxSize().padding(horizontal = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(
                "速度 ${speedPercent.roundToInt()}%",
                color = Color.White.copy(alpha = if (enabled) 1f else .42f),
                fontSize = 11.sp,
                fontWeight = FontWeight.Black,
            )
            Slider(
                value = speedPercent,
                onValueChange = onSpeedChange,
                onValueChangeFinished = onSpeedChangeFinished,
                enabled = enabled,
                valueRange = 40f..100f,
                steps = 5,
                modifier = Modifier.weight(1f),
                colors = SliderDefaults.colors(
                    thumbColor = Cyan,
                    activeTrackColor = Cyan,
                    inactiveTrackColor = Color.White.copy(.18f),
                    disabledThumbColor = Color.White.copy(.24f),
                    disabledActiveTrackColor = Color.White.copy(.14f),
                    disabledInactiveTrackColor = Color.White.copy(.08f),
                ),
            )
        }
    }
}

@Composable
private fun RemoteActionButton(
    title: String,
    glyph: GlyphKind,
    color: Color,
    enabled: Boolean,
    onClick: () -> Unit,
) {
    Card(
        modifier = Modifier.size(width = 76.dp, height = 55.dp)
            .clickable(enabled = enabled, onClick = onClick),
        shape = RoundedCornerShape(18.dp),
        colors = CardDefaults.cardColors(containerColor = color.copy(alpha = if (enabled) .94f else .38f)),
    ) {
        Column(
            Modifier.fillMaxSize(),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            MiniGlyph(glyph, enabled, Modifier.size(24.dp), tint = Ink.copy(alpha = if (enabled) 1f else .45f))
            Text(title, color = Ink.copy(alpha = if (enabled) 1f else .45f),
                fontSize = 10.sp, fontWeight = FontWeight.Black)
        }
    }
}

@Composable
private fun CameraPreviewCard(
    device: NearbyDevice?,
    video: VideoState,
    hasWifiPermission: () -> Boolean,
    onConnect: () -> Unit,
    onDisconnect: () -> Unit,
) {
    val permissions = remember {
        when {
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU ->
                arrayOf(Manifest.permission.NEARBY_WIFI_DEVICES)
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ->
                arrayOf(
                    Manifest.permission.ACCESS_COARSE_LOCATION,
                    Manifest.permission.ACCESS_FINE_LOCATION,
                )
            else -> arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }
    }
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) { grants ->
        if (grants.values.all { it }) onConnect()
    }
    Card(
        shape = RoundedCornerShape(28.dp),
        colors = CardDefaults.cardColors(containerColor = Ink),
    ) {
        Column(
            Modifier.fillMaxWidth().padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("宝宝视角", color = Color.White, fontSize = 17.sp, fontWeight = FontWeight.Black)
                    Text(
                        if (video.framesPerSecond > 0) "${video.framesPerSecond} fps" else "摄像头",
                        color = Color.White.copy(.62f),
                        fontSize = 12.sp,
                        maxLines = 2,
                    )
                }
                StatusPill(
                    when (video.phase) {
                        VideoPhase.Streaming -> "直播中"
                        VideoPhase.Connecting, VideoPhase.Reconnecting -> "连接中"
                        else -> "未连接"
                    },
                    if (video.phase == VideoPhase.Streaming) Cyan else Coral,
                )
            }
            Box(
                Modifier.fillMaxWidth().height(178.dp)
                    .background(Color.Black, RoundedCornerShape(20.dp)),
                contentAlignment = Alignment.Center,
            ) {
                val frame = video.frame
                if (frame != null) {
                    Image(
                        bitmap = frame.asImageBitmap(),
                        contentDescription = "Baby Tank 摄像头实时画面",
                        contentScale = ContentScale.Crop,
                        modifier = Modifier.fillMaxSize(),
                    )
                } else {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        MiniGlyph(GlyphKind.Camera, true, Modifier.size(34.dp), tint = Color.White.copy(.62f))
                        Spacer(Modifier.height(10.dp))
                        Text("摄像头", color = Color.White.copy(.66f))
                    }
                }
            }
            if (video.phase == VideoPhase.Failed || video.phase == VideoPhase.NeedsManualWifi) {
                Text(video.detail, color = Color.White.copy(.8f), fontSize = 12.sp)
            }
            if (video.phase != VideoPhase.Streaming && device != null) {
                Button(
                    onClick = {
                        if (hasWifiPermission()) {
                            onConnect()
                        } else {
                            permissionLauncher.launch(permissions)
                        }
                    },
                    enabled = video.phase !in setOf(VideoPhase.Connecting, VideoPhase.Reconnecting),
                    modifier = Modifier.fillMaxWidth().height(44.dp),
                    shape = RoundedCornerShape(17.dp),
                    colors = ButtonDefaults.buttonColors(containerColor = CyanDeep),
                ) { Text("连接摄像头", fontWeight = FontWeight.Black) }
            } else {
                OutlinedButton(
                    onClick = onDisconnect,
                    modifier = Modifier.fillMaxWidth().height(42.dp),
                    shape = RoundedCornerShape(17.dp),
                ) { Text("断开视频", color = Color.White) }
            }
        }
    }
}

@Composable
private fun DrivePad(
    enabled: Boolean,
    onDrive: (Float, Float) -> Unit,
    onStop: () -> Unit,
) {
    Column(horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(8.dp)) {
        DriveDirectionButton("前进", GlyphKind.Up, 0f, 1f, enabled, onDrive, onStop)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically) {
            DriveDirectionButton("左转", GlyphKind.Left, -1f, 0f, enabled, onDrive, onStop)
            Card(
                modifier = Modifier.size(68.dp).clickable(enabled = enabled) { onStop() },
                shape = CircleShape,
                colors = CardDefaults.cardColors(containerColor = Coral),
            ) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        MiniGlyph(GlyphKind.Stop, true, Modifier.size(25.dp), tint = Ink)
                        Text("停止", color = Ink, fontWeight = FontWeight.Black, fontSize = 12.sp)
                    }
                }
            }
            DriveDirectionButton("右转", GlyphKind.Right, 1f, 0f, enabled, onDrive, onStop)
        }
        DriveDirectionButton("后退", GlyphKind.Down, 0f, -1f, enabled, onDrive, onStop)
    }
}

@Composable
private fun DriveDirectionButton(
    label: String,
    glyph: GlyphKind,
    x: Float,
    y: Float,
    enabled: Boolean,
    onDrive: (Float, Float) -> Unit,
    onStop: () -> Unit,
) {
    var pressed by remember { mutableStateOf(false) }
    val currentOnDrive by rememberUpdatedState(onDrive)
    val currentOnStop by rememberUpdatedState(onStop)
    LaunchedEffect(pressed, enabled) {
        if (pressed && enabled) {
            delay(90)
            while (isActive) {
                currentOnDrive(x, y)
                delay(90)
            }
        }
    }
    DisposableEffect(enabled) {
        onDispose { if (pressed) currentOnStop() }
    }
    Card(
        modifier = Modifier.size(68.dp).pointerInput(enabled) {
            detectTapGestures(onPress = {
                if (!enabled) return@detectTapGestures
                pressed = true
                currentOnDrive(x, y)
                tryAwaitRelease()
                pressed = false
                currentOnStop()
            })
        },
        shape = RoundedCornerShape(21.dp),
        colors = CardDefaults.cardColors(
            containerColor = if (pressed) Cyan else Color.White.copy(alpha = if (enabled) .14f else .06f),
        ),
        border = androidx.compose.foundation.BorderStroke(
            1.dp, Color.White.copy(alpha = if (enabled) .16f else .06f),
        ),
    ) {
        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Column(horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(4.dp)) {
                MiniGlyph(glyph, enabled, Modifier.size(25.dp), tint = if (enabled) Color.White else Color.White.copy(.32f))
                Text(label, color = if (enabled) Color.White else Color.White.copy(.32f),
                    fontSize = 12.sp, fontWeight = FontWeight.Bold)
            }
        }
    }
}

@Composable
private fun HardwareLabScreen(
    device: NearbyDevice?,
    controlDetail: String,
    diagnostics: DiagnosticState,
    onBleTest: () -> Unit,
    onControlTest: () -> Unit,
    onCameraTest: () -> Unit,
    onRefresh: () -> Unit,
    onExpression: (Int) -> Unit,
    onLook: (Int, Int) -> Unit,
    onLight: (Int) -> Unit,
    onSpeaker: (Int) -> Unit,
    onListen: () -> Unit,
    onStopListen: () -> Unit,
    onBack: () -> Unit,
) {
    val online = device != null
    val snapshot = diagnostics.snapshot
    val conversationActive = (snapshot?.conversationPhase ?: 0) in 1..5
    DetailPage(
        title = "功能检测",
        onBack = onBack,
        trailing = {
            StatusPill(if (online) "已连接" else "离线", if (online) CyanDeep else Coral)
        },
    ) {
        item {
        Card(shape = RoundedCornerShape(24.dp), colors = CardDefaults.cardColors(containerColor = Ink)) {
            Row(Modifier.fillMaxWidth().padding(18.dp), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("设备状态", color = Color.White, fontSize = 17.sp, fontWeight = FontWeight.Black)
                    Text(diagnostics.detail, color = Color.White.copy(.64f), fontSize = 12.sp)
                }
                Button(
                    onClick = onRefresh,
                    enabled = online && diagnostics.phase != DiagnosticPhase.Running,
                    colors = ButtonDefaults.buttonColors(containerColor = CyanDeep),
                    shape = RoundedCornerShape(15.dp),
                ) { Text("刷新") }
            }
        }
    }
    item {
        LabPanel("表情与屏幕", "点一下，设备屏幕立即播放") {
            BabyExpressions.chunked(3).forEach { row ->
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    row.forEach { expression ->
                        OutlinedButton(
                            onClick = { onExpression(expression.index) },
                            enabled = online,
                            modifier = Modifier.weight(1f),
                            contentPadding = PaddingValues(horizontal = 4.dp, vertical = 10.dp),
                            shape = RoundedCornerShape(14.dp),
                        ) { Text(expression.title, fontSize = 12.sp, maxLines = 1) }
                    }
                    repeat(3 - row.size) { Spacer(Modifier.weight(1f)) }
                }
            }
            HorizontalDivider(color = Ink.copy(.08f))
            Text("视线", fontWeight = FontWeight.Black, fontSize = 14.sp)
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf(
                    Triple("左", -100, 0), Triple("上", 0, -100),
                    Triple("下", 0, 100), Triple("右", 100, 0),
                ).forEach { (label, x, y) ->
                    OutlinedButton(
                        onClick = { onLook(x, y) }, enabled = online,
                        modifier = Modifier.weight(1f), shape = RoundedCornerShape(14.dp),
                        contentPadding = PaddingValues(horizontal = 4.dp),
                    ) { Text(label) }
                }
            }
        }
    }
    item {
        LabPanel("声音与对话", "使用设备真实扬声器和 AI 链路") {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = { onSpeaker(0) }, enabled = online,
                    modifier = Modifier.weight(1f), shape = RoundedCornerShape(16.dp),
                    colors = ButtonDefaults.buttonColors(containerColor = Ink),
                ) { Text("提示音") }
                OutlinedButton(
                    onClick = if (conversationActive) onStopListen else onListen,
                    enabled = online,
                    modifier = Modifier.weight(1f), shape = RoundedCornerShape(16.dp),
                ) { Text(if (conversationActive) "结束对话" else "开始对话") }
            }
        }
    }
    item {
        LabPanel("触摸", "触摸设备后点刷新") {
            listOf("顶部", "左侧", "右侧").forEachIndexed { index, name ->
                val active = snapshot?.touchActiveMask?.and(1 shl index) != 0
                MetricRow(
                    name,
                    if (snapshot == null) "--" else
                        "${snapshot.touchDeltas[index]}‰ / 阈值 ${snapshot.touchThresholds[index]}‰ · ${snapshot.touchCounts[index]} 次",
                    active,
                )
            }
        }
    }
    item {
        LabPanel("底盘与灯光", "运动测试请回到遥控页") {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf("常亮" to 2, "彩光" to 6, "夜灯" to 8).forEach { (label, mode) ->
                    OutlinedButton(
                        onClick = { onLight(mode) }, enabled = online,
                        modifier = Modifier.weight(1f), shape = RoundedCornerShape(14.dp),
                        contentPadding = PaddingValues(horizontal = 4.dp),
                    ) { Text(label, fontSize = 12.sp) }
                }
            }
            Button(
                onClick = onCameraTest, enabled = online,
                modifier = Modifier.fillMaxWidth(), shape = RoundedCornerShape(16.dp),
                colors = ButtonDefaults.buttonColors(containerColor = Ink),
            ) { Text("打开图传遥控") }
        }
    }
    item {
        val snapshot = diagnostics.snapshot
        LabPanel("连接与系统", null) {
            MetricRow("蓝牙", if (online) "已连接" else "未连接", online)
            MetricRow("底盘", if (snapshot?.chassisVerified == true) "回显正常" else controlDetail, snapshot?.chassisVerified == true)
            MetricRow("家庭网络", if (snapshot?.wifiConnected == true) "已连接" else "未连接", snapshot?.wifiConnected == true)
            MetricRow("AI 服务", if (snapshot?.cloudReady == true) "已配置" else "不可用", snapshot?.cloudReady == true)
            MetricRow("摄像头", if (snapshot?.cameraReady == true) "已就绪" else "不可用", snapshot?.cameraReady == true)
            if (snapshot != null) {
                MetricRow("运行时间", "${snapshot.uptimeSeconds / 60} 分钟", true)
                MetricRow("可用内存", "${snapshot.freeInternalHeap / 1024} KB", snapshot.freeInternalHeap > 64 * 1024)
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = onBleTest, modifier = Modifier.weight(1f), shape = RoundedCornerShape(14.dp)) { Text("重新连接") }
                OutlinedButton(onClick = onControlTest, enabled = online, modifier = Modifier.weight(1f), shape = RoundedCornerShape(14.dp)) { Text("检查底盘") }
            }
        }
    }
        item {
        Card(shape = RoundedCornerShape(20.dp), colors = CardDefaults.cardColors(containerColor = Cream.copy(.42f))) {
            Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(5.dp)) {
                Text("尚未接通", fontWeight = FontWeight.Black)
                Text("电池电量 · IMU 姿态 · OTA 回滚", color = SoftInk, fontSize = 13.sp)
            }
        }
    }
    }
}

@Composable
private fun LabPanel(title: String, subtitle: String?, content: @Composable ColumnScope.() -> Unit) {
    Card(shape = RoundedCornerShape(24.dp), colors = CardDefaults.cardColors(containerColor = Paper)) {
        Column(
            Modifier.fillMaxWidth().padding(17.dp),
            verticalArrangement = Arrangement.spacedBy(11.dp),
        ) {
            Text(title, fontSize = 17.sp, fontWeight = FontWeight.Black)
            if (subtitle != null) Text(subtitle, color = SoftInk, fontSize = 12.sp)
            content()
        }
    }
}

@Composable
private fun MetricRow(label: String, value: String, good: Boolean) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(8.dp).background(if (good) CyanDeep else Coral, CircleShape))
        Spacer(Modifier.width(9.dp))
        Text(label, Modifier.weight(1f), fontWeight = FontWeight.SemiBold, fontSize = 13.sp)
        Text(value, color = SoftInk, fontSize = 12.sp, maxLines = 1)
    }
}

@Composable
private fun CharacterScreen(
    device: NearbyDevice?,
    state: ParentMemoryState,
    onSync: (Int, Int, Int) -> Unit,
    onBack: () -> Unit,
) {
    var curiosity by remember(state.personality.revision) {
        mutableFloatStateOf(state.personality.curiosity / 100f) }
    var warmth by remember(state.personality.revision) {
        mutableFloatStateOf(state.personality.warmth / 100f) }
    var talkative by remember(state.personality.revision) {
        mutableFloatStateOf(state.personality.talkativeness / 100f) }
    DetailPage("我的宝宝", onBack) {
        item {
            Card(shape = RoundedCornerShape(28.dp), colors = CardDefaults.cardColors(containerColor = Ink)) {
                Row(Modifier.fillMaxWidth().padding(18.dp), verticalAlignment = Alignment.CenterVertically) {
                    BabyFace(Modifier.size(112.dp), connected = device != null)
                    Spacer(Modifier.width(16.dp))
                    Column(verticalArrangement = Arrangement.spacedBy(7.dp)) {
                        Text("喵喵同学", color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.Black)
                    }
                }
            }
        }
        item { TraitSlider("好奇心", curiosity) { curiosity = it } }
        item { TraitSlider("亲近感", warmth) { warmth = it } }
        item { TraitSlider("话语量", talkative) { talkative = it } }
        item {
            Button(
                onClick = { onSync((curiosity * 100).toInt(),
                                   (warmth * 100).toInt(),
                                   (talkative * 100).toInt()) },
                enabled = state.pairedDeviceId != null && !state.busy,
                modifier = Modifier.fillMaxWidth().height(52.dp),
                shape = RoundedCornerShape(18.dp),
            ) { Text("保存到云端") }
        }
        item { Text(if (state.headFresh &&
            state.headPersonalityRevision == state.personality.revision)
            "头部已确认版本 ${state.personality.revision}"
            else "云端版本 ${state.personality.revision}；等待头部同步",
            color = SoftInk, fontSize = 12.sp) }
        if (state.error != null) item { Text(state.error, color = SoftInk, fontSize = 12.sp) }
    }
}

@Composable
private fun TraitSlider(title: String, value: Float, onValue: (Float) -> Unit) {
    Card(shape = RoundedCornerShape(22.dp), colors = CardDefaults.cardColors(containerColor = Paper)) {
        Column(Modifier.padding(17.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(title, fontWeight = FontWeight.Black, modifier = Modifier.weight(1f))
                Text("${(value * 100).toInt()}", color = CyanDeep, fontWeight = FontWeight.Black)
            }
            Slider(value = value, onValueChange = onValue)
        }
    }
}
