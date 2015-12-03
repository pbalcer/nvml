#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

#define	RRAND(max, min) (rand() % ((max) - (min)) + (min))

#define RESP_MSG_LEN 50

#define	MAX_KEY_LEN 50
#define KEY_SLOTS 10000

#define MIN_VALUE 10
#define MAX_VALUE (64*1024)

static struct {
	int used;
	char key[MAX_KEY_LEN];
} key_slots[KEY_SLOTS];

static char *send_buf;

static int
find_first_free_slot()
{
	for (int i = 0; i < KEY_SLOTS; ++i)
		if (key_slots[i].used == 0)
			return i;

	return -1;
}

static void
expect_success()
{
	static char buf[RESP_MSG_LEN] = {0};
	scanf("%s", buf);
	if (strcmp(buf, "SUCCESS") != 0) {
		fprintf(stderr, "F");
	}
}

static void
remove_key(int slot)
{
	if (key_slots[slot].used == 0)
		return;

	key_slots[slot].used = 0;

	fprintf(stderr, "-");
	printf("REMOVE %s\n", key_slots[slot].key);
	expect_success();
}

static void
remove_random_keys()
{
	int nremove = RRAND(KEY_SLOTS/10, 1);

	for (int i = 0; i < nremove; ++i) {
		remove_key(RRAND(KEY_SLOTS, 0));
	}
}

void
fill_random(char *buf, size_t len)
{
	for (size_t i = 0; i < len; ++i) {
		buf[i] = RRAND('z', 'a');
	}
}

static char *
create_random_key(int slot)
{
	fill_random(key_slots[slot].key, MAX_KEY_LEN);
	key_slots[slot].used = 1;

	return key_slots[slot].key;
}

static char *create_random_value()
{
	size_t len = RRAND(MAX_VALUE, MIN_VALUE);

	send_buf[len] = 0;
	fill_random(send_buf, len);

	return send_buf;
}

void
kv_op()
{
	int slot = find_first_free_slot();

	if (RRAND(100, 0) == 0) {
		remove_random_keys();
	}

	if (slot == -1) {
		remove_random_keys();
		slot = find_first_free_slot();
		assert(slot != -1);
	}

	printf("INSERT %s %s\n", create_random_key(slot), create_random_value());

	expect_success();

	fprintf(stderr, "+");
}

int main(int argc, const char *argv[])
{
	setbuf(stdout, NULL);
	srand(time(NULL));
	send_buf = malloc(MAX_VALUE);

	while(1) {
		kv_op();
		//sleep(1);
	}

	free(send_buf);

	return 0;
}
