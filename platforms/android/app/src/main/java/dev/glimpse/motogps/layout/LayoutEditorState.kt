package dev.glimpse.motogps.layout

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * 编辑器状态机（§7.2）：撤销/重做栈、拖拽吸附、以及与设备的同步状态。
 * 纯 Kotlin，不依赖 Android 框架，便于 JVM 单测。
 */
class LayoutEditorState(private val store: LayoutStore) {

    /** 同步状态：离线可编辑，连接后推送。 */
    enum class SyncState { NOT_CONNECTED, PENDING, PUSHING, SYNCED, RETRY_EXHAUSTED }

    data class EditorUiState(
        val layout: UiLayout,
        val syncState: SyncState = SyncState.NOT_CONNECTED,
        /** 设备上报的 supported_mask；null 表示尚未收到 UiLayoutState。 */
        val deviceSupportedMask: Int? = null,
        /** 设备上报的当前生效 layoutRevision；null 表示尚未收到。 */
        val deviceLayoutRevision: Int? = null,
        /** 编辑中是否有未推送的变更（离线编辑标记待同步）。 */
        val hasUnsyncedChanges: Boolean = false,
    )

    private val _state = MutableStateFlow(
        EditorUiState(layout = store.current()),
    )
    val state: StateFlow<EditorUiState> = _state.asStateFlow()

    private val undoStack = ArrayDeque<UiLayout>()
    private val redoStack = ArrayDeque<UiLayout>()
    private val maxHistory = 50

    val canUndo: Boolean get() = undoStack.isNotEmpty()
    val canRedo: Boolean get() = redoStack.isNotEmpty()

    val layout: UiLayout get() = _state.value.layout

    // --- 编辑操作（全部入撤销栈，revision 自增） --------------------------

    private fun commit(update: (UiLayout) -> UiLayout) {
        pushUndo(_state.value.layout)
        val next = store.commit(update)
        redoStack.clear()
        _state.value = _state.value.copy(
            layout = next,
            hasUnsyncedChanges = true,
            syncState = if (_state.value.syncState == SyncState.SYNCED ||
                _state.value.syncState == SyncState.NOT_CONNECTED
            ) {
                if (_state.value.syncState == SyncState.SYNCED) SyncState.PENDING else SyncState.NOT_CONNECTED
            } else {
                SyncState.PENDING
            },
        )
    }

    private fun pushUndo(previous: UiLayout) {
        undoStack.addLast(previous)
        if (undoStack.size > maxHistory) undoStack.removeFirst()
    }

    fun updateComponent(id: LayoutComponentId, update: (LayoutComponent) -> LayoutComponent) {
        commit { layout ->
            val component = layout[id]
            layout.copy(components = layout.components + (id to update(component)))
        }
    }

    fun moveComponent(id: LayoutComponentId, x: Int, y: Int) {
        updateComponent(id) { it.copy(x = x, y = y) }
    }

    /** 拖拽松手：非圆弧走钳制+吸附；圆弧走锚点吸附。 */
    fun settleDraggedComponent(id: LayoutComponentId, dragCenterX: Float, dragCenterY: Float) {
        val component = layout[id]
        if (component.isArc) {
            val anchor = LayoutGeometry.nearestArcAnchor(dragCenterX, dragCenterY, component.anchor)
            if (anchor != component.anchor) {
                updateComponent(id) { it.copy(anchor = anchor) }
            }
            return
        }
        val (w, h) = ComponentSizes.sizeOf(id, component.size)
        val (x, y) = LayoutGeometry.settleFreeComponent(
            (dragCenterX - w / 2f).toInt(),
            (dragCenterY - h / 2f).toInt(),
            w,
            h,
        )
        if (x != component.x || y != component.y) {
            moveComponent(id, x, y)
        }
    }

    fun updateMap(update: (MapConfig) -> MapConfig) {
        commit { it.copy(map = update(it.map)) }
    }

    fun updateBasemap(update: (BasemapConfig) -> BasemapConfig) {
        commit { it.copy(basemap = update(it.basemap)) }
    }

    fun updateOrientation(orientation: LayoutOrientation) {
        commit { it.copy(orientation = orientation) }
    }

    fun updateMusicPage(enabled: Boolean) {
        commit { it.copy(musicPageEnabled = enabled) }
    }

    /** 恢复默认布局（调用方负责二次确认）。 */
    fun resetToDefault() {
        pushUndo(_state.value.layout)
        val next = store.resetToDefault()
        redoStack.clear()
        _state.value = _state.value.copy(
            layout = next,
            hasUnsyncedChanges = true,
            syncState = SyncState.PENDING,
        )
    }

    fun undo(): UiLayout? {
        val previous = undoStack.removeLastOrNull() ?: return null
        redoStack.addLast(_state.value.layout)
        store.commit { previous }
        _state.value = _state.value.copy(
            layout = previous,
            hasUnsyncedChanges = true,
            syncState = SyncState.PENDING,
        )
        return previous
    }

    fun redo(): UiLayout? {
        val next = redoStack.removeLastOrNull() ?: return null
        pushUndo(_state.value.layout)
        store.commit { next }
        _state.value = _state.value.copy(
            layout = next,
            hasUnsyncedChanges = true,
            syncState = SyncState.PENDING,
        )
        return next
    }

    // --- 同步状态 ----------------------------------------------------------

    fun onDisconnected() {
        _state.value = _state.value.copy(syncState = SyncState.NOT_CONNECTED)
    }

    /** 连接且固件支持 bit8：进入待推送状态。 */
    fun onConnected() {
        _state.value = _state.value.copy(
            syncState = if (_state.value.hasUnsyncedChanges) SyncState.PENDING else SyncState.SYNCED,
        )
    }

    /** 收到 UiLayoutState：比较版本决定是否推送。 */
    fun onDeviceLayoutState(device: DeviceLayoutState): Boolean {
        val needsPush = device.layoutRevision != layout.revision
        _state.value = _state.value.copy(
            deviceSupportedMask = device.supportedMask,
            deviceLayoutRevision = device.layoutRevision,
            syncState = if (needsPush) SyncState.PENDING else SyncState.SYNCED,
        )
        return needsPush
    }

    fun onPushStarted() {
        _state.value = _state.value.copy(syncState = SyncState.PUSHING)
    }

    fun onPushAcked() {
        _state.value = _state.value.copy(
            syncState = SyncState.SYNCED,
            hasUnsyncedChanges = false,
        )
    }

    fun onPushFailed() {
        _state.value = _state.value.copy(syncState = SyncState.RETRY_EXHAUSTED)
    }

    fun isComponentSupportedOnDevice(id: LayoutComponentId): Boolean =
        _state.value.deviceSupportedMask?.let {
            LayoutCodec.isComponentSupported(it, id)
        } ?: true
}
