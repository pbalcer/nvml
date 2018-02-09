CFLAGS=`pkg-config --cflags libpmemobj`
LDFLAGS=`pkg-config --libs libpmemobj`
LDPATH=../src/nondebug
POOL=/mnt/pmem/aaa
#18437590
#24582757

#POOL=/dev/shm/aaa
#24581691

all: bench_tx_alloc

bench_tx_alloc: bench_tx_alloc.c
	gcc -g bench_tx_alloc.c $(CFLAGS) $(LDFLAGS) -o bench_tx_alloc

run: bench_tx_alloc
	rm -f $(POOL)
	PMEM_IS_PMEM_FORCE=1 LD_LIBRARY_PATH=$(LDPATH):${LD_LIBRARY_PATH} stdbuf -i0 -o0 -e0 taskset 2 ./bench_tx_alloc $(POOL)
	#sleep 1
