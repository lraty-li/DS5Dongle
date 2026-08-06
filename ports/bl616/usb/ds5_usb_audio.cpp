#include "ds5_usb_audio.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <FreeRTOS.h>
#include "portmacro.h"
#include "queue.h"
#include "task.h"

#include "opus.h"
#include "resample.h"
#include "usbd_core.h"

#include "ds5_audio_mailbox.h"
#include "ds5_feature_set_mailbox.h"
#include "ds5_haptics_mailbox.h"
#include "ds5_log.h"
#include "ds5_protocol.h"

#define DS5_USB_AUDIO_QUEUE_LENGTH          8U
#define DS5_USB_AUDIO_TASK_STACK_DEPTH      (configMINIMAL_STACK_SIZE * 12U)
#define DS5_USB_AUDIO_TASK_PRIORITY         (configMAX_PRIORITIES - 3U)
#define DS5_USB_AUDIO_LOG_INTERVAL          4096U
#define DS5_USB_AUDIO_HAPTICS_CHANNELS      2U
#define DS5_USB_AUDIO_HAPTICS_MAX_FRAMES    8U
#define DS5_USB_AUDIO_SPEAKER_INPUT_FRAMES  512U
#define DS5_USB_AUDIO_SPEAKER_OUTPUT_FRAMES 480U
#define DS5_USB_AUDIO_MICROPHONE_PACKET_FRAMES 48U

/*
 * The public Opus API deliberately keeps these structures opaque.  Allocate
 * fixed, aligned storage and validate the exact v1.6.1 requirements returned
 * by opus_*_get_size() before initialization; no codec allocation occurs at
 * runtime.
 */
#define DS5_USB_AUDIO_OPUS_ENCODER_STORAGE_SIZE 65536U
#define DS5_USB_AUDIO_OPUS_DECODER_STORAGE_SIZE 32768U

typedef struct {
    uint32_t generation;
    uint16_t length;
    uint8_t data[DS5_USB_AUDIO_MAX_PACKET_BYTES];
} ds5_usb_audio_packet_t;

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX
    uint8_t audio_receive_buffer[DS5_USB_AUDIO_MAX_PACKET_BYTES];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX
    uint8_t microphone_transmit_buffer[DS5_USB_AUDIO_MICROPHONE_PACKET_BYTES];
static ds5_usb_audio_packet_t audio_staging_packet;
static WDL_ResampleSample speaker_resample_output[
    DS5_USB_AUDIO_SPEAKER_OUTPUT_FRAMES * DS5_AUDIO_SPEAKER_CHANNELS];
static float speaker_opus_input[DS5_USB_AUDIO_SPEAKER_OUTPUT_FRAMES *
                                DS5_AUDIO_SPEAKER_CHANNELS];
static WDL_ResampleSample speaker_accumulator[
    DS5_USB_AUDIO_SPEAKER_INPUT_FRAMES * DS5_AUDIO_SPEAKER_CHANNELS];
static int16_t microphone_pcm[DS5_AUDIO_FRAME_SAMPLES];

static StaticQueue_t audio_queue_storage;
static uint8_t audio_queue_items[DS5_USB_AUDIO_QUEUE_LENGTH *
                                 sizeof(ds5_usb_audio_packet_t)];
static QueueHandle_t audio_queue;

static StaticTask_t audio_task_storage;
static StackType_t audio_task_stack[DS5_USB_AUDIO_TASK_STACK_DEPTH];
static TaskHandle_t audio_task;

alignas(8) static uint8_t
    opus_encoder_storage[DS5_USB_AUDIO_OPUS_ENCODER_STORAGE_SIZE];
alignas(8) static uint8_t
    opus_decoder_storage[DS5_USB_AUDIO_OPUS_DECODER_STORAGE_SIZE];

static volatile bool speaker_stream_open;
static volatile bool microphone_stream_open;
static volatile bool audio_read_pending;
static volatile bool microphone_write_pending;
static volatile uint32_t audio_generation;
static volatile uint32_t received_audio_packets;
static volatile uint32_t invalid_audio_packets;
static volatile uint32_t dropped_audio_packets;
static volatile uint32_t audio_arm_failures;
static volatile uint32_t microphone_write_failures;
static volatile uint32_t published_haptics_blocks;
static volatile uint32_t published_speaker_frames;
static volatile uint32_t decoded_microphone_frames;
static volatile bool speaker_muted;
static volatile bool microphone_muted;
static volatile int speaker_volume_db;
static volatile int microphone_volume_db;
static uint8_t audio_bus_id;

static int16_t ds5_usb_audio_read_s16_le(const uint8_t *data)
{
    uint16_t value = (uint16_t)data[0] |
                     (uint16_t)((uint16_t)data[1] << 8U);

    return (int16_t)value;
}

static void ds5_usb_audio_write_s16_le(uint8_t *data, int16_t value)
{
    uint16_t raw_value = (uint16_t)value;

    data[0] = (uint8_t)(raw_value & 0xffU);
    data[1] = (uint8_t)(raw_value >> 8U);
}

static void ds5_usb_audio_write_u32_le(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value & 0xffU);
    data[1] = (uint8_t)((value >> 8U) & 0xffU);
    data[2] = (uint8_t)((value >> 16U) & 0xffU);
    data[3] = (uint8_t)((value >> 24U) & 0xffU);
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
    if (!speaker_stream_open || audio_read_pending) {
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

extern "C" void ds5_usb_audio_on_out_complete(uint8_t busid,
                                               uint8_t endpoint,
                                               uint32_t transferred_bytes)
{
    (void)busid;
    (void)endpoint;

    audio_read_pending = false;
    if (speaker_stream_open) {
        (void)ds5_usb_audio_queue_packet(transferred_bytes);
        ds5_usb_audio_arm_out();
    }
}

extern "C" void ds5_usb_audio_on_in_complete(uint8_t busid,
                                              uint8_t endpoint,
                                              uint32_t transferred_bytes)
{
    (void)busid;
    (void)endpoint;
    (void)transferred_bytes;

    microphone_write_pending = false;
}

extern "C" size_t ds5_usb_audio_get_diagnostic_feature(uint8_t *data,
                                                         size_t capacity)
{
    uint8_t flags = 0U;

    if ((data == NULL) ||
        (capacity < DS5_USB_AUDIO_DIAGNOSTIC_FEATURE_REPORT_SIZE)) {
        return 0U;
    }

    memset(data, 0, DS5_USB_AUDIO_DIAGNOSTIC_FEATURE_REPORT_SIZE);
    data[0] = 'D';
    data[1] = '5';
    data[2] = 'A';
    data[3] = 'D';
    data[4] = 0x01U;
    if (speaker_stream_open) {
        flags |= 0x01U;
    }
    if (microphone_stream_open) {
        flags |= 0x02U;
    }
    if (audio_read_pending) {
        flags |= 0x04U;
    }
    if (microphone_write_pending) {
        flags |= 0x08U;
    }
    if (speaker_muted) {
        flags |= 0x10U;
    }
    if (microphone_muted) {
        flags |= 0x20U;
    }
    data[5] = flags;
    ds5_usb_audio_write_u32_le(&data[8], received_audio_packets);
    ds5_usb_audio_write_u32_le(&data[12], invalid_audio_packets);
    ds5_usb_audio_write_u32_le(&data[16], dropped_audio_packets);
    ds5_usb_audio_write_u32_le(&data[20], audio_arm_failures);
    ds5_usb_audio_write_u32_le(&data[24], published_haptics_blocks);
    ds5_usb_audio_write_u32_le(&data[28], published_speaker_frames);
    ds5_usb_audio_write_u32_le(&data[32], decoded_microphone_frames);
    ds5_usb_audio_write_u32_le(&data[36], microphone_write_failures);
    ds5_usb_audio_write_u32_le(&data[40], audio_generation);
    ds5_usb_audio_write_u32_le(
        &data[44], ds5_feature_set_mailbox_received_count());
    ds5_usb_audio_write_u32_le(
        &data[48], ds5_feature_set_mailbox_dropped_count());
    ds5_usb_audio_write_u32_le(
        &data[52], ds5_feature_set_mailbox_forwarded_count());
    ds5_usb_audio_write_u32_le(
        &data[56], ds5_feature_set_mailbox_forward_failed_count());
    data[60] = ds5_feature_set_mailbox_last_report_id();
    data[61] = ds5_feature_set_mailbox_last_payload_length();
    return DS5_USB_AUDIO_DIAGNOSTIC_FEATURE_REPORT_SIZE;
}

static bool ds5_usb_audio_init_codecs(OpusEncoder **encoder,
                                      OpusDecoder **decoder)
{
    int encoder_size;
    int decoder_size;
    int result;

    if ((encoder == NULL) || (decoder == NULL)) {
        return false;
    }

    encoder_size = opus_encoder_get_size(DS5_AUDIO_SPEAKER_CHANNELS);
    decoder_size = opus_decoder_get_size(DS5_AUDIO_MIC_CHANNELS);
    if ((encoder_size <= 0) || (decoder_size <= 0) ||
        ((size_t)encoder_size > sizeof(opus_encoder_storage)) ||
        ((size_t)decoder_size > sizeof(opus_decoder_storage))) {
        ds5_log_printf("DS5 USB Audio: static Opus storage is insufficient "
                       "(encoder %d, decoder %d)\r\n",
                       encoder_size, decoder_size);
        return false;
    }

    *encoder = reinterpret_cast<OpusEncoder *>(opus_encoder_storage);
    *decoder = reinterpret_cast<OpusDecoder *>(opus_decoder_storage);

    result = opus_encoder_init(*encoder, DS5_AUDIO_SAMPLE_RATE,
                               DS5_AUDIO_SPEAKER_CHANNELS,
                               OPUS_APPLICATION_AUDIO);
    if (result != OPUS_OK) {
        ds5_log_printf("DS5 USB Audio: Opus encoder init failed (%d)\r\n",
                       result);
        return false;
    }

    result = opus_encoder_ctl(
        *encoder,
        OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
    if (result == OPUS_OK) {
        result = opus_encoder_ctl(*encoder, OPUS_SET_BITRATE(160000));
    }
    if (result == OPUS_OK) {
        result = opus_encoder_ctl(*encoder, OPUS_SET_VBR(0));
    }
    if (result == OPUS_OK) {
        result = opus_encoder_ctl(*encoder, OPUS_SET_COMPLEXITY(0));
    }
    if (result != OPUS_OK) {
        ds5_log_printf("DS5 USB Audio: Opus encoder setup failed (%d)\r\n",
                       result);
        return false;
    }

    result = opus_decoder_init(*decoder, DS5_AUDIO_SAMPLE_RATE,
                               DS5_AUDIO_MIC_CHANNELS);
    if (result != OPUS_OK) {
        ds5_log_printf("DS5 USB Audio: Opus decoder init failed (%d)\r\n",
                       result);
        return false;
    }

    ds5_log_printf("DS5 USB Audio: Opus ready (encoder %d B, decoder %d B)\r\n",
                   encoder_size, decoder_size);
    return true;
}

static void ds5_usb_audio_encode_speaker(
    WDL_Resampler *resampler, OpusEncoder *encoder,
    const WDL_ResampleSample *speaker_samples)
{
    WDL_ResampleSample *input;
    uint8_t opus_frame[DS5_AUDIO_SPEAKER_OPUS_SIZE];
    int prepared_frames;
    int output_frames;
    int encoded_length;
    size_t index;

    if ((resampler == NULL) || (encoder == NULL) ||
        (speaker_samples == NULL)) {
        ++invalid_audio_packets;
        return;
    }

    prepared_frames = resampler->ResamplePrepare(
        DS5_USB_AUDIO_SPEAKER_INPUT_FRAMES,
        DS5_AUDIO_SPEAKER_CHANNELS, &input);
    if (prepared_frames != DS5_USB_AUDIO_SPEAKER_INPUT_FRAMES) {
        ++invalid_audio_packets;
        return;
    }

    memcpy(input, speaker_samples,
           sizeof(WDL_ResampleSample) *
               DS5_USB_AUDIO_SPEAKER_INPUT_FRAMES *
               DS5_AUDIO_SPEAKER_CHANNELS);
    output_frames = resampler->ResampleOut(
        speaker_resample_output, prepared_frames,
        DS5_USB_AUDIO_SPEAKER_OUTPUT_FRAMES, DS5_AUDIO_SPEAKER_CHANNELS);
    if (output_frames != DS5_USB_AUDIO_SPEAKER_OUTPUT_FRAMES) {
        ++invalid_audio_packets;
        return;
    }

    for (index = 0U; index < sizeof(speaker_opus_input) /
                                 sizeof(speaker_opus_input[0]);
         ++index) {
        speaker_opus_input[index] = (float)speaker_resample_output[index];
    }

    encoded_length = opus_encode_float(
        encoder, speaker_opus_input, DS5_USB_AUDIO_SPEAKER_OUTPUT_FRAMES,
        opus_frame, sizeof(opus_frame));
    if (encoded_length <= 0) {
        ++invalid_audio_packets;
        return;
    }

    if ((size_t)encoded_length < sizeof(opus_frame)) {
        memset(&opus_frame[encoded_length], 0,
               sizeof(opus_frame) - (size_t)encoded_length);
    }

    if (ds5_audio_mailbox_publish_speaker_opus(opus_frame,
                                               sizeof(opus_frame))) {
        ++published_speaker_frames;
        if (published_speaker_frames == 1U) {
            ds5_log_printf("DS5 USB Audio: first speaker Opus frame queued\r\n");
        }
    }
}

static void ds5_usb_audio_process_packet(
    WDL_Resampler *haptics_resampler, WDL_Resampler *speaker_resampler,
    OpusEncoder *encoder, const ds5_usb_audio_packet_t *packet,
    uint8_t *haptics_data, size_t *haptics_position,
    WDL_ResampleSample *speaker_data, size_t *speaker_position)
{
    WDL_ResampleSample *input;
    WDL_ResampleSample output[DS5_USB_AUDIO_HAPTICS_MAX_FRAMES *
                              DS5_USB_AUDIO_HAPTICS_CHANNELS];
    size_t input_frames;
    int prepared_frames;
    int output_frames;
    int frame;

    if ((haptics_resampler == NULL) || (speaker_resampler == NULL) ||
        (encoder == NULL) || (packet == NULL) || (haptics_data == NULL) ||
        (haptics_position == NULL) || (speaker_data == NULL) ||
        (speaker_position == NULL)) {
        ++invalid_audio_packets;
        return;
    }

    input_frames = packet->length /
                   (DS5_USB_AUDIO_CHANNEL_COUNT *
                    DS5_USB_AUDIO_SAMPLE_BYTES);
    prepared_frames = haptics_resampler->ResamplePrepare(
        (int)input_frames, DS5_USB_AUDIO_HAPTICS_CHANNELS, &input);
    if ((prepared_frames <= 0) || ((size_t)prepared_frames > input_frames)) {
        ++invalid_audio_packets;
        return;
    }

    for (frame = 0; frame < prepared_frames; ++frame) {
        size_t frame_offset = (size_t)frame *
                              DS5_USB_AUDIO_CHANNEL_COUNT *
                              DS5_USB_AUDIO_SAMPLE_BYTES;
        int16_t speaker_left = ds5_usb_audio_read_s16_le(
            &packet->data[frame_offset + 0U]);
        int16_t speaker_right = ds5_usb_audio_read_s16_le(
            &packet->data[frame_offset + 2U]);
        int16_t haptics_left = ds5_usb_audio_read_s16_le(
            &packet->data[frame_offset + 4U]);
        int16_t haptics_right = ds5_usb_audio_read_s16_le(
            &packet->data[frame_offset + 6U]);

        if (speaker_muted) {
            speaker_left = 0;
            speaker_right = 0;
        }

        input[(size_t)frame * 2U] =
            (WDL_ResampleSample)haptics_left / 32768.0;
        input[(size_t)frame * 2U + 1U] =
            (WDL_ResampleSample)haptics_right / 32768.0;

        speaker_data[(*speaker_position) * DS5_AUDIO_SPEAKER_CHANNELS] =
            (WDL_ResampleSample)speaker_left / 32768.0;
        speaker_data[(*speaker_position) * DS5_AUDIO_SPEAKER_CHANNELS + 1U] =
            (WDL_ResampleSample)speaker_right / 32768.0;
        ++(*speaker_position);

        if (*speaker_position == DS5_USB_AUDIO_SPEAKER_INPUT_FRAMES) {
            ds5_usb_audio_encode_speaker(speaker_resampler, encoder,
                                         speaker_data);
            *speaker_position = 0U;
        }
    }

    output_frames = haptics_resampler->ResampleOut(
        output, prepared_frames, DS5_USB_AUDIO_HAPTICS_MAX_FRAMES,
        DS5_USB_AUDIO_HAPTICS_CHANNELS);
    if ((output_frames < 0) ||
        (output_frames > (int)DS5_USB_AUDIO_HAPTICS_MAX_FRAMES)) {
        ++invalid_audio_packets;
        return;
    }

    for (frame = 0; frame < output_frames; ++frame) {
        haptics_data[(*haptics_position)++] =
            (uint8_t)ds5_usb_audio_float_to_s8(output[(size_t)frame * 2U]);
        haptics_data[(*haptics_position)++] = (uint8_t)
            ds5_usb_audio_float_to_s8(output[(size_t)frame * 2U + 1U]);

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

static bool ds5_usb_audio_start_microphone_write(
    const int16_t *mono_samples, size_t sample_count)
{
    size_t index;

    if (!microphone_stream_open || microphone_write_pending ||
        (mono_samples == NULL) ||
        (sample_count != DS5_USB_AUDIO_MICROPHONE_PACKET_FRAMES)) {
        return false;
    }

    for (index = 0U; index < sample_count; ++index) {
        int16_t sample = microphone_muted ? 0 : mono_samples[index];
        size_t output_offset = index *
                               DS5_USB_AUDIO_MICROPHONE_CHANNEL_COUNT *
                               DS5_USB_AUDIO_SAMPLE_BYTES;

        ds5_usb_audio_write_s16_le(&microphone_transmit_buffer[output_offset],
                                   sample);
        ds5_usb_audio_write_s16_le(
            &microphone_transmit_buffer[output_offset +
                                        DS5_USB_AUDIO_SAMPLE_BYTES],
            sample);
    }

    if (usbd_ep_start_write(audio_bus_id, DS5_USB_AUDIO_IN_EP,
                            microphone_transmit_buffer,
                            sizeof(microphone_transmit_buffer)) != 0) {
        ++microphone_write_failures;
        return false;
    }

    microphone_write_pending = true;
    return true;
}

static void ds5_usb_audio_task(void *parameter)
{
    WDL_Resampler haptics_resampler;
    WDL_Resampler speaker_resampler;
    ds5_usb_audio_packet_t packet;
    uint8_t haptics_data[DS5_HAPTICS_DATA_SIZE];
    uint8_t microphone_opus_data[DS5_AUDIO_MIC_OPUS_SIZE];
    OpusEncoder *encoder = NULL;
    OpusDecoder *decoder = NULL;
    size_t haptics_position = 0U;
    size_t speaker_position = 0U;
    size_t microphone_position = 0U;
    size_t microphone_sample_count = 0U;
    uint32_t active_generation = 0U;
    uint32_t observed_packets = 0U;
    bool codec_ready = false;
    bool codec_initialization_attempted = false;

    (void)parameter;

    while (1) {
        bool did_work = false;

        /*
         * Do not bring up WDL or Opus until Windows has selected a non-zero
         * UAC alternate setting.  HID enumeration and BR/EDR reconnect must
         * remain independent of an unopened audio path.
         */
        if (!speaker_stream_open && !microphone_stream_open) {
            haptics_position = 0U;
            speaker_position = 0U;
            microphone_position = 0U;
            microphone_sample_count = 0U;
            active_generation = audio_generation;
            vTaskDelay(pdMS_TO_TICKS(5U));
            continue;
        }

        if (!codec_initialization_attempted) {
            haptics_resampler.SetMode(true, 0, false);
            haptics_resampler.SetRates((double)DS5_USB_AUDIO_SAMPLE_RATE,
                                       3000.0);
            haptics_resampler.SetFeedMode(true);
            haptics_resampler.Prealloc(DS5_USB_AUDIO_HAPTICS_CHANNELS, 64,
                                       8);

            speaker_resampler.SetMode(true, 0, false);
            speaker_resampler.SetRates(51200.0,
                                       (double)DS5_AUDIO_SAMPLE_RATE);
            speaker_resampler.SetFeedMode(true);
            speaker_resampler.Prealloc(
                DS5_AUDIO_SPEAKER_CHANNELS,
                DS5_USB_AUDIO_SPEAKER_INPUT_FRAMES,
                DS5_USB_AUDIO_SPEAKER_OUTPUT_FRAMES);

            codec_ready = ds5_usb_audio_init_codecs(&encoder, &decoder);
            codec_initialization_attempted = true;
        }

        while (xQueueReceive(audio_queue, &packet, 0U) == pdPASS) {
            did_work = true;
            if (!speaker_stream_open ||
                (packet.generation != audio_generation) || !codec_ready) {
                continue;
            }

            if (packet.generation != active_generation) {
                active_generation = packet.generation;
                haptics_position = 0U;
                speaker_position = 0U;
                haptics_resampler.Reset();
                speaker_resampler.Reset();
            }

            ds5_usb_audio_process_packet(
                &haptics_resampler, &speaker_resampler, encoder, &packet,
                haptics_data, &haptics_position, speaker_accumulator,
                &speaker_position);
        }

        if (microphone_stream_open && codec_ready) {
            if ((microphone_position >= microphone_sample_count) &&
                ds5_audio_mailbox_try_receive_microphone_opus(
                    microphone_opus_data, sizeof(microphone_opus_data))) {
                int decoded_samples = opus_decode(
                    decoder, microphone_opus_data,
                    DS5_AUDIO_MIC_OPUS_SIZE, microphone_pcm,
                    DS5_AUDIO_FRAME_SAMPLES, 0);

                did_work = true;
                microphone_position = 0U;
                microphone_sample_count =
                    decoded_samples > 0 ? (size_t)decoded_samples : 0U;
                if (microphone_sample_count != 0U) {
                    ++decoded_microphone_frames;
                    if (decoded_microphone_frames == 1U) {
                        ds5_log_printf("DS5 USB Audio: first microphone "
                                       "frame decoded\r\n");
                    }
                }
            }

            if ((microphone_sample_count - microphone_position) >=
                DS5_USB_AUDIO_MICROPHONE_PACKET_FRAMES) {
                if (ds5_usb_audio_start_microphone_write(
                        &microphone_pcm[microphone_position],
                        DS5_USB_AUDIO_MICROPHONE_PACKET_FRAMES)) {
                    microphone_position +=
                        DS5_USB_AUDIO_MICROPHONE_PACKET_FRAMES;
                    did_work = true;
                }
            }
        } else {
            microphone_position = 0U;
            microphone_sample_count = 0U;
            while (ds5_audio_mailbox_try_receive_microphone_opus(
                microphone_opus_data, sizeof(microphone_opus_data))) {
                did_work = true;
            }
        }

        if ((received_audio_packets != observed_packets) &&
            ((received_audio_packets == 1U) ||
             ((received_audio_packets % DS5_USB_AUDIO_LOG_INTERVAL) == 0U))) {
            ds5_log_printf(
                "DS5 USB Audio: packets %lu, invalid %lu, dropped %lu, "
                "haptics %lu, speaker %lu, mic %lu\r\n",
                (unsigned long)received_audio_packets,
                (unsigned long)invalid_audio_packets,
                (unsigned long)dropped_audio_packets,
                (unsigned long)published_haptics_blocks,
                (unsigned long)published_speaker_frames,
                (unsigned long)decoded_microphone_frames);
            observed_packets = received_audio_packets;
        }

        if (!did_work) {
            vTaskDelay(pdMS_TO_TICKS(1U));
        }
    }
}

extern "C" void ds5_usb_audio_on_stream_open(uint8_t busid,
                                               uint8_t interface)
{
    (void)busid;

    if (interface == DS5_USB_AUDIO_SPEAKER_INTERFACE) {
        ++audio_generation;
        speaker_stream_open = true;
        audio_read_pending = false;
        ds5_audio_mailbox_set_speaker_stream_active(true);
        ds5_log_printf("DS5 USB Audio: 48 kHz four-channel speaker OUT "
                       "opened\r\n");
        ds5_usb_audio_arm_out();
    } else if (interface == DS5_USB_AUDIO_MICROPHONE_INTERFACE) {
        microphone_stream_open = true;
        microphone_write_pending = false;
        (void)ds5_audio_mailbox_publish_microphone_stream_active(true);
        ds5_log_printf("DS5 USB Audio: 48 kHz stereo microphone IN opened\r\n");
    }
}

extern "C" void ds5_usb_audio_on_stream_close(uint8_t busid,
                                                uint8_t interface)
{
    (void)busid;

    if (interface == DS5_USB_AUDIO_SPEAKER_INTERFACE) {
        speaker_stream_open = false;
        audio_read_pending = false;
        ++audio_generation;
        ds5_audio_mailbox_set_speaker_stream_active(false);
        ds5_log_printf("DS5 USB Audio: speaker OUT closed\r\n");
    } else if (interface == DS5_USB_AUDIO_MICROPHONE_INTERFACE) {
        microphone_stream_open = false;
        microphone_write_pending = false;
        (void)ds5_audio_mailbox_publish_microphone_stream_active(false);
        ds5_log_printf("DS5 USB Audio: microphone IN closed\r\n");
    }
}

extern "C" void ds5_usb_audio_on_set_sampling_freq(uint8_t busid,
                                                     uint8_t endpoint,
                                                     uint32_t sampling_freq)
{
    (void)busid;

    if (((endpoint != DS5_USB_AUDIO_OUT_EP) &&
         (endpoint != DS5_USB_AUDIO_IN_EP)) ||
        (sampling_freq != DS5_USB_AUDIO_SAMPLE_RATE)) {
        ds5_log_printf("DS5 USB Audio: rejected sampling rate %lu on EP %02x\r\n",
                       (unsigned long)sampling_freq,
                       (unsigned int)endpoint);
    }
}

extern "C" uint32_t ds5_usb_audio_on_get_sampling_freq(uint8_t busid,
                                                         uint8_t endpoint)
{
    (void)busid;

    if ((endpoint == DS5_USB_AUDIO_OUT_EP) ||
        (endpoint == DS5_USB_AUDIO_IN_EP)) {
        return DS5_USB_AUDIO_SAMPLE_RATE;
    }

    return 0U;
}

extern "C" void ds5_usb_audio_on_set_volume(uint8_t busid, uint8_t endpoint,
                                             uint8_t channel, int volume_db)
{
    (void)busid;
    (void)channel;

    if (endpoint == DS5_USB_AUDIO_OUT_EP) {
        speaker_volume_db = volume_db;
    } else if (endpoint == DS5_USB_AUDIO_IN_EP) {
        microphone_volume_db = volume_db;
    }
}

extern "C" int ds5_usb_audio_on_get_volume(uint8_t busid, uint8_t endpoint,
                                            uint8_t channel)
{
    (void)busid;
    (void)channel;

    if (endpoint == DS5_USB_AUDIO_OUT_EP) {
        return speaker_volume_db;
    }
    if (endpoint == DS5_USB_AUDIO_IN_EP) {
        return microphone_volume_db;
    }
    return 0;
}

extern "C" void ds5_usb_audio_on_set_mute(uint8_t busid, uint8_t endpoint,
                                           uint8_t channel, int mute)
{
    (void)busid;
    (void)channel;

    if (endpoint == DS5_USB_AUDIO_OUT_EP) {
        speaker_muted = mute != 0;
    } else if (endpoint == DS5_USB_AUDIO_IN_EP) {
        microphone_muted = mute != 0;
    }
}

extern "C" int ds5_usb_audio_on_get_mute(uint8_t busid, uint8_t endpoint,
                                          uint8_t channel)
{
    (void)busid;
    (void)channel;

    if (endpoint == DS5_USB_AUDIO_OUT_EP) {
        return speaker_muted;
    }
    if (endpoint == DS5_USB_AUDIO_IN_EP) {
        return microphone_muted;
    }
    return false;
}

int ds5_usb_audio_init(uint8_t busid)
{
    audio_bus_id = busid;
    speaker_stream_open = false;
    microphone_stream_open = false;
    audio_read_pending = false;
    microphone_write_pending = false;
    audio_generation = 0U;
    received_audio_packets = 0U;
    invalid_audio_packets = 0U;
    dropped_audio_packets = 0U;
    audio_arm_failures = 0U;
    microphone_write_failures = 0U;
    published_haptics_blocks = 0U;
    published_speaker_frames = 0U;
    decoded_microphone_frames = 0U;
    speaker_muted = false;
    microphone_muted = false;
    speaker_volume_db = 0;
    microphone_volume_db = 0;

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

    if (ds5_usb_audio_class_init(busid) != 0) {
        vTaskDelete(audio_task);
        audio_task = NULL;
        vQueueDelete(audio_queue);
        audio_queue = NULL;
        return -1;
    }
    return 0;
}

void ds5_usb_audio_deinit(void)
{
    speaker_stream_open = false;
    microphone_stream_open = false;
    audio_read_pending = false;
    microphone_write_pending = false;
    ds5_audio_mailbox_set_speaker_stream_active(false);
    (void)ds5_audio_mailbox_publish_microphone_stream_active(false);

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
        speaker_stream_open = false;
        microphone_stream_open = false;
        audio_read_pending = false;
        microphone_write_pending = false;
        ++audio_generation;
        ds5_audio_mailbox_set_speaker_stream_active(false);
        (void)ds5_audio_mailbox_publish_microphone_stream_active(false);
        break;
    default:
        break;
    }
}
