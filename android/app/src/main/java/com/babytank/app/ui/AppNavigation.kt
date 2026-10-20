package com.babytank.app.ui

internal enum class AppScreen {
    Home,
    Settings,
    Pair,
    Control,
    Interaction,
    Lab,
    Character,
}

internal fun parentScreen(screen: AppScreen): AppScreen = when (screen) {
    AppScreen.Pair, AppScreen.Lab, AppScreen.Character -> AppScreen.Settings
    AppScreen.Settings, AppScreen.Control, AppScreen.Interaction -> AppScreen.Home
    AppScreen.Home -> AppScreen.Home
}
