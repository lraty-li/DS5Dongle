#include "ds5_usb_audio.h"

#include <stdbool.h>
#include <stdint.h>

#include "usbd_core.h"
#include "usbd_audio.h"

/*
 * BouffaloSDK v2.3.30's UAC header declares packed C structures with a
 * C-only __PACKED form.  Keep that header, its endpoints, and its weak class
 * callbacks in this C translation unit.  The C++ audio task only receives
 * normalized events through ds5_usb_audio.h.
 */
static struct usbd_interface audio_control_interface;
static struct usbd_interface audio_speaker_interface;
static struct usbd_interface audio_microphone_interface;

static struct audio_entity_info audio_entity_table[] = {
    {
        .bDescriptorSubtype = AUDIO_CONTROL_FEATURE_UNIT,
        .bEntityId = 0x02U,
        .ep = DS5_USB_AUDIO_OUT_EP,
    },
    {
        .bDescriptorSubtype = AUDIO_CONTROL_FEATURE_UNIT,
        .bEntityId = 0x05U,
        .ep = DS5_USB_AUDIO_IN_EP,
    },
};

static void ds5_usb_audio_adapter_out_callback(uint8_t busid, uint8_t endpoint,
                                               uint32_t transferred_bytes)
{
    ds5_usb_audio_on_out_complete(busid, endpoint, transferred_bytes);
}

static void ds5_usb_audio_adapter_in_callback(uint8_t busid, uint8_t endpoint,
                                              uint32_t transferred_bytes)
{
    ds5_usb_audio_on_in_complete(busid, endpoint, transferred_bytes);
}

static struct usbd_endpoint audio_out_endpoint = {
    .ep_addr = DS5_USB_AUDIO_OUT_EP,
    .ep_cb = ds5_usb_audio_adapter_out_callback,
};

static struct usbd_endpoint audio_in_endpoint = {
    .ep_addr = DS5_USB_AUDIO_IN_EP,
    .ep_cb = ds5_usb_audio_adapter_in_callback,
};

int ds5_usb_audio_class_init(uint8_t busid)
{
    usbd_add_interface(
        busid,
        usbd_audio_init_intf(busid, &audio_control_interface, 0x0100,
                             audio_entity_table,
                             sizeof(audio_entity_table) /
                                 sizeof(audio_entity_table[0])));
    usbd_add_interface(
        busid,
        usbd_audio_init_intf(busid, &audio_speaker_interface, 0x0100,
                             audio_entity_table,
                             sizeof(audio_entity_table) /
                                 sizeof(audio_entity_table[0])));
    usbd_add_interface(
        busid,
        usbd_audio_init_intf(busid, &audio_microphone_interface, 0x0100,
                             audio_entity_table,
                             sizeof(audio_entity_table) /
                                 sizeof(audio_entity_table[0])));
    usbd_add_endpoint(busid, &audio_out_endpoint);
    usbd_add_endpoint(busid, &audio_in_endpoint);
    return 0;
}

void usbd_audio_open(uint8_t busid, uint8_t interface)
{
    ds5_usb_audio_on_stream_open(busid, interface);
}

void usbd_audio_close(uint8_t busid, uint8_t interface)
{
    ds5_usb_audio_on_stream_close(busid, interface);
}

void usbd_audio_set_sampling_freq(uint8_t busid, uint8_t endpoint,
                                  uint32_t sampling_freq)
{
    ds5_usb_audio_on_set_sampling_freq(busid, endpoint, sampling_freq);
}

uint32_t usbd_audio_get_sampling_freq(uint8_t busid, uint8_t endpoint)
{
    return ds5_usb_audio_on_get_sampling_freq(busid, endpoint);
}

void usbd_audio_set_volume(uint8_t busid, uint8_t endpoint, uint8_t channel,
                           int volume_db)
{
    ds5_usb_audio_on_set_volume(busid, endpoint, channel, volume_db);
}

int usbd_audio_get_volume(uint8_t busid, uint8_t endpoint, uint8_t channel)
{
    return ds5_usb_audio_on_get_volume(busid, endpoint, channel);
}

void usbd_audio_set_mute(uint8_t busid, uint8_t endpoint, uint8_t channel,
                         bool mute)
{
    ds5_usb_audio_on_set_mute(busid, endpoint, channel, mute ? 1 : 0);
}

bool usbd_audio_get_mute(uint8_t busid, uint8_t endpoint, uint8_t channel)
{
    return ds5_usb_audio_on_get_mute(busid, endpoint, channel) != 0;
}
