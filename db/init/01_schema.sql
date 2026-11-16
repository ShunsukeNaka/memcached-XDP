-- Memcached の裏に置く元データ。キーは memcached のキー (最大 250 バイト) と 1:1 に対応させる。
USE kvstore;

CREATE TABLE IF NOT EXISTS kv (
  k          VARCHAR(250)  NOT NULL,
  v          VARBINARY(8192) NOT NULL,
  updated_at TIMESTAMP(6)  NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  PRIMARY KEY (k)
) ENGINE=InnoDB;
