# 計測 (issue #3)

memcached の GET 1 回の時間がどこで使われているかを計測するための道具。
計測の計画、計測点 P0〜P6、区間 [A]〜[E] の定義、注意点は issue #3 を参照。

## 準備

```sh
sudo bench/setup_tools.sh      # bpftrace / bpftool / perf / strace / cpupower と FlameGraph を入れる (要 root)
make bench-build               # 負荷ツール bench/loadgen/mcload をビルド
make bench-up                  # memcached を計測用の設定で起動 (コア 0,1 に固定、-t 2)
make warm                      # DB の内容を memcached に載せる (ヒット率 100%)
```

seccomp の負荷を比べるときは `make bench-up SECCOMP=unconfined` で起動する。
固定するコアは `MEMCACHED_CPUS=0,1`、スレッド数は `MEMCACHED_THREADS=2` で変えられる。

## CPU の設定

```sh
bench/cpu_tune.sh show         # 現在の設定 (root 不要)。結果と一緒に記録する
sudo bench/cpu_tune.sh apply   # ガバナー performance、Turbo オフ、起床に 10us を超える C-state を無効化
sudo bench/cpu_tune.sh restore # apply の前の設定に戻す
```

C-state のしきい値は `MAX_IDLE_LATENCY_US` で変えられる (例: `sudo MAX_IDLE_LATENCY_US=2 bench/cpu_tune.sh apply`)。

## 負荷ツール mcload

接続 1 本につきスレッド 1 本で、GET を 1 つ送って応答を待つクローズドループで動く。
キーは `key:0000000000` から一様乱数で選ぶ。memcached とコアが重ならないよう `taskset` で固定して動かす。

```sh
# 飽和負荷 (UDP、2 接続)
taskset -c 2,3 bench/loadgen/mcload -P udp -c 2 -d 10

# 低負荷 (TCP、1 接続、100us ごとに 1 リクエスト)
taskset -c 2 bench/loadgen/mcload -P tcp -c 1 -i 100 -d 10

# 結果を CSV で残す
mkdir -p bench/results
bench/loadgen/mcload --csv-header > bench/results/run.csv
taskset -c 2,3 bench/loadgen/mcload -P udp -c 2 --csv >> bench/results/run.csv
```

| オプション | 意味 | 既定値 |
| --- | --- | --- |
| `-P udp\|tcp` | プロトコル | udp |
| `-c N` | 接続数 (= スレッド数 = 同時に処理中のリクエスト数) | 1 |
| `-d SEC` / `-w SEC` | 計測時間 / 計測前のウォームアップ | 10 / 1 |
| `-k N` | キーの数 (`key:0` 〜 `key:N-1`) | 100000 |
| `-i USEC` | 1 接続あたりのリクエスト間隔 (0 = 待たずに次を送る) | 0 |
| `-s HOST` / `-p PORT` | 接続先 | 127.0.0.1 / 11211 |

レイテンシは送信の直前から応答を受け取り終わるまでで、100ns 刻みのヒストグラムから P50〜P99.9 を出す。
UDP の応答が 1 秒以内に返ってこなければエラーとして数える (パケットが落ちた場合)。

## システムコールの確認

```sh
sudo bench/strace_syscalls.sh  # UDP と TCP で負荷をかけながら、memcached が使うシステムコールを集計する
```

この結果で、計測点 P4 / P5 に使う tracepoint を決める。
