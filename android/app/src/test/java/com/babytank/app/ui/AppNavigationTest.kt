package com.babytank.app.ui

import org.junit.Assert.assertEquals
import org.junit.Test

class AppNavigationTest {
    @Test
    fun productPagesReturnHome() {
        assertEquals(AppScreen.Home, parentScreen(AppScreen.Settings))
        assertEquals(AppScreen.Home, parentScreen(AppScreen.Control))
        assertEquals(AppScreen.Home, parentScreen(AppScreen.Interaction))
    }

    @Test
    fun setupAndAdvancedPagesReturnSettings() {
        assertEquals(AppScreen.Settings, parentScreen(AppScreen.Pair))
        assertEquals(AppScreen.Settings, parentScreen(AppScreen.Lab))
        assertEquals(AppScreen.Settings, parentScreen(AppScreen.Character))
    }

    @Test
    fun homeDoesNotInventAnotherParent() {
        assertEquals(AppScreen.Home, parentScreen(AppScreen.Home))
    }
}
