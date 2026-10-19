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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
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
                durationMillis = 4200
                1f at 0
                1f at 3180
                0.08f at 3260
                1f at 3400
                1f at 4200
            },
        ),
        label = "blink",
    )
    val time by transition.animateFloat(
        initialValue = 0f,
        targetValue = 6.28318f,
        animationSpec = infiniteRepeatable(
            animation = keyframes {
                durationMillis = 3600
                6.28318f at 3600 using LinearEasing
            },
        ),
        label = "float",
    )

    Canvas(modifier = modifier) {
        drawRoundRect(
            color = Ink,
            cornerRadius = CornerRadius(size.minDimension * 0.16f),
        )
        val floatY = sin(time) * size.height * 0.012f
        val eyeWidth = size.width * 0.17f
        val eyeHeight = size.height * 0.27f * blink
        val eyeY = size.height * 0.37f + floatY - eyeHeight / 2f
        listOf(size.width * 0.31f, size.width * 0.69f).forEach { centerX ->
            drawRoundRect(
                color = if (connected) Cyan else Cyan.copy(alpha = 0.82f),
                topLeft = Offset(centerX - eyeWidth / 2f, eyeY),
                size = Size(eyeWidth, eyeHeight.coerceAtLeast(3f)),
                cornerRadius = CornerRadius(eyeWidth / 2f),
            )
        }
        val mouthWidth = size.width * 0.18f
        val mouthHeight = size.height * 0.055f
        drawRoundRect(
            color = Cream,
            topLeft = Offset(
                size.width / 2f - mouthWidth / 2f,
                size.height * 0.64f + floatY,
            ),
            size = Size(mouthWidth, mouthHeight),
            cornerRadius = CornerRadius(mouthHeight / 2f),
        )
        if (connected) {
            drawCircle(
                color = Color(0xFF72F2A9),
                radius = size.minDimension * 0.025f,
                center = Offset(size.width * 0.88f, size.height * 0.13f),
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
