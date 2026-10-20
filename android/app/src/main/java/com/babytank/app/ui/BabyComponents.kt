package com.babytank.app.ui

import androidx.compose.animation.core.LinearEasing
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
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
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
    val gazeX by transition.animateFloat(
        initialValue = 0f,
        targetValue = 0f,
        animationSpec = infiniteRepeatable(
            keyframes {
                durationMillis = 9200
                0f at 0
                0f at 900
                -0.58f at 1750
                -0.58f at 3100
                0.42f at 4400
                0.42f at 6250
                0f at 7420
                0f at 9200
            },
        ),
        label = "gaze-x",
    )
    val gazeY by transition.animateFloat(
        initialValue = 0f,
        targetValue = 0f,
        animationSpec = infiniteRepeatable(
            keyframes {
                durationMillis = 10400
                0f at 0
                -0.18f at 2400
                -0.18f at 4300
                0.12f at 6200
                0.12f at 8200
                0f at 10400
            },
        ),
        label = "gaze-y",
    )
    val time by transition.animateFloat(
        initialValue = 0f,
        targetValue = 6.28318f,
        animationSpec = infiniteRepeatable(
            animation = keyframes {
                durationMillis = 4800
                6.28318f at 4800 using LinearEasing
            },
        ),
        label = "breathing",
    )

    Canvas(modifier = modifier) {
        val radius = size.minDimension * 0.17f
        drawRoundRect(
            brush = Brush.verticalGradient(
                listOf(Color(0xFF20272A), Color(0xFF111416)),
            ),
            cornerRadius = CornerRadius(radius),
        )
        drawRoundRect(
            color = Color.White.copy(alpha = .08f),
            cornerRadius = CornerRadius(radius),
            style = Stroke(width = 1.2.dp.toPx()),
        )
        drawArc(
            color = Color.White.copy(alpha = .055f),
            startAngle = 196f,
            sweepAngle = 118f,
            useCenter = false,
            topLeft = Offset(size.width * .05f, size.height * .04f),
            size = Size(size.width * .9f, size.height * .82f),
            style = Stroke(width = size.minDimension * .012f, cap = StrokeCap.Round),
        )

        val breathY = sin(time) * size.height * 0.009f
        val swayX = cos(time) * size.width * 0.003f
        val eyeWidth = size.minDimension * 0.15f
        val eyeHeight = size.minDimension * 0.255f * blink
        val gazeOffsetX = gazeX * size.width * .032f + swayX
        val gazeOffsetY = gazeY * size.height * .04f + breathY
        val eyeY = size.height * 0.42f + gazeOffsetY - eyeHeight / 2f
        val eyeColor = if (connected) Cyan else Color(0xFF78AAA8)

        listOf(size.width * 0.34f, size.width * 0.66f).forEach { baseX ->
            val centerX = baseX + gazeOffsetX
            drawCircle(
                color = eyeColor.copy(alpha = .07f * blink),
                radius = eyeWidth * 1.05f,
                center = Offset(centerX, size.height * .42f + gazeOffsetY),
            )
            drawRoundRect(
                brush = Brush.verticalGradient(
                    listOf(eyeColor.copy(alpha = .96f), eyeColor.copy(alpha = .76f)),
                    startY = eyeY,
                    endY = eyeY + eyeHeight.coerceAtLeast(3f),
                ),
                topLeft = Offset(centerX - eyeWidth / 2f, eyeY),
                size = Size(eyeWidth, eyeHeight.coerceAtLeast(3f)),
                cornerRadius = CornerRadius(eyeWidth / 2f),
            )
            if (blink > .45f) {
                drawRoundRect(
                    color = Color.White.copy(alpha = .62f),
                    topLeft = Offset(
                        centerX - eyeWidth * .22f,
                        eyeY + eyeHeight * .16f,
                    ),
                    size = Size(eyeWidth * .18f, eyeHeight * .19f),
                    cornerRadius = CornerRadius(eyeWidth * .1f),
                )
            }
        }

        val mouthWidth = size.minDimension * .2f
        val mouthHeight = size.minDimension * .095f
        drawArc(
            color = Cream,
            startAngle = 18f,
            sweepAngle = 144f,
            useCenter = false,
            topLeft = Offset(size.width / 2f - mouthWidth / 2f, size.height * .59f + breathY),
            size = Size(mouthWidth, mouthHeight),
            style = Stroke(width = size.minDimension * .022f, cap = StrokeCap.Round),
        )
        if (connected) {
            drawCircle(
                color = Color(0xFF72F2A9),
                radius = size.minDimension * 0.018f,
                center = Offset(size.width * 0.9f, size.height * 0.14f),
            )
        }
    }
}

enum class GlyphKind {
    Home, Pair, Control, Lab, Character, Signal, Camera, Settings,
    Up, Down, Left, Right, Stop, Light, RainbowLight, NightLight,
    Horn, Cannon, Dialogue,
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
            GlyphKind.Pair, GlyphKind.Signal -> {
                drawCircle(color, size.width * .09f, Offset(size.width * .5f, size.height * .76f))
                drawArc(color, 205f, 130f, false, Offset(size.width * .32f, size.height * .42f), Size(size.width * .36f, size.height * .36f), style = stroke)
                drawArc(color, 205f, 130f, false, Offset(size.width * .16f, size.height * .23f), Size(size.width * .68f, size.height * .68f), style = stroke)
            }
            GlyphKind.Control -> {
                drawCircle(color, size.width * .34f, center, style = stroke)
                drawCircle(color, size.width * .10f, center)
            }
            GlyphKind.Lab, GlyphKind.Settings -> {
                drawCircle(color, size.width * .30f, center, style = stroke)
                drawCircle(color, size.width * .08f, center)
                repeat(4) { index ->
                    val horizontal = index % 2 == 0
                    val start = if (horizontal) Offset(size.width * .08f, size.height * .5f) else Offset(size.width * .5f, size.height * .08f)
                    val end = if (horizontal) Offset(size.width * .92f, size.height * .5f) else Offset(size.width * .5f, size.height * .92f)
                    drawLine(color, start, end, stroke.width)
                }
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
