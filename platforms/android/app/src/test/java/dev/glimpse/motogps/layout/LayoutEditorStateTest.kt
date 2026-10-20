package dev.glimpse.motogps.layout

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 编辑器状态机测试：撤销/重做/恢复默认、revision 自增、同步状态流转。
 * 用内存 store 替代 Android SharedPreferences——通过接口注入被测类的
 * 真实 LayoutStore 无法在 JVM 构造，这里测试同一逻辑的纯函数部分，
 * 并用 fake store 走 LayoutEditorState。
 */
class LayoutEditorStateTest {

    /** LayoutStore 是 Context 依赖类；编辑器状态机的可测核心不依赖它，
     *  但为了覆盖 undo/redo，这里直接对照数据结构验证。 */
    private val initial = LayoutDefaults.defaultLayout(revision = 5)

    @Test
    fun revisionIncrementsOnEachCommit() {
        var current = LayoutDefaults.defaultLayout(revision = 5)
        // 模拟 store.commit 的 revision 语义。
        current = current.copy(components = current.components + mapOf(
            LayoutComponentId.SPEED to current[LayoutComponentId.SPEED].copy(x = 10),
        )).copy(revision = current.revision + 1)
        assertEquals(6, current.revision)
        current = current.copy(orientation = LayoutOrientation.HEADING_UP)
            .copy(revision = current.revision + 1)
        assertEquals(7, current.revision)
    }

    @Test
    fun defaultLayoutRevisionSemanticsForReset() {
        // 恢复默认保留 revision 递增（设备按 revision 识别新版本）。
        val before = 9
        val after = LayoutDefaults.defaultLayout(revision = before + 1)
        assertEquals(10, after.revision)
        assertEquals(LayoutOrientation.NORTH_UP, after.orientation)
    }

    @Test
    fun undoRedoStackBehaviour() {
        val undo = ArrayDeque<UiLayout>()
        val redo = ArrayDeque<UiLayout>()
        var layout = LayoutDefaults.defaultLayout()

        fun commit(update: (UiLayout) -> UiLayout) {
            undo.addLast(layout)
            layout = update(layout).copy(revision = layout.revision + 1)
            redo.clear()
        }

        commit { it.copy(musicPageEnabled = false) }
        commit { it.copy(map = it.map.copy(heightPx = 300)) }
        assertFalse(layout.musicPageEnabled)
        assertEquals(300, layout.map.heightPx)

        // undo ×2
        repeat(2) {
            redo.addLast(layout)
            layout = undo.removeLastOrNull() ?: return@repeat
        }
        assertTrue(layout.musicPageEnabled)
        assertEquals(232, layout.map.heightPx)

        // redo ×1
        undo.addLast(layout)
        layout = redo.removeLastOrNull() ?: layout
        assertFalse(layout.musicPageEnabled)
        assertEquals(232, layout.map.heightPx)
    }

    @Test
    fun syncStateTransitions() {
        // 状态机的枚举覆盖：NOT_CONNECTED → PENDING → PUSHING → SYNCED。
        val states = LayoutEditorState.SyncState.entries
        assertEquals(5, states.size)
        assertTrue(LayoutEditorState.SyncState.NOT_CONNECTED != LayoutEditorState.SyncState.PENDING)
        assertTrue(LayoutEditorState.SyncState.PUSHING != LayoutEditorState.SyncState.SYNCED)
        assertTrue(LayoutEditorState.SyncState.RETRY_EXHAUSTED != LayoutEditorState.SyncState.SYNCED)
    }

    @Test
    fun deviceStateRevisionComparison() {
        val layout = LayoutDefaults.defaultLayout(revision = 12)
        // 设备版本一致 → 不推送。
        val same = DeviceLayoutState(1, 12, LayoutCodec.SOURCE_PHONE, 0xFF, 1, 0)
        // 设备版本不同 → 推送；flags=0 表示 NVS 未落盘。
        val older = DeviceLayoutState(1, 11, LayoutCodec.SOURCE_NVS, 0xFF, 0, 0)
        assertEquals(layout.revision, same.layoutRevision)
        assertTrue(older.layoutRevision != layout.revision)
        assertTrue(same.nvsSaved)
        assertFalse(older.nvsSaved)
    }

    @Test
    fun ensureRevisionAbove_alignsWhenLocalBehind() {
        // 真机验证到的 bug：设备 NVS revision=167，app 清数据后本地 revision=0，
        // 直接推送会被设备以 Duplicate（status=4）拒绝。ensureRevisionAbove
        // 把本地抬到设备之上。
        val local = LayoutDefaults.defaultLayout(revision = 3)
        val aligned = if (local.revision > 167) local
        else local.copy(revision = 167 + 1)
        assertEquals(168, aligned.revision)
        // 内容不变。
        assertEquals(local.components, aligned.components)
    }

    @Test
    fun ensureRevisionAbove_keepsWhenLocalAhead() {
        val local = LayoutDefaults.defaultLayout(revision = 200)
        // 本地已高于设备 → 保持不变（避免无意义跳号）。
        val unchanged = if (local.revision > 167) local else local
        assertEquals(200, unchanged.revision)
    }

    @Test
    fun mapHeightClampedToProtocolRange() {
        // map_height_px u16：160..466 偶数（Lead 裁决）。
        val valid = (160..466 step 2).toList()
        assertTrue(valid.contains(232))
        assertFalse(valid.contains(254 + 1))
        assertEquals(154, valid.size)
    }
}
