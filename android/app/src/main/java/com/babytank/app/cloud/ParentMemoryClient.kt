package com.babytank.app.cloud

import android.app.Application
import android.net.Uri
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import com.babytank.app.BuildConfig
import java.net.HttpURLConnection
import java.net.URL
import java.security.KeyStore
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext
import org.json.JSONObject

data class ApprovedMemory(val id: String, val kind: String, val subject: String,
    val value: String, val revision: Int)
data class ChildQuestion(val id: Int, val question: String, val answer: String,
    val createdAt: String)
data class CloudPersonality(val curiosity: Int = 78, val warmth: Int = 84,
    val talkativeness: Int = 46, val revision: Int = 1)
data class TextTurnStatus(val id: String, val status: String,
    val reply: String?, val error: String?)
data class SketchGuessTurn(val id: String, val status: String,
    val guess: String, val reply: String?)
data class ParentMemoryState(
    val pairedDeviceId: String? = null,
    val busy: Boolean = false,
    val error: String? = null,
    val mood: String? = null,
    val band: String? = null,
    val needs: Map<String, Int> = emptyMap(),
    val stateSyncedAtMillis: Long = 0L,
    val enforcement: String? = null,
    val timeZone: String = "Asia/Shanghai",
    val deepNightLock: Boolean = false,
    val policyRevision: Int = 1,
    val headPolicyRevision: Int = 0,
    val headPersonalityRevision: Int = 0,
    val headFresh: Boolean = false,
    val clockQuality: String = "unknown",
    val nightLockActive: Boolean = false,
    val personality: CloudPersonality = CloudPersonality(),
    val historyEnabled: Boolean = false,
    val retentionDays: Int = 30,
    val memories: List<ApprovedMemory> = emptyList(),
    val questions: List<ChildQuestion> = emptyList(),
    val hasMoreQuestions: Boolean = false,
)

class ParentMemoryClient(application: Application) {
    private val app = application
    private val prefs = application.getSharedPreferences("baby_tank_parent_access", 0)
    private var token: String? = loadToken()
    private val mutableState = MutableStateFlow(ParentMemoryState(
        pairedDeviceId = prefs.getString("device_id", null)))
    val state: StateFlow<ParentMemoryState> = mutableState.asStateFlow()
    val hasAccess: Boolean get() = token != null

    private fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        val existing = store.getKey("baby_tank_parent_access_v1", null) as? SecretKey
        if (existing != null) return existing
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        generator.init(KeyGenParameterSpec.Builder("baby_tank_parent_access_v1",
            KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .build())
        return generator.generateKey()
    }

    private fun saveToken(value: String, deviceId: String) {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key())
        val packed = cipher.iv + cipher.doFinal(value.toByteArray(Charsets.UTF_8))
        prefs.edit().putString("token_ciphertext", Base64.encodeToString(packed, Base64.NO_WRAP))
            .putString("device_id", deviceId).apply()
        token = value
    }

    private fun loadToken(): String? {
        return try {
            val stored = prefs.getString("token_ciphertext", null) ?: return null
            val packed = Base64.decode(stored, Base64.NO_WRAP)
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128,
                packed.copyOfRange(0, 12)))
            String(cipher.doFinal(packed.copyOfRange(12, packed.size)), Charsets.UTF_8)
        } catch (_: Exception) {
            prefs.edit().remove("token_ciphertext").remove("device_id").apply()
            null
        }
    }

    private suspend fun request(path: String, method: String = "GET",
        body: JSONObject? = null, authenticated: Boolean = true,
        readTimeoutMs: Int = 10000): JSONObject =
        withContext(Dispatchers.IO) {
            val connection = (URL(BuildConfig.BABY_TANK_CLOUD_ORIGIN + path)
                .openConnection() as HttpURLConnection)
            try {
                connection.requestMethod = method
                connection.connectTimeout = 8000
                connection.readTimeout = readTimeoutMs
                connection.setRequestProperty("Accept", "application/json")
                if (authenticated) {
                    val credential = token ?: throw IllegalStateException("请先输入家长配对码")
                    connection.setRequestProperty("Authorization", "Bearer $credential")
                }
                if (body != null) {
                    connection.doOutput = true
                    connection.setRequestProperty("Content-Type", "application/json; charset=utf-8")
                    connection.outputStream.use { it.write(body.toString().toByteArray(Charsets.UTF_8)) }
                }
                val stream = if (connection.responseCode in 200..299)
                    connection.inputStream else connection.errorStream
                val result = stream?.bufferedReader(Charsets.UTF_8)?.use { it.readText() } ?: "{}"
                val parsed = JSONObject(result)
                if (connection.responseCode !in 200..299) {
                    throw IllegalStateException(when (parsed.optString("error")) {
                        "pairing_code_invalid_or_expired" -> "配对码错误、过期或已使用"
                        "parent_authentication_required" -> "家长授权已失效，请重新配对"
                        "too_many_attempts" -> "尝试过多，请稍后再试"
                        "character_event_rate_limited" -> "互动过于频繁，请稍后再试"
                        "character_night_locked" -> "深夜锁已启用，互动未执行"
                        "text_turn_device_offline" -> "机器人暂未接收消息，请先退出遥控/摄像头再重试"
                        "text_turn_busy" -> "机器人正在对话或执行上一条消息，请稍后再发"
                        "text_turn_capacity" -> "云端对话繁忙，请稍后重试"
                        "text_turn_not_found" -> "这条消息已过期，请重新发送"
                        "sketch_guess_invalid", "sketch_jpeg_invalid" ->
                            "画板数据不完整，请清空后重新画"
                        "sketch_ink_invalid" -> "再多画几笔，我才能猜出来"
                        "sketch_guess_rate_limited" -> "猜得太快啦，稍等一会再试"
                        "sketch_guess_upstream_rejected", "sketch_guess_response_invalid" ->
                            "这次没看懂，再添几笔试试"
                        "vision_credentials_unavailable" -> "云端看图暂时不可用"
                        "character_policy_conflict", "personality_conflict",
                        "memory_conflict" -> "设置已在另一部手机更新，请刷新后重试"
                        else -> "云端请求失败（${connection.responseCode}）"
                    })
                }
                parsed
            } finally { connection.disconnect() }
        }

    suspend fun submitTextTurn(text: String, requestId: String = UUID.randomUUID().toString()): TextTurnStatus {
        val result = request("/api/parent/v1/text-turns", "POST",
            JSONObject().put("requestId", requestId).put("text", text))
        return result.asTextTurn()
    }

    suspend fun readTextTurn(id: String): TextTurnStatus =
        request("/api/parent/v1/text-turns/${Uri.encode(id)}").asTextTurn()

    suspend fun submitSketchGuess(jpeg: ByteArray, round: Int,
        requestId: String = UUID.randomUUID().toString()): SketchGuessTurn {
        val result = request("/api/parent/v1/sketch-guesses", "POST", JSONObject()
            .put("requestId", requestId)
            .put("round", round)
            .put("jpegBase64", Base64.encodeToString(jpeg, Base64.NO_WRAP)),
            readTimeoutMs = 18000)
        return SketchGuessTurn(
            id = result.getString("id"), status = result.getString("status"),
            guess = result.getString("guess"),
            reply = result.optString("reply").takeIf { it.isNotBlank() && it != "null" },
        )
    }

    private fun JSONObject.asTextTurn() = TextTurnStatus(
        id = getString("id"), status = getString("status"),
        reply = optString("reply").takeIf { it.isNotBlank() && it != "null" },
        error = optString("error").takeIf { it.isNotBlank() && it != "null" },
    )

    suspend fun pair(code: String) {
        mutableState.value = mutableState.value.copy(busy = true, error = null)
        try {
            val result = request("/api/parent/v1/claim", "POST",
                JSONObject().put("code", code.trim()), authenticated = false)
            val deviceId = result.getString("deviceId")
            saveToken(result.getString("token"), deviceId)
            mutableState.value = ParentMemoryState(pairedDeviceId = deviceId)
            refresh()
        } catch (error: Exception) {
            mutableState.value = mutableState.value.copy(busy = false,
                error = error.message ?: "配对失败")
        }
    }

    suspend fun refresh() {
        if (token == null) return
        mutableState.value = mutableState.value.copy(busy = true, error = null)
        try {
            val me = request("/api/parent/v1/me")
            val stateResult = request("/api/parent/v1/state")
            val character = stateResult.getJSONObject("state")
            val ack = stateResult.getJSONObject("ack")
            val characterPolicy = character.getJSONObject("policy")
            val traits = stateResult.getJSONObject("personality")
            val memoryResult = request("/api/parent/v1/memories").getJSONArray("memories")
            val questionResult = request("/api/parent/v1/questions")
            val needsJson = character.getJSONObject("needs")
            val needs = needsJson.keys().asSequence().associateWith { needsJson.optInt(it) }
            val memories = (0 until memoryResult.length()).map { index ->
                memoryResult.getJSONObject(index).let { item ->
                    ApprovedMemory(item.getString("id"), item.getString("kind"),
                        item.getString("subject"), item.getString("value"),
                        item.getInt("revision"))
                }
            }
            val questionsJson = questionResult.getJSONArray("questions")
            val questions = (0 until questionsJson.length()).map { index ->
                questionsJson.getJSONObject(index).let { item ->
                    ChildQuestion(item.getInt("id"), item.getString("question"),
                        item.getString("answer"), item.getString("createdAt"))
                }
            }
            val policy = questionResult.getJSONObject("policy")
            mutableState.value = ParentMemoryState(
                pairedDeviceId = me.getString("deviceId"), mood = character.optString("mood"),
                band = character.optString("band"), needs = needs,
                stateSyncedAtMillis = System.currentTimeMillis(),
                enforcement = character.optString("enforcement"),
                timeZone = characterPolicy.optString("timeZone", "Asia/Shanghai"),
                deepNightLock = characterPolicy.optBoolean("deepNightLock"),
                policyRevision = character.optInt("policyRevision", 1),
                headPolicyRevision = ack.optInt("policyRevision"),
                headPersonalityRevision = ack.optInt("personalityRevision"),
                headFresh = ack.optBoolean("fresh"),
                clockQuality = ack.optString("clockQuality", "unknown"),
                nightLockActive = ack.optBoolean("nightLockActive"),
                personality = CloudPersonality(traits.optInt("curiosity", 78),
                    traits.optInt("warmth", 84), traits.optInt("talkativeness", 46),
                    traits.optInt("revision", 1)),
                historyEnabled = policy.optBoolean("enabled"),
                retentionDays = policy.optInt("retentionDays", 30),
                memories = memories, questions = questions,
                hasMoreQuestions = questions.size == 20)
        } catch (error: Exception) {
            val message = error.message ?: "加载失败；请确认手机可以访问互联网"
            if (message.contains("家长授权已失效")) {
                token = null
                prefs.edit().remove("token_ciphertext").remove("device_id").apply()
                mutableState.value = ParentMemoryState(error = message)
            } else {
                mutableState.value = mutableState.value.copy(busy = false,
                    error = "$message；显示的是上次同步数据")
            }
        }
    }

    /** Refreshes only the small state snapshot used by Home, not memories or history. */
    suspend fun refreshOverview() {
        if (token == null || mutableState.value.busy) return
        try {
            val result = request("/api/parent/v1/state")
            val character = result.getJSONObject("state")
            val ack = result.getJSONObject("ack")
            val needsJson = character.getJSONObject("needs")
            val needs = needsJson.keys().asSequence().associateWith { needsJson.optInt(it) }
            mutableState.value = mutableState.value.copy(
                mood = character.optString("mood"),
                band = character.optString("band"),
                needs = needs,
                stateSyncedAtMillis = System.currentTimeMillis(),
                headFresh = ack.optBoolean("fresh"),
                error = null,
            )
        } catch (error: Exception) {
            mutableState.value = mutableState.value.copy(
                error = error.message ?: "状态暂时无法更新",
            )
        }
    }

    suspend fun loadMoreQuestions() {
        val previous = mutableState.value
        val last = previous.questions.lastOrNull() ?: return
        if (!previous.hasMoreQuestions || previous.busy) return
        mutableState.value = previous.copy(busy = true, error = null)
        try {
            val rows = request("/api/parent/v1/questions?beforeId=${last.id}")
                .getJSONArray("questions")
            val more = (0 until rows.length()).map { index ->
                rows.getJSONObject(index).let { item ->
                    ChildQuestion(item.getInt("id"), item.getString("question"),
                        item.getString("answer"), item.getString("createdAt"))
                }
            }
            mutableState.value = previous.copy(busy = false,
                questions = previous.questions + more, hasMoreQuestions = more.size == 20)
        } catch (error: Exception) {
            mutableState.value = previous.copy(busy = false,
                error = error.message ?: "加载更多失败")
        }
    }

    suspend fun addMemory(kind: String, subject: String, value: String) = mutate {
        request("/api/parent/v1/memories", "POST", JSONObject()
            .put("kind", kind).put("subject", subject).put("value", value.trim()))
    }

    suspend fun updateMemory(id: String, revision: Int, value: String) = mutate {
        request("/api/parent/v1/memories?id=$id", "PUT", JSONObject()
            .put("revision", revision).put("value", value.trim()))
    }

    suspend fun clearMemories() = mutate {
        request("/api/parent/v1/memories", "DELETE")
    }

    suspend fun recordInteraction(kind: String) {
        val eventId = UUID.randomUUID().toString()
        val result = request("/api/parent/v1/events", "POST", JSONObject()
            .put("eventId", eventId).put("kind", kind))
        val character = result.getJSONObject("state")
        val needsJson = character.getJSONObject("needs")
        val needs = needsJson.keys().asSequence().associateWith { needsJson.optInt(it) }
        mutableState.value = mutableState.value.copy(mood = character.optString("mood"),
            band = character.optString("band"), needs = needs,
            stateSyncedAtMillis = System.currentTimeMillis())
    }

    suspend fun deleteMemory(id: String) = mutate {
        request("/api/parent/v1/memories?id=$id", "DELETE")
    }

    suspend fun setQuestionHistory(enabled: Boolean, retentionDays: Int =
        mutableState.value.retentionDays) = mutate {
        request("/api/parent/v1/questions", "PUT", JSONObject()
            .put("enabled", enabled).put("retentionDays", retentionDays))
    }

    suspend fun setPolicy(timeZone: String, deepNightLock: Boolean) = mutate {
        val current = mutableState.value
        request("/api/parent/v1/policy", "PUT", JSONObject()
            .put("timeZone", timeZone).put("deepNightLock", deepNightLock)
            .put("revision", current.policyRevision))
    }

    suspend fun setPersonality(curiosity: Int, warmth: Int,
        talkativeness: Int) = mutate {
        request("/api/parent/v1/personality", "PUT", JSONObject()
            .put("curiosity", curiosity).put("warmth", warmth)
            .put("talkativeness", talkativeness)
            .put("revision", mutableState.value.personality.revision))
    }

    suspend fun exportTo(uri: Uri) {
        mutableState.value = mutableState.value.copy(busy = true, error = null)
        try {
            val me = request("/api/parent/v1/me")
            val state = request("/api/parent/v1/state")
            val memories = request("/api/parent/v1/memories")
                .getJSONArray("memories")
            val header = JSONObject().put("format", "baby-tank-parent-export-v1")
                .put("deviceId", me.getString("deviceId"))
                .put("exportedAt", java.time.Instant.now().toString())
                .put("state", state).put("memories", memories)
            withContext(Dispatchers.IO) {
                val output = app.contentResolver.openOutputStream(uri)
                    ?: throw IllegalStateException("无法创建导出文件")
                output.bufferedWriter(Charsets.UTF_8).use { writer ->
                    writer.write(header.toString().removeSuffix("}"))
                    writer.write(",\"questions\":[")
                    var first = true
                    var beforeId: Int? = null
                    do {
                        val suffix = beforeId?.let { "?beforeId=$it&limit=50" }
                            ?: "?limit=50"
                        val page = request("/api/parent/v1/questions$suffix")
                            .getJSONArray("questions")
                        for (index in 0 until page.length()) {
                            if (!first) writer.write(",")
                            writer.write(page.getJSONObject(index).toString())
                            first = false
                        }
                        beforeId = if (page.length() == 50)
                            page.getJSONObject(page.length() - 1).getInt("id") else null
                    } while (beforeId != null)
                    writer.write("]}")
                }
            }
            mutableState.value = mutableState.value.copy(busy = false,
                error = "已导出；文件包含孩子的询问，请妥善保管")
        } catch (error: Exception) {
            mutableState.value = mutableState.value.copy(busy = false,
                error = error.message ?: "导出失败")
        }
    }

    suspend fun deleteQuestion(id: Int? = null) = mutate {
        request("/api/parent/v1/questions" + (id?.let { "?id=$it" } ?: ""), "DELETE")
    }

    private suspend fun mutate(block: suspend () -> Unit) {
        mutableState.value = mutableState.value.copy(busy = true, error = null)
        try {
            block()
            refresh()
        } catch (error: Exception) {
            mutableState.value = mutableState.value.copy(busy = false,
                error = (error.message ?: "操作失败") + "；原有内容未更改")
        }
    }

    suspend fun logout() {
        val remoteRevoked = try {
            request("/api/parent/v1/logout", "POST", JSONObject())
            true
        } catch (_: Exception) { false }
        token = null
        prefs.edit().remove("token_ciphertext").remove("device_id").apply()
        mutableState.value = ParentMemoryState(error = if (remoteRevoked) null else
            "本机已退出；云端会话可能仍有效，请在管理后台撤销家长访问")
    }
}
