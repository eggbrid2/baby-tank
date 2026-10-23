export function readDeviceStateAck(db, deviceId, at = new Date().toISOString()) {
  const row = db.prepare(`SELECT policy_revision,personality_revision,
    clock_quality,night_lock_active,updated_at FROM character_state_ack
    WHERE device_id=?`).get(deviceId);
  if (!row) return { policyRevision: 0, personalityRevision: 0,
    clockQuality: 'unknown', nightLockActive: false, updatedAt: null,
    fresh: false };
  return { policyRevision: row.policy_revision,
    personalityRevision: row.personality_revision,
    clockQuality: row.clock_quality,
    nightLockActive: row.night_lock_active === 1,
    updatedAt: row.updated_at,
    fresh: new Date(at).getTime() - new Date(row.updated_at).getTime() <=
      15 * 60_000 };
}

export function recordDeviceStateAck(db, deviceId, input,
                                     at = new Date().toISOString()) {
  if (!Number.isInteger(input?.policyRevision) || input.policyRevision < 1 ||
      !Number.isInteger(input?.personalityRevision) ||
      input.personalityRevision < 1 ||
      !['verified', 'estimated', 'unknown'].includes(input?.clockQuality) ||
      typeof input?.nightLockActive !== 'boolean') {
    throw new RangeError('invalid_device_state_ack');
  }
  db.prepare(`INSERT INTO character_state_ack
    (device_id,policy_revision,personality_revision,clock_quality,
      night_lock_active,updated_at) VALUES (?,?,?,?,?,?)
    ON CONFLICT(device_id) DO UPDATE SET
      policy_revision=excluded.policy_revision,
      personality_revision=excluded.personality_revision,
      clock_quality=excluded.clock_quality,
      night_lock_active=excluded.night_lock_active,
      updated_at=excluded.updated_at`).run(deviceId, input.policyRevision,
    input.personalityRevision, input.clockQuality,
    input.nightLockActive ? 1 : 0, at);
  return readDeviceStateAck(db, deviceId, at);
}
