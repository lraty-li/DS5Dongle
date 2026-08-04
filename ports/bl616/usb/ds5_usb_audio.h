#ifndef DS5_USB_AUDIO_H
#define DS5_USB_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS5_USB_AUDIO_CONTROL_INTERFACE    0x03U
#define DS5_USB_AUDIO_STREAM_INTERFACE     0x04U
#define DS5_USB_AUDIO_OUT_EP               0x01U
#define DS5_USB_AUDIO_CHANNEL_COUNT        4U
#define DS5_USB_AUDIO_SAMPLE_RATE          48000U
#define DS5_USB_AUDIO_SAMPLE_BYTES         2U
#define DS5_USB_AUDIO_PACKET_BYTES         384U
#define DS5_USB_AUDIO_MAX_PACKET_BYTES     392U
#define DS5_USB_AUDIO_CONFIG_DESCRIPTOR_SIZE 111U

int ds5_usb_audio_init(uint8_t busid);
void ds5_usb_audio_deinit(void);
void ds5_usb_audio_handle_event(uint8_t busid, uint8_t event);

#ifdef __cplusplus
}
#endif

#endif
