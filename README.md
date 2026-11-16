# memcached-XDP

Memcached の GET を XDP/eBPF でカーネル入口から返す（BMC 方式）研究用リポジトリ。
背景と研究計画は [doc/README.md](doc/README.md) を参照。

## 実験環境

| コンポーネント | 実行方法 | 待ち受け |
| --- | --- | --- |
| Memcached 1.6.45 | Docker (`network_mode: host`) | `127.0.0.1:11211` TCP + **UDP** |
| MySQL 8.4 | Docker (bridge) | `127.0.0.1:3306`、DB `kvstore`、テーブル `kv(k, v)` |

- Memcached はホストのネットワークで動かしているので、XDP/TC プログラムはホストの
  インターフェース（今は 1 台構成なので `lo`）にそのままアタッチできる。
- UDP はデフォルト無効なので `-U 11211` で有効にしている（BMC は UDP GET が対象）。
- 2 台構成にするときは `MEMCACHED_LISTEN=0.0.0.0 make up` のように待ち受けアドレスを変える。
- MySQL には `key:0000000000` 〜 `key:0000099999`（値 64 バイト）の 10 万件が初期投入される。

### 使い方

```sh
make up                     # 起動
make check                  # memcached の TCP set/get・UDP get と、MySQL の件数を確認
make warm                   # MySQL の全件を memcached に載せる (LIMIT=N で件数指定)
make seed N=1000000 SIZE=256  # DB をキー数・値サイズを変えて作り直す
make flush                  # memcached を空にする
make mysql                  # MySQL シェル
make down                   # 停止 (データは残る) / make clean でボリュームごと削除
```

設定は環境変数で上書きできる（`MEMCACHED_MEMORY_MB`, `MEMCACHED_THREADS`, `MYSQL_ROOT_PASSWORD` など。
一覧は `docker-compose.yml` を参照）。パスワードは開発用のデフォルト値なので外部には公開しないこと。
