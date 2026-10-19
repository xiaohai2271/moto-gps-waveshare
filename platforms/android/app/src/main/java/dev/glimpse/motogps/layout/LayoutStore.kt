package dev.glimpse.motogps.layout

import android.content.Context
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * 布局持久化（§7.4：DataStore/本地存储 + revision 自增）。这里用
 * SharedPreferences 存 §4.9 的 JSON 文本（项目内 RecentPlacesStore 同款
 * 模式），行为满足"离线可编辑、立即保存、revision 自增"的验收要求。
 *
 * 写操作无并发问题的前提是全部经由 [LayoutEditorState] 串行调用。
 */
class LayoutStore(context: Context) {

    private val prefs =
        context.getSharedPreferences("ui_layout_store", Context.MODE_PRIVATE)

    private val _layout = MutableStateFlow(loadOrDefault())
    val layout: StateFlow<UiLayout> = _layout.asStateFlow()

    fun current(): UiLayout = _layout.value

    fun loadOrDefault(): UiLayout {
        val raw = prefs.getString(KEY_LAYOUT, null) ?: return LayoutDefaults.defaultLayout()
        return LayoutJson.decode(raw) ?: LayoutDefaults.defaultLayout()
    }

    /**
     * 提交新布局：revision 自增并立即持久化。
     */
    fun commit(update: (UiLayout) -> UiLayout): UiLayout {
        val next = update(_layout.value).let {
            it.copy(revision = _layout.value.revision + 1)
        }
        prefs.edit().putString(KEY_LAYOUT, LayoutJson.encode(next)).apply()
        _layout.value = next
        return next
    }

    /** 恢复默认布局（保留 revision 继续自增，使设备识别为新版本）。 */
    fun resetToDefault(): UiLayout {
        val next = LayoutDefaults.defaultLayout(revision = _layout.value.revision + 1)
        prefs.edit().putString(KEY_LAYOUT, LayoutJson.encode(next)).apply()
        _layout.value = next
        return next
    }

    private companion object {
        const val KEY_LAYOUT = "ui_layout_v1"
    }
}
