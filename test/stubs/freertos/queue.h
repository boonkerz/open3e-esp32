#ifndef TEST_QUEUE_H
#define TEST_QUEUE_H
#include <stddef.h>
typedef void *QueueHandle_t;
static inline QueueHandle_t xQueueCreate(unsigned n, size_t size)
{ (void)n; (void)size; return (void *)1; }
static inline int xQueueReceive(QueueHandle_t q, void *data, unsigned timeout)
{ (void)q; (void)data; (void)timeout; return 0; }
static inline int xQueueSend(QueueHandle_t q, const void *data, unsigned timeout)
{ (void)q; (void)data; (void)timeout; return 1; }
static inline void vQueueDelete(QueueHandle_t q) { (void)q; }
#endif
