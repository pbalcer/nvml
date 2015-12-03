mkfifo kv_fifo$1; ./kv_client < kv_fifo$1 | nc 10.91.102.192 9001 > kv_fifo$1; rm kv_fifo$1
