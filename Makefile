.PHONY: up down ps logs check seed warm mysql flush clean

up:            ## memcached と mysql を起動 (mysql が healthy になるまで待つ)
	docker compose up -d --wait

down:          ## 停止 (データは残る)
	docker compose down

ps:
	docker compose ps

logs:
	docker compose logs -f

check:         ## memcached (TCP/UDP) と mysql の疎通確認
	python3 scripts/mc_check.py
	docker exec mcxdp-mysql sh -c 'mysql -uroot -p"$$MYSQL_ROOT_PASSWORD" -N kvstore -e "SELECT COUNT(*) FROM kv"' 2>/dev/null

seed:          ## make seed N=1000000 SIZE=256
	scripts/seed_db.sh $(or $(N),100000) $(or $(SIZE),64)

warm:          ## DB の内容を memcached に載せる (make warm LIMIT=10000)
	scripts/warm_cache.sh $(LIMIT)

flush:         ## memcached を空にする
	printf 'flush_all\r\nquit\r\n' | nc -q 2 127.0.0.1 11211

mysql:         ## mysql シェル
	docker exec -it mcxdp-mysql sh -c 'mysql -uroot -p"$$MYSQL_ROOT_PASSWORD" kvstore'

clean:         ## 停止して DB ボリュームも削除
	docker compose down -v
