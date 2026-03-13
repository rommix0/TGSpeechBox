/*
 * TgsbApp — Root Compose UI for TGSpeechBox.
 *
 * Material 3 Scaffold with bottom navigation (3 tabs):
 *   1. Speak — text input, voice/language pickers, sliders
 *   2. Engine Settings — advanced quality sliders
 *   3. Editor — pack settings and phoneme editor
 *
 * Detail routes (editor/pack/{lang}, editor/phoneme/{key}) hide the
 * bottom navigation bar so the editor gets full-screen space.
 *
 * License: GPL-3.0
 */

package com.tgspeechbox.tts

import android.net.Uri
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument

private sealed class Screen(val route: String, val labelRes: Int) {
    data object Speak : Screen("speak", R.string.tab_speak)
    data object Advanced : Screen("advanced", R.string.tab_advanced)
    data object Editor : Screen("editor", R.string.tab_editor)
}

private val screens = listOf(Screen.Speak, Screen.Advanced, Screen.Editor)

/** Routes where the bottom navigation bar should be hidden (full-screen editors). */
private val detailRoutes = setOf("editor/pack/{lang}", "editor/phoneme/{key}")

@Composable
fun TgsbApp(viewModel: TgsbViewModel) {
    val navController = rememberNavController()
    val navBackStackEntry by navController.currentBackStackEntryAsState()
    val currentRoute = navBackStackEntry?.destination?.route
    val snackbarHostState = remember { SnackbarHostState() }
    val showBottomBar = currentRoute !in detailRoutes

    MaterialTheme {
        Scaffold(
            snackbarHost = { SnackbarHost(snackbarHostState) },
            bottomBar = {
                if (showBottomBar) {
                    NavigationBar {
                        for (screen in screens) {
                            NavigationBarItem(
                                selected = currentRoute == screen.route,
                                onClick = {
                                    navController.navigate(screen.route) {
                                        popUpTo(navController.graph.findStartDestination().id) {
                                            saveState = true
                                        }
                                        launchSingleTop = true
                                        restoreState = true
                                    }
                                },
                                icon = {
                                    when (screen) {
                                        Screen.Speak -> Icon(
                                            Icons.Default.PlayArrow,
                                            contentDescription = null
                                        )
                                        Screen.Advanced -> Icon(
                                            Icons.Default.Settings,
                                            contentDescription = null
                                        )
                                        Screen.Editor -> Icon(
                                            Icons.Default.Edit,
                                            contentDescription = null
                                        )
                                    }
                                },
                                label = { Text(stringResource(screen.labelRes)) }
                            )
                        }
                    }
                }
            }
        ) { innerPadding ->
            NavHost(
                navController = navController,
                startDestination = Screen.Speak.route,
                modifier = Modifier.padding(innerPadding)
            ) {
                composable(Screen.Speak.route) {
                    SpeakScreen(viewModel)
                }
                composable(Screen.Advanced.route) {
                    AdvancedScreen(viewModel, snackbarHostState)
                }
                composable(Screen.Editor.route) {
                    EditorScreen(viewModel, navController)
                }
                composable(
                    "editor/pack/{lang}",
                    arguments = listOf(navArgument("lang") { type = NavType.StringType })
                ) { backStackEntry ->
                    val lang = Uri.decode(backStackEntry.arguments?.getString("lang") ?: "")
                    PackSettingsScreen(
                        viewModel = viewModel,
                        langTag = lang,
                        onBack = { navController.popBackStack() }
                    )
                }
                composable(
                    "editor/phoneme/{key}",
                    arguments = listOf(navArgument("key") { type = NavType.StringType })
                ) { backStackEntry ->
                    val key = Uri.decode(backStackEntry.arguments?.getString("key") ?: "")
                    PhonemeDetailScreen(
                        viewModel = viewModel,
                        phonemeKey = key,
                        onBack = { navController.popBackStack() }
                    )
                }
            }
        }
    }
}
