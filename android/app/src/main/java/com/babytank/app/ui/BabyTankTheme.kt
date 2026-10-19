package com.babytank.app.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

val Ink = Color(0xFF17191C)
val SoftInk = Color(0xFF41464D)
val CanvasWarm = Color(0xFFF7F5EF)
val Paper = Color(0xFFFFFFFF)
val Cyan = Color(0xFF16D8D0)
val CyanDeep = Color(0xFF087C79)
val Cream = Color(0xFFFFE6AD)
val Coral = Color(0xFFFF8069)
val Lavender = Color(0xFFC9BCFF)
val MintWash = Color(0xFFDDF8F5)
val Line = Color(0xFFE3E0D8)

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
