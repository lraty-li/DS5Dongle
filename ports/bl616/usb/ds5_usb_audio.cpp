#include "ds5_usb_audio.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <FreeRTOS.h>
#include "bflb_mtimer.h"
#include "portmacro.h"
#include "queue.h"
#include "task.h"

#include "opus.h"
#include "riscv-dsp.h"
#include "usbd_core.h"

#include "ds5_audio_mailbox.h"
#include "ds5_haptics_mailbox.h"
#include "ds5_log.h"
#include "ds5_protocol.h"

#define DS5_USB_AUDIO_QUEUE_LENGTH          4U
#define DS5_USB_SPEAKER_QUEUE_LENGTH        2U
/*
 * Keep USB ingestion and haptics resampling in a short, high-priority task.
 * Opus encoding used to run in this same task.  One encode can occupy most of
 * a 10.67 ms source frame, while the old USB queue held only 8 ms of packets;
 * the resulting overflow delayed haptics and punched holes in speaker PCM.
 */
#define DS5_USB_AUDIO_TASK_STACK_DEPTH      (configMINIMAL_STACK_SIZE * 12U)
#define DS5_USB_AUDIO_TASK_PRIORITY         (configMAX_PRIORITIES - 4U)
/*
 * Keep the same conservative budget as the original src/audio.cpp core1 task.
 * The fixed-point codec uses substantially less stack than the former
 * generic float build, but the diagnostic high-water mark verifies this at
 * runtime before the allocation can be reduced safely.
 */
#define DS5_USB_CODEC_TASK_STACK_DEPTH      (configMINIMAL_STACK_SIZE * 64U)
/*
 * Below the Bluetooth workers (configMAX_PRIORITIES - 5): when encoding is
 * momentarily slower than real time, USB ingestion and BT keep their latency.
 * Two complete raw frames absorb a measured single-frame Opus latency spike;
 * sustained overload still drops the oldest frame (latest wins) instead of
 * delaying haptics and USB ingestion without bound.
 */
#define DS5_USB_CODEC_TASK_PRIORITY         (configMAX_PRIORITIES - 6U)
#define DS5_USB_AUDIO_LOG_INTERVAL          4096U
#define DS5_USB_AUDIO_ENCODE_BUDGET_US      10667U
#define DS5_USB_AUDIO_PACKET_GAP_LIMIT_US    1500U
#define DS5_USB_AUDIO_HAPTICS_DECIMATION    16U
#define DS5_USB_AUDIO_SPEAKER_INPUT_FRAMES  512U
#define DS5_USB_AUDIO_SPEAKER_OUTPUT_FRAMES 480U
#define DS5_USB_AUDIO_SPEAKER_INPUT_STEP    16U
#define DS5_USB_AUDIO_SPEAKER_OUTPUT_STEP   15U
#define DS5_USB_AUDIO_DIV15_MULTIPLIER       UINT64_C(0x88889)
#define DS5_USB_AUDIO_DIV15_SHIFT            23U
#define DS5_USB_AUDIO_MICROPHONE_PACKET_FRAMES 48U

/*
 * The public Opus API deliberately keeps these structures opaque.  Allocate
 * fixed, aligned storage and validate the library requirements returned
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

typedef struct {
    uint32_t generation;
    int16_t data[DS5_USB_AUDIO_SPEAKER_INPUT_FRAMES *
                 DS5_AUDIO_SPEAKER_CHANNELS];
} ds5_usb_speaker_frame_t;

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX
    uint8_t audio_receive_buffer[DS5_USB_AUDIO_MAX_PACKET_BYTES];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX
    uint8_t microphone_transmit_buffer[DS5_USB_AUDIO_MICROPHONE_PACKET_BYTES];
static ds5_usb_audio_packet_t audio_staging_packet;
static ds5_usb_audio_packet_t audio_discard_packet;
static int16_t speaker_opus_input[DS5_USB_AUDIO_SPEAKER_OUTPUT_FRAMES *
                                  DS5_AUDIO_SPEAKER_CHANNELS];
static ds5_usb_speaker_frame_t speaker_staging_frame;
static ds5_usb_speaker_frame_t speaker_codec_frame;
static ds5_usb_speaker_frame_t speaker_discard_frame;
static int16_t microphone_pcm[DS5_AUDIO_FRAME_SAMPLES];

static StaticQueue_t audio_queue_storage;
static uint8_t audio_queue_items[DS5_USB_AUDIO_QUEUE_LENGTH *
                                 sizeof(ds5_usb_audio_packet_t)];
static QueueHandle_t audio_queue;

static StaticQueue_t speaker_queue_storage;
static uint8_t speaker_queue_items[DS5_USB_SPEAKER_QUEUE_LENGTH *
                                   sizeof(ds5_usb_speaker_frame_t)];
static QueueHandle_t speaker_queue;

static StaticTask_t audio_task_storage;
static StackType_t audio_task_stack[DS5_USB_AUDIO_TASK_STACK_DEPTH];
static TaskHandle_t audio_task;
static StaticTask_t codec_task_storage;
static StackType_t codec_task_stack[DS5_USB_CODEC_TASK_STACK_DEPTH];
static TaskHandle_t codec_task;

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
static volatile uint32_t dropped_speaker_input_frames;
static volatile uint32_t speaker_encode_overruns;
static volatile uint32_t speaker_encode_count;
static volatile uint64_t total_speaker_encode_us;
static volatile uint32_t max_speaker_encode_us;
static volatile uint32_t usb_interval_gap_count;
static volatile uint32_t max_usb_interval_us;
static volatile uint32_t decoded_microphone_frames;
static volatile uint16_t audio_task_stack_high_water_words;
static volatile uint16_t codec_task_stack_high_water_words;
static volatile bool codecs_ready;
static volatile bool speaker_muted;
static volatile bool microphone_muted;
static volatile int speaker_volume_db;
static volatile int microphone_volume_db;
static uint8_t audio_bus_id;
static uint64_t last_audio_completion_us;

static uint16_t ds5_usb_audio_current_stack_high_water_words(void)
{
    UBaseType_t words = uxTaskGetStackHighWaterMark(NULL);

    return words > UINT16_MAX ? UINT16_MAX : (uint16_t)words;
}

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

static int8_t ds5_usb_audio_s16_to_s8(int16_t sample)
{
    int32_t value = ((int32_t)sample * 127) / 32768;

    if (value > 127) {
        value = 127;
    } else if (value < -128) {
        value = -128;
    }

    return (int8_t)value;
}

static inline int16_t ds5_usb_audio_divide_round_15(int32_t value)
{
    /*
     * Interpolation weights sum to 15, so abs(value) is at most 491520.
     * For magnitudes through 491527 (including rounding),
     * floor(x / 15) == (x * 0x88889) >> 23.  E907 emits MULSR64 + WEXTI
     * instead of the comparatively long-latency DIV instruction.
     */
    uint32_t sign = (uint32_t)value >> 31;
    uint32_t sign_mask = 0U - sign;
    uint32_t magnitude =
        (((uint32_t)value ^ sign_mask) - sign_mask) + 7U;
    uint32_t quotient = (uint32_t)(
        ((uint64_t)magnitude * DS5_USB_AUDIO_DIV15_MULTIPLIER) >>
        DS5_USB_AUDIO_DIV15_SHIFT);

    /* Restore the sign without an unpredictable branch on PCM polarity. */
    return (int16_t)((quotient ^ sign_mask) + sign);
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
        if (result != pdPASS) {
            if (xQueueReceiveFromISR(audio_queue, &audio_discard_packet,
                                     &task_woken) == pdPASS) {
                ++dropped_audio_packets;
                result = xQueueSendFromISR(audio_queue,
                                           &audio_staging_packet,
                                           &task_woken);
            }
        }
        portYIELD_FROM_ISR(task_woken);
    } else {
        result = xQueueSend(audio_queue, &audio_staging_packet, 0U);
        if (result != pdPASS) {
            if (xQueueReceive(audio_queue, &audio_discard_packet, 0U) ==
                pdPASS) {
                ++dropped_audio_packets;
                result = xQueueSend(audio_queue, &audio_staging_packet, 0U);
            }
        }
    }

    if (result != pdPASS) {
        ++dropped_audio_packets;
        return false;
    }

    ++received_audio_packets;
    return true;
}

static bool ds5_usb_audio_queue_speaker_frame(uint32_t generation)
{
    BaseType_t result;

    if (speaker_queue == NULL) {
        return false;
    }

    speaker_staging_frame.generation = generation;
    result = xQueueSend(speaker_queue, &speaker_staging_frame, 0U);
    if (result == pdPASS) {
        return true;
    }

    if (xQueueReceive(speaker_queue, &speaker_discard_frame, 0U) != pdPASS) {
        ++dropped_speaker_input_frames;
        return false;
    }

    ++dropped_speaker_input_frames;
    return xQueueSend(speaker_queue, &speaker_staging_frame, 0U) == pdPASS;
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
    uint64_t now_us;

    (void)busid;
    (void)endpoint;

    audio_read_pending = false;
    if (speaker_stream_open) {
        now_us = bflb_mtimer_get_time_us();
        if (last_audio_completion_us != 0U) {
            uint64_t interval_us = now_us - last_audio_completion_us;
            uint32_t bounded_interval_us =
                interval_us > UINT32_MAX ? UINT32_MAX :
                                           (uint32_t)interval_us;

            if (bounded_interval_us > max_usb_interval_us) {
                max_usb_interval_us = bounded_interval_us;
            }
            if (interval_us > DS5_USB_AUDIO_PACKET_GAP_LIMIT_US) {
                ++usb_interval_gap_count;
            }
        }
        last_audio_completion_us = now_us;
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

    ds5_log_printf("DS5 USB Audio: %s ready (encoder %d B, decoder %d B)\r\n",
                   opus_get_version_string(), encoder_size, decoder_size);
    return true;
}

static void ds5_usb_audio_encode_speaker(
    OpusEncoder *encoder, const ds5_usb_speaker_frame_t *speaker_frame)
{
    uint8_t opus_frame[DS5_AUDIO_SPEAKER_OPUS_SIZE];
    int encoded_length;
    size_t input_frame = 0U;
    size_t fraction = 0U;
    size_t output_frame;

    if ((encoder == NULL) || (speaker_frame == NULL)) {
        ++invalid_audio_packets;
        return;
    }

    /*
     * The transport consumes exactly 480 samples for every 512 USB samples.
     * WDL's linear mode evaluates the same 16:15 rational positions, but its
     * phase arithmetic is double precision.  Keep the original USB int16 PCM
     * representation and evaluate the 15-tap fractional denominator in int32;
     * this feeds the SDK's fixed-point encoder directly without a float round
     * trip. Every output position remains inside this complete 512-sample frame.
     */
    static_assert(DS5_AUDIO_SPEAKER_CHANNELS == 2U,
                  "packed E907 interpolation requires stereo PCM");
    static_assert(DS5_USB_AUDIO_SPEAKER_OUTPUT_STEP == 15U,
                  "fast rounded division is specialized for denominator 15");

    for (output_frame = 0U;
         output_frame < DS5_USB_AUDIO_SPEAKER_OUTPUT_FRAMES;
         ++output_frame) {
        typedef uint32_t ds5_alias_u32 __attribute__((__may_alias__));
        size_t input_offset =
            input_frame * DS5_AUDIO_SPEAKER_CHANNELS;
        size_t output_offset =
            output_frame * DS5_AUDIO_SPEAKER_CHANNELS;
        uint32_t current = *(const ds5_alias_u32 *)(const void *)
            &speaker_frame->data[input_offset];
        uint32_t next = *(const ds5_alias_u32 *)(const void *)
            &speaker_frame->data[input_offset + DS5_AUDIO_SPEAKER_CHANNELS];
        uint32_t fraction_u32 = (uint32_t)fraction;
        uint32_t current_weight =
            DS5_USB_AUDIO_SPEAKER_OUTPUT_STEP - fraction_u32;
        uint64_t current_products;
        uint64_t next_products;
        int32_t mixed_left;
        int32_t mixed_right;

        fraction_u32 |= fraction_u32 << 16;
        current_weight |= current_weight << 16;
        current_products = __rv__smul16(current, current_weight);
        next_products = __rv__smul16(next, fraction_u32);
        mixed_left = (int32_t)(uint32_t)current_products +
                     (int32_t)(uint32_t)next_products;
        mixed_right = (int32_t)(uint32_t)(current_products >> 32) +
                      (int32_t)(uint32_t)(next_products >> 32);

        speaker_opus_input[output_offset] =
            ds5_usb_audio_divide_round_15(mixed_left);
        speaker_opus_input[output_offset + 1U] =
            ds5_usb_audio_divide_round_15(mixed_right);

        /* Advance 16/15 without a divide/modulo pair per output sample. */
        ++input_frame;
        ++fraction;
        if (fraction == DS5_USB_AUDIO_SPEAKER_OUTPUT_STEP) {
            fraction = 0U;
            ++input_frame;
        }
    }

    encoded_length = opus_encode(
        encoder, speaker_opus_input, DS5_USB_AUDIO_SPEAKER_OUTPUT_FRAMES,
        opus_frame, sizeof(opus_frame));
    if (encoded_length <= 0) {
        ++invalid_audio_packets;
        return;
    }

    if ((size_t)encoded_length < sizeof(opus_frame)) {
        int pad_result = opus_packet_pad(
            opus_frame, encoded_length, sizeof(opus_frame));

        if (pad_result != OPUS_OK) {
            ++invalid_audio_packets;
            return;
        }
        encoded_length = sizeof(opus_frame);
    }

    /*
     * The DualSense report has a fixed 200-byte slot for each Opus packet.
     * Reject a packet before Bluetooth submission unless its standard Opus
     * framing still describes exactly one 10 ms stereo frame after padding.
     */
    if (((size_t)encoded_length != sizeof(opus_frame)) ||
        (opus_packet_get_nb_frames(opus_frame,
                                   sizeof(opus_frame)) != 1) ||
        (opus_packet_get_nb_samples(opus_frame, sizeof(opus_frame),
                                    DS5_AUDIO_SAMPLE_RATE) !=
         (int)DS5_USB_AUDIO_SPEAKER_OUTPUT_FRAMES) ||
        (opus_packet_get_nb_channels(opus_frame) !=
         (int)DS5_AUDIO_SPEAKER_CHANNELS)) {
        ++invalid_audio_packets;
        return;
    }

    if (!speaker_stream_open ||
        (speaker_frame->generation != audio_generation)) {
        return;
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
    const ds5_usb_audio_packet_t *packet,
    uint8_t *haptics_data, size_t *haptics_position,
    size_t *haptics_decimation_phase,
    int16_t *speaker_data, size_t *speaker_position)
{
    size_t input_frames;
    size_t frame;

    if ((packet == NULL) || (haptics_data == NULL) ||
        (haptics_position == NULL) ||
        (haptics_decimation_phase == NULL) || (speaker_data == NULL) ||
        (speaker_position == NULL)) {
        ++invalid_audio_packets;
        return;
    }

    input_frames = packet->length /
                   (DS5_USB_AUDIO_CHANNEL_COUNT *
                    DS5_USB_AUDIO_SAMPLE_BYTES);
    if (input_frames == 0U) {
        ++invalid_audio_packets;
        return;
    }

    for (frame = 0U; frame < input_frames; ++frame) {
        size_t frame_offset = frame *
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

        if (*haptics_decimation_phase == 0U) {
            haptics_data[(*haptics_position)++] =
                (uint8_t)ds5_usb_audio_s16_to_s8(haptics_left);
            haptics_data[(*haptics_position)++] =
                (uint8_t)ds5_usb_audio_s16_to_s8(haptics_right);

            if (*haptics_position == DS5_HAPTICS_DATA_SIZE) {
                if (ds5_haptics_mailbox_publish(
                        haptics_data, DS5_HAPTICS_DATA_SIZE)) {
                    ++published_haptics_blocks;
                    if (published_haptics_blocks == 1U) {
                        ds5_log_printf("DS5 USB Audio: first haptics block "
                                       "queued\r\n");
                    }
                }
                *haptics_position = 0U;
            }
        }

        *haptics_decimation_phase += 1U;
        if (*haptics_decimation_phase ==
            DS5_USB_AUDIO_HAPTICS_DECIMATION) {
            *haptics_decimation_phase = 0U;
        }

        speaker_data[(*speaker_position) * DS5_AUDIO_SPEAKER_CHANNELS] =
            speaker_left;
        speaker_data[(*speaker_position) * DS5_AUDIO_SPEAKER_CHANNELS + 1U] =
            speaker_right;
        ++(*speaker_position);

        if (*speaker_position == DS5_USB_AUDIO_SPEAKER_INPUT_FRAMES) {
            (void)ds5_usb_audio_queue_speaker_frame(packet->generation);
            *speaker_position = 0U;
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
    ds5_usb_audio_packet_t packet;
    uint8_t haptics_data[DS5_HAPTICS_DATA_SIZE];
    size_t haptics_position = 0U;
    size_t haptics_decimation_phase = 0U;
    size_t speaker_position = 0U;
    uint32_t active_generation = 0U;
    uint32_t observed_packets = 0U;

    (void)parameter;

    while (1) {
        if (xQueueReceive(audio_queue, &packet, portMAX_DELAY) != pdPASS) {
            continue;
        }

        ++observed_packets;
        if ((observed_packets & 0xffU) == 1U) {
            audio_task_stack_high_water_words =
                ds5_usb_audio_current_stack_high_water_words();
        }
        if (!speaker_stream_open ||
            (packet.generation != audio_generation)) {
            continue;
        }

        if (packet.generation != active_generation) {
            active_generation = packet.generation;
            haptics_position = 0U;
            haptics_decimation_phase = 0U;
            speaker_position = 0U;
        }

        ds5_usb_audio_process_packet(
            &packet, haptics_data, &haptics_position,
            &haptics_decimation_phase, speaker_staging_frame.data,
            &speaker_position);

        if ((observed_packets == 1U) ||
            ((observed_packets % DS5_USB_AUDIO_LOG_INTERVAL) == 0U)) {
            ds5_log_printf(
                "DS5 USB Audio: packets %lu, invalid %lu, USB dropped %lu, "
                "raw speaker dropped %lu, haptics %lu, speaker %lu, "
                "Opus dropped %lu, encode overruns %lu (max %lu us), "
                "mic %lu\r\n",
                (unsigned long)received_audio_packets,
                (unsigned long)invalid_audio_packets,
                (unsigned long)dropped_audio_packets,
                (unsigned long)dropped_speaker_input_frames,
                (unsigned long)published_haptics_blocks,
                (unsigned long)published_speaker_frames,
                (unsigned long)ds5_audio_mailbox_dropped_speaker_frames(),
                (unsigned long)speaker_encode_overruns,
                (unsigned long)max_speaker_encode_us,
                (unsigned long)decoded_microphone_frames);
        }
    }
}

static void ds5_usb_codec_task(void *parameter)
{
    uint8_t microphone_opus_data[DS5_AUDIO_MIC_OPUS_SIZE];
    OpusEncoder *encoder = NULL;
    OpusDecoder *decoder = NULL;
    size_t microphone_position = 0U;
    size_t microphone_sample_count = 0U;
    bool codec_initialization_attempted = false;

    (void)parameter;

    while (1) {
        bool did_work = false;

        /*
         * Keep HID enumeration and BR/EDR reconnect independent of Opus.
         * Haptics uses the separate ingress task and remains available even
         * if codec initialization fails.
         */
        if (!speaker_stream_open && !microphone_stream_open) {
            microphone_position = 0U;
            microphone_sample_count = 0U;
            while (xQueueReceive(speaker_queue, &speaker_codec_frame,
                                 0U) == pdPASS) {
            }
            vTaskDelay(pdMS_TO_TICKS(5U));
            continue;
        }

        if (!codec_initialization_attempted) {
            codecs_ready = ds5_usb_audio_init_codecs(&encoder, &decoder);
            codec_initialization_attempted = true;
        }

        if (speaker_stream_open && codecs_ready &&
            (xQueueReceive(speaker_queue, &speaker_codec_frame, 0U) ==
             pdPASS)) {
            did_work = true;
            if (speaker_codec_frame.generation == audio_generation) {
                uint64_t encode_start_us;
                uint64_t elapsed_us_64;
                uint32_t elapsed_us;
                uint32_t next_count;

                encode_start_us = bflb_mtimer_get_time_us();
                ds5_usb_audio_encode_speaker(
                    encoder, &speaker_codec_frame);
                elapsed_us_64 =
                    bflb_mtimer_get_time_us() - encode_start_us;
                elapsed_us = elapsed_us_64 > UINT32_MAX ? UINT32_MAX :
                                                               (uint32_t)elapsed_us_64;
                next_count = speaker_encode_count + 1U;
                total_speaker_encode_us += elapsed_us;
                speaker_encode_count = next_count;
                if ((next_count & 0x3fU) == 1U) {
                    codec_task_stack_high_water_words =
                        ds5_usb_audio_current_stack_high_water_words();
                }
                if (elapsed_us > max_speaker_encode_us) {
                    max_speaker_encode_us = elapsed_us;
                }
                if (elapsed_us >= DS5_USB_AUDIO_ENCODE_BUDGET_US) {
                    ++speaker_encode_overruns;
                }
            }
        } else if (!speaker_stream_open || !codecs_ready) {
            while (xQueueReceive(speaker_queue, &speaker_codec_frame,
                                 0U) == pdPASS) {
                did_work = true;
            }
        }

        if (microphone_stream_open && codecs_ready) {
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
        last_audio_completion_us = 0U;
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
        last_audio_completion_us = 0U;
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

extern "C" void ds5_usb_audio_get_diagnostics(
    ds5_usb_audio_diagnostics_t *diagnostics)
{
    if (diagnostics == NULL) {
        return;
    }

    diagnostics->speaker_stream_open = speaker_stream_open;
    diagnostics->microphone_stream_open = microphone_stream_open;
    diagnostics->codec_ready = codecs_ready;
    diagnostics->generation = audio_generation;
    diagnostics->received_packets = received_audio_packets;
    diagnostics->invalid_packets = invalid_audio_packets;
    diagnostics->dropped_packets = dropped_audio_packets;
    diagnostics->arm_failures = audio_arm_failures;
    diagnostics->usb_interval_gap_count = usb_interval_gap_count;
    diagnostics->max_usb_interval_us = max_usb_interval_us;
    diagnostics->published_haptics_blocks = published_haptics_blocks;
    diagnostics->dropped_haptics_blocks =
        ds5_haptics_mailbox_dropped_count();
    diagnostics->dropped_speaker_input_frames =
        dropped_speaker_input_frames;
    diagnostics->published_speaker_frames = published_speaker_frames;
    diagnostics->dropped_speaker_opus_frames =
        ds5_audio_mailbox_dropped_speaker_frames();
    diagnostics->speaker_encode_count = speaker_encode_count;
    diagnostics->speaker_encode_overruns = speaker_encode_overruns;
    diagnostics->average_speaker_encode_us = speaker_encode_count != 0U ?
        (uint32_t)(total_speaker_encode_us / speaker_encode_count) : 0U;
    diagnostics->max_speaker_encode_us = max_speaker_encode_us;
    diagnostics->audio_task_stack_high_water_words =
        audio_task_stack_high_water_words;
    diagnostics->codec_task_stack_high_water_words =
        codec_task_stack_high_water_words;
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
    dropped_speaker_input_frames = 0U;
    speaker_encode_overruns = 0U;
    speaker_encode_count = 0U;
    total_speaker_encode_us = 0U;
    max_speaker_encode_us = 0U;
    usb_interval_gap_count = 0U;
    max_usb_interval_us = 0U;
    decoded_microphone_frames = 0U;
    audio_task_stack_high_water_words = 0U;
    codec_task_stack_high_water_words = 0U;
    codecs_ready = false;
    speaker_muted = false;
    microphone_muted = false;
    speaker_volume_db = 0;
    microphone_volume_db = 0;
    last_audio_completion_us = 0U;

    audio_queue = xQueueCreateStatic(DS5_USB_AUDIO_QUEUE_LENGTH,
                                     sizeof(ds5_usb_audio_packet_t),
                                     audio_queue_items,
                                     &audio_queue_storage);
    if (audio_queue == NULL) {
        return -1;
    }

    speaker_queue = xQueueCreateStatic(DS5_USB_SPEAKER_QUEUE_LENGTH,
                                       sizeof(ds5_usb_speaker_frame_t),
                                       speaker_queue_items,
                                       &speaker_queue_storage);
    if (speaker_queue == NULL) {
        vQueueDelete(audio_queue);
        audio_queue = NULL;
        return -1;
    }

    audio_task = xTaskCreateStatic(ds5_usb_audio_task, "usb_audio",
                                   DS5_USB_AUDIO_TASK_STACK_DEPTH, NULL,
                                   DS5_USB_AUDIO_TASK_PRIORITY,
                                   audio_task_stack,
                                   &audio_task_storage);
    if (audio_task == NULL) {
        vQueueDelete(speaker_queue);
        speaker_queue = NULL;
        vQueueDelete(audio_queue);
        audio_queue = NULL;
        return -1;
    }

    codec_task = xTaskCreateStatic(ds5_usb_codec_task, "usb_codec",
                                   DS5_USB_CODEC_TASK_STACK_DEPTH, NULL,
                                   DS5_USB_CODEC_TASK_PRIORITY,
                                   codec_task_stack,
                                   &codec_task_storage);
    if (codec_task == NULL) {
        vTaskDelete(audio_task);
        audio_task = NULL;
        vQueueDelete(speaker_queue);
        speaker_queue = NULL;
        vQueueDelete(audio_queue);
        audio_queue = NULL;
        return -1;
    }

    if (ds5_usb_audio_class_init(busid) != 0) {
        vTaskDelete(codec_task);
        codec_task = NULL;
        vTaskDelete(audio_task);
        audio_task = NULL;
        vQueueDelete(speaker_queue);
        speaker_queue = NULL;
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
    codecs_ready = false;
    last_audio_completion_us = 0U;
    ds5_audio_mailbox_set_speaker_stream_active(false);
    (void)ds5_audio_mailbox_publish_microphone_stream_active(false);

    if (audio_task != NULL) {
        vTaskDelete(audio_task);
        audio_task = NULL;
    }
    if (codec_task != NULL) {
        vTaskDelete(codec_task);
        codec_task = NULL;
    }
    if (speaker_queue != NULL) {
        vQueueDelete(speaker_queue);
        speaker_queue = NULL;
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
        last_audio_completion_us = 0U;
        ++audio_generation;
        ds5_audio_mailbox_set_speaker_stream_active(false);
        (void)ds5_audio_mailbox_publish_microphone_stream_active(false);
        break;
    default:
        break;
    }
}
