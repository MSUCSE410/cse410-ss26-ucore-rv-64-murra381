#include "queue.h"
#include "defs.h"
#include "proc.h"

void init_queue(struct queue *q)
{
	q->size = 0;
}


void push_queue(struct queue *q, int value)
{
	if (q->size >= NPROC) {
		panic("queue shouldn't be overflow");
	}
	int pos = q->size;
	while (pos > 0) {
		int prev = q->data[pos - 1];
		if (pool[prev].stride < pool[value].stride ||
		    (pool[prev].stride == pool[value].stride && prev < value)) {
			break;
		}
		q->data[pos] = prev;
		pos--;
	}
	q->data[pos] = value;
	q->size++;
}

int pop_queue(struct queue *q)
{
	if (q->size == 0)
		return -1;
	int value = q->data[0];
	for (int i = 1; i < q->size; i++) {
		q->data[i - 1] = q->data[i];
	}
	q->size--;
	return value;
}
