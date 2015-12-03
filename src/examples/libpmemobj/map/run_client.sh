mkfifo kv_fifo$1; ./kv_client < kv_fifo$1 | nc 127.0.0.1 9000 > kv_fifo$1; rm kv_fifo$1
