#include <stdio.h>
#include <stdlib.h>
#include "queue.h"

int empty(struct queue_t *q)
{
	if (q == NULL) 
		return 1;
	return (q->size == 0);
}

void enqueue(struct queue_t *q, struct pcb_t *proc) {
    if (q == NULL || proc == NULL || q->size >= MAX_QUEUE_SIZE) {
        return;
    }
    q->proc[q->size] = proc;
    q->size++;
}

struct pcb_t *dequeue(struct queue_t *q) {
    if (empty(q)) {
        return NULL;
    }

    struct pcb_t *proc = q->proc[0];

    //dồn mảng
    for (int i = 0; i < q->size - 1; i++) {
        q->proc[i] = q->proc[i + 1];
    }
    q->proc[q->size - 1] = NULL;
    q->size--;

    return proc;
}

struct pcb_t *purgequeue(struct queue_t *q, struct pcb_t *proc) {
    if (empty(q) || proc == NULL) {
        return NULL;
    }

    int target_idx = -1;
    for (int i = 0; i < q->size; i++) {
        if (q->proc[i] == proc) {
            target_idx = i;
            break;
        }
    }

    //nếu tìm thấy proc thì dồn mảng
    if (target_idx != -1) {
        for (int i = target_idx; i < q->size - 1; i++) {
            q->proc[i] = q->proc[i + 1];
        }
        q->proc[q->size - 1] = NULL;
        q->size--;
        return proc;
    }

    return NULL;
}