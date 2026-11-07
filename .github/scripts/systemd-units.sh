#!/usr/bin/env bash
# CI: forgecpp applications as real systemd units (GitHub runners boot systemd).
#
# forge-hub and radio-controller are installed with the units forge_add_app
# generates. Checks: radio-controller is restarted after a crash and the hub
# shows the restarts; too many crashes leave the unit failed (start-limit-hit)
# and the hub shows it; the hub's restart command brings it back; stopping it
# from the hub is not a crash.
set -euo pipefail

hub=http://127.0.0.1:80
app=radio-controller

fail() { echo "FAIL: $*" >&2; dump; exit 1; }
dump() {
    systemctl status --no-pager forge-hub $app.service || true
    journalctl --no-pager -n 60 -u $app.service -u forge-hub.service || true
    curl -fs $hub/api/apps || true
}
# wait_for <seconds> <command...>: until the command succeeds
wait_for() {
    local t=$1; shift
    for _ in $(seq $((t * 10))); do
        if "$@" >/dev/null 2>&1; then return 0; fi
        sleep 0.1
    done
    return 1
}
main_pid() { systemctl show -p MainPID --value $app.service; }
prop() { systemctl show -p "$1" --value $app.service; }
# app_field <field>: a field of radio-controller's entry in the hub's list
app_field() {
    curl -fs $hub/api/apps | python3 -c "
import json, sys
apps = [a for a in json.load(sys.stdin).get('apps', []) if a['app']['name'] == '$app']
print(apps[0].get('$1', '') if apps else '')"
}
running() { [ "$(prop ActiveState)" = active ] && [ "$(app_field state)" = STATE_RUNNING ]; }
hub_says() { [ "$(app_field "$1")" = "$2" ]; }

systemctl daemon-reload
systemctl cat $app.service | grep -E '^(Restart|StartLimit|Watchdog)' || fail "generated unit not installed"
systemctl start forge-hub.service
wait_for 20 curl -fs $hub/api/apps || fail "forge-hub does not answer on port 80"
systemctl start $app.service

wait_for 30 running || fail "$app is not running / registered"
hub_says supervision "systemd $app.service" || fail "the hub does not follow the unit"
echo "running under systemd, registered with the hub"

# A crash: systemd restarts it (RestartSec=2), the hub counts it.
pid=$(main_pid)
kill -KILL "$pid"
wait_for 30 bash -c "[ \"\$(systemctl show -p NRestarts --value $app.service)\" = 1 ]" || fail "not restarted by systemd"
wait_for 30 running || fail "not running again after the restart"
wait_for 10 hub_says restarts 1 || fail "the hub does not show the restart"
echo "crash -> restarted by systemd, the hub shows 1 restart"

# MAX_RESTARTS 3: the 4th crash in the window leaves the unit failed.
for n in 2 3 4; do
    pid=$(main_pid)
    kill -KILL "$pid"
    if [ $n -lt 4 ]; then
        wait_for 30 bash -c "[ \"\$(systemctl show -p NRestarts --value $app.service)\" = $n ]" || fail "restart $n missing"
        wait_for 30 bash -c "pid=\$(systemctl show -p MainPID --value $app.service); [ \"\$pid\" != 0 ] && [ \"\$pid\" != $pid ]" \
            || fail "no new process after restart $n"
        wait_for 30 running || fail "not running after restart $n"
    fi
done
wait_for 30 bash -c "[ \"\$(systemctl show -p ActiveState --value $app.service)\" = failed ]" || fail "unit not failed after 4 crashes"
[ "$(prop Result)" = start-limit-hit ] || fail "Result is $(prop Result), expected start-limit-hit"
wait_for 15 hub_says state STATE_FAILED || fail "the hub does not show the failed unit"
wait_for 15 hub_says unit_result start-limit-hit || fail "the hub does not show start-limit-hit"
echo "4 crashes -> unit failed (start-limit-hit), shown by the hub"

# The hub's restart button: reset-failed + restart.
curl -fs -X POST -H 'Content-Type: application/json' -d "{\"name\":\"$app\"}" $hub/forge/api/commands/restart \
    || fail "restart command refused"
wait_for 30 running || fail "not running after the hub's restart"
echo "hub restart -> running again"

# Stopped from the hub: a stop, not a crash.
curl -fs -X POST -H 'Content-Type: application/json' -d "{\"name\":\"$app\"}" $hub/forge/api/commands/stop \
    || fail "stop command refused"
wait_for 30 bash -c "[ \"\$(systemctl show -p ActiveState --value $app.service)\" = inactive ]" || fail "not stopped"
wait_for 15 hub_says state STATE_STOPPED || fail "the hub does not show it stopped"
echo "hub stop -> stopped (not a crash)"
dump >/dev/null
