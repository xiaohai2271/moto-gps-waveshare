package dev.glimpse.motogps.media

import android.service.notification.NotificationListenerService

/**
 * Grants access to MediaSessionManager.getActiveSessions(). The user must
 * enable "notification access" for this app; the service itself does nothing
 * with notification content and never reads message text.
 */
class MediaNotificationListener : NotificationListenerService()
