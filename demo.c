#include "os.h"
#include "os_queue.h"
#include "os_sem.h"

#define LED_1_STACK_SZ      80
#define LED_2_STACK_SZ      80
#define QUEUE_PUB_STACK_SZ  80
#define QUEUE_RECV_STACK_SZ 80

#define MESSAGE_SIZE 32
#define QUEUE_DEPTH  8

#define RING_BUFFER_DEPTH (40U)

static OS_Queue queue;
static OS_Sem sem;

static uint32_t led_1_stack[LED_1_STACK_SZ] __attribute__((aligned(8)));
static uint32_t led_2_stack[LED_2_STACK_SZ] __attribute__((aligned(8)));
static uint32_t queue_pub_stack[QUEUE_PUB_STACK_SZ] __attribute__((aligned(8)));
static uint32_t queue_recv_stack[QUEUE_RECV_STACK_SZ] __attribute__((aligned(8)));

/* Calculate required buffer size: depth * (size + 1) */
/* (+1 byte per slot to store length)               */
static uint8_t queue_buffer[QUEUE_DEPTH * (MESSAGE_SIZE + 1)];

uint8_t queue_recv_ring_buff[RING_BUFFER_DEPTH][8] = {0};
uint8_t queue_recv_ring_buff_index                 = 0;

static void led_1_task(void)
{
    while (1)
    {
        turn_led_ON(LED_1);
        os_sleep_ms(25);
        turn_led_OFF(LED_1);
        os_sleep_ms(25);
    }
}

static void LED_2_task(void)
{
    while (1)
    {
        turn_led_ON(LED_2);
        os_sem_wait_timeout(&sem, OS_WAIT_FOREVER);
        turn_led_OFF(LED_2);
        os_sem_wait_timeout(&sem, OS_WAIT_FOREVER);
    }
}

static void queue_pub_task(void)
{
    uint8_t data[8] = {0};
    uint8_t counter = 0;
    while (1)
    {
        os_sleep_ms(100);
        for (uint8_t i = 0; i < sizeof(data); i++)
        {
            data[i] = counter + i;
        }
        counter++;
        os_queue_send(&queue, data, 8);
    }
}

static void queue_recv_task(void)
{
    uint8_t recv_data[32] = {0};
    uint8_t recv_len      = {0};

    while (1)
    {
        if (os_queue_receive_timeout(&queue, recv_data, &recv_len, OS_WAIT_FOREVER) == 0)
        {
            /* Boundary check before copying to prevent buffer overflow */
            uint8_t copy_len = (recv_len > sizeof(recv_data)) ? sizeof(recv_data) : recv_len;
            memcpy(queue_recv_ring_buff[queue_recv_ring_buff_index], recv_data, copy_len);
            queue_recv_ring_buff_index = (queue_recv_ring_buff_index + 1U) % RING_BUFFER_DEPTH;
        }
    }
}

void timer_cb_50ms_from_isr(void)
{
    uint8_t data[8] = {0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8};
    os_queue_send_from_isr(&queue, data, 8);
    os_sem_signal_from_isr(&sem);
}


void demo_init(void)
{
    os_kernel_init();

    os_queue_init(&queue, queue_buffer, MESSAGE_SIZE, QUEUE_DEPTH);
    os_sem_init(&sem, 0U);

    os_task_create(led_1_task, "led_1", led_1_stack, LED_1_STACK_SZ, 2U);
    os_task_create(LED_2_task, "LED_2", led_2_stack, LED_2_STACK_SZ, 2U);

    os_task_create(queue_pub_task, "queue_pub", queue_pub_stack, QUEUE_PUB_STACK_SZ, 4U);
    os_task_create(queue_recv_task, "queue_recv", queue_recv_stack, QUEUE_RECV_STACK_SZ, 5U);

    os_start(); 
    
    /* Next line shouldn't run */
    while(1);
}
