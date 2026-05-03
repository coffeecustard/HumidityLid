from microcontroller import watchdog as w
from watchdog import WatchDogMode

# Disable Bluetooth to save power
import _bleio
_bleio.adapter.enabled = False

# Watchdog — reset board if it hangs for 5 minutes
w.timeout = 300
w.mode = WatchDogMode.RESET