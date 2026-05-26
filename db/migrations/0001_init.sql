-- Schema version 1: initial layout for user prefs, item history, locres index,
-- pricing cache, and OCR model registry.

CREATE TABLE user_settings (
   key         TEXT PRIMARY KEY,
   value       TEXT NOT NULL,
   updated_at  INTEGER NOT NULL DEFAULT (unixepoch ())
);

CREATE TABLE user_hotkeys (
   action_id    TEXT PRIMARY KEY,
   accelerator  TEXT NOT NULL
);

CREATE TABLE session_runs (
   session_id    INTEGER PRIMARY KEY,
   started_at    INTEGER NOT NULL,
   ended_at      INTEGER,
   game_mode     TEXT,
   game_version  TEXT,
   language      TEXT
);

CREATE TABLE item_finds (
   find_id         INTEGER PRIMARY KEY,
   session_id      INTEGER REFERENCES session_runs (session_id),
   found_at        INTEGER NOT NULL,
   canonical_name  TEXT NOT NULL,
   locres_hash     TEXT NOT NULL,
   rarity          TEXT NOT NULL,
   attrs_json      TEXT NOT NULL,
   market_price    INTEGER,
   vendor_price    INTEGER,
   fingerprint     TEXT NOT NULL
);

CREATE INDEX idx_item_finds_session ON item_finds (session_id, found_at DESC);
CREATE INDEX idx_item_finds_fp      ON item_finds (fingerprint);

CREATE TABLE item_catalog (
   locres_hash  TEXT NOT NULL,
   language     TEXT NOT NULL,
   value        TEXT NOT NULL,
   value_norm   TEXT NOT NULL,
   PRIMARY KEY (locres_hash, language)
);

CREATE VIRTUAL TABLE item_catalog_fts USING fts5 (
   value_norm,
   content    = 'item_catalog',
   content_rowid = 'rowid',
   tokenize   = 'trigram'
);

CREATE TABLE pricing_cache (
   cache_key      TEXT PRIMARY KEY,
   response_json  TEXT NOT NULL,
   fetched_at     INTEGER NOT NULL,
   ttl_seconds    INTEGER NOT NULL
);

CREATE TABLE ocr_models (
   model_id      TEXT PRIMARY KEY,
   version       TEXT NOT NULL,
   size_bytes    INTEGER NOT NULL,
   installed_at  INTEGER,
   sha256        TEXT NOT NULL
);
