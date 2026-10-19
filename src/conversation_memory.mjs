const MAX_TURNS = 8;
const RETENTION_MS = 30 * 24 * 60 * 60 * 1000;

export function readConversationMemory(db, secrets, deviceId, at = Date.now()) {
  const cutoff = new Date(at - RETENTION_MS).toISOString();
  const rows = db.prepare(`SELECT user_ciphertext,assistant_ciphertext
    FROM conversation_turns WHERE device_id=? AND created_at>=?
    ORDER BY id DESC LIMIT ?`).all(deviceId, cutoff, MAX_TURNS);
  const turns = [];
  for (const row of rows.reverse()) {
    try {
      const user = secrets.decrypt(row.user_ciphertext);
      const assistant = secrets.decrypt(row.assistant_ciphertext);
      if (user && assistant) turns.push({ user, assistant });
    } catch {
      // An old key or corrupt row must not prevent the device from answering.
    }
  }
  return turns;
}

export function saveConversationMemory(db, secrets, deviceId, user, assistant,
  at = Date.now()) {
  const timestamp = new Date(at).toISOString();
  const cutoff = new Date(at - RETENTION_MS).toISOString();
  const userCiphertext = secrets.encrypt(String(user).slice(0, 384));
  const assistantCiphertext = secrets.encrypt(String(assistant).slice(0, 1024));
  if (!userCiphertext || !assistantCiphertext) return;
  db.exec('BEGIN IMMEDIATE');
  try {
    db.prepare(`INSERT INTO conversation_turns(device_id,user_ciphertext,
      assistant_ciphertext,created_at) VALUES (?,?,?,?)`).run(
      deviceId, userCiphertext, assistantCiphertext, timestamp);
    db.prepare('DELETE FROM conversation_turns WHERE created_at<?').run(cutoff);
    db.prepare(`DELETE FROM conversation_turns WHERE device_id=? AND id NOT IN
      (SELECT id FROM conversation_turns WHERE device_id=? ORDER BY id DESC LIMIT ?)`)
      .run(deviceId, deviceId, MAX_TURNS);
    db.exec('COMMIT');
  } catch (error) {
    db.exec('ROLLBACK');
    throw error;
  }
}
