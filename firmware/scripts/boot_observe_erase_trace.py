"""Bounded, secret-free observer for one authorized Wi-Fi diagnostic boot."""
import argparse
import json
from pathlib import Path
import re
import sys
import time

READY = 'BLE_WIFI_TEST_READY wifi=production_provisioning'
REASONS = {'destructive_erase_blocked', 'wifi_manager_start_failed', 'ble_start_failed', 'bond_store_full', 'secret_invalid', 'invalid_device_name',
           'nimble_init_failed', 'server_init_failed', 'service_init_failed',
           'characteristic_init_failed', 'advertising_init_failed',
           'advertising_start_failed'}
PRIMITIVES = {'nvs_flash_erase', 'nvs_flash_erase_partition', 'nvs_flash_erase_partition_ptr',
              'nvs_erase_key', 'nvs_erase_all', 'esp_partition_erase_range'}
PHASES = {'framework_startup', 'manager_begin', 'ble_begin', 'loop'}
TRACE = re.compile(r'^BLE_WIFI_TEST_ERASE primitive=([a-z_]+) phase=([a-z_]+) pc=(0x[0-9a-fA-F]{8})$')
FAULTS = ('Guru Meditation', 'assert failed', 'panic', 'abort()', 'Rebooting...')


def consume(line, result):
    text = line.decode('ascii', errors='ignore').strip()
    match = TRACE.fullmatch(text)
    if match and result['erase_denial'] is None:
        primitive, phase, pc = match.groups()
        if primitive in PRIMITIVES and phase in PHASES and int(pc, 16) != 0:
            result['erase_denial'] = {'primitive': primitive, 'phase': phase, 'caller_pc': pc.lower()}
    if text == READY:
        result['ready'] = True
    elif text.startswith('BLE_WIFI_TEST_STOP reason='):
        reason = text.partition('=')[2]
        result['stop_reason'] = reason if reason in REASONS else 'other_stop'
    for token in FAULTS:
        if token in text and token not in result['faults']:
            result['faults'].append(token)


def empty():
    return {'ready': False, 'stop_reason': None, 'erase_denial': None, 'faults': []}


def self_test():
    state = empty()
    consume(b'arbitrary serial/private namespace output', state)
    assert state == empty()
    consume(b'BLE_WIFI_TEST_ERASE primitive=unknown phase=loop pc=0x40381234', state)
    consume(b'BLE_WIFI_TEST_ERASE primitive=nvs_erase_all phase=unknown pc=0x40381234', state)
    consume(b'BLE_WIFI_TEST_ERASE primitive=nvs_erase_all phase=loop pc=0x00000000', state)
    assert state['erase_denial'] is None
    consume(b'BLE_WIFI_TEST_ERASE primitive=nvs_flash_erase phase=framework_startup pc=0x40381234', state)
    consume(b'BLE_WIFI_TEST_ERASE primitive=nvs_erase_all phase=loop pc=0x40389999', state)
    consume(b'BLE_WIFI_TEST_STOP reason=destructive_erase_blocked', state)
    assert state['erase_denial'] == {'primitive': 'nvs_flash_erase', 'phase': 'framework_startup', 'caller_pc': '0x40381234'}
    assert state['stop_reason'] == 'destructive_erase_blocked'
    assert 'private' not in json.dumps(state)
    print('SELF_TEST_PASS cases=7; typed metadata first-hit; arbitrary serial excluded; trace retained before stop')
    return 0


def main():
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--self-test', action='store_true')
    mode.add_argument('--run', action='store_true')
    parser.add_argument('--port', default='/dev/ttyACM0')
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    import serial
    import site
    site.addsitedir('/home/raya/.platformio/packages/tool-esptoolpy/_contrib')
    sys.path.insert(0, '/home/raya/.platformio/packages/tool-esptoolpy/_contrib')
    sys.path.insert(0, '/home/raya/.platformio/packages/tool-esptoolpy')
    from esptool.reset import HardReset
    result = empty()
    serial_port = serial.Serial(port=None, baudrate=115200, timeout=0.1, exclusive=True)
    serial_port.dtr = False
    serial_port.rts = False
    serial_port.port = args.port
    serial_port.open()
    started = time.monotonic()
    pending = bytearray()
    classified = 0
    try:
        serial_port.reset_input_buffer()
        HardReset(serial_port, uses_usb=True).reset()
        serial_port.dtr = True
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            pending.extend(serial_port.read(512))
            while b'\n' in pending:
                line, _, tail = pending.partition(b'\n')
                pending = bytearray(tail)
                consume(line, result)
                classified += 1
            if len(pending) > 4096:
                pending.clear()
            if result['stop_reason'] or result['faults']:
                break
    finally:
        serial_port.close()
    result.update({'observation_seconds': round(time.monotonic() - started, 3),
                   'serial_lines_classified_without_storage': classified,
                   'raw_serial_saved': False, 'browser_ble_actions': 'NOT_RUN'})
    print(json.dumps(result, indent=2))
    return 0 if result['ready'] and not result['stop_reason'] and not result['faults'] else 2


if __name__ == '__main__':
    sys.exit(main())
