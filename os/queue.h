#ifndef QUEUE_H
#define QUEUE_H
#define QUEUE_SIZE (1024)

struct queue {
	int data[QUEUE_SIZE];
	int size;
};

void init_queue(struct queue *);
void push_queue(struct queue *, int);
int pop_queue(struct queue *);

#endif // QUEUE_H
