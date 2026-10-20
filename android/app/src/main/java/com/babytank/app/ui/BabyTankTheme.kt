package com.babytank.app.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

val Ink = Color(0xFF181B1E)
val SoftInk = Color(0xFF555D60)
val CanvasWarm = Color(0xFFF5F1E9)
val Paper = Color(0xFFFFFCF7)
val Cyan = Color(0xFF4DD7D0)
val CyanDeep = Color(0xFF167C78)
val Cream = Color(0xFFFFE9B9)
val Coral = Color(0xFFE77A68)
val Lavender = Color(0xFFC9BCFF)
val MintWash = Color(0xFFE4F1EE)
val PeachWash = Color(0xFFF2E6DE)
val ButterWash = Color(0xFFF1E8D5)
val SkyWash = Color(0xFFE3EEEC)
val Line = Color(0xFFDED8CF)

private val LightColors = lightColorScheme(
    primary = Ink,
    onPrimary = Color.White,
    secondary = CyanDeep,
    onSecondary = Color.White,
    tertiary = Coral,
    background = CanvasWarm,
    onBackground = Ink,
    surface = Paper,
    onSurface = Ink,
    outline = Line,
)

private val DarkColors = darkColorScheme(primary = Cyan, background = Ink)

@Composable
fun BabyTankTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = LightColors,
        typography = MaterialTheme.typography,
        content = content,
    )
}
