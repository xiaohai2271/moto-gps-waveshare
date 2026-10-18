package dev.glimpse.motogps.media

import android.app.NotificationManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.provider.Settings

/**
 * Controls the phone's currently active media session and exposes metadata for
 * the round display. This is a remote control only: the ESP32 has no audio
 * output and the app never starts playback on its own, never uses accessibility
 * services, and cannot control apps that do not publish a MediaSession.
 */
class MediaControllerSource(private val context: Context) {

    data class Snapshot(
        val connected: Boolean,
        val playing: Boolean,
        val trackToken: Long,
        val positionS: Int,
        val durationS: Int,
        val sourceName: String,
        val trackTitle: String,
        val artistName: String,
    )

    private val component = ComponentName(context, MediaNotificationListener::class.java)

    fun hasAccess(): Boolean = runCatching {
        val manager = context.getSystemService(NotificationManager::class.java)
        manager?.isNotificationListenerAccessGranted(component) == true
    }.getOrDefault(false)

    fun openAccessSettings() {
        val intent = Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        runCatching { context.startActivity(intent) }
    }

    fun snapshot(): Snapshot {
        val controller = activeController()
        if (controller == null) {
            return Snapshot(false, false, 0, 0, 0, "", "", "")
        }
        val metadata = controller.metadata
        val title = metadata?.getString(MediaMetadata.METADATA_KEY_TITLE).orEmpty()
        val artist = metadata?.getString(MediaMetadata.METADATA_KEY_ARTIST)
            ?: metadata?.getString(MediaMetadata.METADATA_KEY_ALBUM_ARTIST).orEmpty()
        val durationMs = metadata?.getLong(MediaMetadata.METADATA_KEY_DURATION) ?: 0L
        val positionMs = controller.playbackState?.let {
            if (it.state == PlaybackState.STATE_PLAYING) it.position else it.position
        } ?: 0L
        val playing = controller.playbackState?.state == PlaybackState.STATE_PLAYING
        return Snapshot(
            connected = true,
            playing = playing,
            trackToken = (title + "|" + artist).hashCode().toLong() and 0xFFFFFFFFL,
            positionS = (positionMs / 1000).toInt().coerceIn(0, 65535),
            durationS = (durationMs / 1000).toInt().coerceIn(0, 65535),
            sourceName = controller.packageName.substringAfterLast('.').take(31),
            trackTitle = title.take(63),
            artistName = artist.take(47),
        )
    }

    fun toggle(): Boolean = runCatching {
        val controller = activeController() ?: return false
        if (controller.playbackState?.state == PlaybackState.STATE_PLAYING) {
            controller.transportControls.pause()
        } else {
            controller.transportControls.play()
        }
        true
    }.getOrDefault(false)

    fun next(): Boolean = runCatching {
        val controller = activeController() ?: return false
        controller.transportControls.skipToNext()
        true
    }.getOrDefault(false)

    fun previous(): Boolean = runCatching {
        val controller = activeController() ?: return false
        controller.transportControls.skipToPrevious()
        true
    }.getOrDefault(false)

    private fun activeController(): MediaController? {
        if (!hasAccess()) return null
        val manager = context.getSystemService(MediaSessionManager::class.java) ?: return null
        val sessions = runCatching { manager.getActiveSessions(component) }.getOrNull().orEmpty()
        return sessions.firstOrNull {
            it.playbackState?.state == PlaybackState.STATE_PLAYING
        } ?: sessions.firstOrNull()
    }
}
