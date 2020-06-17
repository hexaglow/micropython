from micropython import const

import bluetooth
import struct
import time
from ble_advertising import advertising_payload

_IRQ_CENTRAL_CONNECT = const(1)
_IRQ_CENTRAL_DISCONNECT = const(2)

_ADV_APPEARANCE_KEYBOARD = const(0x03c1)

_HID_SERVICE_UUID = bluetooth.UUID(0x1812)
_REPORT_CHAR_UUID = bluetooth.UUID(0x2A4D)
_REPORT_MAP_CHAR_UUID = bluetooth.UUID(0x2A4B)
_HID_INFORMATION_CHAR_UUID = bluetooth.UUID(0x2A4A)
_HID_CONTROL_POINT_CHAR_UUID = bluetooth.UUID(0x2A4C)
_REPORT_REF_DESC_UUID = bluetooth.UUID(0x2908)
_PROTOCOL_MODE_CHAR_UUID = bluetooth.UUID(0x2A4E)

_BOOT_KEYBOARD_INPUT_REPORT_CHAR_UUID = bluetooth.UUID(0x2A22)
_BOOT_KEYBOARD_OUTPUT_REPORT_CHAR_UUID = bluetooth.UUID(0x2A32)

_F_READ = bluetooth.FLAG_READ
_F_WRITE = bluetooth.FLAG_WRITE
_F_READ_WRITE = bluetooth.FLAG_READ | bluetooth.FLAG_WRITE
_F_READ_NOTIFY = bluetooth.FLAG_READ | bluetooth.FLAG_NOTIFY

_HID_SERVICE = (
    _HID_SERVICE_UUID,
    (
        (_REPORT_CHAR_UUID, _F_READ_NOTIFY, ((_REPORT_REF_DESC_UUID, _F_READ),)),
        (_REPORT_CHAR_UUID, _F_READ_NOTIFY, ((_REPORT_REF_DESC_UUID, _F_READ),)),
        (_REPORT_CHAR_UUID, _F_READ_NOTIFY, ((_REPORT_REF_DESC_UUID, _F_READ),)),
        (_REPORT_CHAR_UUID, _F_READ_WRITE, ((_REPORT_REF_DESC_UUID, _F_READ),)),
        (_PROTOCOL_MODE_CHAR_UUID, _F_READ_WRITE),
        (_BOOT_KEYBOARD_INPUT_REPORT_CHAR_UUID, _F_READ_NOTIFY),
        (_BOOT_KEYBOARD_OUTPUT_REPORT_CHAR_UUID, _F_READ_WRITE),
        (_REPORT_MAP_CHAR_UUID, _F_READ),
        (_HID_INFORMATION_CHAR_UUID, _F_READ),
        (_HID_CONTROL_POINT_CHAR_UUID, _F_WRITE),
    )
)

_REPORT_TYPE_INPUT = const(1)
_REPORT_TYPE_OUTPUT = const(2)

_PROTOCOL_MODE_REPORT = b'\x01'

HID_DESCRIPTOR = (
    b'\x05\x01'        # Usage Page (Generic Desktop Ctrls)
    b'\x09\x06'        # Usage (Keyboard)
    b'\xA1\x01'        # Collection (Application)
    b'\x85\x01'        #   Report ID (1)
    b'\x05\x07'        #   Usage Page (Kbrd/Keypad)
    b'\x19\xE0'        #   Usage Minimum (\xE0)
    b'\x29\xE7'        #   Usage Maximum (\xE7)
    b'\x15\x00'        #   Logical Minimum (0)
    b'\x25\x01'        #   Logical Maximum (1)
    b'\x75\x01'        #   Report Size (1)
    b'\x95\x08'        #   Report Count (8)
    b'\x81\x02'        #   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    b'\x81\x01'        #   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    b'\x19\x00'        #   Usage Minimum (\x00)
    b'\x29\x65'        #   Usage Maximum (\x65)
    b'\x15\x00'        #   Logical Minimum (0)
    b'\x25\x65'        #   Logical Maximum (101)
    b'\x75\x08'        #   Report Size (8)
    b'\x95\x06'        #   Report Count (6)
    b'\x81\x00'        #   Input (Data,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    b'\x05\x08'        #   Usage Page (LEDs)
    b'\x19\x01'        #   Usage Minimum (Num Lock)
    b'\x29\x05'        #   Usage Maximum (Kana)
    b'\x15\x00'        #   Logical Minimum (0)
    b'\x25\x01'        #   Logical Maximum (1)
    b'\x75\x01'        #   Report Size (1)
    b'\x95\x05'        #   Report Count (5)
    b'\x91\x02'        #   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    b'\x95\x03'        #   Report Count (3)
    b'\x91\x01'        #   Output (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    b'\xC0'            # End Collection
    b'\x05\x01'        # Usage Page (Generic Desktop Ctrls)
    b'\x09\x02'        # Usage (Mouse)
    b'\xA1\x01'        # Collection (Application)
    b'\x09\x01'        #   Usage (Pointer)
    b'\xA1\x00'        #   Collection (Physical)
    b'\x85\x02'        #     Report ID (2)
    b'\x05\x09'        #     Usage Page (Button)
    b'\x19\x01'        #     Usage Minimum (\x01)
    b'\x29\x05'        #     Usage Maximum (\x05)
    b'\x15\x00'        #     Logical Minimum (0)
    b'\x25\x01'        #     Logical Maximum (1)
    b'\x95\x05'        #     Report Count (5)
    b'\x75\x01'        #     Report Size (1)
    b'\x81\x02'        #     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    b'\x95\x01'        #     Report Count (1)
    b'\x75\x03'        #     Report Size (3)
    b'\x81\x01'        #     Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    b'\x05\x01'        #     Usage Page (Generic Desktop Ctrls)
    b'\x09\x30'        #     Usage (X)
    b'\x09\x31'        #     Usage (Y)
    b'\x15\x81'        #     Logical Minimum (-127)
    b'\x25\x7F'        #     Logical Maximum (127)
    b'\x75\x08'        #     Report Size (8)
    b'\x95\x02'        #     Report Count (2)
    b'\x81\x06'        #     Input (Data,Var,Rel,No Wrap,Linear,Preferred State,No Null Position)
    b'\x09\x38'        #     Usage (Wheel)
    b'\x15\x81'        #     Logical Minimum (-127)
    b'\x25\x7F'        #     Logical Maximum (127)
    b'\x75\x08'        #     Report Size (8)
    b'\x95\x01'        #     Report Count (1)
    b'\x81\x06'        #     Input (Data,Var,Rel,No Wrap,Linear,Preferred State,No Null Position)
    b'\xC0'            #   End Collection
    b'\xC0'            # End Collection
    b'\x05\x0C'        # Usage Page (Consumer)
    b'\x09\x01'        # Usage (Consumer Control)
    b'\xA1\x01'        # Collection (Application)
    b'\x85\x03'        #   Report ID (3)
    b'\x75\x10'        #   Report Size (16)
    b'\x95\x01'        #   Report Count (1)
    b'\x15\x01'        #   Logical Minimum (1)
    b'\x26\x8C\x02'    #   Logical Maximum (652)
    b'\x19\x01'        #   Usage Minimum (Consumer Control)
    b'\x2A\x8C\x02'    #   Usage Maximum (AC Send)
    b'\x81\x00'        #   Input (Data,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    b'\xC0'            # End Collection
    b'\x05\x01'        # Usage Page (Generic Desktop Ctrls)
    # b'\x09\x05'        # Usage (Game Pad)
    # b'\xA1\x01'        # Collection (Application)
    # b'\x85\x05'        #   Report ID (5)
    # b'\x05\x09'        #   Usage Page (Button)
    # b'\x19\x01'        #   Usage Minimum (\x01)
    # b'\x29\x10'        #   Usage Maximum (\x10)
    # b'\x15\x00'        #   Logical Minimum (0)
    # b'\x25\x01'        #   Logical Maximum (1)
    # b'\x75\x01'        #   Report Size (1)
    # b'\x95\x10'        #   Report Count (16)
    # b'\x81\x02'        #   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    # b'\x05\x01'        #   Usage Page (Generic Desktop Ctrls)
    # b'\x15\x81'        #   Logical Minimum (-127)
    # b'\x25\x7F'        #   Logical Maximum (127)
    # b'\x09\x30'        #   Usage (X)
    # b'\x09\x31'        #   Usage (Y)
    # b'\x09\x32'        #   Usage (Z)
    # b'\x09\x35'        #   Usage (Rz)
    # b'\x75\x08'        #   Report Size (8)
    # b'\x95\x04'        #   Report Count (4)
    # b'\x81\x02'        #   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    # b'\xC0'            # End Collection
)

class BLEHIDKeyboard:
    def __init__(self, ble, name="mpy-kbd"):
        self._ble = ble
        self._ble.active(True)
        self._ble.irq(handler=self._irq)

        self._adv_payload = advertising_payload(
            name=name, services=[_HID_SERVICE_UUID], appearance=_ADV_APPEARANCE_KEYBOARD
        )

        ((self._h_rep, self._h_d1, _, self._h_d2, _, self._h_d3, _, self._h_d4, self._h_proto, _, _, self._h_hid, self._h_info, _,),) = self._ble.gatts_register_services((_HID_SERVICE,))

        self._ble.gatts_write(self._h_d1, struct.pack('<BB', 1, _REPORT_TYPE_INPUT))
        self._ble.gatts_write(self._h_d2, struct.pack('<BB', 2, _REPORT_TYPE_INPUT))
        self._ble.gatts_write(self._h_d3, struct.pack('<BB', 3, _REPORT_TYPE_INPUT))
        self._ble.gatts_write(self._h_d4, struct.pack('<BB', 1, _REPORT_TYPE_OUTPUT))
        self._ble.gatts_write(self._h_proto, _PROTOCOL_MODE_REPORT)
        self._ble.gatts_write(self._h_hid, HID_DESCRIPTOR)
        self._ble.gatts_write(self._h_info, b'\x01\x01\x00\x02') # bcd1.1, country = 0, flag = normal connect

        self._connections = set()
        self._advertise()

    def _irq(self, event, data):
        # Track connections so we can send notifications.
        if event == _IRQ_CENTRAL_CONNECT:
            conn_handle, _, _, = data
            self._connections.add(conn_handle)
        elif event == _IRQ_CENTRAL_DISCONNECT:
            conn_handle, _, _, = data
            self._connections.remove(conn_handle)
            # Start advertising again to allow a new connection.
            self._advertise()

    def _advertise(self, interval_us=500000):
        self._ble.gap_advertise(interval_us, adv_data=self._adv_payload)

    def _send_char(self, c):
        if c == ' ':
            mod = 0
            code = 0x2c
        elif ord('a') <= ord(c) <= ord('z'):
            mod = 0
            code = 0x04 + ord(c) - ord('a')
        elif ord('A') <= ord(c) <= ord('Z'):
            mod = 2
            code = 0x04 + ord(c) - ord('A')
        else:
            assert 0

        for conn_handle in self._connections:
            print('down:', struct.pack('8B', mod, 0, code, 0, 0, 0, 0, 0))
            print('up:', b'\x00\x00\x00\x00\x00\x00\x00\x00')
            self._ble.gatts_notify(conn_handle, self._h_rep, struct.pack('8B', mod, 0, code, 0, 0, 0, 0, 0))
            self._ble.gatts_notify(conn_handle, self._h_rep, b'\x00\x00\x00\x00\x00\x00\x00\x00')

    def send(self, s):
        for c in s:
            self._send_char(c)

    def is_connected(self):
        return len(self._connections) > 0


def demo():
    ble = bluetooth.BLE()
    kbd = BLEHIDKeyboard(ble)

    while True:
        if kbd.is_connected():
            kbd.send('hello')
        time.sleep_ms(5000)


if __name__ == "__main__":
    demo()
