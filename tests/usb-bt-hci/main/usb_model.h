#ifndef USB_BT_CHECK_MODEL_H
#define USB_BT_CHECK_MODEL_H
#include <stdbool.h>
#include <stdint.h>
void usb_model_present(bool present);
void usb_model_stall(bool stall);
void usb_model_stall_control(bool stall);
/* Stop replies, or remove the adapter, at the given command count. */
void usb_model_stall_after(unsigned count);
void usb_model_remove_after(unsigned count);
void usb_model_fail_after(unsigned count);
/* Fail the command at this count with status 0x1f. */
void usb_model_status_after(unsigned count);
/* Refuse the next resets with status 0x07, as one adapter did after a session. */
void usb_model_refuse_resets(unsigned count);
/* Send an incoming connection request on the next idle event slot. */
void usb_model_connect(void);
/* Refuse Change Connection Packet Type with status 0x07 and this many
   command credits, as one adapter did; a negative value accepts it. */
void usb_model_refuse_packet_type(int credits);
/* Send one HCI event, as the remote side's controller reports it. */
void usb_model_event(unsigned code, const uint8_t *params, unsigned size);
/* Forget seen commands; find the last one with an opcode and copy its
   parameters. */
void usb_model_clear_commands(void);
bool usb_model_command(unsigned opcode, uint8_t *params, unsigned size);
bool usb_model_linked(void);
void usb_model_no_classic(bool value);
void usb_model_acl_size(unsigned size);
/* First byte of the adapter address; the rest follow it. */
void usb_model_address(unsigned first);
unsigned usb_model_allocations(void);
unsigned usb_model_commands(void);
unsigned usb_model_last_opcode(void);
unsigned usb_model_connection_attempts(void);
#endif
