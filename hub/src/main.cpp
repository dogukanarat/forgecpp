// forge-hub: the applications on this device, their state and links to them.
//
//   forge-hub -c /etc/forge-hub/forge-hub.toml     # http://<device>/
//   forgectl -e tcp://<device>:5551 call restart name=radio-controller
#include "hub.hpp"

FORGE_MAIN(forgehub::Hub)
