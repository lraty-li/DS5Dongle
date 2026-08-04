#include "ds5_usb_audio.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <FreeRTOS.h>
#include "portmacro.h"
#include "queue.h"
#include "task.h"

#include "resample.h"
#include "usbd_core.h"
#include "usbd_audio.h"

#include "ds5_haptics_mailbox.h"
#include "ds5_log.h"
#include "ds5_protocol.h"

#define DS5_USB_AUDIO_QUEUE_LENGTH       8U
#define DS5_USB_AUDIO_TASK_STACK_DEPTH   (configMINIMAL_STACK_SIZE * 8U)
#define DS5_USB_AUDIO_TASK_PRIORITY      (configMAX_PRIORITIES - 3U)
#define DS5_USB_AUDIO_LOG_INTERVAL       4096U
#define DS5_USB_AUDIO_OUTPUT_CHANNELS    2U
#define DS5_USB_AUDIO_MAX_OUTPUT_FRAMES  8U

typedef struct {
    uint32_t generation;
    uint16_t length;
    uint8_t data[DS5_USB_AUDIO_MAX_PACKET_BYTES];
} ds5_usb_audio_packet_t;

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX
    uint8_t audio_receive_buffer[DS5_USB_AUDIO_MAX_PACKET_BYTES];
static ds5_usb_audio_packet_t audio_staging_packet;

static StaticQueue_t audio_queue_storage;
static uint8_t audio_queue_items[DS5_USB_AUDIO_QUEUE_LENGTH *
                                 sizeof(ds5_usb_audio_packet_t)];
static QueueHandle_t audio_queue;

static StaticTask_t audio_task_storage;
static StackType_t audio_task_stack[DS5_USB_AUDIO_TASK_STACK_DEPTH];
static TaskHandle_t audio_task;

static struct usbd_interface audio_control_interface;
static struct usbd_interface audio_stream_interface;

static struct audio_entity_info audio_entity_table[] = {
    {
        .bDescriptorSubtype = AUDIO_CONTROL_FEATURE_UNIT,
        .bEntityId = 0x02U,
        .ep = DS5_USB_AUDIO_OUT_EP,
    },
};

static volatile bool audio_stream_open;
static volatile bool audio_read_pending;
static volatile uint32_t audio_generation;
static volatile uint32_t received_audio_packets;
static volatile uint32_t invalid_audio_packets;
static volatile uint32_t dropped_audio_packets;
static volatile uint32_t audio_arm_failures;
static volatile uint32_t published_haptics_blocks;
static uint8_t audio_bus_id;

static int16_t ds5_usb_audio_read_s16_le(const uint8_t *data)
{
    uint16_t value = (uint16_t)data[0] |
                     (uint16_t)((uint16_t)data[1] << 8U);

    return (int16_t)value;
}

static int8_t ds5_usb_audio_float_to_s8(WDL_ResampleSample sample)
{
    int value = (int)(sample * 127.0);

    if (value > 127) {
        value = 127;
    } else if (value < -128) {
        value = -128;
    }

    return (int8_t)value;
}

static bool ds5_usb_audio_queue_packet(uint32_t length)
{
    BaseType_t result;

    if ((audio_queue == NULL) || (length == 0U) ||
        (length > sizeof(audio_staging_packet.data)) ||
        ((length % (DS5_USB_AUDIO_CHANNEL_COUNT *
                    DS5_USB_AUDIO_SAMPLE_BYTES)) != 0U)) {
        ++invalid_audio_packets;
        return false;
    }

    audio_staging_packet.generation = audio_generation;
    audio_staging_packet.length = (uint16_t)length;
    memcpy(audio_staging_packet.data, audio_receive_buffer, length);

    if (xPortIsInsideInterrupt()) {
        BaseType_t task_woken = pdFALSE;

        result = xQueueSendFromISR(audio_queue, &audio_staging_packet,
                                   &task_woken);
        portYIELD_FROM_ISR(task_woken);
    } else {
        result = xQueueSend(audio_queue, &audio_staging_packet, 0U);
    }

    if (result != pdPASS) {
        ++dropped_audio_packets;
        return false;
    }

    ++received_audio_packets;
    return true;
}

static void ds5_usb_audio_arm_out(void)
{
    if (!audio_stream_open || audio_read_pending) {
        return;
    }

    audio_read_pending = true;
    if (usbd_ep_start_read(audio_bus_id, DS5_USB_AUDIO_OUT_EP,
                           audio_receive_buffer,
                           sizeof(audio_receive_buffer)) != 0) {
        audio_read_pending = false;
        ++audio_arm_failures;
    }
}

static void ds5_usb_audio_out_callback(uint8_t busid, uint8_t endpoint,
                                       uint32_t transferred_bytes)
{
    (void)busid;
    (void)endpoint;

    audio_read_pending = false;
    if (audio_stream_open) {
        (void)ds5_usb_audio_queue_packet(transferred_bytes);
        ds5_usb_audio_arm_out();
    }
}

static struct usbd_endpoint audio_out_endpoint = {
    .ep_addr = DS5_USB_AUDIO_OUT_EP,
    .ep_cb = ds5_usb_audio_out_callback,
};

static void ds5_usb_audio_process_packet(
    WDL_Resampler *resampler,
    const ds5_usb_audio_packet_t *packet,
    uint8_t *haptics_data,
    size_t *haptics_position)
{
    WDL_ResampleSample *input;
    WDL_ResampleSample output[DS5_USB_AUDIO_MAX_OUTPUT_FRAMES *
                              DS5_USB_AUDIO_OUTPUT_CHANNELS];
    size_t input_frames;
    int prepared_frames;
    int output_frames;
    int frame;

    input_frames = packet->length /
                   (DS5_USB_AUDIO_CHANNEL_COUNT *
                    DS5_USB_AUDIO_SAMPLE_BYTES);
    prepared_frames = resampler->ResamplePrepare((int)input_frames,
                                                 DS5_USB_AUDIO_OUTPUT_CHANNELS,
                                                 &input);
    if ((prepared_frames <= 0) ||
        ((size_t)prepared_frames > input_frames)) {
        ++invalid_audio_packets;
        return;
    }

    for (frame = 0; frame < prepared_frames; ++frame) {
        size_t frame_offset = (size_t)frame *
                              DS5_USB_AUDIO_CHANNEL_COUNT *
                              DS5_USB_AUDIO_SAMPLE_BYTES;
        int16_t left = ds5_usb_audio_read_s16_le(
            &packet->data[frame_offset + 4U]);
        int16_t right = ds5_usb_audio_read_s16_le(
            &packet->data[frame_offset + 6U]);

        input[(size_t)frame * 2U] =
            (WDL_ResampleSample)left / 32768.0;
        input[(size_t)frame * 2U + 1U] =
            (WDL_ResampleSample)right / 32768.0;
    }

    output_frames = resampler->ResampleOut(
        output, prepared_frames, DS5_USB_AUDIO_MAX_OUTPUT_FRAMES,
        DS5_USB_AUDIO_OUTPUT_CHANNELS);
    if ((output_frames < 0) ||
        (output_frames > (int)DS5_USB_AUDIO_MAX_OUTPUT_FRAMES)) {
        ++invalid_audio_packets;
        return;
    }

    for (frame = 0; frame < output_frames; ++frame) {
        haptics_data[(*haptics_position)++] =
            (uint8_t)ds5_usb_audio_float_to_s8(output[(size_t)frame * 2U]);
        haptics_data[(*haptics_position)++] =
            (uint8_t)ds5_usb_audio_float_to_s8(
                output[(size_t)frame * 2U + 1U]);

        if (*haptics_position == DS5_HAPTICS_DATA_SIZE) {
            if (ds5_haptics_mailbox_publish(haptics_data,
                                            DS5_HAPTICS_DATA_SIZE)) {
                ++published_haptics_blocks;
                if (published_haptics_blocks == 1U) {
                    ds5_log_printf("DS5 USB Audio: first haptics block "
                                   "queued\r\n");
                }
            }
            *haptics_position = 0U;
        }
    }
}

static void ds5_usb_audio_task(void *parameter)
{
    WDL_Resampler resampler;
    ds5_usb_audio_packet_t packet;
    uint8_t haptics_data[DS5_HAPTICS_DATA_SIZE];
    size_t haptics_position = 0U;
    uint32_t active_generation = 0U;
    uint32_t observed_packets = 0U;

    (void)parameter;

    resampler.SetMode(true, 0, false);
    resampler.SetRates((double)DS5_USB_AUDIO_SAMPLE_RATE, 3000.0);
    resampler.SetFeedMode(true);
    resampler.Prealloc(DS5_USB_AUDIO_OUTPUT_CHANNELS, 64, 8);

    while (1) {
        if (xQueueReceive(audio_queue, &packet, portMAX_DELAY) != pdPASS) {
            continue;
        }

        if (!audio_stream_open ||
            (packet.generation != audio_generation)) {
            continue;
        }

        if (packet.generation != active_generation) {
            active_generation = packet.generation;
            haptics_position = 0U;
            resampler.Reset();
        }

        ds5_usb_audio_process_packet(&resampler, &packet,
                                     haptics_data, &haptics_position);

        if ((received_audio_packets != observed_packets) &&
            ((received_audio_packets == 1U) ||
             ((received_audio_packets % DS5_USB_AUDIO_LOG_INTERVAL) == 0U))) {
            ds5_log_printf(
                "DS5 USB Audio: packets %lu, invalid %lu, dropped %lu, "
                "arm failures %lu, haptics blocks %lu\r\n",
                (unsigned long)received_audio_packets,
                (unsigned long)invalid_audio_packets,
                (unsigned long)dropped_audio_packets,
                (unsigned long)audio_arm_failures,
                (unsigned long)published_haptics_blocks);
            observed_packets = received_audio_packets;
        }
    }
}

extern "C" void usbd_audio_open(uint8_t busid, uint8_t interface)
{
    if (interface != DS5_USB_AUDIO_STREAM_INTERFACE) {
        return;
    }

    ++audio_generation;
    audio_stream_open = true;
    audio_read_pending = false;
    ds5_log_printf("DS5 USB Audio: 48 kHz four-channel OUT opened\r\n");
    (void)busid;
    ds5_usb_audio_arm_out();
}

extern "C" void usbd_audio_close(uint8_t busid, uint8_t interface)
{
    (void)busid;

    if (interface != DS5_USB_AUDIO_STREAM_INTERFACE) {
        return;
    }

    audio_stream_open = false;
    audio_read_pending = false;
    ++audio_generation;
    ds5_log_printf("DS5 USB Audio: OUT closed\r\n");
}

extern "C" void usbd_audio_set_sampling_freq(uint8_t busid, uint8_t endpoint,
                                               uint32_t sampling_freq)
{
    (void)busid;
    (void)endpoint;

    if (sampling_freq != DS5_USB_AUDIO_SAMPLE_RATE) {
        ds5_log_printf("DS5 USB Audio: rejected sampling rate %lu\r\n",
                       (unsigned long)sampling_freq);
    }
}

extern "C" uint32_t usbd_audio_get_sampling_freq(uint8_t busid,
                                                  uint8_t endpoint)
{
    (void)busid;
    (void)endpoint;
    return DS5_USB_AUDIO_SAMPLE_RATE;
}

int ds5_usb_audio_init(uint8_t busid)
{
    audio_bus_id = busid;
    audio_stream_open = false;
    audio_read_pending = false;
    audio_generation = 0U;
    received_audio_packets = 0U;
    invalid_audio_packets = 0U;
    dropped_audio_packets = 0U;
    audio_arm_failures = 0U;
    published_haptics_blocks = 0U;

    audio_queue = xQueueCreateStatic(DS5_USB_AUDIO_QUEUE_LENGTH,
                                     sizeof(ds5_usb_audio_packet_t),
                                     audio_queue_items,
                                     &audio_queue_storage);
    if (audio_queue == NULL) {
        return -1;
    }

    audio_task = xTaskCreateStatic(ds5_usb_audio_task, "usb_audio",
                                   DS5_USB_AUDIO_TASK_STACK_DEPTH, NULL,
                                   DS5_USB_AUDIO_TASK_PRIORITY,
                                   audio_task_stack,
                                   &audio_task_storage);
    if (audio_task == NULL) {
        vQueueDelete(audio_queue);
        audio_queue = NULL;
        return -1;
    }

    usbd_add_interface(
        busid,
        usbd_audio_init_intf(busid, &audio_control_interface, 0x0100,
                             audio_entity_table,
                             sizeof(audio_entity_table) /
                                 sizeof(audio_entity_table[0])));
    usbd_add_interface(
        busid,
        usbd_audio_init_intf(busid, &audio_stream_interface, 0x0100,
                             audio_entity_table,
                             sizeof(audio_entity_table) /
                                 sizeof(audio_entity_table[0])));
    usbd_add_endpoint(busid, &audio_out_endpoint);
    return 0;
}

void ds5_usb_audio_deinit(void)
{
    audio_stream_open = false;
    audio_read_pending = false;

    if (audio_task != NULL) {
        vTaskDelete(audio_task);
        audio_task = NULL;
    }
    if (audio_queue != NULL) {
        vQueueDelete(audio_queue);
        audio_queue = NULL;
    }
}

void ds5_usb_audio_handle_event(uint8_t busid, uint8_t event)
{
    (void)busid;

    switch (event) {
    case USBD_EVENT_RESET:
    case USBD_EVENT_DISCONNECTED:
    case USBD_EVENT_SUSPEND:
        audio_stream_open = false;
        audio_read_pending = false;
        ++audio_generation;
        break;
    default:
        break;
    }
}
