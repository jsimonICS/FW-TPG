import onewili

dev = onewili.connect()
try:
    res = dev.hardware.display_functions._call('r', [], [], timeout=180.0)
    print('restore_display_firmware ->', res)
finally:
    dev.close()