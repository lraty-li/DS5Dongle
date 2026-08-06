#ifndef DS5_USB_AUDIO_H
#define DS5_USB_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS5_USB_AUDIO_CONTROL_INTERFACE    0x00U
#define DS5_USB_AUDIO_SPEAKER_INTERFACE    0x01U
#define DS5_USB_AUDIO_MICROPHONE_INTERFACE 0x02U
#define DS5_USB_AUDIO_OUT_EP               0x01U
#define DS5_USB_AUDIO_IN_EP                0x82U
#define DS5_USB_AUDIO_CHANNEL_COUNT        4U
#define DS5_USB_AUDIO_MICROPHONE_CHANNEL_COUNT 2U
#define DS5_USB_AUDIO_SAMPLE_RATE          48000U
#define DS5_USB_AUDIO_SAMPLE_BYTES         2U
#define DS5_USB_AUDIO_PACKET_BYTES         384U
#define DS5_USB_AUDIO_MAX_PACKET_BYTES     392U
#define DS5_USB_AUDIO_MICROPHONE_PACKET_BYTES 192U
/* AudioControl plus the two AudioStreaming interface descriptor blocks. */
#define DS5_USB_AUDIO_CONFIG_DESCRIPTOR_SIZE 186U

int ds5_usb_audio_init(uint8_t busid);
void ds5_usb_audio_deinit(void);
void ds5_usb_audio_handle_event(uint8_t busid, uint8_t event);

/* C bridge for CherryUSB's C-only UAC v1 implementation. */
int ds5_usb_audio_class_init(uint8_t busid);
void ds5_usb_audio_on_stream_open(uint8_t busid, uint8_t interface);
void ds5_usb_audio_on_stream_close(uint8_t busid, uint8_t interface);
void ds5_usb_audio_on_set_sampling_freq(uint8_t busid, uint8_t endpoint,
                                         uint32_t sampling_freq);
uint32_t ds5_usb_audio_on_get_sampling_freq(uint8_t busid, uint8_t endpoint);
void ds5_usb_audio_on_set_volume(uint8_t busid, uint8_t endpoint,
                                 uint8_t channel, int volume_db);
int ds5_usb_audio_on_get_volume(uint8_t busid, uint8_t endpoint,
                                uint8_t channel);
void ds5_usb_audio_on_set_mute(uint8_t busid, uint8_t endpoint,
                               uint8_t channel, int mute);
int ds5_usb_audio_on_get_mute(uint8_t busid, uint8_t endpoint,
                              uint8_t channel);
void ds5_usb_audio_on_out_complete(uint8_t busid, uint8_t endpoint,
                                   uint32_t transferred_bytes);
void ds5_usb_audio_on_in_complete(uint8_t busid, uint8_t endpoint,
                                  uint32_t transferred_bytes);

#ifdef __cplusplus
}
#endif

#endif
