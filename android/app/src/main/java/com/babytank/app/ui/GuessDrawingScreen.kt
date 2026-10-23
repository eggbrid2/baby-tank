package com.babytank.app.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.babytank.app.DrawingGuessState
import com.babytank.app.R
import com.babytank.app.SketchPoint
import com.babytank.app.SketchStroke

@Composable
internal fun GuessDrawingScreen(
    state: DrawingGuessState,
    paired: Boolean,
    onGuess: (List<SketchStroke>) -> Unit,
    onCorrect: () -> Unit,
    onWrong: () -> Unit,
    onReset: () -> Unit,
    onPair: () -> Unit,
    onBack: () -> Unit,
) {
    var strokes by remember { mutableStateOf<List<SketchStroke>>(emptyList()) }
    var canvasSize by remember { mutableStateOf(IntSize.Zero) }
    val canDraw = paired && !state.sending && state.guess == null && !state.completed

    fun resetGame() {
        strokes = emptyList()
        onReset()
    }

    BackHandler(onBack = onBack)
    Column(
        Modifier.fillMaxSize().background(CanvasWarm).padding(horizontal = 18.dp),
    ) {
        Row(
            Modifier.fillMaxWidth().padding(top = 14.dp, bottom = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            OutlinedButton(onClick = onBack) { Text("返回") }
            Text(
                "猜猜我画的",
                Modifier.padding(start = 14.dp).weight(1f),
                color = Ink,
                fontSize = 23.sp,
                fontWeight = FontWeight.Bold,
            )
            if (strokes.isNotEmpty() || state.round > 1 || state.guess != null) {
                OutlinedButton(
                    onClick = ::resetGame,
                    enabled = !state.sending,
                ) { Text("重新开始") }
            }
        }
        Spacer(Modifier.height(4.dp))
        Card(
            Modifier.fillMaxWidth().weight(1f),
            shape = RoundedCornerShape(26.dp),
            colors = CardDefaults.cardColors(containerColor = Paper),
        ) {
            Box(
                Modifier.fillMaxSize().padding(10.dp)
                    .border(2.dp, Ink.copy(alpha = .14f), RoundedCornerShape(18.dp))
                    .onSizeChanged { canvasSize = it }
                    .pointerInput(canDraw, canvasSize) {
                        if (!canDraw || canvasSize.width <= 0 || canvasSize.height <= 0) {
                            return@pointerInput
                        }
                        awaitEachGesture {
                            val down = awaitFirstDown(requireUnconsumed = false)
                            var points = listOf(normalizedPoint(down.position, canvasSize))
                            strokes = strokes + SketchStroke(points)
                            drag(down.id) { change ->
                                change.consume()
                                val next = normalizedPoint(change.position, canvasSize)
                                if ((next.x - points.last().x) * (next.x - points.last().x) +
                                    (next.y - points.last().y) * (next.y - points.last().y) > .00002f
                                ) {
                                    points = points + next
                                    strokes = strokes.dropLast(1) + SketchStroke(points)
                                }
                            }
                        }
                    },
            ) {
                Canvas(Modifier.fillMaxSize()) {
                    strokes.forEach { stroke ->
                        if (stroke.points.size == 1) {
                            drawCircle(Ink, radius = 4.dp.toPx(), center = Offset(
                                stroke.points[0].x * size.width,
                                stroke.points[0].y * size.height,
                            ))
                        } else if (stroke.points.size > 1) {
                            val path = Path().apply {
                                moveTo(stroke.points[0].x * size.width,
                                    stroke.points[0].y * size.height)
                                stroke.points.drop(1).forEach {
                                    lineTo(it.x * size.width, it.y * size.height)
                                }
                            }
                            drawPath(path, Ink, style = Stroke(
                                width = 8.dp.toPx(), cap = StrokeCap.Round,
                                join = StrokeJoin.Round))
                        }
                    }
                }
                if (strokes.isEmpty()) {
                    Text(
                        "在这里画一个东西",
                        Modifier.align(Alignment.Center),
                        color = Ink.copy(alpha = .28f),
                        fontSize = 18.sp,
                        fontWeight = FontWeight.Bold,
                    )
                }
            }
        }
        Spacer(Modifier.height(12.dp))

        if (!paired) {
            Card(colors = CardDefaults.cardColors(containerColor = Cream),
                shape = RoundedCornerShape(18.dp)) {
                Row(Modifier.fillMaxWidth().padding(14.dp),
                    verticalAlignment = Alignment.CenterVertically) {
                    Text("先配对机器人才能开始", Modifier.weight(1f), color = Ink)
                    Button(onClick = onPair) { Text("去配对") }
                }
            }
        } else if (state.guess != null || state.completed) {
            Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
                Text(
                    if (state.completed) "猜对啦！" else "我猜是：${state.guess}",
                    color = Ink, fontSize = 22.sp, fontWeight = FontWeight.Black,
                )
                Spacer(Modifier.height(8.dp))
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(10.dp, Alignment.CenterHorizontally),
                ) {
                    if (!state.completed) {
                        PictureChoice(
                            label = "猜对了", drawable = R.drawable.game_action_correct,
                            modifier = Modifier.weight(1f), onClick = onCorrect)
                        PictureChoice(
                            label = "你猜错了", drawable = R.drawable.flat_game_lose,
                            modifier = Modifier.weight(1f), onClick = onWrong)
                    } else {
                        PictureChoice(
                            label = "再画一个", drawable = R.drawable.game_action_draw_more,
                            modifier = Modifier.fillMaxWidth(.56f), onClick = ::resetGame)
                    }
                }
            }
        }
        if (state.sending && state.status.isNotBlank()) {
            Text(state.status, Modifier.padding(top = 8.dp), color = CyanDeep, fontSize = 12.sp)
        }
        if (state.error != null) {
            Text(state.error, Modifier.padding(top = 6.dp), color = Coral, fontSize = 12.sp)
        }
        if (state.guess == null && !state.completed) {
            Box(Modifier.fillMaxWidth().padding(vertical = 10.dp), contentAlignment = Alignment.Center) {
                GameGuessButton(
                    label = if (state.sending) "正在猜" else "让它猜",
                    enabled = canDraw && strokes.isNotEmpty(),
                    onClick = { onGuess(strokes) },
                )
            }
        }
    }
}

@Composable
private fun PictureChoice(
    label: String,
    drawable: Int,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    val scale by animateFloatAsState(if (pressed) .96f else 1f, label = "choicePress")
    val surface = when (drawable) {
        R.drawable.game_action_correct -> Color(0xFFE5F8E1)
        R.drawable.game_action_draw_more -> Color(0xFFFFEDC5)
        else -> Color(0xFFDFF4F1)
    }
    Box(modifier.height(120.dp)) {
        Box(
            Modifier.fillMaxWidth().height(112.dp).padding(top = 7.dp)
                .background(Ink, RoundedCornerShape(24.dp)),
        )
        Card(
            modifier = Modifier.fillMaxWidth().height(112.dp)
                .graphicsLayer {
                    scaleX = scale
                    scaleY = scale
                    translationY = if (pressed) 6.dp.toPx() else 0f
                },
            onClick = onClick,
            shape = RoundedCornerShape(24.dp),
            colors = CardDefaults.cardColors(containerColor = surface),
            border = BorderStroke(2.dp, Ink),
            interactionSource = interaction,
        ) {
            Row(
                Modifier.fillMaxSize().padding(horizontal = 10.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.Center,
            ) {
                Image(painterResource(drawable), null, Modifier.size(68.dp),
                    contentScale = ContentScale.Fit)
                Text(label, color = Ink, fontSize = 17.sp, fontWeight = FontWeight.Black)
            }
        }
    }
}

@Composable
private fun GameGuessButton(label: String, enabled: Boolean, onClick: () -> Unit) {
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    val scale by animateFloatAsState(if (pressed) .965f else 1f, label = "guessPress")
    Box(Modifier.fillMaxWidth(.62f).height(74.dp)) {
        Box(
            Modifier.fillMaxWidth().height(68.dp).padding(top = 6.dp)
                .background(if (enabled) Ink else Ink.copy(alpha = .22f),
                    RoundedCornerShape(24.dp)),
        )
        Card(
            modifier = Modifier.fillMaxWidth().height(68.dp)
                .graphicsLayer {
                    scaleX = scale
                    scaleY = scale
                    translationY = if (pressed) 5.dp.toPx() else 0f
                },
            onClick = onClick,
            enabled = enabled,
            shape = RoundedCornerShape(24.dp),
            colors = CardDefaults.cardColors(
                containerColor = Color(0xFF56D6BA),
                disabledContainerColor = Color(0xFFE3E0DA),
            ),
            border = BorderStroke(2.dp, if (enabled) Ink else Ink.copy(alpha = .18f)),
            interactionSource = interaction,
        ) {
            Row(
                Modifier.fillMaxSize(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.Center,
            ) {
                Image(
                    painterResource(R.drawable.game_action_guess), null,
                    Modifier.size(58.dp), contentScale = ContentScale.Fit)
                Text(
                    label,
                    Modifier.padding(start = 8.dp),
                    color = if (enabled) Ink else Ink.copy(alpha = .35f),
                    fontSize = 19.sp,
                    fontWeight = FontWeight.Black,
                )
            }
        }
    }
}

private fun normalizedPoint(position: Offset, size: IntSize) = SketchPoint(
    x = (position.x / size.width.toFloat()).coerceIn(0f, 1f),
    y = (position.y / size.height.toFloat()).coerceIn(0f, 1f),
)
