#ifndef TEST_TASK_H
#define TEST_TASK_H
static inline int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                              void *arg, unsigned priority, void *handle)
{
    (void)task; (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    return 1;
}
#endif
