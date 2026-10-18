package dev.glimpse.motogps.ui

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import dev.glimpse.motogps.MotoGpsApplication
import dev.glimpse.motogps.gateway.Place
import dev.glimpse.motogps.nav.NavigationForegroundService
import dev.glimpse.motogps.nav.NavigationSession
import kotlinx.coroutines.flow.StateFlow

class NavViewModel(application: Application) : AndroidViewModel(application) {
    private val session: NavigationSession =
        (application as MotoGpsApplication).navigationSession

    val state: StateFlow<NavigationSession.UiState> = session.state

    fun search(keywords: String) = session.search(keywords)
    fun select(place: Place) = session.select(place)

    fun start() {
        // Foreground service first: navigation must not be killed when the
        // screen turns off. It is started from a visible activity.
        NavigationForegroundService.start(getApplication())
        session.start()
    }

    fun stop() {
        session.stop()
        NavigationForegroundService.stop(getApplication())
    }

    override fun onCleared() {
        // Session outlives the screen because it is application-scoped.
        super.onCleared()
    }
}
