package com.babytank.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.babytank.app.TextChatState

@Composable
internal fun TextChatScreen(
    state: TextChatState,
    paired: Boolean,
    onSend: (String) -> Unit,
    onPair: () -> Unit,
    onBack: () -> Unit,
) {
    var draft by rememberSaveable { mutableStateOf("") }
    val listState = rememberLazyListState()
    LaunchedEffect(state.messages.size) {
        if (state.messages.isNotEmpty()) listState.animateScrollToItem(state.messages.lastIndex)
    }
    Column(Modifier.fillMaxSize().background(CanvasWarm).imePadding()) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically) {
            OutlinedButton(onClick = onBack) { Text("返回") }
            Text("打字聊天", Modifier.padding(start = 14.dp),
                color = Ink, fontSize = 23.sp, fontWeight = FontWeight.Bold)
        }
        Text("文字代替聆听，AI 回答显示在这里，并由机器人播报。",
            Modifier.padding(horizontal = 20.dp), color = CyanDeep, fontSize = 12.sp)
        if (!paired) {
            Row(Modifier.fillMaxWidth().padding(20.dp),
                verticalAlignment = Alignment.CenterVertically) {
                Text("先配对机器人，手机才能向它发消息", Modifier.weight(1f), color = Ink)
                Button(onClick = onPair) { Text("去配对") }
            }
        }
        LazyColumn(
            modifier = Modifier.weight(1f).fillMaxWidth(),
            state = listState,
            contentPadding = androidx.compose.foundation.layout.PaddingValues(20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            items(state.messages, key = { it.id }) { message ->
                Row(Modifier.fillMaxWidth(),
                    horizontalArrangement = if (message.fromUser) Arrangement.End else Arrangement.Start) {
                    Box(
                        Modifier.widthIn(max = 300.dp)
                            .background(if (message.fromUser) CyanDeep else Paper,
                                RoundedCornerShape(18.dp))
                            .padding(horizontal = 16.dp, vertical = 12.dp),
                    ) {
                        Text(message.text, color = if (message.fromUser) Paper else Ink,
                            fontSize = 15.sp)
                    }
                }
            }
        }
        if (state.status.isNotEmpty()) {
            Text(state.status, Modifier.padding(horizontal = 20.dp),
                color = CyanDeep, fontSize = 12.sp)
        }
        if (state.error != null) {
            Text(state.error, Modifier.padding(horizontal = 20.dp, vertical = 6.dp),
                color = Coral, fontSize = 12.sp)
        }
        Row(Modifier.fillMaxWidth().navigationBarsPadding()
            .padding(start = 16.dp, end = 16.dp, bottom = 12.dp, top = 8.dp),
            verticalAlignment = Alignment.Bottom,
            horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedTextField(
                value = draft,
                onValueChange = { if (it.length <= 280) draft = it },
                modifier = Modifier.weight(1f),
                placeholder = { Text("想对宝宝坦克说什么？") },
                maxLines = 4,
                enabled = paired && !state.sending,
            )
            Button(
                onClick = { onSend(draft); draft = "" },
                enabled = paired && !state.sending && draft.isNotBlank(),
                colors = ButtonDefaults.buttonColors(containerColor = CyanDeep),
            ) { Text("发送") }
        }
    }
}
