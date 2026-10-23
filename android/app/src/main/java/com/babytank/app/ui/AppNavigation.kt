package com.babytank.app.ui

internal enum class AppScreen {
    Home,
    Settings,
    Pair,
    Network,
    Control,
    Interaction,
    Lab,
    Character,
    Memory,
    Chat,
    GuessDrawing,
}

internal fun parentScreen(screen: AppScreen): AppScreen = when (screen) {
    AppScreen.Pair, AppScreen.Network, AppScreen.Lab, AppScreen.Character,
    AppScreen.Memory -> AppScreen.Settings
    AppScreen.GuessDrawing -> AppScreen.Interaction
    AppScreen.Settings, AppScreen.Control, AppScreen.Interaction, AppScreen.Chat -> AppScreen.Home
    AppScreen.Home -> AppScreen.Home
}
