-- Scope local history to the authenticated account. Existing rows remain
-- unowned and are never attached to a V2 account retroactively.

ALTER TABLE session_runs ADD COLUMN account_id TEXT;

CREATE INDEX idx_session_runs_account
   ON session_runs (account_id, started_at DESC);
