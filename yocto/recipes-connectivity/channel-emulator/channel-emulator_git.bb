SUMMARY = "RF channel emulator for forgecpp radios"
DESCRIPTION = "Takes a sender's TX stream, adds noise, frequency offset, fading and delay, and feeds \
the result to a receiver: a bench between two radio programs without a transceiver. Built on forgecpp."
FORGECPP_APP = "channel-emulator"
require recipes-connectivity/forgecpp-app.inc

# Off by default: it only does something when a sender's TX points at it.
SYSTEMD_AUTO_ENABLE:${PN} ?= "disable"
RRECOMMENDS:${PN} = "radio-controller"
