package com.babytank.app.ui

import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.keyframes
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathFillType
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.cos
import kotlin.math.sin

@Composable
fun BabyFace(
    modifier: Modifier = Modifier,
    connected: Boolean = false,
    mood: String? = null,
) {
    val transition = rememberInfiniteTransition(label = "baby-face")
    val blink by transition.animateFloat(
        initialValue = 1f,
        targetValue = 1f,
        animationSpec = infiniteRepeatable(
            keyframes {
                durationMillis = 6800
                1f at 0
                1f at 3300
                0.06f at 3390
                1f at 3520
                1f at 3740
                0.12f at 3820
                1f at 3940
                1f at 6800
            },
        ),
        label = "blink",
    )
    Canvas(modifier = modifier) {
        val radius = size.minDimension * 0.13f
        drawRoundRect(
            color = Color(0xFF101314),
            cornerRadius = CornerRadius(radius),
        )
        val eyeColor = if (connected) Cyan else Cyan.copy(alpha = .55f)
        val eyeWidth = size.minDimension * .095f
        val sleepy = mood == "sleepy"
        val eyeHeight = size.minDimension * (if (sleepy) .024f else .15f) * blink
        val eyeY = size.height * .43f - eyeHeight / 2f
        listOf(size.width * .37f, size.width * .63f).forEach { centerX ->
            drawRoundRect(
                color = eyeColor,
                topLeft = Offset(centerX - eyeWidth / 2f, eyeY),
                size = Size(eyeWidth, eyeHeight.coerceAtLeast(3f)),
                cornerRadius = CornerRadius(eyeWidth / 2f),
            )
        }
        val mouthWidth = size.minDimension * (if (mood == "happy" || mood == "excited") .24f else .15f)
        if (mood == "sad" || mood == "worried" || mood == "annoyed") {
            drawArc(eyeColor, 200f, 140f, false,
                Offset(size.width / 2f - mouthWidth / 2f, size.height * .64f),
                Size(mouthWidth, size.minDimension * .08f),
                style = Stroke(width = size.minDimension * .018f, cap = StrokeCap.Round))
        } else {
            drawArc(eyeColor, 20f, 140f, false,
                Offset(size.width / 2f - mouthWidth / 2f, size.height * .6f),
                Size(mouthWidth, size.minDimension * .08f),
                style = Stroke(width = size.minDimension * .018f, cap = StrokeCap.Round))
        }
    }
}

enum class GlyphKind {
    Home, Pair, Control, Lab, Character, Signal, Camera, Settings,
    Up, Down, Left, Right, Stop, Light, RainbowLight, NightLight,
    Horn, Cannon, Dialogue, Food, Water, Sleep,
}

@Composable
fun MiniGlyph(
    kind: GlyphKind,
    selected: Boolean,
    modifier: Modifier = Modifier,
    tint: Color? = null,
) {
    val color = tint ?: if (selected) Ink else SoftInk.copy(alpha = 0.58f)
    Canvas(modifier = modifier.size(23.dp)) {
        val stroke = Stroke(width = 2.1.dp.toPx())
        when (kind) {
            GlyphKind.Home -> {
                drawLine(color, Offset(size.width * .18f, size.height * .48f), Offset(size.width * .5f, size.height * .2f), stroke.width)
                drawLine(color, Offset(size.width * .5f, size.height * .2f), Offset(size.width * .82f, size.height * .48f), stroke.width)
                drawRoundRect(color, Offset(size.width * .26f, size.height * .43f), Size(size.width * .48f, size.height * .42f), CornerRadius(4f), style = stroke)
            }
            GlyphKind.Pair -> {
                val x = size.width
                val y = size.height
                drawLine(color, Offset(x * .5f, y * .12f),
                    Offset(x * .5f, y * .88f), stroke.width, StrokeCap.Round)
                drawLine(color, Offset(x * .5f, y * .12f),
                    Offset(x * .73f, y * .31f), stroke.width, StrokeCap.Round)
                drawLine(color, Offset(x * .73f, y * .31f),
                    Offset(x * .27f, y * .69f), stroke.width, StrokeCap.Round)
                drawLine(color, Offset(x * .27f, y * .31f),
                    Offset(x * .73f, y * .69f), stroke.width, StrokeCap.Round)
                drawLine(color, Offset(x * .73f, y * .69f),
                    Offset(x * .5f, y * .88f), stroke.width, StrokeCap.Round)
            }
            GlyphKind.Signal -> {
                drawCircle(color, size.width * .09f, Offset(size.width * .5f, size.height * .76f))
                drawArc(color, 205f, 130f, false, Offset(size.width * .32f, size.height * .42f), Size(size.width * .36f, size.height * .36f), style = stroke)
                drawArc(color, 205f, 130f, false, Offset(size.width * .16f, size.height * .23f), Size(size.width * .68f, size.height * .68f), style = stroke)
            }
            GlyphKind.Control -> {
                drawCircle(color, size.width * .34f, center, style = stroke)
                drawCircle(color, size.width * .10f, center)
            }
            GlyphKind.Lab -> {
                drawCircle(color, size.width * .30f, center, style = stroke)
                drawCircle(color, size.width * .08f, center)
                repeat(4) { index ->
                    val horizontal = index % 2 == 0
                    val start = if (horizontal) Offset(size.width * .08f, size.height * .5f) else Offset(size.width * .5f, size.height * .08f)
                    val end = if (horizontal) Offset(size.width * .92f, size.height * .5f) else Offset(size.width * .5f, size.height * .92f)
                    drawLine(color, start, end, stroke.width)
                }
            }
            GlyphKind.Settings -> {
                val gear = Path().apply { fillType = PathFillType.EvenOdd }
                repeat(8) { index ->
                    val axis = index * Math.PI / 4.0 - Math.PI / 2.0
                    val profile = arrayOf(
                        -Math.PI / 8.0 to .30f,
                        -Math.PI / 16.0 to .30f,
                        -Math.PI / 16.0 to .41f,
                        Math.PI / 16.0 to .41f,
                        Math.PI / 16.0 to .30f,
                        Math.PI / 8.0 to .30f,
                    )
                    profile.forEach { (offset, radius) ->
                        val x = size.width * (.5f + radius * cos(axis + offset).toFloat())
                        val y = size.height * (.5f + radius * sin(axis + offset).toFloat())
                        if (index == 0 && offset == -Math.PI / 8.0) gear.moveTo(x, y)
                        else gear.lineTo(x, y)
                    }
                }
                gear.close()
                gear.addOval(Rect(
                    center = center,
                    radius = size.width * .115f,
                ))
                drawPath(gear, color)
            }
            GlyphKind.Character -> {
                drawCircle(color, size.width * .20f, Offset(size.width * .5f, size.height * .35f), style = stroke)
                drawArc(color, 190f, 160f, false, Offset(size.width * .2f, size.height * .5f), Size(size.width * .6f, size.height * .4f), style = stroke)
            }
            GlyphKind.Camera -> {
                drawRoundRect(color, Offset(size.width * .12f, size.height * .28f), Size(size.width * .76f, size.height * .5f), CornerRadius(5f), style = stroke)
                drawCircle(color, size.width * .13f, center, style = stroke)
            }
            GlyphKind.Up, GlyphKind.Down, GlyphKind.Left, GlyphKind.Right -> {
                val start: Offset
                val end: Offset
                val wingA: Offset
                val wingB: Offset
                when (kind) {
                    GlyphKind.Up -> {
                        start = Offset(size.width * .5f, size.height * .82f)
                        end = Offset(size.width * .5f, size.height * .18f)
                        wingA = Offset(size.width * .25f, size.height * .43f)
                        wingB = Offset(size.width * .75f, size.height * .43f)
                    }
                    GlyphKind.Down -> {
                        start = Offset(size.width * .5f, size.height * .18f)
                        end = Offset(size.width * .5f, size.height * .82f)
                        wingA = Offset(size.width * .25f, size.height * .57f)
                        wingB = Offset(size.width * .75f, size.height * .57f)
                    }
                    GlyphKind.Left -> {
                        start = Offset(size.width * .82f, size.height * .5f)
                        end = Offset(size.width * .18f, size.height * .5f)
                        wingA = Offset(size.width * .43f, size.height * .25f)
                        wingB = Offset(size.width * .43f, size.height * .75f)
                    }
                    else -> {
                        start = Offset(size.width * .18f, size.height * .5f)
                        end = Offset(size.width * .82f, size.height * .5f)
                        wingA = Offset(size.width * .57f, size.height * .25f)
                        wingB = Offset(size.width * .57f, size.height * .75f)
                    }
                }
                drawLine(color, start, end, stroke.width)
                drawLine(color, end, wingA, stroke.width)
                drawLine(color, end, wingB, stroke.width)
            }
            GlyphKind.Stop -> drawRoundRect(
                color,
                Offset(size.width * .27f, size.height * .27f),
                Size(size.width * .46f, size.height * .46f),
                CornerRadius(size.width * .08f),
            )
            GlyphKind.Light, GlyphKind.RainbowLight, GlyphKind.NightLight -> {
                drawCircle(color, size.width * .22f,
                    Offset(size.width * .5f, size.height * .4f), style = stroke)
                drawLine(color, Offset(size.width * .38f, size.height * .65f),
                    Offset(size.width * .62f, size.height * .65f), stroke.width)
                drawLine(color, Offset(size.width * .42f, size.height * .77f),
                    Offset(size.width * .58f, size.height * .77f), stroke.width)
                if (kind == GlyphKind.RainbowLight) {
                    drawArc(color, 205f, 130f, false,
                        Offset(size.width * .05f, size.height * .04f),
                        Size(size.width * .9f, size.height * .9f), style = stroke)
                } else if (kind == GlyphKind.NightLight) {
                    drawCircle(color, size.width * .09f,
                        Offset(size.width * .76f, size.height * .2f))
                }
            }
            GlyphKind.Horn -> {
                drawLine(color, Offset(size.width * .16f, size.height * .41f),
                    Offset(size.width * .38f, size.height * .41f), stroke.width)
                drawLine(color, Offset(size.width * .16f, size.height * .59f),
                    Offset(size.width * .38f, size.height * .59f), stroke.width)
                drawLine(color, Offset(size.width * .38f, size.height * .41f),
                    Offset(size.width * .69f, size.height * .2f), stroke.width)
                drawLine(color, Offset(size.width * .38f, size.height * .59f),
                    Offset(size.width * .69f, size.height * .8f), stroke.width)
                drawLine(color, Offset(size.width * .69f, size.height * .2f),
                    Offset(size.width * .69f, size.height * .8f), stroke.width)
                drawArc(color, -55f, 110f, false,
                    Offset(size.width * .65f, size.height * .25f),
                    Size(size.width * .28f, size.height * .5f), style = stroke)
            }
            GlyphKind.Cannon -> {
                drawRoundRect(color, Offset(size.width * .18f, size.height * .43f),
                    Size(size.width * .48f, size.height * .2f),
                    CornerRadius(size.width * .05f))
                drawCircle(color, size.width * .12f,
                    Offset(size.width * .35f, size.height * .72f), style = stroke)
                drawLine(color, Offset(size.width * .7f, size.height * .31f),
                    Offset(size.width * .78f, size.height * .16f), stroke.width)
                drawLine(color, Offset(size.width * .76f, size.height * .51f),
                    Offset(size.width * .94f, size.height * .51f), stroke.width)
                drawLine(color, Offset(size.width * .7f, size.height * .69f),
                    Offset(size.width * .78f, size.height * .84f), stroke.width)
            }
            GlyphKind.Dialogue -> {
                drawRoundRect(color, Offset(size.width * .12f, size.height * .17f),
                    Size(size.width * .76f, size.height * .57f),
                    CornerRadius(size.width * .12f), style = stroke)
                drawLine(color, Offset(size.width * .32f, size.height * .73f),
                    Offset(size.width * .24f, size.height * .88f), stroke.width)
                drawCircle(color, size.width * .045f,
                    Offset(size.width * .39f, size.height * .46f))
                drawCircle(color, size.width * .045f,
                    Offset(size.width * .61f, size.height * .46f))
            }
            GlyphKind.Food -> {
                drawArc(color, 0f, 180f, false,
                    Offset(size.width * .16f, size.height * .31f),
                    Size(size.width * .68f, size.height * .52f), style = stroke)
                drawLine(color, Offset(size.width * .2f, size.height * .57f),
                    Offset(size.width * .8f, size.height * .57f), stroke.width,
                    StrokeCap.Round)
                drawLine(color, Offset(size.width * .33f, size.height * .78f),
                    Offset(size.width * .67f, size.height * .78f), stroke.width,
                    StrokeCap.Round)
            }
            GlyphKind.Water -> {
                val drop = Path().apply {
                    moveTo(size.width * .5f, size.height * .12f)
                    cubicTo(size.width * .42f, size.height * .28f,
                        size.width * .23f, size.height * .5f,
                        size.width * .23f, size.height * .65f)
                    cubicTo(size.width * .23f, size.height * .84f,
                        size.width * .36f, size.height * .91f,
                        size.width * .5f, size.height * .91f)
                    cubicTo(size.width * .64f, size.height * .91f,
                        size.width * .77f, size.height * .84f,
                        size.width * .77f, size.height * .65f)
                    cubicTo(size.width * .77f, size.height * .5f,
                        size.width * .58f, size.height * .28f,
                        size.width * .5f, size.height * .12f)
                    close()
                }
                drawPath(drop, color, style = stroke)
            }
            GlyphKind.Sleep -> {
                drawArc(color, 75f, 225f, false,
                    Offset(size.width * .17f, size.height * .13f),
                    Size(size.width * .62f, size.height * .68f), style = stroke)
                drawLine(color, Offset(size.width * .62f, size.height * .73f),
                    Offset(size.width * .82f, size.height * .73f), stroke.width,
                    StrokeCap.Round)
                drawLine(color, Offset(size.width * .82f, size.height * .73f),
                    Offset(size.width * .62f, size.height * .9f), stroke.width,
                    StrokeCap.Round)
                drawLine(color, Offset(size.width * .62f, size.height * .9f),
                    Offset(size.width * .82f, size.height * .9f), stroke.width,
                    StrokeCap.Round)
            }
        }
    }
}

@Composable
fun StatusPill(text: String, color: Color, modifier: Modifier = Modifier) {
    Box(
        modifier = modifier
            .background(color.copy(alpha = 0.14f), RoundedCornerShape(50))
            .padding(horizontal = 11.dp, vertical = 6.dp),
        contentAlignment = Alignment.Center,
    ) {
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(6.dp).background(color, RoundedCornerShape(50)))
            Text(text, color = Ink, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
        }
    }
}

@Composable
fun SectionTitle(kicker: String, title: String, modifier: Modifier = Modifier) {
    Box(modifier) {
        androidx.compose.foundation.layout.Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(kicker.uppercase(), fontSize = 11.sp, letterSpacing = 1.6.sp, color = CyanDeep, fontWeight = FontWeight.Bold)
            Text(title, fontSize = 25.sp, lineHeight = 30.sp, fontWeight = FontWeight.Black, color = Ink)
        }
    }
}
