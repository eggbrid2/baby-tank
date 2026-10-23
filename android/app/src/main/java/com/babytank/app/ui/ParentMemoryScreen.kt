package com.babytank.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.babytank.app.cloud.ParentMemoryState
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

private fun localTime(value: String): String = try {
    DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm")
        .withZone(ZoneId.systemDefault()).format(Instant.parse(value))
} catch (_: Exception) { value.take(16).replace('T', ' ') }

private data class MemoryChoice(val label: String, val kind: String, val subject: String)
private val memoryChoices = listOf(
    MemoryChoice("故事偏好", "preference", "story_theme"),
    MemoryChoice("游戏偏好", "preference", "play_theme"),
    MemoryChoice("一起做过", "shared_activity", "activity"),
    MemoryChoice("称呼", "nickname", "name"),
)
private val needLabels = mapOf(
    "hunger" to "饥饿", "thirst" to "口渴", "energy" to "精力",
    "stamina" to "体力", "joy" to "快乐", "excitement" to "兴奋",
    "bond" to "陪伴感", "curiosity" to "好奇心",
)
private val moodLabels = mapOf("calm" to "平静", "happy" to "开心",
    "curious" to "好奇", "excited" to "兴奋", "sleepy" to "困倦",
    "sad" to "低落", "annoyed" to "有点烦躁")
private val bandLabels = mapOf("morning" to "上午", "midday" to "中午",
    "afternoon" to "下午", "early_evening" to "傍晚",
    "evening" to "夜晚", "late_night" to "深夜")

@Composable
fun ParentMemoryScreen(
    state: ParentMemoryState,
    onPair: (String) -> Unit,
    onRefresh: () -> Unit,
    onAddMemory: (String, String, String) -> Unit,
    onDeleteMemory: (String) -> Unit,
    onUpdateMemory: (String, Int, String) -> Unit,
    onClearMemories: () -> Unit,
    onHistory: (Boolean) -> Unit,
    onRetention: (Int) -> Unit,
    onPolicy: (String, Boolean) -> Unit,
    onExport: () -> Unit,
    onDeleteQuestion: (Int?) -> Unit,
    onLoadMoreQuestions: () -> Unit,
    onCharacter: () -> Unit,
    onLogout: () -> Unit,
    onBack: () -> Unit,
) {
    var code by rememberSaveable { mutableStateOf("") }
    var memoryText by rememberSaveable { mutableStateOf("") }
    var choiceIndex by rememberSaveable { mutableStateOf(0) }
    var confirmClear by remember { mutableStateOf(false) }
    var confirmDisable by remember { mutableStateOf(false) }
    var confirmMemories by remember { mutableStateOf(false) }
    var timeZoneDraft by rememberSaveable(state.timeZone) { mutableStateOf(state.timeZone) }

    if (confirmClear || confirmDisable || confirmMemories) AlertDialog(
        onDismissRequest = {
            confirmClear = false; confirmDisable = false; confirmMemories = false },
        title = { Text(when {
            confirmMemories -> "清空长期记忆？"
            confirmDisable -> "关闭询问记录？"
            else -> "清空询问记录？"
        }) },
        text = { Text(if (confirmMemories) "已批准的共享记忆将从云端删除，下轮对话不再引用。"
            else "已有的询问与回答会从云端记录表删除，无法在 App 恢复。") },
        confirmButton = {
            Button(onClick = {
                when {
                    confirmMemories -> onClearMemories()
                    confirmDisable -> onHistory(false)
                    else -> onDeleteQuestion(null)
                }
                confirmClear = false; confirmDisable = false; confirmMemories = false
            }) { Text("确认删除") }
        },
        dismissButton = {
            OutlinedButton(onClick = {
                confirmClear = false; confirmDisable = false; confirmMemories = false }) {
                Text("取消")
            }
        },
    )

    LazyColumn(
        modifier = Modifier.fillMaxSize().background(CanvasWarm),
        verticalArrangement = Arrangement.spacedBy(14.dp),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(
            start = 20.dp, end = 20.dp, top = 20.dp, bottom = 34.dp),
    ) {
        item {
            Row(verticalAlignment = Alignment.CenterVertically) {
                OutlinedButton(onClick = onBack) { Text("返回") }
                Spacer(Modifier.width(12.dp))
                Text("成长记忆", color = Ink, fontSize = 24.sp, fontWeight = FontWeight.Black)
            }
        }
        if (state.error != null) item {
            Card(colors = CardDefaults.cardColors(containerColor = PeachWash)) {
                Text(state.error, Modifier.fillMaxWidth().padding(15.dp), color = Ink)
            }
        }
        if (state.pairedDeviceId == null) {
            item {
                MemoryCard {
                    Text("首次连接云端档案", fontSize = 19.sp, fontWeight = FontWeight.Black)
                    Text("首次使用需要用一次性配对码确认这是你的设备；连接后进入这里不再重复验证。",
                        color = SoftInk, fontSize = 13.sp)
                    OutlinedTextField(value = code, onValueChange = { code = it.take(24) },
                        label = { Text("16 位配对码") }, singleLine = true,
                        visualTransformation = PasswordVisualTransformation(),
                        modifier = Modifier.fillMaxWidth())
                    Button(onClick = { onPair(code); code = "" },
                        enabled = !state.busy && code.replace("-", "").length == 16,
                        modifier = Modifier.fillMaxWidth()) { Text("连接设备档案") }
                    Text("配对码只使用一次；不会在 App 保存管理员密码或设备令牌。",
                        color = SoftInk, fontSize = 12.sp)
                }
            }
            return@LazyColumn
        }
        item {
            MemoryCard {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically) {
                    Column {
                        Text("宝宝状态", fontSize = 19.sp, fontWeight = FontWeight.Black)
                        Text("设备 ${state.pairedDeviceId}", color = SoftInk, fontSize = 12.sp)
                    }
                    OutlinedButton(onClick = onRefresh, enabled = !state.busy) { Text("刷新") }
                }
                Text("${moodLabels[state.mood] ?: "等待同步"} · ${bandLabels[state.band] ?: "时间待确认"}",
                    color = CyanDeep, fontSize = 17.sp, fontWeight = FontWeight.Bold)
                if (state.needs.isNotEmpty()) {
                    needLabels.forEach { (key, label) ->
                        state.needs[key]?.let { value ->
                            Row(Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween) {
                                Text(label, color = SoftInk)
                                Text(value.toString(), fontWeight = FontWeight.Bold)
                            }
                        }
                    }
                }
                Text("这些是虚构角色的互动数值，不代表真实健康。",
                    color = SoftInk, fontSize = 12.sp)
            }
        }
        item {
            MemoryCard {
                Text("昼夜与深夜锁", fontSize = 19.sp, fontWeight = FontWeight.Black)
                OutlinedTextField(value = timeZoneDraft,
                    onValueChange = { timeZoneDraft = it.take(64) },
                    label = { Text("设备时区（IANA）") }, singleLine = true,
                    modifier = Modifier.fillMaxWidth())
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically) {
                    Text("23:00–06:00 深夜锁")
                    Switch(checked = state.deepNightLock, enabled = !state.busy,
                        onCheckedChange = { onPolicy(timeZoneDraft.trim(), it) })
                }
                Text("只有开启后才会在 23:00 强制停止互动并进入睡眠；关闭时仍按普通夜间空闲节奏休息。",
                    color = SoftInk, fontSize = 12.sp)
                if (timeZoneDraft != state.timeZone) OutlinedButton(
                    onClick = { onPolicy(timeZoneDraft.trim(), state.deepNightLock) },
                    enabled = !state.busy && timeZoneDraft.isNotBlank()) {
                    Text("保存时区")
                }
                val confirmed = state.headFresh &&
                    state.headPolicyRevision == state.policyRevision
                Text(if (confirmed) "头部已确认策略；时钟：${state.clockQuality}"
                    else "云端已保存，等待头部确认；勿将此状态当作已锁定",
                    color = SoftInk, fontSize = 12.sp)
                if (state.deepNightLock && state.clockQuality == "unknown") Text(
                    "头部时间未知时无法按点锁定；请让头部联网校时。完全断电后离线启动会保持可唤醒。",
                    color = SoftInk, fontSize = 12.sp)
            }
        }
        item {
            MemoryCard {
                Text("AI 性格", fontSize = 19.sp, fontWeight = FontWeight.Black)
                Text(if (state.headFresh &&
                    state.headPersonalityRevision == state.personality.revision)
                    "云端和头部已同步版本 ${state.personality.revision}"
                    else "云端版本 ${state.personality.revision}，等待头部确认",
                    color = SoftInk, fontSize = 13.sp)
                OutlinedButton(onClick = onCharacter) { Text("打开性格设置") }
            }
        }
        item {
            MemoryCard {
                Text("长期记忆", fontSize = 19.sp, fontWeight = FontWeight.Black)
                Text("仅存家长亲手添加的共享记忆；不会自动把照片或整段对话记下来。",
                    color = SoftInk, fontSize = 13.sp)
                memoryChoices.chunked(2).forEachIndexed { rowIndex, choices ->
                    Row(Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        choices.forEachIndexed { columnIndex, item ->
                            val index = rowIndex * 2 + columnIndex
                            FilterChip(selected = choiceIndex == index,
                                onClick = { choiceIndex = index },
                                label = { Text(item.label, fontSize = 12.sp) },
                                modifier = Modifier.weight(1f))
                        }
                    }
                }
                OutlinedTextField(value = memoryText,
                    onValueChange = { memoryText = it.take(40) },
                    label = { Text("记住什么？最多 40 字") },
                    modifier = Modifier.fillMaxWidth())
                Button(onClick = {
                    val selected = memoryChoices[choiceIndex]
                    onAddMemory(selected.kind, selected.subject, memoryText)
                    memoryText = ""
                }, enabled = !state.busy && memoryText.isNotBlank(),
                    modifier = Modifier.fillMaxWidth()) { Text("添加记忆") }
            }
        }
        items(state.memories, key = { it.id }) { memory ->
            var editing by remember(memory.id) { mutableStateOf(false) }
            var editText by remember(memory.revision) { mutableStateOf(memory.value) }
            MemoryCard {
                Text(memoryChoices.firstOrNull { it.kind == memory.kind &&
                    it.subject == memory.subject }?.label ?: "记忆",
                    color = SoftInk, fontSize = 12.sp)
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically) {
                    Text(memory.value, Modifier.weight(1f), fontWeight = FontWeight.SemiBold)
                    OutlinedButton(onClick = { editing = !editing },
                        enabled = !state.busy) { Text(if (editing) "取消" else "更正") }
                    OutlinedButton(onClick = { onDeleteMemory(memory.id) },
                        enabled = !state.busy) { Text("删除") }
                }
                if (editing) {
                    OutlinedTextField(value = editText,
                        onValueChange = { editText = it.take(40) },
                        modifier = Modifier.fillMaxWidth(), label = { Text("更正记忆") })
                    Button(onClick = {
                        onUpdateMemory(memory.id, memory.revision, editText)
                        editing = false
                    }, enabled = !state.busy && editText.isNotBlank()) { Text("保存更正") }
                }
            }
        }
        if (state.memories.isNotEmpty()) item {
            OutlinedButton(onClick = { confirmMemories = true },
                enabled = !state.busy) { Text("清空全部长期记忆") }
        }
        item {
            MemoryCard {
                Text("这台设备的询问记录", fontSize = 19.sp, fontWeight = FontWeight.Black)
                Text("默认关闭；开启后只保存成功回答的文字，保留 30 天。多人共用时不猜测是谁问的。",
                    color = SoftInk, fontSize = 13.sp)
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically) {
                    Text(if (state.historyEnabled) "已开启" else "已关闭")
                    Switch(checked = state.historyEnabled, enabled = !state.busy,
                        onCheckedChange = { enabled ->
                            if (enabled) onHistory(true) else confirmDisable = true
                        })
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    listOf(1, 7, 30).forEach { days ->
                        FilterChip(selected = state.retentionDays == days,
                            onClick = { onRetention(days) },
                            enabled = !state.busy,
                            label = { Text("${days} 天") })
                    }
                }
                if (state.historyEnabled && state.questions.isNotEmpty()) {
                    OutlinedButton(onClick = { confirmClear = true },
                        enabled = !state.busy) { Text("清空记录") }
                }
            }
        }
        if (state.historyEnabled && state.questions.isEmpty()) item {
            Text("还没有询问记录。", color = SoftInk, fontSize = 13.sp)
        }
        if (state.historyEnabled) items(state.questions, key = { it.id }) { question ->
            MemoryCard {
                Text(localTime(question.createdAt), color = SoftInk,
                    fontSize = 12.sp)
                Text("问：${question.question}", fontWeight = FontWeight.Bold)
                Text("答：${question.answer}", color = SoftInk)
                OutlinedButton(onClick = { onDeleteQuestion(question.id) },
                    enabled = !state.busy) { Text("删除这条") }
            }
        }
        if (state.historyEnabled && state.hasMoreQuestions) item {
            OutlinedButton(onClick = onLoadMoreQuestions, enabled = !state.busy,
                modifier = Modifier.fillMaxWidth()) { Text("加载更多询问") }
        }
        item {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = onExport, enabled = !state.busy) {
                    Text("导出成长记忆与询问记录")
                }
                Text("已配对手机可直接查看孩子的询问；请保管手机。导出文件也含这些内容。",
                    color = SoftInk, fontSize = 12.sp)
                OutlinedButton(onClick = onLogout, enabled = !state.busy) {
                    Text("断开云端档案")
                }
            }
        }
    }
}

@Composable
private fun MemoryCard(content: @Composable androidx.compose.foundation.layout.ColumnScope.() -> Unit) {
    Card(shape = RoundedCornerShape(23.dp),
        colors = CardDefaults.cardColors(containerColor = Paper)) {
        Column(Modifier.fillMaxWidth().padding(17.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp), content = content)
    }
}
