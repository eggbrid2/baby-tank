import { DatabaseSync } from 'node:sqlite';
import { mkdirSync } from 'node:fs';
import { dirname } from 'node:path';
import { hashPassword, now } from './lib.mjs';

export function openDatabase(path, bootstrap = {}) {
  mkdirSync(dirname(path), { recursive: true });
  const db = new DatabaseSync(path);
  db.exec('PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA busy_timeout=5000;');
  db.exec(`
    CREATE TABLE IF NOT EXISTS users (
      id INTEGER PRIMARY KEY,
      username TEXT NOT NULL UNIQUE,
      password_hash TEXT NOT NULL,
      created_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS sessions (
      token_hash TEXT PRIMARY KEY,
      user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      csrf_token TEXT NOT NULL,
      expires_at TEXT NOT NULL,
      created_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS provider_configs (
      id INTEGER PRIMARY KEY,
      kind TEXT NOT NULL CHECK(kind IN ('llm','asr','tts')),
      provider TEXT NOT NULL,
      label TEXT NOT NULL,
      enabled INTEGER NOT NULL DEFAULT 0,
      base_url TEXT,
      model TEXT,
      secret_ciphertext TEXT,
      options_json TEXT NOT NULL DEFAULT '{}',
      updated_at TEXT NOT NULL,
      UNIQUE(kind, provider)
    );
    CREATE TABLE IF NOT EXISTS devices (
      id INTEGER PRIMARY KEY,
      device_id TEXT NOT NULL UNIQUE,
      name TEXT NOT NULL,
      board TEXT NOT NULL,
      token_hash TEXT NOT NULL UNIQUE,
      firmware_version TEXT,
      ip_address TEXT,
      last_seen_at TEXT,
      created_at TEXT NOT NULL,
      revoked_at TEXT
    );
    CREATE TABLE IF NOT EXISTS firmware (
      id INTEGER PRIMARY KEY,
      version TEXT NOT NULL,
      board TEXT NOT NULL,
      channel TEXT NOT NULL DEFAULT 'stable',
      file_name TEXT NOT NULL,
      file_path TEXT NOT NULL,
      byte_size INTEGER NOT NULL,
      sha256 TEXT NOT NULL,
      notes TEXT NOT NULL DEFAULT '',
      chip TEXT NOT NULL DEFAULT 'esp32s3',
      secure_version INTEGER NOT NULL DEFAULT 0,
      mandatory INTEGER NOT NULL DEFAULT 0,
      rollout_percent INTEGER NOT NULL DEFAULT 100,
      secure_boot_required INTEGER NOT NULL DEFAULT 0,
      published INTEGER NOT NULL DEFAULT 0,
      created_at TEXT NOT NULL,
      UNIQUE(version, board, channel)
    );
    CREATE TABLE IF NOT EXISTS cloud_accounts (
      id INTEGER PRIMARY KEY,
      email TEXT NOT NULL UNIQUE COLLATE NOCASE,
      display_name TEXT NOT NULL DEFAULT '',
      password_hash TEXT NOT NULL,
      status TEXT NOT NULL DEFAULT 'active' CHECK(status IN ('active','disabled')),
      email_verified_at TEXT,
      created_at TEXT NOT NULL,
      updated_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS cloud_sessions (
      token_hash TEXT PRIMARY KEY,
      account_id INTEGER NOT NULL REFERENCES cloud_accounts(id) ON DELETE CASCADE,
      csrf_token TEXT NOT NULL,
      expires_at TEXT NOT NULL,
      created_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS audit_log (
      id INTEGER PRIMARY KEY,
      action TEXT NOT NULL,
      actor TEXT NOT NULL,
      detail_json TEXT NOT NULL DEFAULT '{}',
      created_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS ota_events (
      id INTEGER PRIMARY KEY,
      device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
      firmware_id INTEGER REFERENCES firmware(id) ON DELETE SET NULL,
      event TEXT NOT NULL CHECK(event IN ('available','downloaded','installed','validated','rollback','failed')),
      detail TEXT NOT NULL DEFAULT '',
      created_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS conversation_metrics (
      id INTEGER PRIMARY KEY,
      trace_id TEXT NOT NULL UNIQUE,
      device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
      metrics_json TEXT NOT NULL,
      created_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS conversation_turns (
      id INTEGER PRIMARY KEY,
      device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
      user_ciphertext TEXT NOT NULL,
      assistant_ciphertext TEXT NOT NULL,
      created_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS device_mood_state (
      device_id INTEGER PRIMARY KEY REFERENCES devices(id) ON DELETE CASCADE,
      valence INTEGER NOT NULL CHECK(valence BETWEEN -100 AND 100),
      arousal INTEGER NOT NULL CHECK(arousal BETWEEN -100 AND 100),
      dominance INTEGER NOT NULL CHECK(dominance BETWEEN -100 AND 100),
      label TEXT NOT NULL,
      intensity INTEGER NOT NULL CHECK(intensity BETWEEN 20 AND 100),
      thought_key TEXT NOT NULL,
      thought_ciphertext TEXT,
      updated_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS device_mood_events (
      id INTEGER PRIMARY KEY,
      device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
      trace_id TEXT,
      source TEXT NOT NULL CHECK(source IN ('speech','touch','vision')),
      trigger_emotion TEXT NOT NULL,
      trigger_intensity INTEGER NOT NULL,
      valence INTEGER NOT NULL,
      arousal INTEGER NOT NULL,
      dominance INTEGER NOT NULL,
      label TEXT NOT NULL,
      thought_key TEXT NOT NULL,
      created_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS device_dream_decks (
      device_id INTEGER PRIMARY KEY REFERENCES devices(id) ON DELETE CASCADE,
      local_day TEXT NOT NULL,
      entries_json TEXT NOT NULL,
      source TEXT NOT NULL CHECK(source IN ('ai','fallback')),
      generated_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS character_state (
      device_id INTEGER PRIMARY KEY REFERENCES devices(id) ON DELETE CASCADE,
      needs_json TEXT NOT NULL,
      time_zone TEXT NOT NULL DEFAULT 'Asia/Shanghai',
      deep_night_lock INTEGER NOT NULL DEFAULT 0 CHECK(deep_night_lock IN (0,1)),
      revision INTEGER NOT NULL DEFAULT 1,
      policy_revision INTEGER NOT NULL DEFAULT 1,
      updated_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS character_state_events (
      id INTEGER PRIMARY KEY,
      device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
      event_id TEXT NOT NULL,
      source TEXT NOT NULL CHECK(source IN ('device','parent','conversation')),
      kind TEXT NOT NULL,
      revision INTEGER NOT NULL,
      created_at TEXT NOT NULL,
      UNIQUE(device_id,event_id)
    );
    CREATE TABLE IF NOT EXISTS character_state_ack (
      device_id INTEGER PRIMARY KEY REFERENCES devices(id) ON DELETE CASCADE,
      policy_revision INTEGER NOT NULL DEFAULT 0,
      personality_revision INTEGER NOT NULL DEFAULT 0,
      clock_quality TEXT NOT NULL DEFAULT 'unknown'
        CHECK(clock_quality IN ('verified','estimated','unknown')),
      night_lock_active INTEGER NOT NULL DEFAULT 0 CHECK(night_lock_active IN (0,1)),
      updated_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS character_personality (
      device_id INTEGER PRIMARY KEY REFERENCES devices(id) ON DELETE CASCADE,
      curiosity INTEGER NOT NULL CHECK(curiosity BETWEEN 0 AND 100),
      warmth INTEGER NOT NULL CHECK(warmth BETWEEN 0 AND 100),
      talkativeness INTEGER NOT NULL CHECK(talkativeness BETWEEN 0 AND 100),
      revision INTEGER NOT NULL DEFAULT 1,
      source TEXT NOT NULL DEFAULT 'default'
        CHECK(source IN ('default','device','parent')),
      updated_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS approved_memories (
      id TEXT PRIMARY KEY,
      device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
      kind TEXT NOT NULL CHECK(kind IN ('nickname','preference','shared_activity')),
      subject TEXT NOT NULL,
      value_ciphertext TEXT NOT NULL,
      created_at TEXT NOT NULL,
      updated_at TEXT NOT NULL,
      expires_at TEXT,
      revision INTEGER NOT NULL DEFAULT 1
    );
    CREATE TABLE IF NOT EXISTS question_history_policy (
      device_id INTEGER PRIMARY KEY REFERENCES devices(id) ON DELETE CASCADE,
      enabled INTEGER NOT NULL DEFAULT 0 CHECK(enabled IN (0,1)),
      retention_days INTEGER NOT NULL DEFAULT 30 CHECK(retention_days BETWEEN 1 AND 30),
      updated_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS question_history (
      id INTEGER PRIMARY KEY,
      device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
      trace_id TEXT NOT NULL,
      question_ciphertext TEXT NOT NULL,
      answer_ciphertext TEXT NOT NULL,
      created_at TEXT NOT NULL,
      UNIQUE(device_id,trace_id)
    );
    CREATE TABLE IF NOT EXISTS parent_claims (
      code_hash TEXT PRIMARY KEY,
      device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
      expires_at TEXT NOT NULL,
      consumed_at TEXT,
      created_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS parent_sessions (
      token_hash TEXT PRIMARY KEY,
      device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
      expires_at TEXT NOT NULL,
      revoked_at TEXT,
      created_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS ai_action_log (
      id INTEGER PRIMARY KEY,
      trace_id TEXT NOT NULL UNIQUE,
      device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
      source TEXT NOT NULL CHECK(source IN ('speech','touch')),
      status TEXT NOT NULL CHECK(status IN ('none','shadow','rejected')),
      action_type TEXT NOT NULL CHECK(action_type IN ('none','drive')),
      direction TEXT CHECK(direction IN ('forward','backward','left','right')),
      speed_per_mille INTEGER,
      duration_ms INTEGER,
      reason TEXT NOT NULL DEFAULT '',
      diagnostic INTEGER NOT NULL DEFAULT 0,
      executed INTEGER NOT NULL DEFAULT 0 CHECK(executed=0),
      created_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS ai_action_execution (
      trace_id TEXT PRIMARY KEY REFERENCES ai_action_log(trace_id) ON DELETE CASCADE,
      device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
      direction TEXT NOT NULL CHECK(direction IN ('forward','backward','left','right')),
      authorized_at TEXT NOT NULL,
      result TEXT CHECK(result IN ('completed','blocked','failed')),
      reason TEXT NOT NULL DEFAULT '',
      reported_at TEXT
    );
    CREATE TABLE IF NOT EXISTS ai_exploration_events (
      id INTEGER PRIMARY KEY,
      trace_id TEXT NOT NULL,
      device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
      step_index INTEGER NOT NULL,
      event TEXT NOT NULL CHECK(event IN ('authorized','observed','planned','stopped','completed','blocked','failed')),
      direction TEXT,
      duration_ms INTEGER,
      light_mode TEXT,
      emotion TEXT,
      detail TEXT NOT NULL DEFAULT '',
      created_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS ai_camera_events (
      id INTEGER PRIMARY KEY,
      trace_id TEXT NOT NULL,
      device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
      event TEXT NOT NULL CHECK(event IN ('requested','received','answered','failed')),
      detail TEXT NOT NULL DEFAULT '',
      created_at TEXT NOT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_firmware_release ON firmware(board, channel, published);
    CREATE INDEX IF NOT EXISTS idx_cloud_accounts_created ON cloud_accounts(created_at DESC);
    CREATE INDEX IF NOT EXISTS idx_cloud_sessions_account ON cloud_sessions(account_id,expires_at);
    CREATE INDEX IF NOT EXISTS idx_devices_seen ON devices(last_seen_at);
    CREATE INDEX IF NOT EXISTS idx_ota_device_time ON ota_events(device_id, created_at);
    CREATE INDEX IF NOT EXISTS idx_conversation_metrics_time ON conversation_metrics(created_at);
    CREATE INDEX IF NOT EXISTS idx_conversation_turns_device ON conversation_turns(device_id,id DESC);
    CREATE INDEX IF NOT EXISTS idx_device_mood_events_device ON device_mood_events(device_id,id DESC);
    CREATE INDEX IF NOT EXISTS idx_character_state_events_device ON character_state_events(device_id,created_at DESC);
    CREATE INDEX IF NOT EXISTS idx_approved_memories_device ON approved_memories(device_id,updated_at DESC);
    CREATE INDEX IF NOT EXISTS idx_question_history_device ON question_history(device_id,id DESC);
    CREATE INDEX IF NOT EXISTS idx_parent_sessions_device ON parent_sessions(device_id,expires_at);
    CREATE INDEX IF NOT EXISTS idx_ai_action_log_time ON ai_action_log(created_at);
    CREATE INDEX IF NOT EXISTS idx_ai_exploration_events_trace ON ai_exploration_events(trace_id,id);
    CREATE INDEX IF NOT EXISTS idx_ai_camera_events_trace ON ai_camera_events(trace_id,id);
  `);

  // Existing devices already have character_state rows. A policy version
  // must not advance when a child feeds the character or an event is retried.
  if (!db.prepare('PRAGMA table_info(character_state)').all()
    .some((column) => column.name === 'policy_revision')) {
    db.exec('ALTER TABLE character_state ADD COLUMN policy_revision INTEGER NOT NULL DEFAULT 1');
  }

  const count = db.prepare('SELECT COUNT(*) AS count FROM users').get().count;
  if (count === 0) {
    if (!bootstrap.password || bootstrap.password.length < 12) {
      throw new Error('ADMIN_PASSWORD must contain at least 12 characters on first start');
    }
    db.prepare('INSERT INTO users(username, password_hash, created_at) VALUES (?, ?, ?)')
      .run(bootstrap.username || 'admin', hashPassword(bootstrap.password), now());
  }
  return db;
}
